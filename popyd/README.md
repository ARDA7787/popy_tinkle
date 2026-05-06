# popyd — quarantine daemon and CLI for AI agents

A small C++ daemon (`popyd`) and CLI (`popy`) that bring the browser
extension's `_popy` quarantine to the command line, designed for **AI CLI
agents** like Claude Code, Cursor, and Codex.

## What this protects

When an AI agent downloads a file with `curl`/`wget`/a tool call, that file
lands in `~/Downloads` (or wherever) with its original extension. Spotlight
indexes it. The on-access AV scanner reads it. Preview handlers may parse
it. A misclick or `bash -c "open ./payload"` runs it. None of those should
happen for a file the agent has not yet inspected.

popy separates two enforcement paths:

| | **Guaranteed path — `popy fetch`**  *(recommended)* | **Best-effort path — `popyd` watcher mode** |
|---|---|---|
| How | Stream the network response straight into `<stage>/<uuid>/<name>.<ext>_popy` opened with `O_CREAT \| O_EXCL`. Mode `0000` from the moment the fd is created. | Watch a directory; when a file appears, atomically rename it to `<name>_popy` and `chmod 0000`. |
| Original-extension filename ever exists on disk? | **No.** | Yes — for the milliseconds between create and rename. |
| Stops OS handlers / preview / AV scanners? | **Yes.** | Best-effort. The watch dir is excluded from Spotlight/Tracker by the installer. |
| AI agent uses via | `popy_fetch` MCP tool, or `popy fetch <url>` from a Bash hook. | Drops files into `~/.popy-stage/` as a side effect of any tool that writes there. |

The browser extension provides Mode A's property natively via OPFS. Mode A
in this CLI is the only honest equivalent outside a browser.

## Build

```bash
# macOS
brew install cmake          # libcurl ships with macOS
cmake -S popyd -B popyd/build -DCMAKE_BUILD_TYPE=Release
cmake --build popyd/build -j

# Ubuntu / Debian
sudo apt install cmake libcurl4-openssl-dev
cmake -S popyd -B popyd/build -DCMAKE_BUILD_TYPE=Release
cmake --build popyd/build -j
```

Produces `popyd/build/popy` and `popyd/build/popyd`. Optional install prefix:

```bash
cmake --install popyd/build --prefix ~/.local
```

This also runs the indexer-exclusion step on `~/.popy-stage` (`mdutil -i off`
on macOS, a `.trackerignore` on Linux).

### Build options

| Flag                           | Effect |
|--------------------------------|--------|
| `-DPOPY_WERROR=ON`             | warnings as errors (CI default) |
| `-DPOPY_SANITIZERS=ON`         | ASan + UBSan (Linux/older macOS; broken on Apple Clang 16 + macOS 26) |
| `-DPOPY_BUILD_TESTS=OFF`       | skip the test binaries |

## Run the daemon

popyd is meant to be supervised by launchd (macOS) or systemd (Linux):

```bash
# macOS
cp popyd/dist/com.popy.daemon.plist ~/Library/LaunchAgents/
# edit the path inside the plist to match your popyd install
launchctl load ~/Library/LaunchAgents/com.popy.daemon.plist
launchctl start com.popy.daemon

# Linux
mkdir -p ~/.config/systemd/user
cp popyd/dist/popyd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now popyd.service
```

Or just run it in the foreground (it does NOT daemonize itself; launchd /
systemd handle that):

```bash
~/.local/bin/popyd
```

Logs:
- macOS: `~/Library/Logs/popyd.log`
- Linux: `~/.local/state/popy/popyd.log` (or `journalctl --user -u popyd`)

## CLI

Every command takes `--json`. `<file>` resolves by full UUID, UUID prefix
(≥ 4 chars), original filename, or `<name>_popy` filename.

```text
popy fetch <url> [--out <name>] [--mime <type>] [--max-bytes N] [--json]
popy list [--json]
popy read <file> [--mode raw|text]              # `--mode png|text` for PDF/image needs M3
popy release <file> --to <path> [--force] [--json]
popy delete <file> [--json]
popy status [--json]
popy pause | popy resume                         # transient; cleared on daemon restart
popy config [--print] [--path]
```

`popy status --json` includes enforcement reporting fields:
- `enforcement_mode`: `strict` | `popy_only` | `off`
- `guarantee_level`: `guaranteed` | `best_effort`
- `bypass_surface`: list of known non-enforced channels

Quick smoke:

```bash
popy fetch https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf --mime application/pdf
popy list
popy release dummy.pdf --to /tmp/dummy.pdf
shasum -a 256 /tmp/dummy.pdf
```

## AI agent integration

### Claude Code via MCP

Add the popy MCP server:

