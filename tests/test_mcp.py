#!/usr/bin/env python3
# ps5-mcp - end-to-end tests against the host mock build.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   python3 tests/test_mcp.py --binary build/ps5-mcp-host
#
# Starts the server on a free port with a throwaway mock root, speaks raw
# JSON-RPC over HTTP (stdlib only), and checks every tool end to end.
import argparse
import base64
import concurrent.futures
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

PASSED = []
FAILED = []


def check(name, condition, detail=""):
    (PASSED if condition else FAILED).append(name)
    print(f"{'ok  ' if condition else 'FAIL'} {name}{'' if condition else ' -- ' + str(detail)[:400]}")


class Client:
    def __init__(self, url):
        self.url = url
        self.next_id = 1

    def post(self, body, headers=None):
        data = json.dumps(body).encode() if not isinstance(body, bytes) else body
        h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        h.update(headers or {})
        req = urllib.request.Request(self.url, data=data, headers=h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=150) as r:
                raw = r.read()
                return r.status, (json.loads(raw) if raw else None)
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                return e.code, json.loads(raw)
            except ValueError:
                return e.code, raw

    def rpc(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        return self.post(msg)[1]

    def call(self, tool, **args):
        resp = self.rpc("tools/call", {"name": tool, "arguments": args})
        if "error" in resp:
            raise RuntimeError(resp["error"])
        return resp["result"]


def text_of(result):
    return "\n".join(c["text"] for c in result["content"] if c["type"] == "text")


def images_of(result):
    return [c for c in result["content"] if c["type"] == "image"]


def image_size(block):
    data = base64.b64decode(block["data"])
    if block["mimeType"] == "image/png":
        assert data[:8] == b"\x89PNG\r\n\x1a\n"
        return struct.unpack(">II", data[16:24])
    assert data[:3] == b"\xff\xd8\xff", "not a JPEG"
    i = 2
    while i < len(data):
        marker, length = data[i + 1], struct.unpack(">H", data[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + length
    raise ValueError("no SOF")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def write_png(path, w, h, rgb=(10, 200, 30)):
    import zlib
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, payload):
        c = kind + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    with open(path + ".part", "wb") as f:
        f.write(png)
    os.rename(path + ".part", path)


def run(binary):
    root = tempfile.mkdtemp(prefix="ps5mcp-test-")
    port = free_port()
    env = dict(os.environ, PS5MCP_MOCK_ROOT=root, PS5MCP_PORT=str(port),
               ASAN_OPTIONS="detect_leaks=0")
    log = open(os.path.join(root, "server.log"), "w+")
    proc = subprocess.Popen([binary], env=env, stdout=log, stderr=subprocess.STDOUT)
    url = f"http://127.0.0.1:{port}/mcp"
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.05)
    c = Client(url)
    try:
        protocol(c, url)
        pad_and_screens(c, root)
        diagnostics(c, root)
        apps(c)
        elf(c, root)
        concurrency(c)
        r = c.call("server_shutdown", confirm=True)
        check("server_shutdown replies", not r["isError"], r)
        try:
            code = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            code = None
        check("server exits cleanly after shutdown", code == 0, code)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGKILL)
            proc.wait()
        log.seek(0)
        out = log.read()
        check("no sanitizer reports", "AddressSanitizer" not in out and "runtime error" not in out,
              out[-2000:])
        if FAILED:
            print("---- server log (tail) ----\n" + out[-3000:])
        shutil.rmtree(root, ignore_errors=True)


