# Popy

> _A browser download quarantine. Every file you download lands in an isolated sandbox, renamed so no operating-system handler will touch it, and waits there until you decide what happens to it._

<p align="center">
  <img src="public/icons/icon-128.png" alt="Popy icon" width="96" />
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-b84a2f.svg" alt="Apache 2.0"></a>
  <a href="https://developer.chrome.com/docs/extensions/reference/manifest"><img src="https://img.shields.io/badge/manifest-v3-18181a.svg" alt="Manifest V3"></a>
  <img src="https://img.shields.io/badge/chrome-116%2B-d6a24a.svg" alt="Chrome 116+">
  <img src="https://img.shields.io/badge/status-public%20preview-6a8a6a.svg" alt="Public preview">
</p>

---

## What Popy does

When a website tries to send you a file, your browser usually saves it straight into your `Downloads/` folder. That simple act is enough for a surprising number of attacks — previewing in Explorer or Finder, accidental double-clicks, parser exploits in the OS shell — to compromise your machine before you even open the thing. Popy puts a buffer between the network and your filesystem:

1. **It intercepts every download** through the browser's extension APIs, before the bytes reach your user-visible filesystem.
2. **It re-streams the file into the Origin Private File System (OPFS)** — a browser-managed sandbox that your operating system cannot see. No indexer, no preview handler, no antivirus on-access scanner, nothing. The file is invisible to everything outside the browser.
3. **It renames it with a `_popy` suffix.** Even if the file somehow escaped the sandbox, no OS handler would recognize the extension, so nothing executes by accident.
4. **It waits.** The file sits there, harmless, until you open Popy's dashboard, look it over, and either _release_ it (write it out to a location you choose, with the `_popy` suffix stripped) or _delete_ it (gone in milliseconds).

You install Popy once and forget about it. Your downloads quietly pile up in the sandbox. You review them on your schedule.

## Why this exists

Most "download protection" is a scanner trying to catch malware after the file has already been written to a location where the OS can touch it. That's backwards. The correct order is:

```
network ─────→  [ browser sandbox ]  ─────→  your filesystem
                     ↑                         ↑
                no OS can see this         this requires your consent
```

OPFS finally gave web platforms the primitive to do this properly from inside an extension, without native code or a background service. Popy is a small, focused implementation of that idea.

## What Popy does **not** do

Read this section. It's the most important section.

- **Popy is not an antivirus.** It does not scan file contents, pattern-match against known-bad hashes, or judge whether a file is "safe." It refuses to let the OS near the file until you explicitly allow it.
- **Popy does not protect against browser exploits.** If an attacker compromises the browser renderer itself, no download ever happens; Popy cannot see the attack.
- **Popy cannot retroactively quarantine files you downloaded before installing it.** It only sees new downloads.
- **Popy cannot stop you from releasing a malicious file.** It is a speed bump, not a gate. If you release `setup.exe` and double-click it, all bets are off.

For the full honest breakdown, read [THREAT_MODEL.md](THREAT_MODEL.md).

## How to install (from source)

```bash
git clone https://github.com/ARDA7787/popy_tinkle.git
cd popy_tinkle
npm install
npm run build
```

Then in Chrome (or Edge, or Brave, or any Chromium-derived browser):

1. Open `chrome://extensions`.
2. Enable **Developer mode** (top-right toggle).
3. Click **Load unpacked**.
4. Choose the `dist/` folder Popy just produced.

That's it. The onboarding tab will open. When you're ready, click **Begin**. Popy is now watching your downloads.

### Smoke test

Once loaded, run this 30-second sanity check before trusting Popy with a real download:

1. Visit any HTTPS site that serves a small PDF or image (e.g. a GitHub release asset).
2. Click _Download_.
3. The download should _not_ appear in your `Downloads/` folder. Click the Popy icon in your toolbar — you should see it listed in the popup, sealed in the sandbox.
4. Click **Release**, choose a destination, and confirm the file lands there with the correct contents and **without** the `_popy` suffix.
5. Try a tricky download too — a `blob:` URL or a Google Drive large-file confirmation — to exercise the fallback path. The file should land in `Downloads/popy-quarantine/<uuid>_<name>_popy`, harmless until you rename it.

