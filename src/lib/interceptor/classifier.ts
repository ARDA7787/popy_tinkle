// Classifier: given a DownloadItem (and context from webRequest observation),
// decide which path it should take.
//
// Three outcomes:
//   "opfs"     — cancel native, re-fetch into OPFS
//   "fallback" — let native proceed but rename with _popy suffix into a quarantine subfolder
//   "allow"    — user allowlisted this origin, let native proceed untouched
//
// The classifier is the single place where we encode our knowledge of
// which URLs we CAN'T re-fetch safely. Being conservative here is free —
// fallback is nearly as safe as OPFS and always works.

import type { InterceptPath } from "@lib/types";
import { matchHost } from "./hostMatch";

export type Classification =
  | { kind: "opfs" }
  | { kind: "fallback"; reason: string }
  | { kind: "allow" };

export interface ClassifyInput {
  finalUrl: string;
  referrer: string;
  mime: string;
  method?: string;
  /** Allowlist patterns — exact "host.tld" or wildcard "*.host.tld". */
  allowlistPatterns: string[];
  sizeBytes: number;
  freeOpfsBytes: number;
  largeSizeThreshold: number;
}

export function classify(i: ClassifyInput): Classification {
  let host = "";
  try {
    host = new URL(i.finalUrl).host;
  } catch {
    // Not a parseable URL — be safe, fall back.
    return { kind: "fallback", reason: "unparseable URL" };
  }

  if (host && matchHost(host, i.allowlistPatterns)) {
    return { kind: "allow" };
  }

  // Blob URLs: the bytes exist only in the originating page's memory.
  // We cannot re-fetch them from an offscreen document.
  if (i.finalUrl.startsWith("blob:")) {
    return { kind: "fallback", reason: "blob: URL — bytes live in the originating page" };
  }

  // data: URLs are fine — re-decode in offscreen.
  // (No special case needed; fetch() handles them.)

  // Google Drive's >40MB confirmation interstitial requires a session cookie
  // that our SW jar doesn't have.
  if (
    host === "drive.usercontent.google.com" &&
    /[?&]confirm=/i.test(i.finalUrl) === false &&
    /\/download\b/.test(i.finalUrl)
  ) {
    return { kind: "fallback", reason: "Google Drive confirmation flow" };
  }

  // Non-GET methods. webRequest observation provides this.
  if (i.method && i.method !== "GET") {
    return {
      kind: "fallback",
      reason: `cannot replay ${i.method} without the original body`,
    };
  }

  // Size doesn't fit in remaining OPFS quota.
  if (i.sizeBytes > 0 && i.freeOpfsBytes < i.sizeBytes * 1.1) {
    return { kind: "fallback", reason: "insufficient OPFS quota" };
  }

  // Very large file — quarantine to disk instead of re-fetching to OPFS.
  // Multi-GB re-fetches are slow and user-hostile; fallback still protects
  // against execution.
  if (i.sizeBytes > i.largeSizeThreshold) {
    return {
      kind: "fallback",
      reason: `file larger than ${Math.round(i.largeSizeThreshold / 1e9)} GB threshold`,
    };
  }

  return { kind: "opfs" };
}

export function pathFromKind(k: Classification["kind"]): InterceptPath {
  return k === "opfs" ? "opfs" : "fallback";
}
