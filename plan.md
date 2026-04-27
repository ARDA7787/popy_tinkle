# popyd: Background Quarantine Daemon + `popy` CLI for AI Agents

A lightweight C++ daemon (`popyd`) and CLI (`popy`) under `popyd/` that quarantines downloads
into `<name>_popy` files with JSON sidecars, designed for AI CLI agents (Claude Code, Cursor,
Codex) to use as a zero-trust file gateway. Two paths:

- **Mode A (primary, strong):** the agent fetches *through* popyd via `popy fetch <url>` or the
  bundled MCP server. Bytes stream from the network straight into `<name>_popy` — the file's
  *first existence on disk is already suffixed and `chmod 0000`*. No OS handler, indexer, or
  on-access scanner ever sees the original extension. This is the OPFS-equivalent property
  the browser extension provides.
- **Mode B (secondary, best-effort):** popyd watches `~/.popy-stage/` (a user-owned directory
  the installer excludes from Spotlight/Tracker/Baloo) and renames new files to `<name>_popy`
  + `chmod 0000`. The race window between file-create and rename is honestly documented;
  `~/Downloads` watching is opt-in and labeled "convenience, not security."

Everything else — the sidecar schema, the `_popy` suffix, the streaming hash + magic-byte
probe, the sanitization regex — mirrors the browser extension field-for-field so a future
sync layer ingests sidecars into the extension's IndexedDB with zero translation.

## 1. Mental model

- **`popy fetch` is the strong path.** popyd's libcurl streams the network response directly
  into `<dir>/<uuid>/<name>_popy` opened with `O_CREAT | O_EXCL`, runs `picosha2` and a
  16-byte magic-byte probe in the same loop (mirrors `src/offscreen/offscreen.ts:91-138`),
  `fsync`s, writes the sidecar, `chmod 0000`s the file. The original-extension filename
  never exists on disk. **This is what AI agents are expected to use.**
- **The MCP server is the agent surface.** `popyd/mcp/server.py` is a stdlib-only Python
  shim that exposes `popy_fetch` / `popy_list` / `popy_read_text` / `popy_release` /
  `popy_delete` as MCP tools by subprocess-calling `popy --json`. README ships a one-line
  `claude mcp add` snippet.
- **The watcher is best-effort.** Default `watch_dirs = ["~/.popy-stage"]`, with the
  installer running `mdutil -i off` (macOS) and creating `.trackerignore` / setting Baloo
  exclusions (Linux). `~/Downloads` is opt-in and the README is explicit that watcher mode
  cannot guarantee pre-OS handling.
- **Single source of truth is the filesystem.** Sidecars are inline next to each file
  (`<name>_popy.meta.json`, `chmod 0400`). `popy list/read/release/delete` read sidecars
  directly — no heavy IPC. popyd exposes only a tiny status socket for `popy status` /
  `popy pause`.
- **`popy read` is sandboxed.** The libmupdf/stb_image render path runs in a forked child
  with `setrlimit(RLIMIT_AS|RLIMIT_CPU)` and `sandbox-exec` (macOS) / `seccomp` (Linux),
  no filesystem access beyond the input fd, output piped back to the parent. A crash or RCE
  in the parser is bounded to the child; the agent's process tree is untouched.

## 2. Directory layout (new — under `popyd/`)

