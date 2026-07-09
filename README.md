# Popy

A download quarantine that follows you from your browser to your terminal. Every file — whether a website hands it to you or an AI agent fetches it on your behalf — lands in an isolated sandbox first, renamed so nothing on your machine will touch it, and waits there until you decide what happens to it.

## What it is

One rule, enforced at two points:

> A downloaded file does not get its real name, its real permissions, or a real place on your filesystem until you explicitly release it.

- **Browser extension** — enforces that rule for anything Chrome/Edge/Brave downloads. Streams it into a sandbox (OPFS) the OS can't see.
- **`popyd` / `popy` (CLI daemon)** — enforces the same rule for anything downloaded from the terminal, built for AI coding agents (Claude Code, Cursor, Codex) that fetch files without you watching every byte.

They share the same `_popy`-suffix-and-quarantine design and an on-disk metadata format, but you can use either one alone.

## Why

Most "download protection" scans a file after it's already been written somewhere the OS can touch it. That's backwards. The order should always be:

```
network / agent ─────→  [ sandbox ]  ─────→  your filesystem
                             ↑                    ↑
                     no OS/AV/indexer sees    this requires your
                     this until you say so    explicit consent
```

## What it doesn't do

- Not an antivirus — doesn't scan content or judge "safe." It just refuses to let the OS near a file until you say so.
- Doesn't protect against a browser/agent runtime that's already compromised.
- Doesn't retroactively quarantine anything from before install.
- Doesn't stop you (or your agent) from releasing a malicious file and running it. It's a speed bump, not a gate.
- The two halves have separate threat models — extension = OPFS (invisible to OS), popyd = disk-based + permission-locked. Don't assume a guarantee from one applies to the other.

## How it works

### Browser extension

```
Page triggers download
        │
chrome.downloads.onDeterminingFilename (background SW)
        │  classify: blob: / POST / Drive>40MB / too large → fallback
        │            otherwise                             → OPFS
        ▼
offscreen document (no lifetime cap)
  fetch → validateHeaders → magic-byte check → hash-wasm SHA-256 (in-stream)
        → pipeTo(FileSystemWritableFileStream)
        │
IndexedDB metadata record + notification + badge
```

The service worker just routes events. All long-running work (fetch, streaming hash, OPFS write) lives in an offscreen document so a multi-GB file can stream in without OOMing or getting killed mid-write. Details in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### CLI daemon

```
agent → popy fetch <url>                 ─┐
                                          │  Mode A: file's first existence
        libcurl streaming GET             │  on disk is already _popy 0000.
          ↳ 16-byte magic probe           │  Original-extension filename
          ↳ picosha2 sha256                │  never appears.
          ↳ write(fd) into <stage>/<id>/<name>_popy
          ↳ fsync, fchmod 0000, atomic sidecar write ─┘

writer → ~/.popy-stage/foo.pdf           ─┐
                                          │  Mode B: watcher renames in place
        FSEvents (macOS) / inotify        │  the moment the file goes quiet.
          ↳ stability: 750ms quiet         │  Mode is 0000 within ~1s.
          ↳ rename → _popy, fchmod 0000, atomic sidecar write ─┘
```

Sidecar `<file>_popy.meta.json` mirrors the extension's `QuarantineRecord` field-for-field.

## Part 1 — Browser extension

### Build + load

```bash
npm install
npm run build
```

In Chrome (or Edge/Brave): `chrome://extensions` → enable **Developer mode** → **Load unpacked** → pick `dist/`.

Sanity check: download a small PDF, confirm it doesn't land in `Downloads/`, click the Popy icon, see it quarantined, **Release** it, confirm it lands where you chose without the `_popy` suffix.

```bash
npm run typecheck
npm run test:unit
```

### Using it

- **Popup** — toolbar icon. Recent quarantined files, one-click Release/Delete.
- **Dashboard** — <kbd>Alt</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd>. Search, filter by origin, preview PDFs/images, inspect hashes, settings.
- **Trusted origins** — allowlist hostnames you actually trust in Settings; downloads from those bypass quarantine.

### Sandbox vs fallback

Some downloads (blob: URLs, POST bodies, single-use confirmation tokens — roughly 20-40% of real-world downloads) can't be safely re-fetched. Those fall back to letting the native download proceed and renaming it with a `_popy` suffix into `Downloads/popy-quarantine/` instead of streaming into OPFS. Still blocks double-click and most preview handlers; doesn't block Spotlight indexing or on-access AV. Every file in the dashboard is labelled with which path it took.

