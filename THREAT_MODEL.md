# Popy Threat Model

_This document states plainly what Popy does and does not defend against. If something in here is wrong, file an issue — accuracy matters more than marketing._

## The one-sentence summary

Popy moves the act of writing a downloaded file to your user-visible filesystem from "immediately and automatically" to "only when you explicitly consent." Everything else follows from that.

## What Popy actually does

Popy intercepts downloads in one of two ways and places the resulting file in one of two destinations, both of which prevent accidental execution by any operating-system component.

### The sandbox path (OPFS)

The file is streamed, via a fresh `fetch()` from the extension's offscreen document, directly into the **Origin Private File System** — a storage area that the browser owns and that no OS component can enumerate. The on-disk representation is controlled by the browser's storage subsystem, not by a recognizable filesystem layout. Concretely:

- **Windows Explorer**, Windows Search, Defender's on-access scanner, OneDrive sync, the thumbnail cache, and preview handlers do not see it.
- **macOS Finder**, Spotlight, QuickLook, and Gatekeeper's scan-on-download hooks do not see it.
- **Linux** file managers, `inotify`-based indexers, and `xdg-open` dispatch do not see it.

The file is also given a `_popy` suffix on its logical name in the sandbox. This suffix is the second line of defense: even in the pathological case where someone or something gained access to the sandbox's contents, no OS handler would recognize the extension and nothing would execute.

### The fallback path

Some downloads cannot be re-fetched from a neutral context. Blob URLs synthesized by the page's JavaScript. POST requests with non-idempotent request bodies. Google Drive confirmation flows for files larger than 40 MB. Downloads whose original response was assembled by a page service worker from IndexedDB. For all of these, a second `fetch()` from Popy's context would fail or return the wrong bytes.

Rather than give up and allow such files through unprotected, Popy takes the second-best option: it lets the native browser download proceed but **intercepts the suggested filename** via `chrome.downloads.onDeterminingFilename` and redirects it into a dedicated `Downloads/popy-quarantine/` subfolder with a `_popy` suffix. This means:

- The file exists on disk. Indexers and AV scanners will see it. Mark-of-the-Web and macOS `com.apple.quarantine` attributes remain in force — Popy does not disturb them.
- The `_popy` suffix defuses double-click execution across Windows, macOS, and Linux (no registered handler / no `UTI` match / no MIME-type dispatch).
- The `_popy` suffix defuses preview handlers and icon parsers that dispatch on the _last_ extension — which includes the entire FileFix / `.scf` / `.library-ms` / `.url` class of attacks.
- The fallback does NOT defuse: Spotlight content scanning, Windows Search indexing, or on-access AV scanners encountering a container format like ZIP.

Every file in Popy's dashboard is labelled with which path it took and why. You always know which protections are active for a given file.

## What Popy defuses

The following attack classes are meaningfully mitigated by the sandbox path. The fallback path mitigates the ones marked †.

| Attack class                                                             | Sandbox | Fallback | Notes                                                                                                     |
| ------------------------------------------------------------------------ | :-----: | :------: | --------------------------------------------------------------------------------------------------------- |
| Accidental double-click of `.exe`, `.msi`, `.scr`, `.bat`, `.hta`, `.vbs` |    ✓    |    ✓†    | Both paths yield an unrecognized-extension file. Double-click produces "Open with…" dialog.               |
| Explorer preview-handler exploits (e.g. SCF icon parsing, CVE-2025-21377) |    ✓    |    ✓†    | Preview handlers dispatch on the last extension.                                                          |
| macOS QuickLook parser exploits                                          |    ✓    |    ✓†    | LaunchServices UTI lookup fails on `_popy`.                                                               |
| Thumbnail-generator exploits (CVE-2017-8464 LNK, CVE-2024-21320 Themes)  |    ✓    |    ✓†    | Shell does not generate thumbnails for unknown extensions.                                                |
| PDF reader exploits (embedded JS execution)                              |    ✓    |    ✗     | Sandbox prevents the OS from ever dispatching to Adobe Reader. Fallback requires the user to open the file. |
| Office macro documents                                                   |    ✓    |    ✗     | Same reasoning as PDF.                                                                                    |
| Auto-mount DMG on macOS (Shlayer-class)                                  |    ✓    |    ✗     | Sandbox prevents `hdiutil` dispatch. Fallback only renames; a renamed `.dmg` is still a DMG.              |
| MOTW / Gatekeeper bypass via container formats (CVE-2022-44698, CVE-2023-24880) |    ✓    |    ✗     | Sandbox never triggers MOTW because no disk write happens.                                                |
| Unintended download from watering-hole sites (passive drive-by)          |    ✓    |    ✓†    | The bytes still arrive, but nothing is executed or parsed until you release.                              |