```bash
claude mcp add popy -- python3 ~/.local/share/popy/mcp/server.py
# or, if popy is not on $PATH:
POPY_BIN=$HOME/.local/bin/popy claude mcp add popy -- python3 ~/.local/share/popy/mcp/server.py
```

The agent now has five tools: `popy_fetch`, `popy_list`, `popy_read_text`,
`popy_release`, `popy_delete`.

### Claude Code via Bash hook (optional, recommended)

Refuse direct `curl`/`wget`/`http` invocations and tell the agent to use
`popy_fetch` instead. Merge `popyd/dist/claude-hooks.json` into your
`~/.claude/settings.json`. The hook is `popyd/dist/popy-bash-hook.sh`.

After install:

```text
agent: I'll download the file
agent → Bash("curl https://example.com/x.pdf -o /tmp/x.pdf")
hook  → exit 2 with stderr:
        popy: refusing direct download. Use the `popy_fetch` MCP tool …
agent → popy_fetch(url=https://example.com/x.pdf)
agent → popy_read_text(file=...)   # decides what to do with it
```

### Other agents

Any agent that can run subprocesses can use `popy fetch <url> --json` and
parse the resulting JSON. The MCP server is just a thin shim.

## Threat model

What popyd **does** stop:

- Accidental double-click on a downloaded file (mode `0000`).
- OS preview handlers / shell handlers parsing attacker-controlled bytes
  (no original-extension filename exists in Mode A; renamed within ~1s in
  Mode B with the watch dir excluded from indexers).
- AI agents from silently writing executable downloads into your filesystem
  (when paired with the Bash hook).

What popyd **does not** stop:

- Browser exploits before the download ever happens.
- An agent that explicitly `popy release`s a malicious file and runs it.
- Mode B's race window between the first byte landing on disk and the
  rename — Spotlight/AV/preview can still touch a file in that window.
  Mode A closes this; Mode B makes it as small as possible (~ms).
- Forensic recovery after `popy delete` — we don't overwrite, just unlink.
- Anything the OS kernel does. We're not a kernel module.

## How it works

```
agent → popy fetch <url>                 ─┐
                                          │  Guaranteed path: file's first existence
        ┌─────────────────────────────┐   │  on disk is already _popy 0000.
        │ libcurl streaming GET       │   │  Original-extension filename
        │   ↳ 16-byte magic probe     │   │  never appears.
        │   ↳ picosha2 sha256          │   │
        │   ↳ write(fd) into          │   │
        │     <stage>/<id>/<name>_popy│   │
        │   ↳ fsync, fchmod 0000      │   │
        │   ↳ atomic sidecar write    │   │
        └─────────────────────────────┘  ─┘

writer → ~/.popy-stage/foo.pdf           ─┐
                                          │  Best-effort path: watcher renames in place
        ┌─────────────────────────────┐   │  the moment FSEvents/inotify
        │ FSEvents (macOS) / inotify  │   │  reports the file is stable.
        │ stability: 750ms quiet      │   │
        │   ↳ rename(...) → _popy     │   │  Mode is 0000 within ~1s.
        │   ↳ read fd → sha256        │   │
        │   ↳ fchmod 0000             │   │
        │   ↳ atomic sidecar write    │   │
        └─────────────────────────────┘  ─┘
```

Sidecar `<file>_popy.meta.json` mirrors the browser extension's
`QuarantineRecord` field-for-field.

## Layout

```
popyd/
├── src/
│   ├── main_popy.cpp              # CLI entrypoint
│   ├── main_popyd.cpp             # daemon entrypoint
│   ├── core/                      # paths, naming, hash, mime, sidecar, config, log, safe_fs
│   ├── net/fetch.cpp              # Mode A — popy fetch
│   ├── store/quarantine.cpp       # list/release/delete + Mode B in-place quarantine
│   ├── ipc/status.cpp             # AF_UNIX status socket (server + client)
│   ├── watch/watcher_*.cpp        # FSEvents (macOS) / inotify (Linux)
│   └── cli/commands.cpp           # subcommand dispatch
├── third_party/                   # vendored single-header libs (see SOURCES.md)
├── mcp/server.py                  # JSON-RPC MCP server (stdlib only)
├── dist/                          # service files, Claude hook, example config
└── tests/                         # ctest binaries — naming, hash, sidecar, config,
                                   #   safe_fs, e2e_fetch, watcher
```

## Status

This is M2: Mode A + Mode B both work, MCP server ships, Claude hook ships.
Tests cover the in-process pieces; the daemon has a manual smoke test in
this README.

Coming in M3: sandboxed `popy read --mode png|text` for PDF/image (libmupdf
+ stb_image isolated behind `fork()` + `setrlimit` + `sandbox-exec`/seccomp).

## License

Apache 2.0 — same as the parent project.