```
popyd/
├── CMakeLists.txt
├── README.md                     # build, install, config, agent integration
├── src/
│   ├── main_popyd.cpp            # daemon: watcher loop + status socket
│   ├── main_popy.cpp             # CLI: subcommand dispatch
│   ├── main_popy_render.cpp      # forked render child (separate binary, sandboxed)
│   ├── core/
│   │   ├── paths.{h,cpp}         # XDG paths, config, PID, socket, log locations
│   │   ├── config.{h,cpp}        # parse ~/.config/popy/config.toml
│   │   ├── naming.{h,cpp}        # _popy suffix, sanitization (mirrors src/lib/opfs/fs.ts)
│   │   ├── sidecar.{h,cpp}       # QuarantineRecord ↔ JSON, atomic write via rename
│   │   ├── hash.{h,cpp}          # streaming SHA-256 (picosha2 wrapper)
│   │   ├── uuid.{h,cpp}          # UUID v4 from /dev/urandom
│   │   ├── mime.{h,cpp}          # extension → MIME, magic-byte sniff (16B probe)
│   │   ├── log.{h,cpp}           # simple syslog/file logger
│   │   └── safe_fs.{h,cpp}       # *at syscalls, O_NOFOLLOW, watch-root containment
│   ├── watch/
│   │   ├── watcher.h
│   │   ├── watcher_fsevents.cpp  # macOS — FSEventStream, stability polling
│   │   └── watcher_inotify.cpp   # Linux — IN_CLOSE_WRITE / IN_MOVED_TO
│   ├── ipc/
│   │   └── status.{h,cpp}        # AF_UNIX line-protocol for status/pause/resume
│   ├── net/
│   │   └── fetch.{h,cpp}         # libcurl streaming GET, writes _popy directly
│   ├── store/
│   │   └── quarantine.{h,cpp}    # list/release/delete against the sidecar set
│   ├── render/
│   │   ├── pdf.{h,cpp}           # libmupdf: page count, page→PNG, page→text
│   │   ├── image.{h,cpp}         # stb_image decode → stb_image_write PNG (EXIF strip)
│   │   ├── markdown.{h,cpp}      # verbatim stdout
│   │   └── sandbox.{h,cpp}       # fork + setrlimit + sandbox-exec/seccomp barrier
│   └── cli/
│       └── commands.{h,cpp}      # argv parsing, subcommand dispatch, --json formatting
├── third_party/                  # vendored single-header, permissive
│   ├── picosha2.h
│   ├── stb_image.h
│   ├── stb_image_write.h
│   ├── nlohmann_json.hpp
│   └── toml.hpp                  # toml++ single-header
├── mcp/
│   └── server.py                 # ~80-line stdlib MCP server wrapping `popy --json`
├── dist/
│   ├── com.popy.daemon.plist     # macOS launchd
│   ├── popyd.service             # Linux systemd (user unit)
│   ├── claude-hooks.json         # example .claude/settings.json snippet
│   └── config.example.toml
└── tests/
    ├── CMakeLists.txt
    ├── test_naming.cpp
    ├── test_sidecar.cpp
    ├── test_hash.cpp
    ├── test_config.cpp
    ├── test_safe_fs.cpp
    ├── test_e2e_fetch.cpp        # real loopback HTTP; verifies Mode A property
    └── fuzz/
        ├── fuzz_sidecar.cpp      # libFuzzer: sidecar JSON parser
        └── fuzz_mime.cpp         # libFuzzer: magic-byte sniffer
```

No existing files are touched except the repo-root `README.md`, which gains a "## CLI Daemon"
section at the end.

## 3. Mode A — `popy fetch` (the primary path)

```
agent ─→ popy fetch <url>
            │
            ├─ alloc UUID, build dest path: <stage>/<uuid>/<sanitized-name>_popy
            ├─ openat(dir_fd, name, O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW, 0600)
            ├─ libcurl streaming GET (CURLOPT_WRITEFUNCTION)
            │     ├─ first 16 B → magic_sniff() compared to expected MIME (if --mime)
            │     ├─ picosha2::hash256_one_by_one .process(chunk)
            │     └─ write(fd, chunk)
            ├─ on completion: fsync(fd) → fchmod(fd, 0000) → close
            ├─ write sidecar atomically: <name>_popy.meta.json.new + renameat
            └─ stdout: {"id","sha256","path","sizeBytes","mime"} (--json)
```

The original-extension name **never exists**. Renaming is not a step.

Failure modes:
- HTTP non-2xx, redirect to non-https, response > `max_bytes` (config), magic mismatch with
  `--mime`, network timeout → unlink the in-progress `_popy` + sidecar, return non-zero.
- TLS verification on by default; `--insecure` forbidden in the CLI for v1.

## 4. CLI surface (`popy`)

Every command accepts `--json` (default: human-friendly). `<file>` resolves by full UUID,
UUID prefix, basename of original, or basename of `_popy` file.

- **`popy fetch <url> [--out <name>] [--dir <root>] [--mime <type>] [--max-bytes N]`** —
  Mode A primary. JSON on stdout: `{id, sha256, path, sizeBytes, mime}`.
- **`popy list [--status <s>] [--root <dir>] [--json]`** — enumerate sidecars across all
  watch roots + the stage dir, newest first.
- **`popy read <file> [--page N] [--all] [--info] [--mode png|text|raw]`** — agent-facing.
  `--mode raw` is bytes-as-is for non-rasterized types (markdown, txt, json).
  `--mode png|text` and PDF/image rendering go through the **sandboxed render child**
  (`popy-render` binary, fork + setrlimit + sandbox-exec/seccomp).
- **`popy release <file> --to <path> [--force]`** — strip `_popy`, copy to `<path>`,
  update sidecar `status: "released"`, then unlink source + sidecar. Refuses if `<path>`
  exists unless `--force`. Restores `chmod 0644` on the released copy.
- **`popy delete <file>`** — `unlink()` file + sidecar. No overwrite (honest about SSD
  wear-leveling). Documented in the threat model section of the README.
- **`popy status [--json]`** — queries popyd's status socket; prints PID, uptime, watched
  roots, file count, bytes. Exit 1 if daemon not running.
