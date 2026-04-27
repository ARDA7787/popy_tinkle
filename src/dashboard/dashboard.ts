import {
  formatAgo,
  formatBytes,
  formatHost,
  getRecord,
  listRecords,
  msg,
  shortHash,
} from "@lib/ui";
import type { Message, QuarantineRecord } from "@lib/types";
import { getFile } from "@lib/opfs/fs";
import { releaseRecord } from "../popup/release";
import { renderSettings } from "./settings";

type ViewName = "all" | "fetching" | "history" | "settings";

const views: Record<ViewName, HTMLElement> = {
  all: qs('[data-view="all"]'),
  fetching: qs('[data-view="fetching"]'),
  history: qs('[data-view="history"]'),
  settings: qs('[data-view="settings"]'),
};
const listEls = {
  all: qs("#list-all"),
  fetching: qs("#list-fetching"),
  history: qs("#list-history"),
};
const countEls = {
  all: qs("#count-all"),
  fetching: qs("#count-fetching"),
};
const drawerEl = qs("#drawer");
const drawerBody = qs("#drawer-body");
const searchEl = qs<HTMLInputElement>("#search");
const hostFilter = qs<HTMLSelectElement>("#filter-host");

let currentView: ViewName = "all";
let selectedId: string | null = null;
let allRecords: QuarantineRecord[] = [];

// ───────────────────────────── Nav ─────────────────────────────

document.querySelectorAll<HTMLButtonElement>(".nav-item").forEach((btn) => {
  btn.addEventListener("click", () => {
    const name = btn.dataset.view as ViewName;
    switchView(name);
  });
});
qs("#drawer-close").addEventListener("click", () => closeDrawer());

// Deep-link: #<recordId> opens the drawer; #settings switches view.
window.addEventListener("hashchange", handleHash);
handleHash();

function handleHash() {
  const h = location.hash.slice(1);
  if (!h) return;
  if (h === "settings") switchView("settings");
  else openDrawer(h);
}

function switchView(name: ViewName) {
  currentView = name;
  for (const [k, el] of Object.entries(views)) {
    el.classList.toggle("hidden", k !== name);
  }
  document.querySelectorAll<HTMLButtonElement>(".nav-item").forEach((b) => {
    b.classList.toggle("active", b.dataset.view === name);
  });
  if (name === "settings") renderSettings(qs("#settings-body"));
  else void refresh();
}

// ───────────────────────────── Data ─────────────────────────────

chrome.runtime.onMessage.addListener((raw) => {
  const m = raw as Message;
  if (m.type === "fetch-progress") {
    updateProgress(m.payload.id, m.payload.received, m.payload.total);
  } else if (m.type === "fetch-complete" || m.type === "fetch-failed") {
    void refresh();
  }
});

searchEl.addEventListener("input", renderAll);
hostFilter.addEventListener("change", renderAll);

document.getElementById("bulk-delete")?.addEventListener("click", () => void doBulkDelete());

async function doBulkDelete(): Promise<void> {
  const targets = filteredQuarantined();
  if (targets.length === 0) return;
  if (!confirm(`Delete ${targets.length} quarantined file${targets.length === 1 ? "" : "s"} permanently?`)) return;
  for (const r of targets) {
    try {
      await msg({ type: "delete-file", payload: { id: r.id } });
    } catch (e) {
      console.warn("[popy] bulk delete failed for", r.id, e);
    }
  }
  await refresh();
  if (selectedId && targets.some((t) => t.id === selectedId)) closeDrawer();
}

async function refresh() {
  allRecords = await listRecords();
  updateCounts();
  updateHostFilter();
  renderAll();
  updateQuota();
}

function updateCounts() {
  // "fallback" is a path discriminator (where the file lives), not a
  // status — active files always carry status "fetching" or "stored".
  const stored = allRecords.filter((r) => r.status === "stored");
  const fetching = allRecords.filter((r) => r.status === "fetching");
  countEls.all.textContent = String(stored.length);
  countEls.fetching.textContent = String(fetching.length);
}

