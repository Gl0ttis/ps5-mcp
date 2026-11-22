# ps5-mcp

A payload for a jailbroken PS5 that **is itself an MCP server**. Send one ELF to the
console's ELF loader and any MCP client (Claude Code, Claude Desktop, Cursor, the MCP SDKs)
can drive and inspect the console directly over the network. Nothing runs on the host:
no bridge, no stdio.

- **Input:** a system-wide virtual DualSense. You can press, hold, chord, use sticks,
  triggers and the touchpad, and run whole timed sequences.
- **Sight:** a screenshot of the whole composited screen, home screen and system dialogs
  included. It is cropped, scaled and JPEG/PNG-encoded on the console and returned as an
  MCP image. A title can also hand over its own frame through a small grab hook.
- **Diagnostics:** processes (with title ids, memory, CPU, threads, memory maps, open
  files), a kernel-log ring buffer with cursors, regex filters and wait-for-pattern,
  system info, files, installed titles, launching or closing titles, and starting ELF
  payloads through the console's ELF loader with their output captured.

```
claude mcp add --transport http ps5 http://<console-ip>:7165/mcp
```

> For homebrew development on a console you own. It listens on your LAN with **no
> authentication** (by design). Run it only on a network you trust.

## Tools

| Tool | What it does |
| --- | --- |
| `pad_open` | Create the virtual pad, bind it to the foreground user, dismiss "Who's using this controller?", and (by default) disconnect the real controller so titles read this one |
| `pad_close` / `pad_status` / `pad_release` | Remove the pad / show its state and counters / return to neutral |
| `pad_press` | Press a button or chord (`cross`, `l1+r1`), with repeat, and an optional screenshot after |
| `pad_hold` | Hold buttons, sticks (-1..1), triggers (0..1) or a touchpad finger, for a duration or until released |
| `pad_sequence` | `"down*2 cross wait:1500 ls:up:400 shot"`: presses, stick moves, waits and screenshots, timed on the console and validated before the first press |
| `screenshot` | `native` (default — triggers a fresh 4K compose inside SceShellUI via ptrace and reads the frame from the compositor shared memory; no pad, no gallery file, ~200 ms SceShellUI freeze), `system` (whole screen via the console's own Create-hold capture), `title` (the title's own frame via the grab hook), `latest` (newest existing capture). Supports `max_width`, `crop` (to read small text), `format`, `quality`, `grayscale` |
| `system_info` | Firmware, model, temperatures, CPU clock, memory, uptime, foreground user, running title, IPs, free storage, and server state |
| `list_processes` | Table or JSON of every process: pid, ppid, state, threads, CPU%, authid, title id, RSS/VSZ; filter and sort |
| `process_info` | One process's threads (name, state, CPU time), and optionally its memory map and open files |
| `kill_process` | Signal a process; refuses system (`Sce*`) processes unless `force=true` |
| `klog_read` | Kernel log from an in-payload ring (8192 lines): `tail`, `cursor`, `filter`/`exclude` regex, `wait_ms`, `until` (wait for a line, e.g. a title's startup message) |
| `fs_list` / `fs_read` | Directory listings (recursive); file reads as text, hex dump, base64, or an image for JPEG/PNG/BMP; negative offsets read the tail |
| `app_list` / `app_launch` / `app_kill` | Installed titles with names, which one is running; launch by title id; close the running title |
| `elf_run` | Start an ELF payload through the console's ELF loader (127.0.0.1:9021), from a file on the console or uploaded as base64 (up to 16 MiB); returns what it prints and the processes it started |
| `notify` | Show a toast on the TV |
| `server_shutdown` | Close the pad and end the payload (e.g. before sending a new build) |

The server's `initialize` result carries an operating guide for the agent (look, act,
look again; when to use each screenshot mode; etiquette), so clients that pass server
instructions to the model need no extra prompt.

## Build

Requirements: the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) (default
`/opt/ps5-payload-sdk`) and a C compiler plus Python 3 for the host tests.