### Permissions

| Permission | Why |
|---|---|
| `downloads` / `downloads.ui` | intercept downloads via `onDeterminingFilename`, suppress the shelf during interception |
| `offscreen` | host the long-running streaming fetch beyond the SW's 30s idle cap |
| `storage` | preferences + IndexedDB metadata, never leaves the browser |
| `notifications` | quarantine notice with Release/Delete, throttled, switchable off |
| `declarativeNetRequest` | preserve Referer/Origin on re-fetches, scoped to our own requests |
| `webRequest` | observe (never block) request method/headers to route POST downloads to fallback |
| `<all_urls>` | downloads originate from arbitrary hosts, can't predict them at install time |
| `unlimitedStorage` | OPFS quota should track disk, not the ~6% default |

No telemetry, no analytics, no network requests beyond re-fetching the download itself.

### Layout

```
src/
├── manifest.json
├── background/background.ts     # SW: listener registration + interception
├── offscreen/offscreen.ts       # fetch → validate → hash → OPFS pipeline
├── popup/                       # toolbar popup UI
├── dashboard/                   # full-tab management UI
├── onboarding/                  # first-run page
└── lib/
    ├── types/                   # shared types + message contracts
    ├── opfs/                    # OPFS wrapper
    ├── interceptor/             # classifier + webRequest ring buffer
    ├── validator/                # response validator
    ├── metadata/                 # IndexedDB store + preferences
    └── dnr/                       # declarativeNetRequest helpers
public/icons/
```

## Part 2 — CLI daemon (popyd)

Same idea, for the terminal. When an agent downloads a file with `curl`/`wget`/a tool call, it normally lands with its real extension — indexed, previewed, scanned, sometimes run. popy gives the agent two paths instead:

| | **Mode A — `popy fetch`** (recommended) | **Mode B — `popyd` watcher** (best-effort) |
|---|---|---|
| How | Stream response straight into `<stage>/<uuid>/<name>_popy`, `O_CREAT \| O_EXCL`, mode `0000` from fd creation | Watch a directory, rename to `_popy` + `chmod 0000` when a file appears |
| Original-extension name ever on disk? | No | Briefly, between create and rename |
| Used via | `popy_fetch` MCP tool, or `popy fetch <url>` from a Bash hook | Side effect of anything writing into `~/.popy-stage/` |

### Build

```bash
# macOS
brew install cmake          # libcurl ships with macOS
cmake -S popyd -B popyd/build -DCMAKE_BUILD_TYPE=Release
cmake --build popyd/build -j
cmake --install popyd/build --prefix ~/.local

# Ubuntu / Debian
sudo apt install cmake libcurl4-openssl-dev
cmake -S popyd -B popyd/build -DCMAKE_BUILD_TYPE=Release
cmake --build popyd/build -j
```

Produces `popy` (CLI) and `popyd` (watcher daemon). Install also excludes `~/.popy-stage` from Spotlight/Tracker indexing.

Build flags: `-DPOPY_WERROR=ON` (warnings as errors), `-DPOPY_SANITIZERS=ON` (ASan+UBSan), `-DPOPY_BUILD_TESTS=OFF`.

### Run the daemon

```bash
# macOS
cp popyd/dist/com.popy.daemon.plist ~/Library/LaunchAgents/
# edit the path inside to match your install
launchctl load ~/Library/LaunchAgents/com.popy.daemon.plist
launchctl start com.popy.daemon

# Linux
mkdir -p ~/.config/systemd/user
cp popyd/dist/popyd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now popyd.service
```