### Run the test suite (optional)

```bash
npm run typecheck
npm run test:unit
```

## How to use it

- **Popup** — click the Popy icon in your toolbar. Shows your most recent quarantined files with one-click **Release** and **Delete**.
- **Dashboard** — press <kbd>Alt</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd>, or click _Dashboard ↗_ in the popup. The full surface: search, filter by origin, preview PDFs/images in-browser, inspect file hashes, configure settings.
- **Trusted origins** — for sites you genuinely trust (your own file server, GitHub releases, etc.), add their hostname to the allowlist in Settings. Downloads from those hosts bypass quarantine.

## The two paths: sandbox and fallback

Not every download can be safely re-fetched. Roughly 20–40% of real-world downloads use techniques (JavaScript-created blob URLs, POST requests with non-idempotent bodies, single-use confirmation tokens) that a neutral re-fetch from the extension's context will fail on. For those, Popy has a second line of defense:

|                             | **Sandbox path** (`opfs`)                                                       | **Fallback path** (`fallback`)                                                         |
| --------------------------- | ------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| How                         | Cancel native download → re-fetch → stream into OPFS                            | Let native download proceed; rename with `_popy` suffix into `Downloads/popy-quarantine/` |
| File visible to OS?         | **No**                                                                          | Yes, but with no handler-recognized extension                                          |
| Stops double-click?         | ✓                                                                               | ✓                                                                                      |
| Stops preview handlers?     | ✓                                                                               | ✓ for most (they dispatch on the last extension)                                       |
| Stops Spotlight / indexing? | ✓                                                                               | ✗                                                                                      |
| Stops on-access AV scanner? | ✓                                                                               | ✗ (and MOTW is still applied)                                                          |
| Release                     | `showSaveFilePicker` → stream bytes out                                         | `chrome.downloads.show` → user renames manually                                        |

Every file in the dashboard is clearly labelled with which path it took, and why. You're never in the dark.

## Architecture

```
┌────────────────────────────┐
│  Page triggers download    │
└──────────────┬─────────────┘
               ▼
┌────────────────────────────┐   classify:
│  chrome.downloads          │   • blob: ─────────→ fallback
│  .onDeterminingFilename    │   • POST detected ──→ fallback
│      (background SW)       │   • Drive >40MB ────→ fallback
└──────────────┬─────────────┘   • too large ──────→ fallback
               │                 • otherwise ──────→ OPFS
               ▼
┌────────────────────────────┐
│      Offscreen document    │   fetch → ReadableStream
│      (no lifetime cap)     │     → validateHeaders
│                            │     → magic-byte check
│   network → hasher → OPFS  │     → hash-wasm SHA-256 (in-stream)
│   end-to-end backpressure  │     → pipeTo(FileSystemWritableFileStream)
└──────────────┬─────────────┘
               ▼
┌────────────────────────────┐
│  IndexedDB metadata record │
│  + notification + badge    │
└────────────────────────────┘
```

The service worker is an event router and nothing else. All long-running work — fetch, streaming hash, OPFS write — lives in an offscreen document, which has no lifetime cap. This is what lets Popy stream a multi-gigabyte file into the sandbox without OOMing or being killed mid-write.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full walkthrough.

## Project layout

```
popy/
├── src/
│   ├── manifest.json           # MV3 manifest — permissions, entrypoints, CSP
│   ├── background/
│   │   └── background.ts       # Service worker: listener registration + interception
│   ├── offscreen/
│   │   ├── offscreen.html
│   │   └── offscreen.ts        # fetch → validate → hash → OPFS streaming pipeline
│   ├── popup/                  # Toolbar popup UI
│   ├── dashboard/              # Full-tab management UI
│   ├── onboarding/             # First-run explanation page
│   ├── ui.css                  # Shared design system
│   └── lib/
│       ├── types/              # Shared TypeScript types and message contracts
│       ├── opfs/               # OPFS wrapper (dir layout, sanitization, streaming)
│       ├── interceptor/        # Classifier + webRequest ring buffer
│       ├── validator/          # Six-layer response validator
│       ├── metadata/           # IndexedDB store + preferences
│       ├── dnr/                # declarativeNetRequest session rule helpers
│       └── ui.ts               # UI formatting helpers
├── public/icons/               # 16 / 32 / 48 / 128 PNGs
├── docs/                       # Long-form documentation
├── THREAT_MODEL.md             # Honest threat model
├── SECURITY.md                 # Responsible-disclosure policy
├── CONTRIBUTING.md
└── vite.config.ts              # Built with @crxjs/vite-plugin
```

