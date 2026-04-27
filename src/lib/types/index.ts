// Shared types for Popy.
// Everything else in the codebase imports from here.

export type QuarantineStatus =
  | "fetching" // SW dispatched the download, offscreen is streaming it
  | "stored" // safely in OPFS, awaiting user action
  | "fallback" // could not re-fetch, saved with _popy suffix in Downloads/popy-quarantine
  | "released" // user released the file to disk, removed from OPFS
  | "deleted" // user deleted from OPFS
  | "failed"; // fetch failed; no bytes stored

export type InterceptPath = "opfs" | "fallback";

export interface QuarantineRecord {
  /** UUID v4 — primary key. Also used as the OPFS subdirectory name. */
  id: string;
  /** Path within OPFS root — "<id>/<original>_popy". Empty for fallback. */
  opfsPath: string;
  /** Path written to disk for the fallback path — relative to Downloads. */
  diskPath?: string;
  /** Original filename as suggested by server (before _popy suffix). */
  originalFilename: string;
  /** Final URL the server served. */
  sourceUrl: string;
  /** Host portion of sourceUrl, denormalized for indexing. */
  sourceHost: string;
  /** Referrer at the time of download. */
  referrer?: string;
  /** URL of the tab that initiated the download. */
  tabUrl?: string;
  /** Size in bytes. 0 if unknown until fetch completes. */
  sizeBytes: number;
  /** Server-reported MIME type. */
  mime: string;
  /** Hex SHA-256 of the file contents. Empty until fetch completes. */
  sha256: string;
  /** Whether this went through OPFS or fell back to disk rename. */
  path: InterceptPath;
  /** Current status. */
  status: QuarantineStatus;
  /** Unix ms of creation. */
  createdAt: number;
  /** Unix ms of release/delete. */
  resolvedAt?: number;
  /** Human-readable reason for fallback or failure. */
  note?: string;
}

// Messages between contexts. All messages are typed; no free-form blobs.
export type Message =
  | {
      type: "fetch-to-opfs";
      payload: {
        id: string;
        url: string;
        referrer?: string;
        originalFilename: string;
        totalBytes: number;
        mime: string;
      };
    }
  | { type: "fetch-progress"; payload: { id: string; received: number; total: number } }
  | {
      type: "fetch-complete";
      payload: { id: string; sha256: string; bytes: number };
    }
  | { type: "fetch-failed"; payload: { id: string; reason: string } }
  | { type: "release-file"; payload: { id: string } }
  | { type: "delete-file"; payload: { id: string } }
  | { type: "list-records" }
  | { type: "records"; payload: QuarantineRecord[] }
  | { type: "get-record"; payload: { id: string } }
  | { type: "record"; payload: QuarantineRecord | null }
  | { type: "preview-blob-url"; payload: { id: string } }
  | { type: "blob-url"; payload: { url: string | null } };

export interface Preferences {
  /** Show desktop notifications on each quarantine. */
  notifications: boolean;
  /** Maximum files to show in popup. */
  popupLimit: number;
  /** Auto-expire OPFS files older than this many days. 0 = never. */
  autoExpireDays: number;
  /**
   * Per-origin allowlist patterns. Two forms:
   *   "github.com"    → exact host match only
   *   "*.github.com"  → matches github.com AND any subdomain of it
   */
  allowlistPatterns: string[];
  /** Size above which we prompt user instead of auto-quarantining (bytes). */
  largeSizeThreshold: number;
  /** Has the user completed onboarding? */
  onboarded: boolean;
}

export const DEFAULT_PREFS: Preferences = {
  notifications: true,
  popupLimit: 8,
  autoExpireDays: 14,
  allowlistPatterns: [],
  largeSizeThreshold: 2 * 1024 * 1024 * 1024, // 2 GB
  onboarded: false,
};

export const POPY_SUFFIX = "_popy";
export const QUARANTINE_DIR = "popy-quarantine";