- **`popy pause` / `popy resume`** — transient via status socket; cleared on daemon restart.
- **`popy config [--print] [--path]`** — show config or its location. No `--edit`; the
  user edits the file in their editor of choice. No `enable`/`disable` subcommands.

## 5. Sidecar JSON schema

One sidecar per file, inline next to it. Mirrors `QuarantineRecord` from
`src/lib/types/index.ts:14-47` field-for-field.

```json
{
  "schemaVersion": 1,
  "id": "3f2e9a40-...-uuid-v4",
  "opfsPath": "",
  "diskPath": "Downloads/report.pdf_popy",
  "originalFilename": "report.pdf",
  "sourceUrl": "",
  "sourceHost": "",
  "referrer": null,
  "tabUrl": null,
  "sizeBytes": 1048576,
  "mime": "application/pdf",
  "sha256": "abc123...",
  "path": "fallback",
  "status": "stored",
  "createdAt": 1735000000000,
  "resolvedAt": null,
  "note": "fetched via popy fetch",
  "agent": "popyd/0.1",
  "origin": "fetch"
}
```

- `path: "fallback"` is kept (already a valid `InterceptPath` value, no TS change needed).
- `sourceUrl`/`sourceHost` populated by `popy fetch`, empty for watcher captures.
- `schemaVersion`, `agent`, `origin` (`"fetch" | "watcher"`) are additive; `idb` accepts
  unknown keys.
- `originalFilename` sanitized by the same algorithm as `src/lib/opfs/fs.ts:13-25` so a
  release back to disk produces byte-identical filenames to the browser's.
- Sidecars written atomically: `<name>_popy.meta.json.new` then `renameat`. Permission
  `0400`. The daemon never modifies a sidecar in place.

## 6. Config file (`~/.config/popy/config.toml`)

```toml
enabled = true

# Default is the staging dir, excluded from the OS indexer at install time.
# Add ~/Downloads only if you accept that watcher mode is best-effort against
# index/AV races.
watch_dirs = ["~/.popy-stage"]
stage_dir  = "~/.popy-stage"   # where `popy fetch` writes Mode A files

# Glob patterns relative to a watch_dir; any match skips the file.
exclude = [
  "*_popy",
  "*.meta.json", "*.meta.json.new",
  "*.crdownload", "*.part", "*.download", "*.tmp",
  ".*",
  "**/popy-released/**",
]

# macOS FSEvents stability window in milliseconds.
stability_ms = 750

# `popy fetch` cap and read-render cap.
fetch_max_bytes   = 5368709120     # 5 GiB
preview_max_bytes = 536870912      # 512 MiB
```

`config.example.toml` ships under `popyd/dist/` and is copied to `~/.config/popy/config.toml`
on first `popyd` start if absent.

## 7. Build system & dependencies

- **CMake ≥ 3.20**, **C++20**, `-Wall -Wextra -Wpedantic -Werror -fstack-protector-strong
  -D_FORTIFY_SOURCE=2`. Debug builds add `-fsanitize=address,undefined`.
- **System runtime deps:**
  - macOS 13+: `brew install mupdf` (libcurl, CoreServices/CoreFoundation are system).
  - Ubuntu 22+: `apt install libmupdf-dev libcurl4-openssl-dev` (inotify in glibc).
- **Vendored in `popyd/third_party/`:** picosha2.h, stb_image.h, stb_image_write.h,
  nlohmann/json.hpp, toml.hpp. All permissive single-headers.
- **Outputs:** three binaries — `popyd`, `popy`, `popy-render` — sharing a `popy_core`
  static lib. `popy-render` is split out so libmupdf's blast radius is bounded; if mupdf is
  not installed at build time, `popy-render` is skipped and `popy read --mode text|png` for
  PDF/image types returns `unsupported_type` until mupdf is installed.
- Install prefix defaults to `~/.local`. `cmake --install` deposits launchd/systemd templates
  + `claude-hooks.json` into `~/.local/share/popy/dist/` for the user to copy.
- **Streaming SHA-256** via `picosha2::hash256_one_by_one`, updated chunk-by-chunk inside
  the watcher's copy loop and inside libcurl's write callback — conceptually identical to
  `src/offscreen/offscreen.ts:81-143`.

## 8. Service files & install hardening

- **macOS** — `popyd/dist/com.popy.daemon.plist`: `RunAtLoad=true`, `KeepAlive=true`,
  `ProgramArguments=[~/.local/bin/popyd]`, logs to `~/Library/Logs/popyd.log`. Loaded via
  `launchctl load ~/Library/LaunchAgents/com.popy.daemon.plist`.
- **Linux** — `popyd/dist/popyd.service`: user unit, `Type=simple`,
  `ExecStart=%h/.local/bin/popyd`, `Restart=on-failure`. Installed via
  `systemctl --user enable --now popyd.service`.
