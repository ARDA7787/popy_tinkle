// Vitest setup — runs before every test file.
//
//   1. Replaces global indexedDB with fake-indexeddb so store.ts works in Node.
//   2. Installs a tiny Chromium-API double for the bits prefs.ts touches
//      (chrome.storage.local). Tests that need richer chrome surface should
//      replace these on `globalThis.chrome` themselves.

import "fake-indexeddb/auto";
import { vi } from "vitest";

interface StorageArea {
  get: (key: string | string[] | null) => Promise<Record<string, unknown>>;
  set: (items: Record<string, unknown>) => Promise<void>;
  remove: (key: string) => Promise<void>;
  clear: () => Promise<void>;
}

function makeArea(): StorageArea {
  let data: Record<string, unknown> = {};
  return {
    async get(key) {
      if (key === null || typeof key === "undefined") return { ...data };
      const keys = Array.isArray(key) ? key : [key];
      const out: Record<string, unknown> = {};
      for (const k of keys) if (k in data) out[k] = data[k];
      return out;
    },
    async set(items) {
      data = { ...data, ...items };
    },
    async remove(key) {
      delete data[key];
    },
    async clear() {
      data = {};
    },
  };
}

const local = makeArea();
const session = makeArea();

(globalThis as unknown as { chrome: unknown }).chrome = {
  storage: {
    local,
    session,
    onChanged: {
      addListener: vi.fn(),
      removeListener: vi.fn(),
    },
  },
  runtime: {
    id: "popy-test",
    getURL: (p: string) => `chrome-extension://popy-test/${p}`,
    getManifest: () => ({ version: "0.0.0-test" }),
    sendMessage: vi.fn(async () => undefined),
    onMessage: { addListener: vi.fn() },
    getContexts: vi.fn(async () => []),
  },
  alarms: {
    get: (_n: string, cb: (a: unknown) => void) => cb(undefined),
    create: vi.fn(),
    onAlarm: { addListener: vi.fn() },
  },
};