def protocol(c, url):
    s, r = c.post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                   "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "test", "version": "0"}}})
    check("initialize 200", s == 200, s)
    check("initialize echoes a supported version", r["result"]["protocolVersion"] == "2025-06-18", r)
    check("initialize advertises tools", "tools" in r["result"]["capabilities"], r)
    check("initialize carries instructions", "pad_open" in r["result"].get("instructions", ""))
    r = c.rpc("initialize", {"protocolVersion": "1999-01-01", "capabilities": {}})
    check("unknown version -> latest", r["result"]["protocolVersion"] == "2025-11-25", r)

    s, body = c.post({"jsonrpc": "2.0", "method": "notifications/initialized"})
    check("notification -> 202 no body", s == 202 and body is None, (s, body))
    try:
        urllib.request.urlopen(url, timeout=5)
        check("GET /mcp -> 405", False, "200")
    except urllib.error.HTTPError as e:
        check("GET /mcp -> 405", e.code == 405, e.code)
    with urllib.request.urlopen(url.replace("/mcp", "/health"), timeout=5) as h:
        check("GET /health lists tools", b"pad_sequence" in h.read())
    s, r = c.post(b"{not json")
    check("bad JSON -> parse error", s == 400 and r["error"]["code"] == -32700, (s, r))
    r = c.rpc("nope/nope")
    check("unknown method -> -32601", r["error"]["code"] == -32601, r)
    r = c.rpc("tools/call", {"name": "does_not_exist", "arguments": {}})
    check("unknown tool -> -32602", r["error"]["code"] == -32602, r)
    s, r = c.post([{"jsonrpc": "2.0", "id": 10, "method": "ping"},
                   {"jsonrpc": "2.0", "method": "notifications/initialized"},
                   {"jsonrpc": "2.0", "id": 11, "method": "ping"}])
    check("batch answers requests only", s == 200 and [x["id"] for x in r] == [10, 11], r)
    s, r = c.post({"jsonrpc": "2.0", "id": 3, "method": "ping"},
                  headers={"Origin": "http://evil.example"})
    check("foreign Origin -> 403", s == 403, s)
    s, r = c.post({"jsonrpc": "2.0", "id": 3, "method": "ping"},
                  headers={"Origin": "http://localhost:3000"})
    check("localhost Origin allowed", s == 200, s)
    s, r = c.post({"jsonrpc": "2.0", "id": 3, "method": "ping"},
                  headers={"Origin": "http://localhost.evil.example"})
    check("localhost.evil Origin refused", s == 403, s)
    s, r = c.post({"jsonrpc": "2.0", "id": 3, "method": "ping"},
                  headers={"Origin": "http://rebind.example:17165", "Host": "rebind.example:17165"})
    check("rebinding (name in Origin and Host) refused", s == 403, s)
    s, r = c.post({"jsonrpc": "2.0", "id": 3, "method": "ping"},
                  headers={"Origin": "http://127.0.0.1:1234"})
    check("same-IP Origin allowed", s == 200, s)
    # chunked request body
    import http.client
    hc = http.client.HTTPConnection("127.0.0.1", int(url.split(":")[2].split("/")[0]), timeout=5)
    body = json.dumps({"jsonrpc": "2.0", "id": 4, "method": "ping"}).encode()
    hc.putrequest("POST", "/mcp")
    hc.putheader("Content-Type", "application/json")
    hc.putheader("Transfer-Encoding", "chunked")
    hc.endheaders()
    hc.send(b"%x\r\n%s\r\n0\r\n\r\n" % (len(body), body))
    resp = hc.getresponse()
    check("chunked request body", resp.status == 200 and json.loads(resp.read())["id"] == 4)

    tools = c.rpc("tools/list")["result"]["tools"]
    names = {t["name"] for t in tools}
    expected = {"pad_open", "pad_close", "pad_status", "pad_press", "pad_hold", "pad_release",
                "pad_sequence", "screenshot", "system_info", "notify", "server_shutdown",
                "list_processes", "process_info", "kill_process", "klog_read", "fs_list",
                "fs_read", "app_list", "app_launch", "app_kill", "elf_run"}
    check("tools/list has every tool", names == expected, names ^ expected)
    shot_schema = next(t["inputSchema"] for t in tools if t["name"] == "screenshot")
    check("screenshot mode enum includes native",
          "native" in shot_schema["properties"]["mode"]["enum"], shot_schema["properties"]["mode"])
    check("every schema is an object schema",
          all(t["inputSchema"].get("type") == "object" for t in tools))
    check("annotations present", all("readOnlyHint" in t["annotations"] for t in tools))