function updateHostFilter() {
  const hosts = new Set<string>();
  for (const r of allRecords) if (r.sourceHost) hosts.add(r.sourceHost);
  const current = hostFilter.value;
  hostFilter.innerHTML = `<option value="">All origins</option>`;
  for (const h of [...hosts].sort()) {
    const opt = document.createElement("option");
    opt.value = h;
    opt.textContent = formatHost(h);
    hostFilter.appendChild(opt);
  }
  if ([...hosts].includes(current)) hostFilter.value = current;
}

function renderAll() {
  renderList(
    listEls.all,
    filteredQuarantined(),
    "Nothing quarantined yet.",
    "New downloads will arrive here, sealed, until you decide what happens to them.",
  );
  renderList(
    listEls.fetching,
    allRecords.filter((r) => r.status === "fetching"),
    "No active streams.",
    "Popy is idle. When you start a download, its progress will appear here.",
  );
  renderList(
    listEls.history,
    allRecords.filter(
      (r) => r.status === "released" || r.status === "deleted" || r.status === "failed",
    ),
    "No history yet.",
    "Released, deleted, and failed downloads will be listed here.",
  );
}

function filteredQuarantined(): QuarantineRecord[] {
  const q = searchEl.value.trim().toLowerCase();
  const h = hostFilter.value;
  return allRecords.filter((r) => {
    if (r.status !== "stored") return false;
    if (h && r.sourceHost !== h) return false;
    if (
      q &&
      !r.originalFilename.toLowerCase().includes(q) &&
      !r.sourceHost.toLowerCase().includes(q)
    )
      return false;
    return true;
  });
}

function renderList(
  el: HTMLElement,
  records: QuarantineRecord[],
  emptyHeading: string,
  emptyBody: string,
) {
  if (records.length === 0) {
    el.innerHTML = `
      <div class="empty-state">
        <div class="glyph">⌀</div>
        <p>${emptyHeading}</p>
        <p class="dim" style="margin-top:6px;font-size:13px">${emptyBody}</p>
      </div>`;
    return;
  }
  el.innerHTML = "";
  for (const r of records) el.appendChild(renderRecord(r));
}

function renderRecord(r: QuarantineRecord): HTMLElement {
  const el = document.createElement("div");
  el.className = "record";
  if (r.id === selectedId) el.classList.add("selected");
  el.dataset.id = r.id;

  const pathLabel =
    r.status === "fetching"
      ? "streaming"
      : r.path === "fallback"
        ? "on disk · renamed"
        : "in sandbox";
  const pillClass = r.status === "fetching" ? "fetching" : r.path === "fallback" ? "fallback" : "stored";

  el.innerHTML = `
    <div class="fname">${esc(r.originalFilename)}</div>
    <div class="actions"></div>
    <div class="meta">
      <span class="pill ${pillClass}">${pathLabel}</span>
      <span class="mono nums">${formatBytes(r.sizeBytes)}</span>
      <span class="mono">${esc(formatHost(r.sourceHost))}</span>
      <span class="mono">${formatAgo(r.createdAt)}</span>
      ${r.sha256 ? `<span class="mono" title="SHA-256: ${r.sha256}">sha:${shortHash(r.sha256)}</span>` : ""}
    </div>
  `;

  el.addEventListener("click", (e) => {
    if ((e.target as HTMLElement).closest(".actions button")) return;
    openDrawer(r.id);
  });

  const actions = el.querySelector(".actions")!;
  if (r.status === "stored") {
    const b = document.createElement("button");
    b.className = "primary";
    b.textContent = "Release";
    b.addEventListener("click", (e) => {
      e.stopPropagation();
      void doRelease(r);
    });
    actions.appendChild(b);
  }
  if (r.status !== "deleted") {
    const b = document.createElement("button");
    b.className = "danger";
    b.textContent = "Delete";
    b.addEventListener("click", (e) => {
      e.stopPropagation();
      void doDelete(r);
    });
    actions.appendChild(b);
  }
  return el;
}

