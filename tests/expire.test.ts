import { beforeEach, describe, expect, it, vi } from "vitest";

// Stub out the OPFS module so the expire sweep doesn't try to call
// `navigator.storage.getDirectory()` in Node.
vi.mock("@lib/opfs/fs", () => ({
  removeQuarantineEntry: vi.fn(async () => undefined),
}));

import { runSweep } from "@lib/metadata/expire";
import { listRecords, putRecord } from "@lib/metadata/store";
import { setPrefs } from "@lib/metadata/prefs";
import type { QuarantineRecord } from "@lib/types";

const DAY = 24 * 60 * 60 * 1000;

function rec(over: Partial<QuarantineRecord>): QuarantineRecord {
  return {
    id: crypto.randomUUID(),
    opfsPath: "x/y_popy",
    originalFilename: "y",
    sourceUrl: "https://example.com/y",
    sourceHost: "example.com",
    sizeBytes: 100,
    mime: "application/octet-stream",
    sha256: "deadbeef",
    path: "opfs",
    status: "stored",
    createdAt: Date.now(),
    ...over,
  };
}

// Tests use fresh UUIDs per run so they don't depend on IDB being
// cleared between tests; assertions are scoped to the records this test
// created.
beforeEach(async () => {
  await chrome.storage.local.clear();
});

describe("runSweep", () => {
  it("expires stored records older than autoExpireDays", async () => {
    await setPrefs({ autoExpireDays: 14 });
    const fresh = rec({ createdAt: Date.now() - 1 * DAY });
    // 20 days: older than the 14-day expire threshold but younger than
    // the 30-day history-prune threshold so we can still observe the
    // marked-deleted record after the sweep.
    const stale = rec({ createdAt: Date.now() - 20 * DAY });
    await putRecord(fresh);
    await putRecord(stale);

    await runSweep();

    const after = await listRecords();
    const ids = new Map(after.map((r) => [r.id, r]));
    expect(ids.get(fresh.id)?.status).toBe("stored");
    expect(ids.get(stale.id)?.status).toBe("deleted");
  });

  it("does nothing when autoExpireDays is 0", async () => {
    await setPrefs({ autoExpireDays: 0 });
    // 100 days old, but we want it to STAY status=stored (not pruned by
    // history sweep, which only touches terminal statuses). Stored never
    // gets auto-pruned, so this is safe.
    const stale = rec({ createdAt: Date.now() - 100 * DAY });
    await putRecord(stale);

    await runSweep();

    const after = await listRecords();
    expect(after.find((r) => r.id === stale.id)?.status).toBe("stored");
  });

  it("prunes ancient terminal records (released/deleted/failed)", async () => {
    await setPrefs({ autoExpireDays: 0 });
    const ancient = rec({
      status: "released",
      createdAt: Date.now() - 60 * DAY,
    });
    const recent = rec({
      status: "released",
      createdAt: Date.now() - 1 * DAY,
    });
    await putRecord(ancient);
    await putRecord(recent);

    await runSweep();

    const after = await listRecords();
    expect(after.find((r) => r.id === ancient.id)).toBeUndefined();
    expect(after.find((r) => r.id === recent.id)).toBeDefined();
  });
});
