// Small typed wrapper over chrome.storage.local for preferences.
//
// Forward-compatible: if a future build adds a field, missing keys fall back
// to DEFAULT_PREFS. If a user is upgrading from a pre-rename build that
// stored `allowlistHosts`, migrate transparently on first read.

import { DEFAULT_PREFS, type Preferences } from "@lib/types";

const PREFS_KEY = "prefs";

interface LegacyPreferences {
  allowlistHosts?: string[];
}

function migrate(raw: unknown): Partial<Preferences> {
  if (!raw || typeof raw !== "object") return {};
  const r = raw as Partial<Preferences> & LegacyPreferences;
  // Copy legacy `allowlistHosts` into the new `allowlistPatterns` slot once.
  if (
    (!r.allowlistPatterns || r.allowlistPatterns.length === 0) &&
    Array.isArray(r.allowlistHosts) &&
    r.allowlistHosts.length > 0
  ) {
    r.allowlistPatterns = [...r.allowlistHosts];
  }
  delete (r as LegacyPreferences).allowlistHosts;
  return r;
}

export async function getPrefs(): Promise<Preferences> {
  const { [PREFS_KEY]: p } = await chrome.storage.local.get(PREFS_KEY);
  return { ...DEFAULT_PREFS, ...migrate(p) };
}

export async function setPrefs(patch: Partial<Preferences>): Promise<Preferences> {
  const cur = await getPrefs();
  const next = { ...cur, ...patch };
  await chrome.storage.local.set({ [PREFS_KEY]: next });
  return next;
}

export function onPrefsChanged(cb: (p: Preferences) => void): () => void {
  const listener = (
    changes: { [k: string]: chrome.storage.StorageChange },
    area: chrome.storage.AreaName,
  ) => {
    if (area !== "local" || !changes[PREFS_KEY]) return;
    const newValue = changes[PREFS_KEY]?.newValue;
    if (newValue) cb({ ...DEFAULT_PREFS, ...migrate(newValue) });
  };
  chrome.storage.onChanged.addListener(listener);
  return () => chrome.storage.onChanged.removeListener(listener);
}
