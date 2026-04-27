import { requestPersistence } from "@lib/opfs/fs";
import { setPrefs } from "@lib/metadata/prefs";

document.getElementById("done")!.addEventListener("click", async () => {
  // Ask the browser to not evict our OPFS under memory pressure.
  await requestPersistence().catch(() => false);
  await setPrefs({ onboarded: true });
  const tab = await chrome.tabs.getCurrent();
  if (tab?.id !== undefined) {
    chrome.tabs.update(tab.id, {
      url: chrome.runtime.getURL("src/dashboard/dashboard.html"),
    });
  }
});

document.getElementById("docs")!.addEventListener("click", (e) => {
  e.preventDefault();
  chrome.tabs.create({
    url: "https://github.com/ARDA7787/popy_tinkle/blob/main/THREAT_MODEL.md",
  });
});
