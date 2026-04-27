// OPFS helpers.
//
// Layout:
//   <opfs-root>/
//     <uuid>/<sanitized-original-name>_popy
//
// A per-file UUID subdirectory means filename collisions are
// impossible across concurrent downloads from different origins, and
// cleanup after release/delete is a single `removeEntry(uuid, { recursive })`.

import { POPY_SUFFIX } from "@lib/types";

const ILLEGAL_RE = /[<>:"/\\|?*\x00-\x1f]/g;

export function sanitizeFilename(name: string): string {
  // Strip path components, collapse whitespace, remove illegal chars.
  const base = name.split(/[\\/]/).pop() || "download";
  const cleaned = base.replace(ILLEGAL_RE, "_").replace(/\s+/g, " ").trim();
  // Windows reserved names
  if (/^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)/i.test(cleaned)) {
    return `_${cleaned}`;
  }
  // Cap length — 230 leaves room for the suffix plus UUID path.
  return cleaned.slice(0, 230) || "download";
}

export function popyNameFor(originalFilename: string): string {
  return sanitizeFilename(originalFilename) + POPY_SUFFIX;
}

export function opfsPathFor(id: string, originalFilename: string): string {
  return `${id}/${popyNameFor(originalFilename)}`;
}

async function root(): Promise<FileSystemDirectoryHandle> {
  return navigator.storage.getDirectory();
}

export async function ensureDir(id: string): Promise<FileSystemDirectoryHandle> {
  const r = await root();
  return r.getDirectoryHandle(id, { create: true });
}

export async function getFile(opfsPath: string): Promise<File> {
  const [dir, file] = opfsPath.split("/");
  if (!dir || !file) throw new Error("Invalid OPFS path: " + opfsPath);
  const r = await root();
  const d = await r.getDirectoryHandle(dir);
  const fh = await d.getFileHandle(file);
  return fh.getFile();
}

/** Returns a Writable that commits atomically on close() and aborts cleanly on abort(). */
export async function openWritable(
  id: string,
  originalFilename: string,
): Promise<FileSystemWritableFileStream> {
  const d = await ensureDir(id);
  const fh = await d.getFileHandle(popyNameFor(originalFilename), { create: true });
  return fh.createWritable();
}

export async function removeQuarantineEntry(id: string): Promise<void> {
  const r = await root();
  try {
    await r.removeEntry(id, { recursive: true });
  } catch (e) {
    // NotFoundError is fine — we may be cleaning up a failed fetch.
    if ((e as DOMException).name !== "NotFoundError") throw e;
  }
}

/** Request persistent storage so Chromium doesn't evict OPFS under pressure. */
export async function requestPersistence(): Promise<boolean> {
  if (navigator.storage && "persist" in navigator.storage) {
    return navigator.storage.persist();
  }
  return false;
}

/** Bytes free in the origin's quota. */
export async function freeBytes(): Promise<number> {
  if (!navigator.storage?.estimate) return Number.POSITIVE_INFINITY;
  const { quota = 0, usage = 0 } = await navigator.storage.estimate();
  return Math.max(0, quota - usage);
}
