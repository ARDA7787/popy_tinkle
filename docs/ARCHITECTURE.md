# Architecture

A one-page map of how Popy is wired. For what is and isn't mitigated, see `THREAT_MODEL.md`.

## Boundary

```
network ─→ [extension SW]
              │ classify
              │
        ┌─────┴─────┐
        ↓           ↓
     OPFS path    fallback path
        │            │
        ↓            ↓
   [offscreen]   [Downloads/popy-quarantine/<uuid>_<name>_popy]
        │
        ↓
     OPFS sandbox (invisible to OS)
```

## Components

- **`src/background/background.ts`** — service worker. Hooks `chrome.downloads.onDeterminingFilename`. For each download: classify → either suggest a `_popy` path (fallback) or cancel + dispatch to offscreen (OPFS).
- **`src/offscreen/offscreen.ts`** — long-running document. Re-fetches the download, validates headers + first 16 bytes (magic), pipes through a streaming SHA-256 hasher and into an OPFS writable. Single-pass, constant memory.
- **`src/lib/interceptor/`**
  - `webRequestCache.ts` — captures recent request methods/headers so the SW knows when a download is non-GET (must fallback).
  - `classifier.ts` — single decision point: `opfs` / `fallback` / `allow`.
  - `hostMatch.ts` — exact and `*.subdomain` host matching for the trusted-origins allowlist.
- **`src/lib/dnr/rules.ts`** — installs short-lived `declarativeNetRequest` session rules to rewrite `Referer`/`Origin` for the offscreen re-fetch only.
- **`src/lib/validator/validate.ts`** — pure functions: header validator (status, Content-Type family, Content-Length sanity, login-redirect heuristic) and magic-byte signature checks.
- **`src/lib/opfs/fs.ts`** — OPFS layout, sanitization, persistence, quota helpers.
- **`src/lib/metadata/`**
  - `store.ts` — IndexedDB wrapper for `QuarantineRecord`s.
  - `prefs.ts` — `chrome.storage.local` for `Preferences`, with one-way migration from older shapes.
  - `expire.ts` — `chrome.alarms`-driven sweep that removes auto-expired sandbox files and prunes ancient terminal records.
- **`src/popup/`** — toolbar popup. Recent quarantines, release/delete.
- **`src/dashboard/`** — full UI. Search, filter, drawer with details, in-browser preview, settings.
- **`src/onboarding/`** — first-run intro page.

## Data model

```ts
interface QuarantineRecord {
  id: string;                // UUID v4 — primary key + OPFS subdir name
  opfsPath: string;          // "<id>/<name>_popy" (empty for fallback)
  diskPath?: string;         // "popy-quarantine/<id-prefix>_<name>_popy"
  originalFilename: string;
  sourceUrl: string;
  sourceHost: string;
  referrer?: string;
  sizeBytes: number;
  mime: string;
  sha256: string;            // hex; empty until fetch completes
  path: "opfs" | "fallback"; // where the bytes live
  status: "fetching" | "stored" | "released" | "deleted" | "failed";
  createdAt: number;
  resolvedAt?: number;
  note?: string;             // human-readable reason for fallback / failure
}
```

`status` is the lifecycle. `path` is the storage location. The two are independent.

## Failure handling

| Scenario | Behaviour |
| --- | --- |
| Re-fetch returns HTML (auth interstitial) | header validator fails → record marked `failed`, OPFS dir cleaned. The user's original download was already cancelled, so they can re-issue it; classifier sees the new attempt and may route to fallback if it learns enough. |
| Re-fetch redirected to login URL | same as above (login-hint regex). |
| Magic bytes don't match expected MIME | `validateMagic` fails on the buffered first 16 bytes; writable aborted, record marked `failed`. |
| Offscreen doc not ready | `dispatchToOffscreen` retries up to 4 times re-creating the doc each time. After exhaustion the record is marked `failed`. |
| OPFS quota exceeded | classifier routes to fallback when `freeOpfsBytes < sizeBytes * 1.1`. |
| Service worker termination mid-stream | offscreen doc continues independently; sends `fetch-complete` on success which wakes the SW. |
| User cancels save-file picker | `AbortError` is silently swallowed; record stays `stored`. |
