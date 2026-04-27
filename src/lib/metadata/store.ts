// Metadata store.
// IndexedDB is preferred over sidecar files in OPFS because
//   (a) we get real indexes, and
//   (b) one transaction is cheaper than two filesystem writes.

import { openDB, type DBSchema, type IDBPDatabase } from "idb";
import type { QuarantineRecord } from "@lib/types";

interface PopySchema extends DBSchema {
  records: {
    key: string;
    value: QuarantineRecord;
    indexes: {
      "by-createdAt": number;
      "by-status": string;
      "by-sha256": string;
      "by-host": string;
    };
  };
}

const DB_NAME = "popy";
const DB_VERSION = 1;

let _db: IDBPDatabase<PopySchema> | null = null;

async function db(): Promise<IDBPDatabase<PopySchema>> {
  if (_db) return _db;
  _db = await openDB<PopySchema>(DB_NAME, DB_VERSION, {
    upgrade(db) {
      const s = db.createObjectStore("records", { keyPath: "id" });
      s.createIndex("by-createdAt", "createdAt");
      s.createIndex("by-status", "status");
      s.createIndex("by-sha256", "sha256");
      s.createIndex("by-host", "sourceHost");
    },
  });
  return _db;
}

export async function putRecord(r: QuarantineRecord): Promise<void> {
  const d = await db();
  await d.put("records", r);
}

export async function updateRecord(
  id: string,
  patch: Partial<QuarantineRecord>,
): Promise<QuarantineRecord | null> {
  const d = await db();
  const tx = d.transaction("records", "readwrite");
  const cur = await tx.store.get(id);
  if (!cur) {
    await tx.done;
    return null;
  }
  const next = { ...cur, ...patch };
  await tx.store.put(next);
  await tx.done;
  return next;
}

export async function getRecord(id: string): Promise<QuarantineRecord | null> {
  const d = await db();
  return (await d.get("records", id)) ?? null;
}

export async function deleteRecord(id: string): Promise<void> {
  const d = await db();
  await d.delete("records", id);
}

export async function listRecords(opts?: {
  limit?: number;
  status?: QuarantineRecord["status"];
}): Promise<QuarantineRecord[]> {
  const d = await db();
  const tx = d.transaction("records", "readonly");
  const idx = tx.store.index("by-createdAt");
  // newest first
  const out: QuarantineRecord[] = [];
  let cursor = await idx.openCursor(null, "prev");
  while (cursor) {
    if (!opts?.status || cursor.value.status === opts.status) {
      out.push(cursor.value);
      if (opts?.limit && out.length >= opts.limit) break;
    }
    cursor = await cursor.continue();
  }
  return out;
}

export async function countByStatus(
  status: QuarantineRecord["status"],
): Promise<number> {
  const d = await db();
  const idx = d.transaction("records").store.index("by-status");
  return idx.count(status);
}

export async function findBySha256(hash: string): Promise<QuarantineRecord[]> {
  const d = await db();
  return d.getAllFromIndex("records", "by-sha256", hash);
}

/**
 * Return all records with the given status whose `createdAt` is older than
 * `olderThanMs`. Used by the auto-expire sweep and the history-pruner.
 */
export async function listOlderThan(
  status: QuarantineRecord["status"],
  olderThanMs: number,
): Promise<QuarantineRecord[]> {
  const d = await db();
  const cutoff = Date.now() - olderThanMs;
  const tx = d.transaction("records", "readonly");
  const idx = tx.store.index("by-status");
  const out: QuarantineRecord[] = [];
  let cursor = await idx.openCursor(IDBKeyRange.only(status));
  while (cursor) {
    if (cursor.value.createdAt < cutoff) out.push(cursor.value);
    cursor = await cursor.continue();
  }
  return out;
}
