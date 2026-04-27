import { getPrefs, setPrefs } from "@lib/metadata/prefs";
import { normalizePattern } from "@lib/interceptor/hostMatch";
import type { Preferences } from "@lib/types";

export async function renderSettings(root: HTMLElement): Promise<void> {
  const prefs = await getPrefs();
  root.innerHTML = ""; // rebuild on each visit

  root.appendChild(
    row({
      title: "Desktop notifications",
      body: "Pop a short notice when a download is quarantined. Throttled to three per ten seconds.",
      control: toggle(prefs.notifications, async (v) => {
        await setPrefs({ notifications: v });
      }),
    }),
  );

  root.appendChild(
    row({
      title: "Auto-expire sandbox files",
      body: "Automatically delete files from the sandbox after this many days. Zero means never.",
      control: numberInput(prefs.autoExpireDays, 0, 365, async (v) => {
        await setPrefs({ autoExpireDays: v });
      }),
    }),
  );

  root.appendChild(
    row({
      title: "Large-file threshold",
      body: "Files larger than this are quarantined on disk (renamed _popy) instead of into the sandbox. Multi-gigabyte installers and ISOs belong here — re-fetching them into the sandbox is slow and wasteful.",
      control: sizeInput(prefs.largeSizeThreshold, async (v) => {
        await setPrefs({ largeSizeThreshold: v });
      }),
    }),
  );

  root.appendChild(allowlistRow(prefs));

  root.appendChild(
    row({
      title: "Reset onboarding",
      body: "Show the introduction page again the next time you open Popy.",
      control: btn("Replay intro", async () => {
        await setPrefs({ onboarded: false });
        chrome.tabs.create({ url: chrome.runtime.getURL("src/onboarding/onboarding.html") });
      }),
    }),
  );
}

function row(opts: { title: string; body: string; control: HTMLElement }): HTMLElement {
  const el = document.createElement("div");
  el.className = "setting-row";
  el.innerHTML = `<div><h3>${escapeHtml(opts.title)}</h3><p>${escapeHtml(opts.body)}</p></div>`;
  const control = document.createElement("div");
  control.className = "control";
  control.appendChild(opts.control);
  el.appendChild(control);
  return el;
}

function allowlistRow(prefs: Preferences): HTMLElement {
  const el = document.createElement("div");
  el.className = "setting-row";
  el.innerHTML = `
    <div>
      <h3>Trusted origins</h3>
      <p>Downloads from these hosts bypass quarantine completely. Only add origins you fully trust — this is the escape hatch, use it sparingly.</p>
      <p class="dim" style="margin-top:6px;font-size:12px">
        Use <code>example.com</code> for an exact host, or
        <code>*.example.com</code> to also include any subdomain.
      </p>
    </div>
    <div class="control"></div>
    <div class="allowlist-input" style="grid-column: 1 / -1; margin-top: 16px;">
      <input type="text" placeholder="e.g. github.com or *.github.com" id="allow-input" />
      <button id="allow-add">Add</button>
      <span id="allow-error" class="dim" style="font-size:12px;margin-left:8px;color:var(--accent-warm,#b84a2f)"></span>
    </div>
    <div class="allowlist-items" id="allow-chips"></div>
  `;

  const input = el.querySelector<HTMLInputElement>("#allow-input")!;
  const addBtn = el.querySelector<HTMLButtonElement>("#allow-add")!;
  const errEl = el.querySelector<HTMLElement>("#allow-error")!;
  const chipsEl = el.querySelector<HTMLElement>("#allow-chips")!;

  const render = (list: string[]) => {
    chipsEl.innerHTML = "";
    for (const h of list) {
      const chip = document.createElement("span");
      chip.className = "chip";
      chip.innerHTML = `${escapeHtml(h)} <button aria-label="Remove">×</button>`;
      chip.querySelector("button")!.addEventListener("click", async () => {
        const next = list.filter((x) => x !== h);
        await setPrefs({ allowlistPatterns: next });
        render(next);
      });
      chipsEl.appendChild(chip);
    }
  };
  render(prefs.allowlistPatterns);

  const add = async () => {
    errEl.textContent = "";
    const cleaned = normalizePattern(input.value);
    if (!cleaned) {
      errEl.textContent = "Not a valid host pattern.";
      return;
    }
    const cur = (await getPrefs()).allowlistPatterns;
    if (cur.includes(cleaned)) {
      input.value = "";
      return;
    }
    const next = [...cur, cleaned].sort();
    await setPrefs({ allowlistPatterns: next });
    input.value = "";
    render(next);
  };
  addBtn.addEventListener("click", () => void add());
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") void add();
  });

  return el;
}

function toggle(initial: boolean, onChange: (v: boolean) => void | Promise<void>): HTMLElement {
  const el = document.createElement("div");
  el.className = "switch" + (initial ? " on" : "");
  el.setAttribute("role", "switch");
  el.setAttribute("aria-checked", String(initial));
  el.addEventListener("click", async () => {
    const next = !el.classList.contains("on");
    el.classList.toggle("on", next);
    el.setAttribute("aria-checked", String(next));
    await onChange(next);
  });
  return el;
}

function numberInput(
  initial: number,
  min: number,
  max: number,
  onChange: (v: number) => void | Promise<void>,
): HTMLElement {
  const input = document.createElement("input");
  input.type = "number";
  input.min = String(min);
  input.max = String(max);
  input.value = String(initial);
  input.style.width = "90px";
  input.addEventListener("change", async () => {
    const v = Math.max(min, Math.min(max, Number(input.value) || 0));
    input.value = String(v);
    await onChange(v);
  });
  return input;
}

function sizeInput(
  initial: number,
  onChange: (bytes: number) => void | Promise<void>,
): HTMLElement {
  const gb = (initial / 1_000_000_000).toFixed(1);
  const wrap = document.createElement("div");
  wrap.style.display = "flex";
  wrap.style.gap = "6px";
  wrap.style.alignItems = "center";

  const input = document.createElement("input");
  input.type = "number";
  input.step = "0.1";
  input.min = "0.1";
  input.max = "100";
  input.value = gb;
  input.style.width = "80px";
  const unit = document.createElement("span");
  unit.className = "mono";
  unit.style.color = "var(--bone-dim)";
  unit.style.fontSize = "11px";
  unit.textContent = "GB";

  input.addEventListener("change", async () => {
    const v = Math.max(0.1, Number(input.value) || 2);
    input.value = v.toFixed(1);
    await onChange(Math.round(v * 1_000_000_000));
  });

  wrap.append(input, unit);
  return wrap;
}

function btn(label: string, onClick: () => void | Promise<void>): HTMLElement {
  const b = document.createElement("button");
  b.textContent = label;
  b.addEventListener("click", () => void onClick());
  return b;
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) =>
    c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : c === '"' ? "&quot;" : "&#39;",
  );
}