```bash
make                         # build/ps5-mcp.elf   (PS5_PAYLOAD_SDK=/path/to/sdk to override)
make PORT=7165               # change the default listening port at build time
make host                    # build/ps5-mcp-host  (same server on a mock backend, ASan+UBSan)
make test                    # run the end-to-end suite against the host build
```

## CI and releases

Versions follow [semantic versioning](https://semver.org), derived from
[Conventional Commits](https://www.conventionalcommits.org) by
[semantic-release](https://semantic-release.gitbook.io):

| Commit | Release |
| --- | --- |
| `fix: ...`, `perf: ...` | patch (1.2.3 → 1.2.4) |
| `feat: ...` | minor (1.2.3 → 1.3.0) |
| `feat!: ...` or a `BREAKING CHANGE:` footer | major (1.2.3 → 2.0.0) |
| `docs:`, `test:`, `ci:`, `chore:`, `refactor:` | no release |

- **Pull requests to `main`** (`.github/workflows/ci.yml`): the PR title must be a
  Conventional Commit, since it becomes the squash-merge message. The payload is built
  with the pinned SDK (v0.45), `make test` runs, and `build/ps5-mcp.elf` is uploaded as
  an artifact.
- **Pushes to `main`** (`.github/workflows/release.yml`): the same build and test, then
  semantic-release. It rebuilds the payload with the new version baked in (`make
  VERSION=x.y.z`, reported by `system_info` and `initialize`), tags `vX.Y.Z`, and
  publishes a GitHub release with notes and `ps5-mcp-X.Y.Z.elf` attached.

Local builds take their version from the nearest `vX.Y.Z` tag, or `0.0.0-dev`.

## Run on the console

The console needs an **ELF loader** (port 9021 in the usual chains). It should also run a
**klog server** (port 3232). The payload reads the kernel log through it, and the pad needs
that to learn its device id.

```bash
make deploy PS5_HOST=<console-ip>          # prospero-deploy to port 9021
# or: nc -q0 <console-ip> 9021 < build/ps5-mcp.elf
```

It listens on **port 7165** (`http://<console-ip>:7165/mcp`, and `GET /health` for a
plain-text check). A second copy finds the port taken and exits. Logs go to klog and to
`/data/ps5-mcp/ps5-mcp.log`. A payload's stdout is the ELF loader's socket, so the
server logs through `klog_printf`.

### Connect a client

```bash
claude mcp add --transport http ps5 http://<console-ip>:7165/mcp     # Claude Code
```

```json
{ "mcpServers": { "ps5": { "type": "http", "url": "http://<console-ip>:7165/mcp" } } }
```

`tests/test_sdk_client.py` drives the server with the official MCP Python SDK
(`pip install mcp`). It is useful as a first check against a real console:

```bash
python3 tests/test_sdk_client.py http://<console-ip>:7165/mcp           # read-only checks
python3 tests/test_sdk_client.py http://<console-ip>:7165/mcp --pad     # open the pad, take a screenshot
```

## Design

```
            MCP client ──HTTP POST /mcp (JSON-RPC)──▶  ps5-mcp payload (one process)
                                                      ┌──────────────────────────────┐
  http.c    thread per connection, Content-Length     │ mcp.c   JSON-RPC, registry   │
            and chunked bodies, Origin check          │ tools_*.c                    │
  pad.c     held state re-inserted every 8 ms,        │ screen.c  capture → image.c  │
            console-side timing, one console job      │ klog.c    ring buffer thread │
            at a time (input + system shots)          │ backend_ps5.c  (SDK calls)   │
                                                      └──────────────────────────────┘
```

- **Streamable HTTP, stateless.** Every request is a POST answered with
  `application/json`; notifications get `202`; `GET /mcp` is `405` (no server-initiated
  stream). Protocol versions 2024-11-05 through 2025-11-25 are negotiated. Batches are
  accepted. Requests with an `Origin` header are accepted only from `localhost` or from
  the console's own IP address. In DNS rebinding both `Origin` and `Host` carry the
  attacker's domain name, so such requests are refused. Non-browser clients send no
  `Origin` and are unaffected.
- **Console-side timing.** Presses, holds and waits run on the console, not across the
  network. A whole `pad_sequence` is parsed and checked before anything is pressed, so a
  typo never leaves a menu half-navigated. Input and system screenshots share one lock;
  read-only tools never wait on it.
- **Images processed on the console.** A 4K capture is decoded (stb_image), optionally
  cropped, area-averaged down (so thin UI lines and text survive), and re-encoded. A
  1280-wide JPEG is about 50–200 KB instead of a multi-megabyte 4K file.
- **One klog reader for everything.** A background thread keeps the kernel log in a
  ring with sequence numbers. `klog_read` pages through it with cursors. `pad_open`
  finds the new device's id and the controller table there too, so no second klog
  connection can race it. The line formats are SceShellCore's own format strings, read
  from the firmware binary (`src/pad_klog.h`).
- **Portable core, two backends.** `backend_ps5.c` holds every SDK and system call.
  `backend_host.c` is a mock (a pad log, a test-card "screenshot" when Create is held
  ≥ 1 s, `/proc` processes, a heartbeat klog). The full server builds and is tested on
  Linux with sanitizers.

### Screenshots

`mode=system` holds Create on the virtual pad for 1.2 s, the console's own "take
screenshot" shortcut. It waits for the new file under `/user/av_contents/photo`, sends it,
then deletes it through `sceContentDeleteByPath`, as the gallery's Delete does (file,
thumbnail and database row). It takes about 3 s.

`mode=title` asks the running title for its own frame through a grab hook. It takes
about 1 s and leaves nothing in the gallery. The contract:

1. The server creates a request file, by default `/data/homebrew/<TITLE_ID>/ps5-mcp-grab`
   (`/app0/ps5-mcp-grab` from inside the title).
2. Every few frames the title tries to `unlink()` it. One system call fails on every
   ordinary frame. When the unlink succeeds, the title writes its next frame (JPEG or PNG)
   as a new file, mode 0666, in a folder of its own, by default
   `/data/homebrew/<TITLE_ID>/screenshots`.
3. The server sends the newest picture in that folder and deletes it.

```c
static void grab_hook(void)            /* call once per frame */
{
    static int frame;
    if (++frame % 4 == 0 && unlink("/app0/ps5-mcp-grab") == 0)
        write_next_frame_to("/app0/screenshots");   /* your engine's screenshot */
}
```

`request_path` and `folder` override the defaults, so an existing hook with other file
names works too.

With **HDR on**, console captures are JPEG XR. They can't be decoded on the console, so
the tool says so and leaves the capture on the console. Turn HDR off for agent sessions.

#### Native capture (default)

`mode=native` is the default and triggers a fresh, live 4K compose on every call. No pad,
no Create hold, no file written to the gallery, and the whole composited screen is
captured (home screen, system dialogs and overlays included). Typical cost is about 1.5 s
end-to-end on the first call per SceShellUI PID, dropping to ~200 ms on later calls once
the per-PID offsets are cached.

The payload finds SceShellUI's main executable through `sysctl KERN_PROC_VMMAP`, then
signature-scans the text segment for two addresses: the internal
`sceScreenshotCaptureForCoreDump` function (the compose-config byte sequence
`48 B? 93 00 00 00 (03|05)` lives in its prologue) and the compositor shared-memory base
pointer (loaded by the `TZCNT` + `SHL 0x19` sequence in one of the compose vtable
methods). A single byte in the function is patched from mode 3 (720p) to mode 5 (4K),
ptrace attaches to SceShellUI, a fast-trap `pt_call` invokes the patched function
synchronously inside SceShellUI, the mode byte is reverted, and ptrace detaches. The
composed 3840×2160 BGRA frame lives in the compositor shared memory at a deterministic
offset from the scanned base pointer; it is read out with `kernel_proc_copyout`, scaled
and encoded on the console, and returned as an MCP image. The intermediate file that
CoreDump's save step writes before failing its format check is `unlink()`ed on the way
out, so the filesystem stays clean.

All SceShellUI offsets are discovered at runtime, so the path is firmware-agnostic — no
addresses are baked into the build.

## What has been verified

| | Status |
| --- | --- |
| MCP protocol, HTTP layer, tool logic | **Tested** on the host build: `make test` (88 checks, ASan + UBSan clean) and the official MCP Python SDK client (mcp 2.3.0). The mock backend replaces only the system calls. The klog parsing, eviction choice, capture pipeline, image processing and ELF-loader protocol run the same code as on the console |
| klog formats (DEVICE_ADDED, `[CIM]` controller table) | The format strings were read from SceShellCore (firmware build 1777439). Not yet observed live |
| `mode=native` fresh 4K compose via ptrace + CoreDump | **Run on a 13.20 console (CFI-7119).** The default screenshot mode. Returns a live composited 3840×2160 BGRA frame on every call, scaled and encoded on the console. First call per SceShellUI PID is ~1.5 s (signature-scanning the ~47 MB eboot); later calls ~200 ms. All SceShellUI offsets are discovered at runtime so the build has no firmware-specific addresses. |
| PS5 payload | **Builds and links** against the ps5-payload-dev SDK |
| Console paths (`/user/av_contents/photo`, `/user/app`, appmeta `param.json` layout) | Checked against a real console's filesystem |
| Read-only tools on a console | **Run on a 13.20 console (CFI-7119)** through Payload Manager: `system_info`, `list_processes`, `klog_read` work as described |
| Pad and Create-hold capture on a console | **Run on a 13.20 console** with a USB DualSense and ASTRO's PLAYROOM: `pad_open` both ways (exclusive evicted the USB pad, then recreated the virtual pad so it took the first slot; the game then read it), buttons, both sticks, both triggers (held through a screenshot), `screenshot mode=system` with `crop`, `pad_sequence` with `shot`, `pad_close`, `app_launch` and `app_kill`. Touchpad input is sent without error but unconfirmed (nothing on screen showed it). Not yet run: `mode=title`, `elf_run` |

Every console-facing tool returns the raw result code of each system call (for example
`pad_open` reports `padInit`, `add`, `device`, `bind`, `insert_tries`), so bring-up on
another firmware works from the tool output.

## Limits

- Pictures are stills, not video. `mode=native` costs ~200 ms (plus a one-time ~1.5 s
  scan on the first call per SceShellUI PID); `mode=system` costs about 3 s. Some titles
  block the `system` path.
- A title reads only its user's first controller slot. `pad_open` therefore disconnects
  the real one by default, then recreates the virtual pad so it takes that first slot (a
  pad keeps the slot it was given). The PS button on the real controller reconnects it. `exclusive=false` keeps it, but
  then only the home screen sees the virtual pad.
- The pad's device id comes from the kernel log: without a klog server (or `/dev/klog`),
  `pad_open` can't bind the pad.
- No authentication, by request. Anyone on the LAN who can reach the port can drive the
  console.

## Credits

Prior art for the virtual pad and the Create-hold capture:
[PS5 DevLoop](https://github.com/griches/PS5_DevLoop),
[SplashDown](https://github.com/seregonwar/SplashDown),
[Control4Free](https://github.com/MoHadiShibli/Control4Free),
[OmniPad-PS5](https://github.com/diegobarbosaa/OmniPad-PS5) and
[Ghostpad](https://github.com/srbraboo/Ghostpad). App launch/kill and notifications
follow [ps5-payload-dev/websrv](https://github.com/ps5-payload-dev/websrv), and process
listing follows the SDK's `ps` sample. Bundled: [cJSON](https://github.com/DaveGamble/cJSON)
(MIT) and [stb_image / stb_image_write](https://github.com/nothings/stb) (public domain
/ MIT).

Not affiliated with Sony Interactive Entertainment.

## License

GPL-3.0-or-later. See `LICENSE`. Third-party code keeps its own license
(`third_party/*/LICENSE`).
