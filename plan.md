# popyd: Background Quarantine Daemon + `popy` CLI for AI Agents

A lightweight C++ daemon (`popyd`) under `popyd/` that watches configurable download roots (default `~/Downloads/`), auto-renames every new file to `<name>_popy`, and writes a JSON sidecar — paired with a `popy` CLI that every command supports `--json` on so AI agents (Claude Code, Cursor, Codex) can list/read/release quarantined files as subprocess calls, with a future thin MCP shim.

## 1. Mental model

- **popyd quarantines everything, by default.** Any new file in a watched root is renamed in place to `<name>_popy`, hashed (streaming SHA-256), and sidecar'd. The user opts out globally (`popy disable`), per-directory (drop a `.popy-ignore` sentinel file), or via config exclude globs.
- **Single source of truth is the filesystem.** Sidecars are inline next to each file (`<name>_popy.meta.json`). `popy list/read/release/delete` all read sidecars directly — no heavy IPC. popyd exposes only a tiny status socket.
- **Agents consume popyd via the `popy` CLI with `--json`.** No MCP wire protocol in C++. A future ~50-line Python/TS shim can expose the CLI as a real MCP server; that's out of scope here.
- **Rastering is in-process, in the CLI.** `popy read`/`popy preview` link libmupdf + stb_image statically/dynamically and render inside the `popy` binary — the OS never dispatches a handler against the `_popy` file.

## 2. Directory layout (new — under `popyd/`)

```
popyd/
├── CMakeLists.txt
├── README.md                     # build, install, config, agent integration
├── src/
│   ├── main_popyd.cpp            # daemon entrypoint: watcher loop + status socket
│   ├── main_popy.cpp             # CLI entrypoint: subcommand dispatch
│   ├── core/
│   │   ├── paths.{h,cpp}         # XDG paths, config, PID, socket, log locations
│   │   ├── config.{h,cpp}        # parse ~/.config/popy/config.toml
│   │   ├── naming.{h,cpp}        # _popy suffix, sanitization (mirrors src/lib/opfs/fs.ts)
│   │   ├── sidecar.{h,cpp}       # QuarantineRecord ↔ JSON, read/write, lock
│   │   ├── hash.{h,cpp}          # streaming SHA-256 (picosha2 wrapper)
│   │   ├── uuid.{h,cpp}          # UUID v4 from /dev/urandom
│   │   ├── mime.{h,cpp}          # extension → MIME, magic-byte sniff (16B probe)
│   │   └── log.{h,cpp}           # simple syslog/file logger
│   ├── watch/
│   │   ├── watcher.h             # common interface
│   │   ├── watcher_fsevents.cpp  # macOS — FSEventStream, stability polling
│   │   └── watcher_inotify.cpp   # Linux — IN_CLOSE_WRITE / IN_MOVED_TO
│   ├── ipc/
│   │   └── status.{h,cpp}        # tiny AF_UNIX line-protocol for `popy status`
│   ├── net/
│   │   └── fetch.{h,cpp}         # libcurl streaming GET for `popy fetch`
│   ├── store/
│   │   └── quarantine.{h,cpp}    # list/release/delete against the sidecar set
│   ├── render/
│   │   ├── pdf.{h,cpp}           # libmupdf: page count, page→PNG, page→text
│   │   ├── image.{h,cpp}         # stb_image decode → stb_image_write PNG (EXIF strip)
│   │   ├── markdown.{h,cpp}      # verbatim stdout / light ANSI for preview
│   │   └── terminal.{h,cpp}      # Sixel/Kitty/iTerm2 detection + emit (preview only)
│   └── cli/
│       └── commands.{h,cpp}      # argv parsing, subcommand dispatch, --json formatting
├── third_party/                  # all vendored single-header, permissive
│   ├── picosha2.h
│   ├── stb_image.h
│   ├── stb_image_write.h
│   ├── nlohmann_json.hpp
│   └── toml.hpp                  # toml++ single-header
├── dist/
│   ├── com.popy.daemon.plist     # macOS launchd
│   ├── popyd.service             # Linux systemd (user unit)
│   └── config.example.toml
└── tests/
    ├── CMakeLists.txt
    ├── test_naming.cpp
    ├── test_sidecar.cpp
    ├── test_hash.cpp
    └── test_config.cpp
```

No existing files are touched except the repo-root `README.md`, which gains a "## CLI Daemon" section at the end.

## 3. Watcher behaviour

- **Watched roots:** read from config. Default `["~/Downloads"]`. Multiple roots supported.
- **Excludes:** glob list from config. Defaults:
  - `*_popy` (never re-quarantine our own files)
  - `*.meta.json` (sidecars)
  - `*.crdownload`, `*.part`, `*.download`, `*.tmp` (in-flight browser downloads)
  - `.*` (dotfiles, including `.popy-ignore`)
  - `**/popy-released/**`
