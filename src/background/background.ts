// Background service worker.
//
// THE ONE RULE: every listener we care about must be registered
// synchronously at the top level of this file, BEFORE the first
// `await`. MV3 SWs terminate after 30s idle and re-run this script
// on wake; any listener registered inside an async callback is lost
// across restarts.
//
// Responsibilities:
//   - listen for chrome.downloads.onDeterminingFilename
//   - classify each download (opfs vs fallback vs allow)
//   - for opfs: cancel native, erase, dispatch to offscreen doc
//   - for fallback: suggest() a _popy-suffixed filename in a quarantine folder
//   - maintain the webRequest header cache
//   - respond to popup/dashboard messages
//   - keep the badge count fresh

import { install as installWebRequestCache, lookup } from "@lib/interceptor/webRequestCache";
import { classify } from "@lib/interceptor/classifier";
import {
  countByStatus,
  getRecord,
  listRecords,
  putRecord,
  updateRecord,
} from "@lib/metadata/store";
import { getPrefs } from "@lib/metadata/prefs";
import {
  opfsPathFor,
  popyNameFor,
  sanitizeFilename,
  freeBytes,
  removeQuarantineEntry,
} from "@lib/opfs/fs";
import { installExpireAlarm, runSweep } from "@lib/metadata/expire";
import { QUARANTINE_DIR, type Message, type QuarantineRecord } from "@lib/types";

// ─────────────────────────────────────────────────────────────────────
// Top-level listener registration. Nothing async above this line.
// ─────────────────────────────────────────────────────────────────────

chrome.runtime.onInstalled.addListener(({ reason }) => {
  if (reason === "install") {
    chrome.tabs.create({ url: chrome.runtime.getURL("src/onboarding/onboarding.html") });
  }
  refreshBadge().catch(console.error);
  installExpireAlarm();
  void runSweep().catch((e) => console.warn("[popy] sweep", e));
});

chrome.runtime.onStartup.addListener(() => {
  refreshBadge().catch(console.error);
  installExpireAlarm();
  void runSweep().catch((e) => console.warn("[popy] sweep", e));
});

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === "popy-expire-sweep") {
    void runSweep().catch((e) => console.warn("[popy] sweep", e));
  }
});

installWebRequestCache();

chrome.downloads.onDeterminingFilename.addListener((item, suggest) => {
  // Returning true enables async suggest().
  handleDetermining(item, suggest).catch((e) => {
    console.error("[popy] onDeterminingFilename error", e);
    try {
      suggest(); // fall through to default — better than hanging the download
    } catch {
      /* suggest may already have been called */
    }
  });
  return true;
});

chrome.downloads.onChanged.addListener((delta) => {
  // Detect when a fallback download completes so we can mark it "stored"
  if (delta.state?.current === "complete") {
    void markFallbackStored(delta.id);
  }
});

chrome.runtime.onMessage.addListener((raw, _sender, sendResponse) => {
  handleMessage(raw as Message).then(sendResponse).catch((e) => {
    console.error("[popy] message error", e);
    sendResponse({ ok: false, error: String(e) });
  });
  return true;
});

chrome.commands.onCommand.addListener((cmd) => {
  if (cmd === "open-dashboard") {
    chrome.tabs.create({ url: chrome.runtime.getURL("src/dashboard/dashboard.html") });
  }
});

chrome.notifications.onButtonClicked.addListener((notifId, btnIdx) => {
  // notifId is the quarantine record id (we set it below).
  if (btnIdx === 0) {
    chrome.tabs.create({
      url: chrome.runtime.getURL(`src/dashboard/dashboard.html#${notifId}`),
    });
  } else if (btnIdx === 1) {
    void doDelete(notifId);
  }
  chrome.notifications.clear(notifId);
});

// Keep track of offscreen doc
let offscreenReady: Promise<void> | null = null;

async function ensureOffscreen(): Promise<void> {
  if (offscreenReady) return offscreenReady;
  offscreenReady = (async () => {
    const url = chrome.runtime.getURL("src/offscreen/offscreen.html");
    const ctx = await chrome.runtime.getContexts({
      contextTypes: ["OFFSCREEN_DOCUMENT" as chrome.runtime.ContextType],
      documentUrls: [url],
    });
    if (ctx.length > 0) return;
    await chrome.offscreen.createDocument({
      url: "src/offscreen/offscreen.html",
      reasons: [
        "WORKERS" as chrome.offscreen.Reason,
        "BLOBS" as chrome.offscreen.Reason,
      ],
      justification:
        "Long-running fetch + streaming SHA-256 + OPFS write that can exceed service worker lifetime.",
    });
  })();
  try {
    await offscreenReady;
  } catch (e) {
    offscreenReady = null;
    throw e;
  }
}

// ─────────────────────────────────────────────────────────────────────
// Interception
// ─────────────────────────────────────────────────────────────────────