- **Indexer-exclusion installer** — `cmake --install` runs (post-install, idempotent):
  - macOS: `mdutil -i off "$HOME/.popy-stage"` and creates a `.metadata_never_index` sentinel.
  - Linux: creates `$HOME/.popy-stage/.trackerignore` and adds the path to Baloo's
    `~/.config/baloofilerc` exclude list if Baloo is detected.

## 9. MCP server (`popyd/mcp/server.py`)

- ~80 lines, **stdlib only** (no `mcp` SDK runtime dep — speaks the JSON-RPC stdio protocol
  directly). Tools: `popy_fetch`, `popy_list`, `popy_read_text`, `popy_release`, `popy_delete`.
- All tools subprocess-call `popy --json` and return the parsed JSON.
- `popy_read_text` is allowed; `popy_read_png` is *not* exposed (binary in MCP is awkward
  and agents rarely need pixel data — they want text).
- README snippet:
  ```
  claude mcp add popy -- python3 ~/.local/share/popy/mcp/server.py
  ```
- Plus `popyd/dist/claude-hooks.json` — a `.claude/settings.json` snippet showing a
  `PreToolUse` regex on `Bash` that rewrites `curl|wget|http(ie)?` invocations through
  `popy fetch`. Optional but the lowest-friction integration.

## 10. Tests (`popyd/tests/`, ctest)

- **`test_naming`** — suffix append, sanitization against the same regex as the TS version,
  collision handling `.1`/`.2`, Windows-reserved names.
- **`test_sidecar`** — round-trip JSON ↔ struct; required keys present; unknown keys
  preserved; invalid JSON rejected.
- **`test_hash`** — chunked SHA-256 against known vectors (empty, "abc", 1 MiB random, 100
  MiB random); cross-checked against `openssl dgst -sha256` at test time.
- **`test_config`** — TOML parsing, default fill-in, glob match, watch-dir-outside-home
  refusal.
- **`test_safe_fs`** — `O_NOFOLLOW` rejection of symlinks, `*at` containment within watch
  root, refusal of paths that resolve outside.
- **`test_e2e_fetch`** — spins up a loopback HTTP server in-process, runs `popy fetch`,
  asserts: original-name file never appears in the dir, `_popy` exists with mode `0000`,
  sidecar is well-formed and SHA-256 matches the body. **This is the test that proves the
  Mode A property.**
- **Fuzz harnesses** under `tests/fuzz/` — sidecar parser and MIME sniffer. CI runs each
  for ≥60 s on every PR.
- **Sidecar contract test** — at test time, generate a JSON Schema from the TS
  `QuarantineRecord` interface (via `quicktype`) and validate sample C++ output against it.
  Catches cross-language drift.

Manual smoke path (in README): `popy fetch https://example.com/sample.pdf --mime application/pdf`,
verify only `<name>_popy` exists, `popy list --json | jq .[0]`, `popy read --mode text <id>`,
`popy release <id> --to /tmp/x`, `popy delete <id>`.

## 11. Phased delivery

Each milestone is independently mergeable. No half-finished implementations.

- **M1 — `popy_core` + `popy fetch` + `popy list/release/delete/status` + tests + CMake.**
  Mode A end-to-end, no daemon yet (only the CLI). Acceptance: `test_e2e_fetch` passes.
- **M2 — `popyd` watcher + service files + indexer-exclusion installer + MCP server +
  Claude hooks example.** Mode B working, agent integrations shipped.
- **M3 — `popy-render` sandboxed child + `popy read --mode png|text` for PDF/image.**
  libmupdf/stb_image isolated behind fork+sandbox.
- **M4 — Fuzz harnesses on every PR, hardening pass, README threat-model rewrite.**

## 12. Deliberately out of scope

- **Windows**: per spec.
- **Browser extension modifications**: per spec.
- **Antivirus / content heuristics**: per spec.
- **Secure-delete against physical forensics**: docs are honest about this limit.
- **`popy preview` (Sixel/Kitty/iTerm2)**: deferred. `popy read --mode text` covers agents;
  humans use `popy release --to /tmp/x && open /tmp/x`.
- **`.popy-ignore` per-directory opt-out**: deferred until threat model justifies it.
- **`/dev/urandom` overwrite on delete**: not implemented; theatre on SSDs with wear-leveling.
- **`popy enable`/`disable` config-mutating subcommands**: removed; user edits config.toml.

---

Code style: simple, clear, no slop. RAII-only, no raw `new`/`delete`. `std::span` and
`std::string_view` for non-owning views. `-Werror` in CI. Every function under 60 lines or
it gets refactored. Comments explain *why*, never *what*.
