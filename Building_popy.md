# 📄 Building Popy: OPFS-Based Download Quarantine Extension

## Architecture and Implementation Guide

---

## Overview

Popy is a **Chromium-only MV3 browser extension** implementing a **pre-execution quarantine system** for downloaded files using **Origin Private File System (OPFS)**.

Key premise:

* `_popy` renaming = **cosmetic protection**
* **OPFS isolation = real security boundary**

⚠️ Critical constraint:

> Re-fetch pipelines fail on ~20–40% of downloads → fallback design is essential.

---

## 1. Threat Model

### ✅ Strongly Mitigated

* Passive OS-level file parsing exploits:

  * `.scf`, `.url`, `.lnk`, `.library-ms`
* NTLM credential leakage via file preview
* Explorer / Finder auto-indexing vulnerabilities
* Double-click execution:

  * `.exe`, `.msi`, `.scr`, `.hta`, `.vbs`, `.jar`, `.ps1`, `.bat`
* MOTW bypass via containers (ISO, VHD, etc.) — **only in OPFS path**

### ⚠️ Partially Mitigated

* PDF JavaScript → only safe if previewed in-browser (PDF.js)
* Office macros → user can still re-enable after rename
* macOS DMG auto-mount → mitigated only in OPFS path

### ❌ Not Mitigated

* Browser RCE (V8, WebGPU, WASM)
* Drive-by attacks (no download event)
* Existing files in Downloads folder
* Supply chain compromise
* User manually executing malware
* Social engineering (PowerShell copy-paste)

---

## 2. Why OPFS Matters

### `_popy` Rename

* Blocks double-click
* Prevents preview handler execution
* Easily reversible

### OPFS Isolation

* Files never touch OS filesystem
* Not visible to:

  * Explorer / Finder
  * Windows Defender (on-access)
  * Spotlight / indexing
* Cannot be moved out programmatically

👉 Core value:

> “Files never touch your real filesystem until you explicitly release them.”

---

## 3. Core Architecture (MV3 + OPFS)

### Event Flow

```
Download triggered
    ↓
onCreated
    ↓
onDeterminingFilename
    ↓
Cancel + erase
    ↓
Offscreen document fetch
    ↓
Stream → OPFS
    ↓
Metadata stored (IndexedDB)
```

---

### Interception Hook

Use:

```ts
chrome.downloads.onDeterminingFilename.addListener((item, suggest) => {
  void handleDetermining(item, suggest);
  return true;
});
```

NOT `onCreated`.

---

### Offscreen Document (Required)

Service workers:

* Die after ~30s idle
* Hard limit ~5 min

Solution:

```ts
chrome.offscreen.createDocument({
  url: "offscreen.html",
  reasons: ["WORKERS", "BLOBS"],
  justification: "Long-running download handling"
});
```

---

## 4. Streaming Fetch → OPFS

```ts
const resp = await fetch(url, {
  credentials: "include",
  referrer
});

await resp.body
  .pipeThrough(transformStream)
  .pipeTo(opfsWritable);
```

### Benefits

* Constant memory usage
* Backpressure-aware
* Handles multi-GB files

---

## 5. SHA-256 Hashing

Recommended:

```ts
import { createSHA256 } from "hash-wasm";
```

Why:

* Streaming support
* High performance
* No buffering

---

## 6. Metadata Design

### IndexedDB Schema

```ts
interface QuarantineRecord {
  id: string;
  opfsPath: string;
  originalFilename: string;
  sourceUrl: string;
  sizeBytes: number;
  mime: string;
  sha256: string;
  status: "fetching" | "stored" | "released" | "failed";
  createdAt: number;
}
```

---

### OPFS Layout

```
opfs-root/
  uuid/
    filename_popy
```

---

## 7. Hard Problems (Failure Modes)

### Categories

| Case                     | Issue             | Action          |
| ------------------------ | ----------------- | --------------- |
| Signed URLs              | Usually OK        | Retry           |
| Google Drive large files | HTML confirm page | Fallback        |
| POST downloads           | Cannot replay     | Fallback        |
| blob URLs                | Not accessible    | Fallback        |
| data URLs                | OK                | Handle directly |
| Auth headers             | Missing tokens    | Replay via DNR  |
| SameSite cookies         | Not sent          | Partial failure |

---

## 8. Layered Validator

Apply before writing:

1. URL check
2. HTTP status
3. Content-Type
4. Content-Length
5. Magic bytes
6. Final URL heuristics

---

## 9. Fallback Strategy

```ts
suggest({
  filename: `popy-quarantine/${uuid}_${name}_popy`
});
```

### Guarantees

* MOTW preserved
* Execution blocked
* Preview handlers disabled

---

## 10. Hybrid Pipeline (Correct Strategy)

1. Detect download
2. If risky → fallback immediately
3. Else attempt re-fetch
4. Validate
5. Success → OPFS
6. Failure → fallback

---

## 11. Permissions (Manifest)

```json
{
  "permissions": [
    "downloads",
    "downloads.ui",
    "offscreen",
    "storage",
    "notifications",
    "declarativeNetRequest",
    "webRequest"
  ],
  "host_permissions": ["<all_urls>"]
}
```

---

## 12. OPFS Quota

* ~60% of disk per origin
* Incognito: ~300MB

### Check:

```ts
const { usage, quota } = await navigator.storage.estimate();
```

---

## 13. Release Flow

```ts
const handle = await showSaveFilePicker();
await file.stream().pipeTo(handle.createWritable());
```

Constraints:

* Must be user gesture
* Cannot move file directly

---

## 14. In-Browser Preview

### Supported

* PDFs → PDF.js
* Images → `<img>`
* Text → `<pre>`

### Not Supported

* Office files
* Executables
* Archives

---

## 15. UX Design

* Badge count
* Notifications
* Dashboard
* Keyboard shortcut
* Onboarding

---

## 16. Tech Stack

### Recommended: **WXT**

Advantages:

* MV3-native
* Fast builds (Vite)
* Multi-context support

---

## 17. Repo Structure

```
entrypoints/
  background.ts
  offscreen.ts
  popup/
  dashboard/

src/
  lib/
    opfs/
    interceptor/
    validator/
    metadata/
```

---

## 18. Testing

### Unit

* Vitest

### E2E

* Playwright
* Persistent context with extension

---

## 19. Chrome Web Store Requirements

* Justify permissions
* Privacy policy mandatory
* No remote code
* Clear single-purpose statement

---

## 20. Roadmap

### v0.1

* Core interception
* OPFS storage
* Basic UI
* Fallback system

### v0.2

* Header replay
* VirusTotal
* Preview

### v1.0

* Bulk actions
* UX polish

### v2.0

* Native integration
* Dangerzone
* Enterprise features

---

## 21. Key Insight

> The product’s success depends **not on OPFS**, but on handling the **40% failure edge cases correctly**.

---

## 22. Conclusion

Popy works because:

* OPFS creates a **true isolation boundary**
* Browser APIs now support streaming + sandboxing
* Hybrid architecture ensures reliability

Biggest differentiator:

> Publishing a **transparent threat model**