async function doRelease(r: QuarantineRecord) {
  try {
    await releaseRecord(r);
    await refresh();
    closeDrawer();
  } catch (e) {
    if ((e as DOMException).name === "AbortError") return; // user cancelled picker
    alert(String(e instanceof Error ? e.message : e));
  }
}

async function doDelete(r: QuarantineRecord) {
  if (!confirm(`Delete "${r.originalFilename}" permanently?`)) return;
  await msg({ type: "delete-file", payload: { id: r.id } });
  await refresh();
  if (selectedId === r.id) closeDrawer();
}

function updateProgress(id: string, received: number, total: number) {
  const pct = total > 0 ? Math.min(100, Math.round((received / total) * 100)) : 0;
  // Update meta row inline
  const el = document.querySelector(`.record[data-id="${CSS.escape(id)}"] .meta`);
  if (el) {
    const sizeNode = el.querySelector(".nums");
    if (sizeNode) {
      sizeNode.textContent = `${formatBytes(received)} / ${formatBytes(total)} · ${pct}%`;
    }
  }
}

// ───────────────────────────── Drawer ─────────────────────────────

async function openDrawer(id: string) {
  const r = await getRecord(id);
  if (!r) return;
  // Revoke any preview from a previously selected record before we
  // even start rendering this one.
  revokePreview();
  selectedId = id;
  drawerEl.classList.remove("hidden");

  const canPreview =
    r.path === "opfs" &&
    r.status === "stored" &&
    (r.mime.startsWith("application/pdf") || r.mime.startsWith("image/"));

  drawerBody.innerHTML = `
    <h2>${esc(r.originalFilename)}</h2>
    <div class="source">from <a href="${esc(r.sourceUrl)}" target="_blank" rel="noopener">${esc(r.sourceUrl)}</a></div>

    <dl class="detail-table">
      <dt>Status</dt><dd>${esc(drawerStatus(r))}</dd>
      <dt>Size</dt><dd>${formatBytes(r.sizeBytes)} <span style="color:var(--bone-dim)">(${r.sizeBytes.toLocaleString()} bytes)</span></dd>
      <dt>MIME</dt><dd>${esc(r.mime || "—")}</dd>
      <dt>SHA-256</dt><dd>${r.sha256 ? esc(r.sha256) : "<span style=\"color:var(--bone-dim)\">pending</span>"}</dd>
      <dt>Received</dt><dd>${new Date(r.createdAt).toLocaleString()}</dd>
      ${r.note ? `<dt>Note</dt><dd>${esc(r.note)}</dd>` : ""}
      ${r.referrer ? `<dt>Referrer</dt><dd>${esc(r.referrer)}</dd>` : ""}
    </dl>

    <div class="btn-row">
      ${r.status === "stored" ? `<button class="primary" id="dr-release">Release to disk</button>` : ""}
      ${canPreview ? `<button id="dr-preview">Preview in place</button>` : ""}
      ${r.sha256 ? `<button id="dr-vt">Check on VirusTotal</button>` : ""}
      ${r.status !== "deleted" ? `<button class="danger" id="dr-delete">Delete forever</button>` : ""}
    </div>

    <div class="explainer">
      ${drawerExplainer(r)}
    </div>

    <div id="preview-container"></div>
  `;

  drawerBody.querySelector("#dr-release")?.addEventListener("click", () => void doRelease(r));
  drawerBody.querySelector("#dr-delete")?.addEventListener("click", () => void doDelete(r));
  drawerBody.querySelector("#dr-vt")?.addEventListener("click", () => {
    chrome.tabs.create({ url: `https://www.virustotal.com/gui/file/${r.sha256}` });
  });
  drawerBody.querySelector("#dr-preview")?.addEventListener("click", () => void preview(r));

  for (const el of document.querySelectorAll(".record.selected")) el.classList.remove("selected");
  document.querySelector(`.record[data-id="${CSS.escape(id)}"]`)?.classList.add("selected");
}

