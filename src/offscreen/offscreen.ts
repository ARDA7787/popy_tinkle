// Offscreen document.
//
// The service worker has a 30s idle cap. The offscreen document does not.
// We do all long-running work here: fetch, streaming SHA-256, OPFS write.
//
// End-to-end backpressure:
//   network → fetch ReadableStream → validator TransformStream → hasher TransformStream → FileSystemWritableFileStream
//
// A multi-GB download uses only tens of MB of RAM because pipeTo() propagates
// backpressure all the way back to TCP.

import { createSHA256 } from "hash-wasm";
import { openWritable, removeQuarantineEntry } from "@lib/opfs/fs";
import { installReplayRule } from "@lib/dnr/rules";
import {
  validateHeaders,
  validateMagic,
  type ValidationResult,
} from "@lib/validator/validate";
import type { Message } from "@lib/types";

chrome.runtime.onMessage.addListener((raw, _sender, sendResponse) => {
  const msg = raw as Message;
  if (msg.type !== "fetch-to-opfs") return;
  void doFetch(msg.payload).then((res) => sendResponse(res));
  return true;
});

async function doFetch(p: {
  id: string;
  url: string;
  referrer?: string;
  originalFilename: string;
  totalBytes: number;
  mime: string;
}): Promise<{ ok: boolean; error?: string }> {
  const postFail = async (reason: string) => {
    await removeQuarantineEntry(p.id);
    await send({ type: "fetch-failed", payload: { id: p.id, reason } });
  };

  // Install a short-lived DNR rule so the re-fetch preserves Referer/Origin.
  let rule: { remove: () => Promise<void> } | null = null;
  try {
    if (p.referrer) {
      let origin: string | undefined;
      try {
        origin = new URL(p.referrer).origin;
      } catch {
        /* referrer may be relative/weird — skip origin */
      }
      rule = await installReplayRule({
        urlFilter: p.url,
        referer: p.referrer,
        origin,
      });
    }
  } catch (e) {
    // Non-fatal — proceed without header rewrite.
    console.warn("[popy] DNR rule install failed", e);
  }

  let writable: FileSystemWritableFileStream | null = null;
  try {
    writable = await openWritable(p.id, p.originalFilename);

    const resp = await fetch(p.url, {
      credentials: "include",
      // `referrer` on fetch is best-effort; DNR rule is the real source of truth.
      referrer: p.referrer ?? "",
    });

    const v1 = validateHeaders(resp, {
      expectedMime: p.mime,
      expectedBytes: p.totalBytes,
    });
    if (!v1.ok) throw validationError(v1);

    if (!resp.body) throw new Error("empty response body");

    const hasher = await createSHA256();
    hasher.init();

    let received = 0;
    let lastReport = 0;

    // Accumulate the first up-to-MAGIC_PROBE bytes across however many
    // transport chunks it takes — HTTP/2 can deliver very small initial
    // DATA frames, and validating magic bytes against the second chunk
    // would be nonsense.
    const MAGIC_PROBE = 16;
    let probe: Uint8Array | null = new Uint8Array(MAGIC_PROBE);
    let probeLen = 0;
    let probeDone = false;

    const tap = new TransformStream<Uint8Array, Uint8Array>({
      transform: (chunk, controller) => {
        if (!probeDone && probe) {
          const take = Math.min(MAGIC_PROBE - probeLen, chunk.byteLength);
          probe.set(chunk.subarray(0, take), probeLen);
          probeLen += take;
          if (probeLen >= MAGIC_PROBE || probeLen === p.totalBytes) {
            const mv = validateMagic(probe.subarray(0, probeLen), p.mime);
            if (!mv.ok) {
              controller.error(new Error("validation: " + mv.reason));
              return;
            }
            probeDone = true;
            probe = null; // free
          }
        }
        hasher.update(chunk);
        received += chunk.byteLength;

        const now = performance.now();
        if (now - lastReport > 250) {
          lastReport = now;
          void send({
            type: "fetch-progress",
            payload: {
              id: p.id,
              received,
              total: p.totalBytes || received,
            },
          });
        }
        controller.enqueue(chunk);
      },
      flush: (controller) => {
        // Stream ended — run a final validation on whatever we collected.
        if (!probeDone && probe && probeLen > 0) {
          const mv = validateMagic(probe.subarray(0, probeLen), p.mime);
          if (!mv.ok) {
            controller.error(new Error("validation: " + mv.reason));
          }
        }
      },
    });

    await resp.body.pipeThrough(tap).pipeTo(writable);
    // close() is implicit in pipeTo's successful completion.

    const sha256 = hasher.digest("hex");
    await send({
      type: "fetch-complete",
      payload: { id: p.id, sha256, bytes: received },
    });
    return { ok: true };
  } catch (e) {
    const reason = e instanceof Error ? e.message : String(e);
    try {
      if (writable) await writable.abort(reason);
    } catch {
      /* already aborted */
    }
    await postFail(reason);
    return { ok: false, error: reason };
  } finally {
    if (rule) await rule.remove();
  }
}

function validationError(v: ValidationResult): Error {
  const reason = v.ok ? "unknown" : v.reason;
  return new Error("validation: " + reason);
}

async function send(m: Message): Promise<void> {
  try {
    await chrome.runtime.sendMessage(m);
  } catch {
    // SW may be asleep; message is best-effort
  }
}
