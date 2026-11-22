# Working on ps5-mcp (agent guide)

A PS5 payload that is itself an MCP server (Streamable HTTP, stateless JSON, no auth,
default port 7165). C11, built with the ps5-payload-dev SDK. `README.md` describes the
tools and the design.

## Layout

- `src/backend.h`: the platform interface. **Every SDK or system call lives in
  `src/backend_ps5.c`**, and `src/backend_host.c` mocks the same interface. Keep the
  rest portable, so it builds and is tested on Linux.
- `src/http.c` (server), `src/mcp.c` (JSON-RPC, tool registry, result builders),
  `src/pad.c` (held state + insert thread + console lock), `src/klog.c` (ring buffer),
  `src/screen.c` + `src/image.c` (captures), `src/tools_*.c` (one file per tool group).
- `refs/` (ignored): reference checkouts (the SDK, ps5-payload-dev repos, prior art).
- `scratch/` (ignored): local research data (NID catalogue, console FTP manifest, notes).
- `.github/`: CI (PRs) and release (pushes to `main`) workflows sharing the
  `build-and-test` composite action; `.releaserc.json` configures semantic-release.

## Adding a tool

Add a `tool_def_t` to the group's `k_tools[]`. Write the schema with **single quotes**;
they become double quotes at registration, so a schema must contain no apostrophe and no
double quote. A schema that does not parse is logged as `BUG:` at startup, and `make test`
checks every tool's schema. Report errors with `tr_error` (an `isError` result, not a
JSON-RPC error). Take `console_lock()` around anything that presses buttons.

## Checks before handing back

```bash
make            # PS5 payload, -Werror
make test       # host build with ASan/UBSan + end-to-end suite
```

Add a test to `tests/test_mcp.py` for new behaviour. Say plainly what ran only on the
host mock and what ran on a console.

## Commits

Conventional Commits (`feat:`, `fix:`, `docs:`, ...). They drive semantic-release on
`main`, so the type decides the next version (see README, "CI and releases"). PR titles
are checked, and they become the squash-merge message.

## Console etiquette (when testing on hardware)

One console job at a time, and never over a title someone is playing. `pad_open`
disconnects the real controller, so say so. Never delete files under
`/user/av_contents` by hand; the server deletes captures through
`sceContentDeleteByPath`. Never print the console's address in committed files.