- **Per-directory opt-out:** if a watched directory contains a file called `.popy-ignore`, popyd skips that directory (and its subdirs) entirely.
- **Global disable:** `enabled = false` in config, or transient `popy pause` (sets an in-memory flag; cleared on restart).
- **Stability detection:**
  - Linux: trigger on `IN_CLOSE_WRITE` and `IN_MOVED_TO`. Reliable.
  - macOS FSEvents: on `kFSEventStreamEventFlagItemCreated | ItemModified | ItemRenamed`, enqueue the path; worker polls size+mtime every 250ms and processes once stable for 750ms (or the `exclude` glob removes it mid-write).
- **Processing a new file (streaming, single pass):**
  1. Open source for read.
  2. Create `<name>_popy` in the same directory with O_EXCL. On collision, append `.N`.
  3. Stream source → SHA-256 hasher → dest, in 64 KiB chunks. Handles multi-GB without mmap.
  4. Sniff MIME from first 16 bytes (mirrors the browser's magic-byte check in `@/Users/nikhildonde/Codes/popy/popy/src/offscreen/offscreen.ts:91-138`).
  5. `fsync` dest, `rename` source→dest as the atomic commit if same-filesystem (else copy-then-unlink-source on success).
  6. Write `<name>_popy.meta.json` with `O_EXCL | O_CREAT`.
  7. Log one line to the daemon log.
- **popyd never touches files that already have `_popy` suffix** and never rewrites an existing sidecar.

## 4. CLI surface (`popy`)

Every command accepts `--json` (default: human-friendly). `<file>` resolves by full UUID, UUID prefix, basename of original, or basename of `_popy` file.

- **`popy list [--status <s>] [--root <dir>] [--json]`** — enumerate sidecars across all watched roots, newest first.
- **`popy read <file> [--page N] [--all] [--info] [--mode png|text]`** — agent-facing. In-process rastering:
  - PDF: `--page N` → raw PNG bytes on stdout; `--all` → length-prefixed PNG stream `[4B BE len][PNG bytes]` per page; `--mode text` → `fz_new_stext_page` text per page; `--info` → JSON `{pages, size, mime}`.
  - PNG/JPEG: decode via stb_image → re-encode PNG via stb_image_write → stdout (strips EXIF, ICC, ancillary chunks).
  - Markdown: verbatim stdout.
  - Other: exit 2 with `unsupported_type`.
- **`popy preview <file> [--page N]`** — human-facing: Sixel/Kitty/iTerm2 detection via `$TERM`/`$TERM_PROGRAM`, ASCII-block fallback. ANSI-styled MD.
- **`popy release <file> --to <path> [--force]`** — strip `_popy`, copy to `<path>`, update sidecar `status: "released"`, then unlink source + sidecar. Refuses if `<path>` exists unless `--force`.
- **`popy delete <file>`** — single-pass overwrite with `/dev/urandom`, unlink file + sidecar. Honest about not defeating physical forensics in docs.
- **`popy fetch <url> [--out <name>] [--dir <root>] [--mime <type>]`** — popyd's libcurl streaming GET, writing directly to a watched root with the `_popy` suffix already applied (and the daemon's watcher skipping the already-suffixed file). Prints `{id, sha256, path, sizeBytes, mime}` JSON on stdout.
- **`popy status [--json]`** — queries popyd's status socket; prints PID, uptime, watched roots, file count, bytes. Exit 1 if daemon not running.
- **`popy enable` / `popy disable`** — toggle `enabled` in the config file.
- **`popy pause` / `popy resume`** — transient, via status socket; cleared on daemon restart.
- **`popy config [--print] [--edit] [--path]`** — show/edit the config; `--path` prints its location.

## 5. Sidecar JSON schema

One sidecar per file, inline next to it. Mirrors `QuarantineRecord` from `@/Users/nikhildonde/Codes/popy/popy/src/lib/types/index.ts:14-47` field-for-field so a future sync layer can ingest sidecars into the browser extension's IndexedDB with zero translation.

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
  "note": "quarantined by popyd watcher",
  "agent": "popyd/0.1",
  "origin": "watcher"
}
```

- `path: "fallback"` is kept (it's already a valid `InterceptPath` value — no TS change needed).
- `sourceUrl`/`sourceHost` are empty for watcher-captured files, populated by `popy fetch`.
- `schemaVersion`, `agent`, `origin` (`"watcher" | "fetch"`) are additive; the existing `idb` store accepts unknown keys.
- `originalFilename` is sanitized by the same algorithm as `@/Users/nikhildonde/Codes/popy/popy/src/lib/opfs/fs.ts:13-25` so a `release` back to disk produces byte-identical filenames to the browser's.

## 6. Config file (`~/.config/popy/config.toml`)

```toml
enabled = true