Or run it in the foreground (`~/.local/bin/popyd` — it doesn't daemonize itself). Logs: `~/Library/Logs/popyd.log` (macOS) or `journalctl --user -u popyd` (Linux).

### CLI reference

`<file>` resolves by UUID, UUID prefix (≥4 chars), original filename, or `<name>_popy` filename. Every command takes `--json`.

```text
popy fetch <url> [--out <name>] [--mime <type>] [--max-bytes N] [--json]
popy list [--json]                                  # flags sidecars whose data file is gone as data-missing
popy read <file> [--mode raw|text|png] [--page N] [--max-bytes N]
                                                    # verified (sig+size+hash) before any byte leaves
popy release <file> --to <path> [--force] [--json]  # hash-verified, symlink-safe atomic release
popy delete <file> [--json]
popy verify <file> [--json]                         # check sidecar HMAC + content SHA-256
popy resign [--json]                                # one-time: sign pre-upgrade (unsigned) sidecars
popy status [--json]
popy pause | popy resume                            # transient; cleared on daemon restart
popy config [--print] [--path]
```

Smoke test:

```bash
popy fetch https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf --mime application/pdf
popy list
popy release dummy.pdf --to /tmp/dummy.pdf
shasum -a 256 /tmp/dummy.pdf
```

### Claude Code integration

MCP server:

```bash
claude mcp add popy -- python3 ~/.local/share/popy/mcp/server.py
# or if popy isn't on $PATH:
POPY_BIN=$HOME/.local/bin/popy claude mcp add popy -- python3 ~/.local/share/popy/mcp/server.py
```

Gives the agent six tools: `popy_fetch`, `popy_list`, `popy_read_text`, `popy_verify`, `popy_release`, `popy_delete`.

Bash hook (optional, recommended) — refuses direct `curl`/`wget`/`http` and pushes the agent to `popy_fetch` instead. Merge `popyd/dist/claude-hooks.json` into `~/.claude/settings.json`; hook script is `popyd/dist/popy-bash-hook.sh`.

```text
agent → Bash("curl https://example.com/x.pdf -o /tmp/x.pdf")
hook  → exit 2: "popy: refusing direct download. Use the popy_fetch MCP tool …"
agent → popy_fetch(url=https://example.com/x.pdf)
agent → popy_read_text(file=...)
```

### What popyd stops (and doesn't)

Stops: accidental double-click (mode `0000`), reading in-flight bytes (mode 0000 from first byte, no real name appears until the signed sidecar is on disk), tampering while quarantined (every read/release verifies sidecar signature + size + full SHA-256 first), symlink-redirect at a release destination, OS preview/shell handlers touching attacker-controlled bytes, agents silently writing executables (paired with the Bash hook).

Doesn't stop: root or the file owner acting deliberately (a same-user HMAC key can't defend against a process with your own privileges), browser exploits before a download happens, an agent explicitly releasing and running a malicious file, Mode B's small race window between file-appears and rename, forensic recovery after `popy delete` (unlink, not overwrite).

### Layout

```
popyd/
├── src/
│   ├── main_popy.cpp / main_popyd.cpp / main_popy_render.cpp
│   ├── core/                      # paths, naming, hash, mime, sidecar, config, log, safe_fs
│   ├── net/fetch.cpp              # Mode A
│   ├── store/quarantine.cpp       # list/release/delete + Mode B
│   ├── ipc/status.cpp             # AF_UNIX status socket
│   ├── watch/watcher_*.cpp        # FSEvents (macOS) / inotify (Linux)
│   ├── render/                    # sandboxed pdf/image/markdown renderers
│   └── cli/commands.cpp
├── third_party/                   # vendored single-header libs
├── mcp/server.py                  # JSON-RPC MCP server (stdlib only)
├── dist/                          # service files, Claude hook, example config
└── tests/                         # ctest binaries
```

### Status

Mode A + Mode B both work, MCP server ships, Claude hook ships. Core tests (`ctest`) cover naming, hashing, sidecar, config, safe_fs, e2e fetch, and the watcher.

`popy read --mode png|text` for PDF/image (via libmupdf + stb_image, sandboxed with `fork()` + `setrlimit`) is **implemented but partial**:

- **macOS**: builds when `libmupdf` is installed (`brew install mupdf`). Uses Seatbelt + setrlimit — defense-in-depth, not a hard boundary yet.
- **Linux**: seccomp allow-list code exists in `render/sandbox.cpp` but `popy-render` is currently excluded from the Linux build pending seccomp-policy verification. `--mode png` returns `unsupported_type`; `--mode raw|text` works everywhere.
- **Other platforms**: not built/supported outside macOS and Linux. On Windows the browser extension is your only coverage.

## Repository layout

```
popy_tinkle/
├── src/                    # browser extension source
├── popyd/                  # CLI daemon + CLI source
├── public/icons/
├── docs/ARCHITECTURE.md
├── tests/                  # extension unit tests (Vitest)
├── .github/workflows/ci.yml
└── vite.config.ts
```