async function handleDetermining(
  item: chrome.downloads.DownloadItem,
  suggest: (sug?: chrome.downloads.DownloadFilenameSuggestion) => void,
): Promise<void> {
  const prefs = await getPrefs();
  const observed = lookup(item.finalUrl || item.url);
  const free = await freeBytes();

  const decision = classify({
    finalUrl: item.finalUrl || item.url,
    referrer: item.referrer || "",
    mime: item.mime || "application/octet-stream",
    method: observed?.method,
    allowlistPatterns: prefs.allowlistPatterns,
    sizeBytes: item.totalBytes || 0,
    freeOpfsBytes: free,
    largeSizeThreshold: prefs.largeSizeThreshold,
  });

  if (decision.kind === "allow") {
    // User trusts this host — pass through untouched.
    suggest();
    return;
  }

  if (decision.kind === "fallback") {
    // Suggest a _popy name inside a dedicated quarantine subfolder.
    // The file lands on disk but with no OS-recognizable extension,
    // so double-click / preview-handler paths are defused.
    const id = crypto.randomUUID();
    const safe = sanitizeFilename(item.filename || "download");
    const suggested = `${QUARANTINE_DIR}/${id.slice(0, 8)}_${popyNameFor(safe)}`;
    suggest({ filename: suggested, conflictAction: "uniquify" });

    const rec: QuarantineRecord = {
      id,
      opfsPath: "",
      diskPath: suggested,
      originalFilename: safe,
      sourceUrl: item.finalUrl || item.url,
      sourceHost: safeHost(item.finalUrl || item.url),
      referrer: item.referrer || undefined,
      sizeBytes: item.totalBytes || 0,
      mime: item.mime || "application/octet-stream",
      sha256: "",
      path: "fallback",
      status: "fetching",
      createdAt: Date.now(),
      note: decision.reason,
    };
    await putRecord(rec);
    // Stash downloadId → recId so onChanged can finalize.
    await chrome.storage.session.set({ [`dl:${item.id}`]: id });
    await notifyQuarantine(rec);
    await refreshBadge();
    return;
  }

  // OPFS path: cancel + erase + re-fetch.
  suggest(); // we'll cancel below; suggest() first so the callback is well-formed
  try {
    await chrome.downloads.cancel(item.id);
  } catch {
    /* may already be complete for small files; fall through */
  }
  try {
    await chrome.downloads.erase({ id: item.id });
  } catch {
    /* best-effort */
  }

  const id = crypto.randomUUID();
  const safe = sanitizeFilename(item.filename || "download");
  const rec: QuarantineRecord = {
    id,
    opfsPath: opfsPathFor(id, safe),
    originalFilename: safe,
    sourceUrl: item.finalUrl || item.url,
    sourceHost: safeHost(item.finalUrl || item.url),
    referrer: item.referrer || undefined,
    sizeBytes: item.totalBytes || 0,
    mime: item.mime || "application/octet-stream",
    sha256: "",
    path: "opfs",
    status: "fetching",
    createdAt: Date.now(),
  };
  await putRecord(rec);

  const msg: Message = {
    type: "fetch-to-opfs",
    payload: {
      id,
      url: item.finalUrl || item.url,
      referrer: item.referrer || undefined,
      originalFilename: safe,
      totalBytes: item.totalBytes || 0,
      mime: item.mime || "application/octet-stream",
    },
  };
  void dispatchToOffscreen(msg, id).catch(async (e) => {
    console.error("[popy] offscreen dispatch failed", e);
    await updateRecord(id, {
      status: "failed",
      note: `offscreen unavailable: ${e instanceof Error ? e.message : String(e)}`,
    });
    await removeQuarantineEntry(id);
    await refreshBadge();
  });
}

/**
 * Deliver a fetch-to-opfs job to the offscreen doc, creating the doc if
 * needed and retrying a handful of times while it boots. Throws if every
 * attempt fails so the caller can mark the record failed.
 */
async function dispatchToOffscreen(msg: Message, recId: string): Promise<void> {
  await ensureOffscreen();
  let lastErr: unknown = null;
  for (let attempt = 0; attempt < 4; attempt++) {
    try {
      await chrome.runtime.sendMessage(msg);
      return;
    } catch (e) {
      lastErr = e;
      // If the doc went missing between ensureOffscreen() and now,
      // re-create it before the next try.
      offscreenReady = null;
      await ensureOffscreen();
      await new Promise((r) => setTimeout(r, 150 * (attempt + 1)));
    }
  }
  const base = lastErr instanceof Error ? lastErr.message : String(lastErr ?? "unknown");
  throw new Error(`dispatch(${recId}): ${base}`);
}

