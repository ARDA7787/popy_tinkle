// Small UI helpers used across popup, dashboard, onboarding.

import type { Message, QuarantineRecord } from "@lib/types";

export function formatBytes(n: number): string {
  if (!n) return "—";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let i = 0;
  let v = n;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v < 10 && i > 0 ? v.toFixed(1) : Math.round(v)} ${units[i]}`;
}

export function formatAgo(ms: number): string {
  const s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h} hr ago`;
  const d = Math.floor(h / 24);
  if (d < 30) return `${d} day${d === 1 ? "" : "s"} ago`;
  const mo = Math.floor(d / 30);
  return `${mo} mo ago`;
}

export function formatHost(host: string): string {
  if (!host) return "unknown source";
  return host.replace(/^www\./, "");
}

export function shortHash(hex: string): string {
  if (!hex || hex.length < 12) return hex || "—";
  return `${hex.slice(0, 6)}…${hex.slice(-6)}`;
}

export async function msg<T = unknown>(m: Message): Promise<T> {
  return (await chrome.runtime.sendMessage(m)) as T;
}

export async function listRecords(): Promise<QuarantineRecord[]> {
  const r = (await msg<{ type: "records"; payload: QuarantineRecord[] }>({
    type: "list-records",
  })) as { type: "records"; payload: QuarantineRecord[] };
  return r.payload;
}

export async function getRecord(id: string): Promise<QuarantineRecord | null> {
  const r = (await msg<{ type: "record"; payload: QuarantineRecord | null }>({
    type: "get-record",
    payload: { id },
  })) as { type: "record"; payload: QuarantineRecord | null };
  return r.payload;
}