def pad_and_screens(c, root):
    r = c.call("pad_press", buttons="cross")
    check("press before open is an error", r["isError"] and "pad_open" in text_of(r), r)
    r = c.call("screenshot")
    check("system screenshot before open is an error", r["isError"], r)
    r = c.call("screenshot", mode="latest")
    check("latest with no captures is an error", r["isError"], r)

    t = time.time()
    r = c.call("pad_open")
    check("pad_open", not r["isError"], r)
    check("pad_open dismisses the prompt", "dismissed" in text_of(r), text_of(r))
    out = text_of(r)
    check("pad_open parsed the device id from a DEVICE_ADDED klog line (subType 22 only)",
          r["structuredContent"].get("device") == "0x2a00000000001234", r.get("structuredContent"))
    ev = r["structuredContent"].get("evicted", [])
    check("pad_open parsed the [CIM] table and evicted only the real controller",
          len(ev) == 2 and ev[0].startswith("0x2a0000000000beef (main 1 sub 1)")
          and ev[1].startswith("0x2a00000000005555 (main 1 sub 22)")
          and not any("0x2a00000000001234" in e for e in ev), ev)
    check("exclusive pad_open recreated the pad for the first slot",
          "recreated_for_first_slot" in r["structuredContent"], list(r["structuredContent"]))
    r = c.call("pad_status")
    check("pad_status open", r["structuredContent"]["open"] is True, r)

    r = c.call("pad_press", buttons="crosss")
    check("unknown button rejected", r["isError"] and "unknown button" in text_of(r), r)
    r = c.call("pad_press", buttons=["l1", "r1"], hold_ms=50, repeat=2, gap_ms=20)
    check("chord press x2", not r["isError"] and "l1+r1 x2" in text_of(r), text_of(r))

    r = c.call("pad_sequence", steps="down*2 cross:200 wait:100 ls:up:100 bogus")
    check("bad sequence rejected before pressing",
          r["isError"] and "nothing was pressed" in text_of(r), text_of(r))
    t = time.time()
    r = c.call("pad_sequence", steps="down*2 cross:200 wait:100 ls:up:100 rs:0.5,-0.5:50 shot")
    imgs = images_of(r)
    check("sequence with shot returns an image", not r["isError"] and len(imgs) == 1, text_of(r))
    if imgs:
        check("shot scaled to 1280x720 JPEG", image_size(imgs[0]) == (1280, 720)
              and imgs[0]["mimeType"] == "image/jpeg", image_size(imgs[0]))
    check("sequence log lists steps", "ls:up:100" in text_of(r), text_of(r))
    photos = []
    for d, _, files in os.walk(os.path.join(root, "user/av_contents")):
        photos += [f for f in files if f.endswith(".png")]
    check("system capture deleted after delivery", photos == [], photos)

    r = c.call("screenshot", crop={"x": 0, "y": 0, "width": 480, "height": 480}, max_width=0,
               format="png", keep_on_console=True)
    imgs = images_of(r)
    check("cropped PNG at source scale", imgs and image_size(imgs[0]) == (480, 480)
          and imgs[0]["mimeType"] == "image/png", text_of(r))
    r = c.call("screenshot", mode="native")
    check("native capture: clean console-only error on host",
          r["isError"] and "console" in text_of(r) and not images_of(r), text_of(r))
    r = c.call("screenshot", mode="latest-ish")
    check("bogus mode rejected", r["isError"] and "system, title, latest or native" in text_of(r),
          text_of(r))
    r = c.call("screenshot", mode="latest", max_width=640, grayscale=True)
    check("latest returns the kept capture", not r["isError"] and images_of(r)
          and image_size(images_of(r)[0]) == (640, 360), text_of(r))

    jxr_dir = os.path.join(root, "user/av_contents/photo/HDR00000")
    os.makedirs(jxr_dir)
    jxr = os.path.join(jxr_dir, "zz_hdr.jxr")
    with open(jxr, "wb") as f:
        f.write(b"II\xbc\x01" + b"\0" * 1000)
    future = time.time() + 60
    os.utime(jxr, (future, future))
    r = c.call("screenshot", mode="latest")
    check("JXR capture: text-only error, file left", r["isError"] and not images_of(r)
          and len(r["content"]) == 1 and "HDR" in text_of(r) and os.path.exists(jxr), r)
    os.unlink(jxr)

    # A title with the grab hook, simulated.
    req = os.path.join(root, "title-request")
    folder = os.path.join(root, "title-shots")
    os.makedirs(folder)
    stop = threading.Event()

    def title():
        while not stop.is_set():
            try:
                os.unlink(req)
                write_png(os.path.join(folder, f"frame-{time.time():.3f}.png"), 1920, 1080)
            except FileNotFoundError:
                pass
            time.sleep(0.016)

    th = threading.Thread(target=title, daemon=True)
    th.start()
    r = c.call("screenshot", mode="title", request_path=req, folder=folder)
    stop.set()
    th.join()
    check("title grab returns the frame", not r["isError"] and images_of(r)
          and image_size(images_of(r)[0]) == (1280, 720), text_of(r))
    check("title frame deleted", os.listdir(folder) == [], os.listdir(folder))
    r = c.call("screenshot", mode="title", request_path=req, folder=folder, timeout_ms=600)
    check("title without hook times out cleanly", r["isError"] and "never took" in text_of(r)
          and not os.path.exists(req), text_of(r))

    r = c.call("pad_hold", buttons="r2", left_stick={"x": -1, "y": 0}, duration_ms=100)
    check("pad_hold with duration", not r["isError"] and "lx=0" in text_of(r) and "r2=255" in text_of(r),
          text_of(r))
    r = c.call("pad_hold", right_stick={"x": 1, "y": 1}, l2=0.5)
    check("pad_hold stays held", "until pad_release" in text_of(r), text_of(r))
    st = c.call("pad_status")["structuredContent"]["held"]
    check("held state visible", st["rx"] == 255 and st["ry"] == 255 and st["l2"] == 128
          and "l2" in st["buttons"], st)
    c.call("pad_release")
    st = c.call("pad_status")["structuredContent"]["held"]
    check("pad_release neutral", st["buttons"] == "none" and st["lx"] == 128, st)
    r = c.call("pad_press", buttons="cross", screenshot_after=True, settle_ms=0,
               screenshot={"max_width": 320})
    check("press with screenshot_after", images_of(r) and image_size(images_of(r)[0]) == (320, 180),
          text_of(r))
    r = c.call("pad_sequence", steps="wait:130000")
    check("over-long sequence refused", r["isError"], text_of(r))


