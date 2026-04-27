import {
  formatAgo,
  formatBytes,
  formatHost,
  listRecords,
  msg,
} from "@lib/ui";
import type { Message, QuarantineRecord } from "@lib/types";
import { releaseRecord } from "./release";

const listEl = document.getElementById("list") as HTMLElement;
const quotaEl = document.getElementById("quota") as HTMLElement;

document.getElementById("open-dashboard")!.addEventListener("click", () => {
  chrome.tabs.create({ url: chrome.runtime.getURL("src/dashboard/dashboard.html") });
  window.close();
});
document.getElementById("open-settings")!.addEventListener("click", () => {
  chrome.tabs.create({
    url: chrome.runtime.getURL("src/dashboard/dashboard.html#settings"),
  });
  window.close();
});

// Live updates from progress messages
chrome.runtime.onMessage.addListener((raw) => {
  const m = raw as Message;
  if (m.type === "fetch-progress") {
    updateProgress(m.payload.id, m.payload.received, m.payload.total);
  } else if (m.type === "fetch-complete" || m.type === "fetch-failed") {
    void refresh();
  }
});

async function refresh() {
  const all = await listRecords();
  // "fallback" is a PATH (where the file lives), not a status. Active files
  // show up as "fetching" while streaming and "stored" once complete.
  const visible = all
    .filter((r) => r.status === "stored" || r.status === "fetching")
    .slice(0, 8);

  if (visible.length === 0) {
    listEl.innerHTML = `
      <div class="empty">
        <p>All quiet.</p>
        <p class="mono dim">
          Popy is watching. New downloads will appear here, isolated,
          until you release them.
        </p>
      </div>`;
  } else {
    listEl.innerHTML = "";
    for (const r of visible) listEl.appendChild(renderItem(r));
  }

  const { quota = 0, usage = 0 } = (await navigator.storage?.estimate?.()) ?? {};
  if (quota) {
    const pct = Math.round((usage / quota) * 100);
    quotaEl.textContent = `Sandbox: ${formatBytes(usage)} / ${formatBytes(quota)} (${pct}%)`;
  }
}

function renderItem(r: QuarantineRecord): HTMLElement {
  const el = document.createElement("div");
  el.className = "item";
  el.dataset.id = r.id;

  const pillClass = r.status;
  const statusLabel =
    r.status === "fetching"
      ? "streaming…"
      : r.path === "fallback"
        ? "on disk · renamed"
        : "in sandbox";

  el.innerHTML = `
    <div class="fname">${escapeHtml(r.originalFilename)}</div>
    <div class="actions"></div>
    <div class="meta">
      <span class="pill ${pillClass}">${statusLabel}</span>
      <span class="mono dim nums">${formatBytes(r.sizeBytes)}</span>
      <span class="mono dim">${escapeHtml(formatHost(r.sourceHost))}</span>
      <span class="mono dim">${formatAgo(r.createdAt)}</span>
    </div>
    ${r.status === "fetching" ? `<div class="progress"><span style="width:0%"></span></div>` : ""}
  `;

  const actions = el.querySelector(".actions")!;
  if (r.status === "stored") {
    const release = document.createElement("button");
    release.className = "primary";
    release.textContent = "Release";
    release.addEventListener("click", async () => {
      release.disabled = true;
      release.textContent = "…";
      try {
        await releaseRecord(r);
        await refresh();
      } catch (e) {
        // User cancelling the save-file picker throws AbortError — silent.
        if ((e as DOMException)?.name !== "AbortError") {
          console.error(e);
        }
        release.disabled = false;
        release.textContent = "Release";
      }
    });
    actions.appendChild(release);
  }

  const del = document.createElement("button");
  del.className = "danger";
  del.textContent = "Delete";
  del.addEventListener("click", async () => {
    if (!confirm(`Delete "${r.originalFilename}" permanently?`)) return;
    await msg({ type: "delete-file", payload: { id: r.id } });
    await refresh();
  });
  actions.appendChild(del);

  return el;
}

function updateProgress(id: string, received: number, total: number) {
  const el = listEl.querySelector(`.item[data-id="${CSS.escape(id)}"] .progress span`);
  if (!el) return;
  const pct = total > 0 ? Math.min(100, (received / total) * 100) : 0;
  (el as HTMLElement).style.width = `${pct}%`;
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) =>
    c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : c === '"' ? "&quot;" : "&#39;",
  );
}

refresh();
