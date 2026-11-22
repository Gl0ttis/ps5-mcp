#!/usr/bin/env python3
# ps5-mcp - interoperability check with the official MCP Python SDK client.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   pip install mcp
#   python3 tests/test_sdk_client.py http://HOST:7165/mcp            # read-only checks
#   python3 tests/test_sdk_client.py http://HOST:7165/mcp --pad      # also open the pad and look
#
# Works against the host mock build or a real console.
import argparse
import asyncio
import base64
import sys

from mcp import ClientSession
try:  # mcp >= 2
    from mcp.client.streamable_http import streamable_http_client as http_client
except ImportError:  # mcp 1.x
    from mcp.client.streamable_http import streamablehttp_client as http_client


def field(obj, snake, camel):
    """mcp 2.x uses snake_case attributes, 1.x camelCase."""
    return getattr(obj, snake) if hasattr(obj, snake) else getattr(obj, camel)


async def main(url, pad, save):
    async with http_client(url) as streams:
        read, write = streams[0], streams[1]
        async with ClientSession(read, write) as session:
            init = await session.initialize()
            server = field(init, "server_info", "serverInfo")
            print(f"server {server.name} {server.version}, "
                  f"protocol {field(init, 'protocol_version', 'protocolVersion')}")
            tools = (await session.list_tools()).tools
            print(f"{len(tools)} tools: {', '.join(t.name for t in tools)}")

            info = await session.call_tool("system_info", {})
            print("system_info:", "error" if field(info, "is_error", "isError") else "ok")
            print(info.content[0].text[:1500])

            procs = await session.call_tool("list_processes", {"sort": "memory", "limit": 10})
            print(procs.content[0].text)

            klog = await session.call_tool("klog_read", {"tail": 15})
            print(klog.content[0].text)

            if pad:
                r = await session.call_tool("pad_open", {})
                print("pad_open:", r.content[0].text)
                print(r.content[1].text if len(r.content) > 1 else "")
                if not field(r, "is_error", "isError"):
                    shot = await session.call_tool("screenshot", {"max_width": 1280})
                    for block in shot.content:
                        if block.type == "image":
                            data = base64.b64decode(block.data)
                            with open(save, "wb") as f:
                                f.write(data)
                            print(f"screenshot: {len(data)} bytes -> {save}")
                        elif block.type == "text":
                            print("screenshot:", block.text)
                    await session.call_tool("pad_close", {})
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("--pad", action="store_true", help="open the pad and take a screenshot")
    ap.add_argument("--save", default="shot.jpg")
    a = ap.parse_args()
    sys.exit(asyncio.run(main(a.url, a.pad, a.save)))