def diagnostics(c, root):
    r = c.call("system_info")
    s = r["structuredContent"]
    check("system_info sections", all(k in s for k in ("server", "console", "pad", "klog")), s.keys())
    r = c.call("klog_read", tail=50)
    check("klog_read tail has a cursor", text_of(r).startswith("cursor="), text_of(r))
    cursor = int(text_of(r).split()[0].split("=")[1])
    c.call("notify", message="hello from the test")
    r = c.call("klog_read", cursor=cursor, filter="notification", wait_ms=3000)
    check("notify appears in klog via cursor", "hello from the test" in text_of(r), text_of(r))
    t = time.time()
    r = c.call("klog_read", tail=0, until="heartbeat [0-9]+", wait_ms=5000)
    check("klog until waits for a match", "until=matched" in text_of(r)
          and "heartbeat" in text_of(r), text_of(r))
    r = c.call("klog_read", tail=0, filter="never-ever", wait_ms=300)
    check("klog wait times out empty", "lines=0" in text_of(r), text_of(r))
    r = c.call("klog_read", tail=100, filter="(")
    check("bad regex reported", r["isError"], text_of(r))

    r = c.call("list_processes", filter="ps5-mcp")
    check("list_processes finds the server", "ps5-mcp" in text_of(r), text_of(r))
    r = c.call("list_processes", format="json", sort="memory", limit=5)
    procs = r["structuredContent"]["processes"]
    check("json process list sorted by memory", len(procs) == 5 and
          procs[0]["rss_mib"] >= procs[-1]["rss_mib"], procs)
    sleeper = subprocess.Popen(["sleep", "60"])
    r = c.call("process_info", pid=sleeper.pid)
    check("process_info threads", r["structuredContent"]["threads"], r)
    r = c.call("kill_process", pid=1)
    check("kill pid 1 refused", r["isError"], text_of(r))
    r = c.call("kill_process", pid=sleeper.pid, signal="KILL")
    check("kill_process", not r["isError"], text_of(r))
    check("process died", sleeper.wait(timeout=5) == -9)

    with open(os.path.join(root, "data/ps5-mcp/notes.txt"), "w") as f:
        f.write("line one\nline two\nthe end\n")
    r = c.call("fs_list", path=root + "/data", depth=2)
    check("fs_list recursive", "notes.txt" in text_of(r) and "ps5-mcp.log" in text_of(r), text_of(r))
    r = c.call("fs_read", path=root + "/data/ps5-mcp/notes.txt", offset=-8)
    check("fs_read negative offset", text_of(r).endswith("the end\n"), text_of(r))
    r = c.call("fs_read", path="/bin/sh", length=64)
    check("fs_read binary -> hex", "00000000  7f 45 4c 46" in text_of(r), text_of(r)[:200])
    write_png(os.path.join(root, "pic.png"), 2000, 1000)
    r = c.call("fs_read", path=os.path.join(root, "pic.png"))
    check("fs_read picture -> image", images_of(r) and image_size(images_of(r)[0]) == (1280, 640),
          text_of(r))
    r = c.call("fs_read", path="/dev/zero")
    check("fs_read refuses device nodes", r["isError"] and "regular" in text_of(r), text_of(r))
    fifo = os.path.join(root, "fifo")
    os.mkfifo(fifo)
    r = c.call("fs_read", path=fifo)
    check("fs_read refuses a FIFO without hanging", r["isError"], text_of(r))
    r = c.call("fs_read", path="/definitely/not/here")
    check("fs_read missing file", r["isError"], text_of(r))


