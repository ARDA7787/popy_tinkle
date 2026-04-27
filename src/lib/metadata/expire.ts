// Auto-expire sweep.
//
// Two passes, both lazy:
//
//   1. "stored" records older than prefs.autoExpireDays → delete
//      (OPFS bytes + record). Disabled when autoExpireDays === 0.
//
//   2. Terminal records ("released" / "deleted" / "failed") older than
//      HISTORY_RETENTION_DAYS → prune the metadata row. Keeps IndexedDB
//      from growing unboundedly for users who never open the dashboard.
//
// Called on SW startup and from a `chrome.alarms` tick every 6h.

import { getPrefs } from "./prefs";
import { deleteRecord, listOlderThan, updateRecord } from "./store";
import { removeQuarantineEntry } from "@lib/opfs/fs";

const DAY_MS = 24 * 60 * 60 * 1000;
const HISTORY_RETENTION_DAYS = 30;

export interface SweepResult {
  expiredStored: number;
  prunedHistory: number;
}

export async function runSweep(): Promise<SweepResult> {
  const prefs = await getPrefs();
  let expiredStored = 0;
  let prunedHistory = 0;

  // Pass 1 — auto-expire quarantined files.
  if (prefs.autoExpireDays > 0) {
    const maxAge = prefs.autoExpireDays * DAY_MS;
    const stale = await listOlderThan("stored", maxAge);
    for (const r of stale) {
      try {
        if (r.path === "opfs") {
          await removeQuarantineEntry(r.id);
        }
        // For fallback, we do NOT touch the file on disk — the user may
        // have moved it. Just archive the record.
        await updateRecord(r.id, {
          status: "deleted",
          resolvedAt: Date.now(),
          note: `auto-expired after ${prefs.autoExpireDays} day(s)`,
        });
        expiredStored++;
      } catch (e) {
        console.warn("[popy] expire sweep: failed to expire", r.id, e);
      }
    }
  }

  // Pass 2 — prune ancient terminal records from IDB.
  const pruneMs = HISTORY_RETENTION_DAYS * DAY_MS;
  for (const status of ["released", "deleted", "failed"] as const) {
    const old = await listOlderThan(status, pruneMs);
    for (const r of old) {
      try {
        await deleteRecord(r.id);
        prunedHistory++;
      } catch (e) {
        console.warn("[popy] expire sweep: failed to prune", r.id, e);
      }
    }
  }

  return { expiredStored, prunedHistory };
}

/** Install the `chrome.alarms` trigger. Safe to call multiple times. */
export function installExpireAlarm(): void {
  const NAME = "popy-expire-sweep";
  chrome.alarms.get(NAME, (existing) => {
    if (!existing) {
      chrome.alarms.create(NAME, {
        delayInMinutes: 5, // first run shortly after startup
        periodInMinutes: 6 * 60, // every 6 hours
      });
    }
  });
}
