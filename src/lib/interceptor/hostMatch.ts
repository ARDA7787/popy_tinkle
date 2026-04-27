// Host-pattern matcher for the trusted-origins allowlist.
//
// Two pattern forms:
//   "github.com"    — exact host match
//   "*.github.com"  — matches github.com AND any subdomain (release.github.com,
//                     raw.githubusercontent.com would NOT match — different
//                     registered domain)
//
// Matching is case-insensitive and trims a leading "www." only when the
// pattern itself begins with "www." (so users can opt in or out).

/** Normalize a host — lowercase, strip trailing dot. */
function norm(host: string): string {
  return host.toLowerCase().replace(/\.+$/, "");
}

/** Validate a user-entered pattern. Returns the cleaned form or null. */
export function normalizePattern(raw: string): string | null {
  let p = raw.trim().toLowerCase();
  if (!p) return null;
  // Strip protocol + path if user pasted a URL.
  p = p.replace(/^https?:\/\//, "").replace(/\/.*$/, "");
  // Strip port.
  p = p.replace(/:\d+$/, "");
  if (!p) return null;

  // Validate. A pattern is either
  //   *.<labels>
  //   <labels>
  // where each label is [a-z0-9-]+ and labels are dot-separated.
  const body = p.startsWith("*.") ? p.slice(2) : p;
  if (!/^[a-z0-9-]+(\.[a-z0-9-]+)+$/.test(body)) return null;
  return p;
}

/** True if `host` matches any pattern in `patterns`. */
export function matchHost(host: string, patterns: Iterable<string>): boolean {
  const h = norm(host);
  for (const raw of patterns) {
    const p = norm(raw);
    if (p.startsWith("*.")) {
      const base = p.slice(2);
      if (h === base || h.endsWith("." + base)) return true;
    } else if (h === p) {
      return true;
    }
  }
  return false;
}