## Permissions, explained

Popy asks for six permissions. You should understand why.

| Permission              | Why                                                                                                                                                            |
| ----------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `downloads`             | The core reason we exist: observe and intercept every download via `onDeterminingFilename`.                                                                    |
| `downloads.ui`          | Briefly suppress the download shelf during interception so the user doesn't see a file start, disappear, and reappear in Popy.                                 |
| `offscreen`             | Spawn an offscreen document that hosts long-running streaming fetches beyond the 30-second service-worker idle cap.                                            |
| `storage`               | Persist your preferences in `chrome.storage.local` and per-file metadata in IndexedDB. **None of this leaves your browser.**                                   |
| `notifications`         | Show a discreet notice when a file is quarantined, with inline Release/Delete buttons. Throttled; switchable off in Settings.                                  |
| `declarativeNetRequest` | Install short-lived session rules that preserve `Referer` and `Origin` headers on Popy's re-fetches. Rules are scoped to our own initiator and removed immediately after each request. |
| `webRequest`            | **Observe** (non-blocking) outbound request methods and headers so the classifier can route POST-initiated downloads directly to the fallback path. We never block or modify requests via this API. |
| `<all_urls>`            | Downloads originate from arbitrary origins; we cannot predict them at install time. No bytes are ever sent anywhere outside of your own downloads.             |
| `unlimitedStorage`      | OPFS quota should track available disk, not the standard ~6% default.                                                                                          |

Popy makes zero network requests of its own beyond re-fetching downloads. No telemetry, no analytics, no update pings beyond the browser's built-in extension-store mechanism. Verify this yourself: the entire codebase is a few thousand lines.

## Contributing

Popy is intentionally small. The entire source is readable in an afternoon. If you find a security issue, see [SECURITY.md](SECURITY.md) for how to report it privately. For everything else, open an issue or PR — see [CONTRIBUTING.md](CONTRIBUTING.md).

The most valuable contributions right now:

- **Reproducing re-fetch failure modes** — if you find a real website whose downloads don't work with Popy, file it with a URL and a HAR if you can. These are how the classifier gets smarter.
- **Testing on Edge and Brave.** Popy targets all Chromium-derived browsers equally, but the author's primary test target is Chrome itself.
- **Documentation.** The code has opinions; the docs should explain them.

## License

Apache 2.0. See [LICENSE](LICENSE). Apache was chosen over MIT specifically for its explicit patent-grant language — a small but real safeguard for contributors to a security tool.

## CLI Daemon — popyd

The browser extension protects browser downloads. **popyd** (a small C++ daemon) and **popy** (its CLI) bring the same `_popy` quarantine to the command line, designed for **AI CLI agents** like Claude Code, Cursor, and Codex.

### What it protects

When an AI agent downloads a file with `curl`/`wget`/a tool call, that file lands in `~/Downloads` (or wherever) with its original extension. Spotlight indexes it. The on-access AV scanner reads it. Preview handlers may parse it. A misclick or `bash -c "open ./payload"` runs it. None of those should happen for a file the agent has not yet inspected.

popy gives the agent two paths:

| | **Mode A — `popy fetch`** *(recommended, strong)* | **Mode B — `popyd` watcher** *(best-effort)* |
|---|---|---|
| How | Stream the network response straight into `<stage>/<uuid>/<name>_popy` opened with `O_CREAT \| O_EXCL`. Mode `0000` from the moment the fd is created. | Watch a directory; when a file appears, atomically rename it to `<name>_popy` and `chmod 0000`. |
| Original-extension filename ever exists on disk? | **No.** | Yes — for the milliseconds between create and rename. |
| Stops OS handlers / preview / AV scanners? | **Yes.** | Best-effort. The watch dir is excluded from Spotlight/Tracker by the installer. |
| AI agent uses via | `popy_fetch` MCP tool, or `popy fetch <url>` from a Bash hook. | Drops files into `~/.popy-stage/` as a side effect of any tool that writes there. |

The browser extension provides Mode A's property natively via OPFS. Mode A in this CLI is the only honest equivalent outside a browser.

### Build

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

This also runs the indexer-exclusion step on `~/.popy-stage` (`mdutil -i off` on macOS, a `.trackerignore` on Linux).

#### Build options

| Flag | Effect |
|---|---|
| `-DPOPY_WERROR=ON` | warnings as errors (CI default) |
| `-DPOPY_SANITIZERS=ON` | ASan + UBSan (Linux/older macOS; broken on Apple Clang 16 + macOS 26) |
| `-DPOPY_BUILD_TESTS=OFF` | skip the test binaries |

### Run the daemon

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

Or just run it in the foreground (it does NOT daemonize itself; launchd / systemd handle that):

```bash
~/.local/bin/popyd
```

Logs:
- macOS: `~/Library/Logs/popyd.log`
- Linux: `~/.local/state/popy/popyd.log` (or `journalctl --user -u popyd`)

### CLI reference

Every command takes `--json`. `<file>` resolves by full UUID, UUID prefix (≥ 4 chars), original filename, or `<name>_popy` filename.

```text
popy fetch <url> [--out <name>] [--mime <type>] [--max-bytes N] [--json]
popy list [--json]
popy read <file> [--mode raw|text|png] [--page N]   # png/text-for-PDF need libmupdf, see Status
popy release <file> --to <path> [--force] [--json]
popy delete <file> [--json]
popy status [--json]                                # --json adds enforcementMode/guaranteeLevel/bypassSurface
popy pause | popy resume                            # transient; cleared on daemon restart
popy config [--print] [--path]
```

Quick smoke:

```bash
popy fetch https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf --mime application/pdf
popy list
popy release dummy.pdf --to /tmp/dummy.pdf
shasum -a 256 /tmp/dummy.pdf
```

### AI agent integration

**Claude Code via MCP** — add the popy MCP server:

```bash
claude mcp add popy -- python3 ~/.local/share/popy/mcp/server.py
# or, if popy is not on $PATH:
POPY_BIN=$HOME/.local/bin/popy claude mcp add popy -- python3 ~/.local/share/popy/mcp/server.py
```

The agent now has five tools: `popy_fetch`, `popy_list`, `popy_read_text`, `popy_release`, `popy_delete`.

**Claude Code via Bash hook** (optional, recommended) — refuse direct `curl`/`wget`/`http` invocations and tell the agent to use `popy_fetch` instead. Merge `popyd/dist/claude-hooks.json` into your `~/.claude/settings.json`. The hook is `popyd/dist/popy-bash-hook.sh`.

After install:

```text
agent: I'll download the file
agent → Bash("curl https://example.com/x.pdf -o /tmp/x.pdf")
hook  → exit 2 with stderr:
        popy: refusing direct download. Use the `popy_fetch` MCP tool …
agent → popy_fetch(url=https://example.com/x.pdf)
agent → popy_read_text(file=...)   # decides what to do with it
```

**Other agents** — any agent that can run subprocesses can use `popy fetch <url> --json` and parse the resulting JSON. The MCP server is just a thin shim.

### popyd threat model

What popyd **does** stop:

- Accidental double-click on a downloaded file (mode `0000`).
- OS preview handlers / shell handlers parsing attacker-controlled bytes (no original-extension filename exists in Mode A; renamed within ~1s in Mode B with the watch dir excluded from indexers).
- AI agents from silently writing executable downloads into your filesystem (when paired with the Bash hook).

