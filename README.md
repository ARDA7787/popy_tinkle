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

The browser extension protects browser downloads. **[popyd](popyd/README.md)** brings the same `_popy` quarantine to the command line, designed for AI CLI agents (Claude Code, Cursor, Codex).

- **Guaranteed path (`popy fetch`)**: direct-stream fetch/write where the first on-disk name is `<name>.<ext>_popy` in `<stage>/<uuid>/`, mode `0000`.
- **Best-effort path (`popyd` watcher mode)**: monitor `~/.popy-stage/` (excluded from Spotlight/Tracker by the installer) and rename after create to `<name>_popy` mode `0000` within ~1s.

A bundled MCP server (`popyd/mcp/server.py`) exposes `popy_fetch`/`popy_list`/`popy_read_text`/`popy_release`/`popy_delete` to AI agents. A Claude Code Bash hook (`popyd/dist/popy-bash-hook.sh`) refuses direct `curl`/`wget` invocations and routes them through `popy_fetch` instead.

```bash
cmake -S popyd -B popyd/build -DCMAKE_BUILD_TYPE=Release
cmake --build popyd/build -j
cmake --install popyd/build --prefix ~/.local
```

Sidecar JSON mirrors the browser extension's `QuarantineRecord` field-for-field, so a future sync layer ingests them into the extension's IndexedDB with zero translation. Full design and threat model in [popyd/README.md](popyd/README.md). Plan and milestone breakdown in [plan.md](plan.md).

---

<p align="center"><em>Trust files less. Trust yourself more.</em></p>