watch_dirs = ["~/Downloads"]

# Glob patterns; any match (relative to a watch_dir) skips the file.
exclude = [
  "*_popy",
  "*.meta.json",
  "*.crdownload", "*.part", "*.download", "*.tmp",
  ".*",
  "**/popy-released/**",
]

# Stability window for macOS FSEvents in milliseconds.
stability_ms = 750

# Max bytes for in-process rastering preview (PDFs above this refuse to render).
preview_max_bytes = 536870912  # 512 MiB
```

`config.example.toml` ships under `popyd/dist/` and is copied to `~/.config/popy/config.toml` on first `popyd` start if absent.

## 7. Build system & dependencies

- **CMake ≥ 3.20**, **C++20**, warnings-as-errors except where libmupdf headers require otherwise.
- **System runtime deps:**
  - macOS 13+: `brew install mupdf` (libcurl from system, CoreServices/CoreFoundation for FSEvents).
  - Ubuntu 22+: `apt install libmupdf-dev libcurl4-openssl-dev` (inotify is in glibc).
- **Vendored in `popyd/third_party/`:** picosha2.h, stb_image.h, stb_image_write.h, nlohmann/json.hpp, toml.hpp. All permissive single-headers.
- **Outputs:** two binaries, `popyd` and `popy`, sharing a common `popy_core` static lib. Install prefix defaults to `~/.local`. `cmake --install` deposits launchd/systemd templates into `popyd/dist/` for the user to copy.
- **Streaming SHA-256** via `picosha2::hash256_one_by_one`, updated chunk-by-chunk inside the watcher's copy loop and inside libcurl's write callback — conceptually identical to `@/Users/nikhildonde/Codes/popy/popy/src/offscreen/offscreen.ts:81-143`.

## 8. Service files

- **macOS** — `popyd/dist/com.popy.daemon.plist`: `RunAtLoad=true`, `KeepAlive=true`, `ProgramArguments=[~/.local/bin/popyd]`, logs to `~/Library/Logs/popyd.log`. Installed via `brew services start popyd` (brew formula out of scope) or `launchctl load ~/Library/LaunchAgents/com.popy.daemon.plist`.
- **Linux** — `popyd/dist/popyd.service`: user unit, `Type=simple`, `ExecStart=%h/.local/bin/popyd`, `Restart=on-failure`. Installed via `systemctl --user enable --now popyd.service`.

README gives copy-paste install steps for both.

## 9. Tests (`popyd/tests/`, ctest)

- **`test_naming`** — suffix append, sanitization against the same regex as the TS version, collision handling `.1`/`.2`, Windows-reserved names.
- **`test_sidecar`** — round-trip JSON ↔ struct; required keys present; unknown keys preserved; invalid JSON rejected.
- **`test_hash`** — chunked SHA-256 against known vectors (empty, "abc", 1 MiB random, 100 MiB random if fixture available); cross-checked against `openssl dgst -sha256`.
- **`test_config`** — TOML parsing, default fill-in, glob match, `.popy-ignore` handling.

Manual smoke path (in README): create a file in `~/Downloads/`, verify it's renamed within ~1s, verify sidecar schema, `popy list --json | jq .[0]`, `popy read --page 0 <id> | file -`, `popy release <id> --to /tmp/x`, `popy delete <id>`.

## 10. Documentation

- **`popyd/README.md`** (short, scannable): prerequisites, `cmake -S popyd -B build && cmake --build build && cmake --install build`, service install, config, CLI cheatsheet, schema table, agent-integration snippet.
- **Root `README.md`** (append a "## CLI Daemon" section): one-paragraph pitch linking to `popyd/README.md`; diagram `agent → popy CLI → sidecar/raster`; note about the shared `_popy` convention and schema with the browser extension.

## 11. Open items / deliberately out of scope

- **Real MCP server**: deferred to a future thin Python/TS shim that subprocess-calls the `popy` CLI with `--json`. No C++ protocol code now.
- **Windows**: explicitly out of scope per spec.
- **Browser extension modifications**: forbidden per spec.
- **Antivirus / content heuristics**: explicitly excluded per spec.
- **Secure-delete against physical forensics**: one-pass overwrite only; docs are honest about this limit.
- **Preview for types beyond PDF/PNG/JPEG/MD**: `popy read`/`preview` refuse with `unsupported_type`; `release` still works.


Make sure the code is simple and clear and there is no slop, code shouldnt be over complicated.Make sure the code is well documented and easy to understand and that it is very lightweight and efficient. It should be very lightweight. 