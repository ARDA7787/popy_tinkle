// A tiny ring-buffered cache of recent outgoing requests (method + headers
// + tabId), keyed by URL.
//
// We use this for exactly two things:
//
//   1. Detecting non-GET downloads EARLIER than onDeterminingFilename
//      tells us, so we can route to fallback instead of trying to replay.
//
//   2. Replaying `Authorization` and other custom headers on the
//      extension's re-fetch via declarativeNetRequest session rules.
//
// Entries live for 30 seconds. That's long enough to cover the
// server-side processing for the heaviest downloads and short enough
// that we never confuse two unrelated requests to the same URL.

interface ObservedRequest {
  method: string;
  headers: Record<string, string>;
  tabId: number;
  at: number;
}

const TTL_MS = 30_000;
const MAX_ENTRIES = 512;

const cache = new Map<string, ObservedRequest>();

function prune() {
  const now = Date.now();
  // Drop expired
  for (const [k, v] of cache) {
    if (now - v.at > TTL_MS) cache.delete(k);
  }
  // Drop oldest if still over cap
  if (cache.size > MAX_ENTRIES) {
    const sorted = [...cache.entries()].sort((a, b) => a[1].at - b[1].at);
    const excess = cache.size - MAX_ENTRIES;
    for (let i = 0; i < excess; i++) cache.delete(sorted[i]![0]);
  }
}

/** Normalize the URL — strip hashes and trailing `&` — so we don't miss matches. */
function key(url: string): string {
  const i = url.indexOf("#");
  return (i >= 0 ? url.slice(0, i) : url).replace(/&+$/, "");
}

export function record(url: string, req: Omit<ObservedRequest, "at">): void {
  cache.set(key(url), { ...req, at: Date.now() });
  if (cache.size % 64 === 0) prune();
}

export function lookup(url: string): ObservedRequest | null {
  prune();
  return cache.get(key(url)) ?? null;
}

export function clear(): void {
  cache.clear();
}

/** Register webRequest observers. Call once from the background script. */
export function install(): void {
  chrome.webRequest.onBeforeRequest.addListener(
    (d) => {
      if (d.tabId < 0) return; // service-worker / extension / navigation
      if (d.method === "GET" && !d.requestBody) return; // most common, skip noise
      record(d.url, {
        method: d.method,
        headers: {}, // filled by onBeforeSendHeaders
        tabId: d.tabId,
      });
    },
    { urls: ["<all_urls>"] },
    [],
  );

  chrome.webRequest.onBeforeSendHeaders.addListener(
    (d) => {
      if (d.tabId < 0) return;
      const headers: Record<string, string> = {};
      for (const h of d.requestHeaders ?? []) {
        if (!h.name || !h.value) continue;
        const n = h.name.toLowerCase();
        // Capture headers a re-fetch might need. Cookies are handled by
        // credentials:'include'; DON'T carry them in the replay map.
        if (["authorization", "x-csrf-token", "x-requested-with"].includes(n)) {
          headers[h.name] = h.value;
        }
      }
      const existing = lookup(d.url);
      record(d.url, {
        method: d.method,
        headers,
        tabId: existing?.tabId ?? d.tabId,
      });
    },
    { urls: ["<all_urls>"] },
    ["requestHeaders", "extraHeaders"],
  );
}