def apps(c):
    r = c.call("app_list")
    check("app_list names titles", "Mock Racer" in text_of(r) and "running: no title" in text_of(r),
          text_of(r))
    r = c.call("app_launch", title_id="bad")
    check("bad title id refused", r["isError"], text_of(r))
    r = c.call("app_launch", title_id="PPSA01234")
    check("app_launch", not r["isError"], text_of(r))
    r = c.call("app_list", filter="racer")
    check("running title marked", "[running]" in text_of(r), text_of(r))
    r = c.call("app_launch", title_id="FAKE00001")
    check("launch over a running title refused", r["isError"] and "replace_running" in text_of(r))
    r = c.call("app_kill")
    check("app_kill", not r["isError"], text_of(r))
    r = c.call("app_kill")
    check("app_kill with nothing running", r["isError"], text_of(r))


def elf(c, root):
    # A stand-in for elfldr: reads the ELF, answers as the payload would.
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)
    port = srv.getsockname()[1]
    got = []

    def loader():
        for _ in range(2):
            conn, _ = srv.accept()
            conn.settimeout(0.3)
            data = b""
            try:
                while True:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    data += chunk
            except socket.timeout:
                pass
            got.append(data)
            conn.sendall(b"hello from payload\n")
            conn.close()

    th = threading.Thread(target=loader, daemon=True)
    th.start()
    with open("/bin/true", "rb") as f:
        true_elf = f.read()
    r = c.call("elf_run", path="/bin/true", loader_port=port, capture_ms=3000)
    check("elf_run from a path sends the whole ELF and returns its output",
          not r["isError"] and "hello from payload" in text_of(r)
          and r["structuredContent"]["output_closed"] is True, text_of(r))
    tiny = b"\x7fELF" + bytes(200)
    r = c.call("elf_run", data_base64=base64.b64encode(tiny).decode(), loader_port=port)
    th.join(timeout=5)
    check("elf_run from base64", not r["isError"] and "hello from payload" in text_of(r), text_of(r))
    check("loader received the exact bytes", got == [true_elf, tiny], [len(g) for g in got])
    srv.close()
    r = c.call("elf_run", path="/bin/true", loader_port=port)
    check("elf_run without a loader", r["isError"] and "no ELF loader" in text_of(r), text_of(r))
    r = c.call("elf_run", data_base64=base64.b64encode(b"not an elf at all" * 8).decode())
    check("elf_run refuses a non-ELF", r["isError"] and "not an ELF" in text_of(r), text_of(r))
    r = c.call("elf_run", path="/bin/true", data_base64="AAAA")
    check("elf_run needs exactly one source", r["isError"], text_of(r))


def concurrency(c):
    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        long = pool.submit(c.call, "pad_sequence", steps="wait:2500")
        time.sleep(0.3)
        t = time.time()
        info = pool.submit(c.call, "system_info").result()
        quick = time.time() - t
        check("read-only tools are not blocked by a running sequence", quick < 1.5, quick)
        t = time.time()
        pool.submit(c.call, "pad_press", buttons="cross").result()
        waited = time.time() - t
        check("input waits for the running sequence (one console job at a time)", waited > 1.0,
              waited)
        check("long sequence finished", not long.result()["isError"])
    r = c.call("pad_close")
    check("pad_close", not r["isError"], r)
    r = c.call("pad_close")
    check("pad_close twice is harmless", "not open" in text_of(r), text_of(r))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="build/ps5-mcp-host")
    run(ap.parse_args().binary)
    print(f"\n{len(PASSED)} passed, {len(FAILED)} failed")
    sys.exit(1 if FAILED else 0)
