// declarativeNetRequest session-rule helpers.
//
// We use session rules (not dynamic) because:
//   - they vanish when the browser closes
//   - each download gets its own rule id, added before fetch, removed after
//   - scoping by initiatorDomains: [chrome.runtime.id] means they only
//     apply to OUR re-fetches, never to the user's own page traffic
//
// The rules rewrite forbidden headers (Referer, Origin, Cookie)
// that `fetch({headers})` refuses to set.

const ID_RANGE_START = 10_000;
const ID_RANGE_END = 100_000;

let nextId = ID_RANGE_START;

function allocId(): number {
  const id = nextId++;
  if (nextId > ID_RANGE_END) nextId = ID_RANGE_START;
  return id;
}

export interface ReplayRule {
  ruleId: number;
  remove: () => Promise<void>;
}

/**
 * Install a short-lived rule that rewrites Referer + Origin for a specific URL,
 * then call `remove()` in a finally block after your fetch.
 */
export async function installReplayRule(opts: {
  urlFilter: string;
  referer?: string;
  origin?: string;
  extraHeaders?: Record<string, string>;
}): Promise<ReplayRule> {
  const ruleId = allocId();
  const requestHeaders: chrome.declarativeNetRequest.ModifyHeaderInfo[] = [];
  if (opts.referer) {
    requestHeaders.push({
      header: "Referer",
      operation: "set" as chrome.declarativeNetRequest.HeaderOperation,
      value: opts.referer,
    });
  }
  if (opts.origin) {
    requestHeaders.push({
      header: "Origin",
      operation: "set" as chrome.declarativeNetRequest.HeaderOperation,
      value: opts.origin,
    });
  }
  for (const [k, v] of Object.entries(opts.extraHeaders ?? {})) {
    requestHeaders.push({
      header: k,
      operation: "set" as chrome.declarativeNetRequest.HeaderOperation,
      value: v,
    });
  }
  if (requestHeaders.length === 0) {
    return { ruleId, remove: async () => {} };
  }

  await chrome.declarativeNetRequest.updateSessionRules({
    addRules: [
      {
        id: ruleId,
        priority: 1,
        action: {
          type: "modifyHeaders" as chrome.declarativeNetRequest.RuleActionType,
          requestHeaders,
        },
        condition: {
          urlFilter: opts.urlFilter,
          initiatorDomains: [chrome.runtime.id],
          resourceTypes: [
            "xmlhttprequest" as chrome.declarativeNetRequest.ResourceType,
          ],
        },
      },
    ],
  });

  return {
    ruleId,
    remove: async () => {
      try {
        await chrome.declarativeNetRequest.updateSessionRules({
          removeRuleIds: [ruleId],
        });
      } catch {
        // best-effort cleanup
      }
    },
  };
}
