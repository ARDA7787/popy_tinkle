import { beforeEach, describe, expect, it } from "vitest";
import { getPrefs, setPrefs } from "@lib/metadata/prefs";
import { DEFAULT_PREFS } from "@lib/types";

beforeEach(async () => {
  await chrome.storage.local.clear();
});

describe("prefs", () => {
  it("returns defaults when nothing is stored", async () => {
    const p = await getPrefs();
    expect(p).toEqual(DEFAULT_PREFS);
    expect(p.autoExpireDays).toBe(14);
    expect(p.allowlistPatterns).toEqual([]);
  });

  it("round-trips a patch", async () => {
    await setPrefs({ notifications: false, allowlistPatterns: ["github.com"] });
    const p = await getPrefs();
    expect(p.notifications).toBe(false);
    expect(p.allowlistPatterns).toEqual(["github.com"]);
  });

  it("migrates legacy allowlistHosts to allowlistPatterns", async () => {
    // Simulate an older version of Popy that stored under the old key.
    await chrome.storage.local.set({
      prefs: {
        notifications: true,
        allowlistHosts: ["github.com", "example.org"],
      },
    });
    const p = await getPrefs();
    expect(p.allowlistPatterns).toEqual(["github.com", "example.org"]);
    // The legacy key is dropped from the migrated view.
    expect((p as unknown as { allowlistHosts?: unknown }).allowlistHosts).toBeUndefined();
  });

  it("does not clobber a populated allowlistPatterns when legacy is also set", async () => {
    await chrome.storage.local.set({
      prefs: {
        allowlistHosts: ["legacy.com"],
        allowlistPatterns: ["modern.com"],
      },
    });
    const p = await getPrefs();
    expect(p.allowlistPatterns).toEqual(["modern.com"]);
  });
});