async function markFallbackStored(downloadId: number): Promise<void> {
  const key = `dl:${downloadId}`;
  const { [key]: recId } = await chrome.storage.session.get(key);
  if (!recId) return;
  await chrome.storage.session.remove(key);
  const rec = await getRecord(recId);
  if (!rec) return;
  const items = await chrome.downloads.search({ id: downloadId });
  const item = items[0];
  const finalSize = item?.fileSize ?? rec.sizeBytes;
  // If Chrome tacked an extension onto our suggestion (rare but possible
  // when MIME-sniffing kicks in), record the true leaf name so cleanup
  // regex-matches reliably.
  const realLeaf = item?.filename ? item.filename.split(/[\\/]/).pop() : undefined;
  await updateRecord(recId, {
    status: "stored",
    sizeBytes: finalSize,
    ...(realLeaf && realLeaf !== rec.originalFilename
      ? { diskPath: `${QUARANTINE_DIR}/${realLeaf}` }
      : {}),
  });
  await refreshBadge();
}

// ─────────────────────────────────────────────────────────────────────
// Message router
// ─────────────────────────────────────────────────────────────────────

async function handleMessage(msg: Message): Promise<unknown> {
  switch (msg.type) {
    case "fetch-complete": {
      await updateRecord(msg.payload.id, {
        status: "stored",
        sha256: msg.payload.sha256,
        sizeBytes: msg.payload.bytes,
      });
      const r = await getRecord(msg.payload.id);
      if (r) await notifyQuarantine(r);
      await refreshBadge();
      return { ok: true };
    }
    case "fetch-failed": {
      await updateRecord(msg.payload.id, {
        status: "failed",
        note: msg.payload.reason,
      });
      await removeQuarantineEntry(msg.payload.id);
      await refreshBadge();
      return { ok: true };
    }
    case "fetch-progress": {
      // Forward to any open UI listeners.
      chrome.runtime.sendMessage(msg).catch(() => {});
      return { ok: true };
    }
    case "list-records": {
      return { type: "records", payload: await listRecords({ limit: 200 }) };
    }
    case "get-record": {
      return { type: "record", payload: await getRecord(msg.payload.id) };
    }
    case "delete-file": {
      await doDelete(msg.payload.id);
      return { ok: true };
    }
    default:
      return { ok: false, error: "unknown message" };
  }
}

async function doDelete(id: string): Promise<void> {
  const rec = await getRecord(id);
  if (!rec) return;
  if (rec.path === "opfs") {
    await removeQuarantineEntry(id);
  } else if (rec.diskPath) {
    // Best-effort cleanup from disk. Chrome stores the absolute path in
    // `item.filename`; on Windows the separators are backslashes. We
    // search on the leaf filename (after the final slash in our virtual
    // path), which is the portion Chrome uniquely places and won't
    // collide with user files.
    const leaf = rec.diskPath.split("/").pop() ?? rec.diskPath;
    const rx = escapeRegex(leaf);
    const items = await chrome.downloads.search({ filenameRegex: rx });
    for (const it of items) {
      try {
        await chrome.downloads.removeFile(it.id);
      } catch {
        /* file may be gone already */
      }
      try {
        await chrome.downloads.erase({ id: it.id });
      } catch {
        /* ditto */
      }
    }
  }
  await updateRecord(id, { status: "deleted", resolvedAt: Date.now() });
  await refreshBadge();
}

// ─────────────────────────────────────────────────────────────────────
// Badge + notifications
// ─────────────────────────────────────────────────────────────────────

async function refreshBadge(): Promise<void> {
  const n = await countByStatus("stored");
  await chrome.action.setBadgeBackgroundColor({ color: "#b84a2f" });
  await chrome.action.setBadgeText({ text: n > 0 ? String(n) : "" });
}

let notifyWindowStart = 0;
let notifyWindowCount = 0;

async function notifyQuarantine(rec: QuarantineRecord): Promise<void> {
  const prefs = await getPrefs();
  if (!prefs.notifications) return;

  // Throttle: 3 per 10s window; beyond that, silent.
  const now = Date.now();
  if (now - notifyWindowStart > 10_000) {
    notifyWindowStart = now;
    notifyWindowCount = 0;
  }
  if (++notifyWindowCount > 3) return;

  const title =
    rec.path === "opfs"
      ? `Quarantined in sandbox: ${rec.originalFilename}`
      : `Quarantined on disk: ${rec.originalFilename}`;
  const msg =
    rec.path === "opfs"
      ? "Isolated in Popy's sandbox. It cannot be opened until you release it."
      : `Saved with _popy suffix so it can't be executed. (${rec.note ?? ""})`;

  chrome.notifications.create(rec.id, {
    type: "basic",
    iconUrl: chrome.runtime.getURL("public/icons/icon-128.png"),
    title,
    message: msg,
    buttons: [{ title: "Review" }, { title: "Delete" }],
    priority: 0,
  });
}

// ─────────────────────────────────────────────────────────────────────
// Utils
// ─────────────────────────────────────────────────────────────────────

function safeHost(url: string): string {
  try {
    return new URL(url).host;
  } catch {
    return "";
  }
}

function escapeRegex(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