## What Popy does not defuse

Read this list carefully. These are real, important threats that Popy is the wrong tool for. Do not rely on it to defend against any of them.

1. **Browser-renderer RCE.** If an attacker pops the browser sandbox via a V8 bug, a WebGPU / ANGLE bug, a WebAssembly JIT bug, or similar, they never need to download anything. Popy is blind to this entire class. Keep your browser updated.

2. **Drive-by attacks that never touch the download API.** Some attacks install persistence, exfiltrate data, or pivot without triggering `chrome.downloads`. The WebUSB bugs, the WebBluetooth bugs, the generation of fake notifications, the cryptomining scripts. Popy sees none of these.

3. **Files already on your disk before Popy was installed.** The extension has no API to enumerate or retroactively quarantine `Downloads/`. If you want to audit what's there, use an existing file manager.

4. **User-initiated release of a malicious file.** Popy is a speed bump. It introduces a pause, a review step, and a chance to think. It cannot stop you from deciding to release `setup.exe` and run it. Threat-intelligence integrations (VirusTotal hash lookup, MalwareBazaar) are provided in v0.2+ to _help_ you decide, but the decision is yours.

5. **Social-engineering flows that bypass downloads entirely.** The ClickFix family of attacks tricks users into pasting PowerShell into their own Run dialog. No file is ever downloaded. Popy is powerless against this.

6. **Supply-chain compromise of Popy itself.** If the Chrome Web Store publisher account were compromised, or if a malicious dependency were pulled in, malicious code could reach your browser. The mitigations are standard: pinned dependencies, reproducible builds, small maintainer circle, and — for the paranoid — building from source after review.

7. **Quota exhaustion attacks.** A malicious site could try to fill Popy's OPFS quota by triggering many downloads. The classifier's large-file threshold and the `unlimitedStorage` permission mitigate this; the dashboard's sort-by-size makes cleanup easy. But this is not zero risk.

## Trust boundaries

- Between the **network** and **OPFS**: Popy's offscreen document. Runs our own code; we control the fetch and the validation.
- Between **OPFS** and **your filesystem**: The `showSaveFilePicker()` flow, gated on explicit user gesture. The only way a sandboxed file reaches disk.
- Between **Popy's extension context** and **the rest of your browser**: The Chrome extension isolation boundary. Popy's context cannot read other extensions' storage, other tabs' DOM, or your profile's cookies beyond what `host_permissions` exposes (which it uses only for re-fetching).

## Threat actors considered

- **Commodity malware distributor** (exploit kits, malvertising, typosquatted downloads). **Primary target.** Popy's sandbox path breaks most of their playbook.
- **Targeted but non-state-level attacker** (phishing with attachment, drive-by from compromised site). **Strong mitigation.** The pause-to-review model thwarts the time-pressure element of most attacks.
- **State-level actor with browser 0-day.** Popy offers **no meaningful defense.** Use hardware sandboxing (Qubes, application-layer VMs, hardened browsers).
- **Malicious insider / local attacker with file-system access.** **No defense.** Popy runs inside a browser; anyone with your shell has more authority than Popy does.

## Data that Popy collects

None. Popy has no network endpoints of its own. It does not phone home. The codebase contains zero calls to any server not triggered by an explicit user action (the optional VirusTotal lookup, which sends only a hash, and only when you click the button).

Popy stores the following locally:

- Preferences in `chrome.storage.local` (the JSON object defined in `src/lib/types/index.ts`).
- Per-file quarantine metadata in IndexedDB (source URL, referrer, timestamp, size, MIME, SHA-256, status).
- Short-lived in-memory headers observed by `webRequest` for the classifier (30-second TTL, 512-entry cap).

None of this is accessible to any other extension, to any website, or to Anthropic, Google, or the Popy maintainers.

## How to verify

- Read the source. It is a few thousand lines of TypeScript. Start at `src/background/background.ts` and `src/offscreen/offscreen.ts`.
- Build it yourself and diff the resulting `dist/` against the Chrome Web Store build (reproducible-builds support is on the roadmap).
- Monitor outgoing traffic with `chrome://net-export` while Popy is active. The only extension-originated requests you should see are re-fetches of URLs you initiated.

## Reporting issues

See [SECURITY.md](SECURITY.md) for the private-disclosure process. For non-security bugs, open a GitHub issue.