What popyd **does not** stop:

- Browser exploits before the download ever happens.
- An agent that explicitly `popy release`s a malicious file and runs it.
- Mode B's race window between the first byte landing on disk and the rename — Spotlight/AV/preview can still touch a file in that window. Mode A closes this; Mode B makes it as small as possible (~ms).
- Forensic recovery after `popy delete` — we don't overwrite, just unlink.
- Anything the OS kernel does. We're not a kernel module.

This is a separate, disk-based trust model from the browser extension's OPFS sandbox (see [THREAT_MODEL.md](THREAT_MODEL.md), which currently documents the extension only) — files quarantined by popyd exist on disk, permission-locked, rather than being invisible to the OS entirely.

### How it works

```
agent → popy fetch <url>                 ─┐
                                          │  Mode A: file's first existence
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
                                          │  Mode B: watcher renames in place
        ┌─────────────────────────────┐   │  the moment FSEvents/inotify
        │ FSEvents (macOS) / inotify  │   │  reports the file is stable.
        │ stability: 750ms quiet      │   │
        │   ↳ rename(...) → _popy     │   │  Mode is 0000 within ~1s.
        │   ↳ read fd → sha256        │   │
        │   ↳ fchmod 0000             │   │
        │   ↳ atomic sidecar write    │   │
        └─────────────────────────────┘  ─┘
```

Sidecar `<file>_popy.meta.json` mirrors the browser extension's `QuarantineRecord` field-for-field, so a future sync layer could ingest them into the extension's IndexedDB with zero translation.

### popyd layout

```
popyd/
├── src/
│   ├── main_popy.cpp              # CLI entrypoint
│   ├── main_popyd.cpp             # daemon entrypoint
│   ├── main_popy_render.cpp       # sandboxed render child entrypoint
│   ├── core/                      # paths, naming, hash, mime, sidecar, config, log, safe_fs
│   ├── net/fetch.cpp              # Mode A — popy fetch
│   ├── store/quarantine.cpp       # list/release/delete + Mode B in-place quarantine
│   ├── ipc/status.cpp             # AF_UNIX status socket (server + client)
│   ├── watch/watcher_*.cpp        # FSEvents (macOS) / inotify (Linux)
│   ├── render/                    # sandbox, pdf, image, markdown renderers (see Status)
│   └── cli/commands.cpp           # subcommand dispatch
├── third_party/                   # vendored single-header libs (see SOURCES.md)
├── mcp/server.py                  # JSON-RPC MCP server (stdlib only)
├── dist/                          # service files, Claude hook, example config
└── tests/                         # ctest binaries — naming, hash, sidecar, config,
                                    #   safe_fs, e2e_fetch, watcher
```

### popyd status

M2 is shipped: Mode A + Mode B both work, MCP server ships, Claude hook ships. Core tests (`ctest`) cover naming, hashing, sidecar, config, safe_fs, e2e fetch, and the watcher.

M3 (sandboxed `popy read --mode png|text` for PDF/image, via libmupdf + stb_image isolated behind `fork()` + `setrlimit` + a per-platform sandbox) is **implemented but partial**, honestly:

- **macOS**: builds by default when `libmupdf` is installed (`brew install mupdf`). Uses `sandbox_init` (Seatbelt) + `setrlimit`. The current Seatbelt profile is deliberately conservative but not yet a tight allow-list — treat it as defense-in-depth, not a hard boundary.
- **Linux**: the seccomp allow-list code exists in `render/sandbox.cpp`, but `popy-render` is **currently excluded from the Linux build** in `CMakeLists.txt` pending CI verification of the seccomp policy against libmupdf's actual syscall footprint. `popy read --mode png` returns `unsupported_type` on Linux today; `--mode raw|text` for non-PDF files works everywhere.
- **Other platforms**: no sandbox is applied if `popy-render` is built outside macOS/Linux-with-seccomp — don't rely on it there.

### License

popyd is Apache 2.0 — same as the parent project.

---

<p align="center"><em>Trust files less. Trust yourself more.</em></p>
