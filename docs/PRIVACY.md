# Popy Privacy Policy

_Last updated: 2025-04-25_

Popy is a privacy-respecting browser extension. The short version: **Popy does not transmit your data anywhere.**

## What Popy stores

Locally on your machine, inside the browser's per-extension storage:

- **Quarantined file bytes** — kept inside the browser's Origin Private File System (OPFS). Invisible to your operating system, your antivirus, and any other extension.
- **Metadata for each quarantined file** — filename, source URL, host, MIME type, size, SHA-256, timestamps. Stored in IndexedDB inside the extension's origin.
- **Your preferences** — notification toggle, auto-expire days, trusted-origins allowlist, large-file threshold. Stored in `chrome.storage.local`.

## What Popy does NOT do

- Popy does **not** make any network request to any server we control.
- Popy does **not** send file bytes, file hashes, URLs, or telemetry off your machine.
- Popy does **not** include analytics or tracking SDKs.
- Popy does **not** modify, read, or share data outside the contexts above.

## Optional outbound requests

Popy will only contact the network when **you explicitly click** to do so:

- _"Check on VirusTotal"_ — opens `virustotal.com/gui/file/<your-sha256>` in a new tab. The hash leaves your machine only when you click that button. Popy itself does not call VirusTotal's API.
- _"Read the threat model"_ — opens this repository's `THREAT_MODEL.md` on GitHub.

## Re-fetch traffic

To stream a download into the OPFS sandbox, Popy issues an HTTP request to **the same URL the original download targeted**. This request:

- Goes only to the host that served the original download.
- Carries your existing cookies for that host (`credentials: "include"`) — same as the original.
- Is never logged or forwarded.

## Removing your data

Removing the extension from `chrome://extensions` deletes:

- All OPFS bytes
- The IndexedDB metadata store
- Your preferences

There is no server-side state to delete.

## Contact

Bugs and concerns: open an issue on the repository. Security reports: see `SECURITY.md`.
