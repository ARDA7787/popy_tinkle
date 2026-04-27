// Release flow: move a file out of OPFS (or off the quarantine disk folder)
// into a user-chosen location, stripping the _popy suffix.
//
// CRITICAL ordering: showSaveFilePicker() must be called from a user gesture
// and BEFORE the first await that crosses a microtask boundary. We lean into
// this by asking for the picker first and doing all other work (fetching
// the OPFS file, hashing, cleanup) after the handle is returned.

import { getFile, removeQuarantineEntry } from "@lib/opfs/fs";
import { POPY_SUFFIX, type QuarantineRecord } from "@lib/types";
import { msg } from "@lib/ui";

// Widen the window type for the File System Access API — TS DOM lib
// still marks these as experimental in some versions.
declare global {
  interface Window {
    showSaveFilePicker?: (opts?: SaveFilePickerOpts) => Promise<FileSystemFileHandle>;
  }
  interface SaveFilePickerOpts {
    suggestedName?: string;
    types?: Array<{ description?: string; accept: Record<string, string[]> }>;
  }
}

export async function releaseRecord(r: QuarantineRecord): Promise<void> {
  if (r.path === "fallback") {
    // File is already on disk under Downloads/popy-quarantine/<uuid>_name_popy.
    // Chrome doesn't expose a rename API, but chrome.downloads.show() opens
    // Explorer/Finder on the file so the user can rename it manually. Offer
    // that as the path forward; also copy it via fetch+blob+saveAs if they'd
    // prefer a clean rename.
    await releaseFallback(r);
    return;
  }
  await releaseOpfs(r);
}

async function releaseOpfs(r: QuarantineRecord): Promise<void> {
  if (!window.showSaveFilePicker) {
    throw new Error("File System Access API is not available in this browser.");
  }

  const suggestedName = stripPopySuffix(r.originalFilename);

  // Picker FIRST — consumes the user gesture.
  const handle = await window.showSaveFilePicker({
    suggestedName,
    types: r.mime
      ? [
          {
            description: r.mime,
            accept: { [r.mime]: extForMime(r.mime, suggestedName) },
          },
        ]
      : [],
  });

  // Stream bytes from OPFS through the picker handle. Zero buffering.
  const writable = await handle.createWritable();
  const file = await getFile(r.opfsPath);
  try {
    await file.stream().pipeTo(writable);
  } catch (e) {
    try {
      await writable.abort(String(e));
    } catch {
      /* already aborted */
    }
    throw e;
  }

  // Clean up the OPFS side. The record goes to status=released for history.
  await removeQuarantineEntry(r.id);
  await notifyBackgroundReleased(r.id);
}

async function releaseFallback(r: QuarantineRecord): Promise<void> {
  // For fallback files, the best UX is: open the file's folder in the OS,
  // where the user can rename-remove the _popy suffix themselves. This
  // keeps the extension from needing broader filesystem permissions.
  //
  // We search on the leaf of diskPath only — Chrome's `filenameRegex`
  // matches against the absolute on-disk path, which on Windows uses
  // backslashes. The leaf is the same on every OS and uniquely ours.
  const source = r.diskPath ?? r.originalFilename;
  const leaf = source.split("/").pop() ?? source;
  const items = await chrome.downloads.search({
    filenameRegex: escapeRegex(leaf),
  });
  const item = items[0];
  if (!item) {
    throw new Error("Quarantined file not found — it may have been moved or deleted.");
  }
  chrome.downloads.show(item.id);

  // Mark as released (the user takes it from here).
  await notifyBackgroundReleased(r.id);
}

async function notifyBackgroundReleased(id: string): Promise<void> {
  // We don't have a dedicated message for this yet — reuse delete,
  // the background marks the record resolved and clears the badge.
  await msg({ type: "delete-file", payload: { id } });
}

function stripPopySuffix(name: string): string {
  return name.endsWith(POPY_SUFFIX) ? name.slice(0, -POPY_SUFFIX.length) : name;
}

function extForMime(mime: string, name: string): string[] {
  // Prefer the filename's own extension; fall back to a crude MIME table.
  const dot = name.lastIndexOf(".");
  if (dot > 0 && dot < name.length - 1) return [name.slice(dot)];
  const map: Record<string, string> = {
    "application/pdf": ".pdf",
    "image/png": ".png",
    "image/jpeg": ".jpg",
    "image/gif": ".gif",
    "application/zip": ".zip",
    "application/x-msdownload": ".exe",
    "application/vnd.microsoft.portable-executable": ".exe",
    "application/x-apple-diskimage": ".dmg",
    "application/octet-stream": ".bin",
  };
  return [map[mime.toLowerCase().split(";")[0] ?? ""] ?? ".bin"];
}

function escapeRegex(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