function closeDrawer() {
  drawerEl.classList.add("hidden");
  selectedId = null;
  revokePreview();
  if (location.hash) history.replaceState(null, "", location.pathname);
}

function drawerStatus(r: QuarantineRecord): string {
  switch (r.status) {
    case "fetching":
      return "Streaming into sandbox…";
    case "stored":
      return r.path === "opfs" ? "Sealed in sandbox" : "Quarantined on disk";
    case "fallback":
      return "Quarantined on disk (renamed _popy)";
    case "released":
      return "Released to disk";
    case "deleted":
      return "Deleted";
    case "failed":
      return "Fetch failed";
  }
}

function drawerExplainer(r: QuarantineRecord): string {
  if (r.path === "opfs") {
    return `
      <strong>This file never touched your filesystem.</strong>
      It lives inside Popy's browser-managed sandbox — invisible to the
      OS shell, Spotlight, Windows Search, and your antivirus's on-access
      scanner. Releasing it writes a fresh copy to a location you choose,
      with the <code>_popy</code> suffix stripped. Deleting it is instant and total.
    `;
  }
  return `
    <strong>Popy couldn't safely re-fetch this download.</strong>
    Reason: <em>${esc(r.note ?? "unknown")}</em>. Rather than drop the protection,
    Popy let the download proceed into a dedicated quarantine subfolder and
    suffixed it with <code>_popy</code> so no OS handler will execute or preview it.
    Releasing opens the file's folder so you can rename it manually; deleting
    removes it from disk.
  `;
}

// Track the active preview blob URL so we can revoke it whenever the
// drawer closes — by close button, hashchange, or another record being
// opened. Otherwise we leak large objects (PDF blobs especially).
let activePreviewUrl: string | null = null;

function revokePreview(): void {
  if (activePreviewUrl) {
    URL.revokeObjectURL(activePreviewUrl);
    activePreviewUrl = null;
  }
}

async function preview(r: QuarantineRecord) {
  const container = qs("#preview-container");
  revokePreview();
  container.innerHTML = "";
  try {
    const file = await getFile(r.opfsPath);
    const url = URL.createObjectURL(file);
    activePreviewUrl = url;
    if (r.mime.startsWith("application/pdf")) {
      // PDF.js via the browser's native PDF viewer — sandboxed, scripts disabled by default
      const frame = document.createElement("iframe");
      frame.className = "preview-frame";
      frame.src = url;
      frame.setAttribute("sandbox", "allow-same-origin");
      container.appendChild(frame);
    } else if (r.mime.startsWith("image/")) {
      const img = document.createElement("img");
      img.src = url;
      img.style.maxWidth = "100%";
      img.style.marginTop = "16px";
      img.style.border = "1px solid var(--ink-line)";
      img.style.borderRadius = "var(--radius)";
      container.appendChild(img);
    }
  } catch (e) {
    container.innerHTML = `<p class="dim" style="margin-top:12px">Preview failed: ${esc(String(e))}</p>`;
  }
}

// ───────────────────────────── Quota ─────────────────────────────

async function updateQuota() {
  const { quota = 0, usage = 0 } = (await navigator.storage?.estimate?.()) ?? {};
  const fill = qs<HTMLSpanElement>("#quota-fill");
  const text = qs("#quota-text");
  if (quota > 0) {
    const pct = Math.round((usage / quota) * 100);
    fill.style.width = `${Math.min(100, pct)}%`;
    text.textContent = `${formatBytes(usage)} of ${formatBytes(quota)} (${pct}%)`;
  } else {
    fill.style.width = "0%";
    text.textContent = "—";
  }
}

// ───────────────────────────── Utilities ─────────────────────────────

function qs<T extends HTMLElement = HTMLElement>(sel: string): T {
  const el = document.querySelector<T>(sel);
  if (!el) throw new Error(`Missing element: ${sel}`);
  return el;
}
function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) =>
    c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : c === '"' ? "&quot;" : "&#39;",
  );
}

// Version footer
qs("#version").textContent = `v${chrome.runtime.getManifest().version}`;

void refresh();
