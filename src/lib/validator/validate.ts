// Response validator.
//
// Runs before we pipeTo() the response body into OPFS. If any layer fails
// we abort the writable (no bytes committed) and the caller re-dispatches
// as a fallback download.
//
// Ordered cheap-to-expensive:
//
//   1. HTTP status
//   2. Content-Type family matches the expected MIME family
//   3. Content-Length roughly matches the original totalBytes
//   4. Final URL hasn't bounced to a login flow
//   5. Magic-byte check on the first 64 KB (run during streaming)
//
// Layers 1-4 run before we touch the body stream. Layer 5 runs inline
// during streaming — if it fails we abort mid-write.

export interface ValidationContext {
  expectedMime: string;
  expectedBytes: number;
}

export interface ValidationFailure {
  ok: false;
  reason: string;
}

export type ValidationResult = { ok: true } | ValidationFailure;

const LOGIN_HINT_RE = /\/(login|signin|sign-in|auth|accounts\.google|oauth2?)\b/i;

export function validateHeaders(
  resp: Response,
  ctx: ValidationContext,
): ValidationResult {
  // 1. status
  if (!resp.ok) return { ok: false, reason: `HTTP ${resp.status}` };

  // 2. content-type family.
  // Specifically: if the original DownloadItem's MIME was anything except
  // text/*, but the re-fetch came back as text/html, this is almost
  // always an auth interstitial or a "click here to confirm" page. We
  // accept text/plain and text/csv for any expectation because some
  // servers serve those without proper Content-Disposition.
  const ct = (resp.headers.get("content-type") ?? "").toLowerCase();
  const wantedFamily = ctx.expectedMime.split("/")[0]?.toLowerCase() ?? "";
  const gotFamily = ct.split(";")[0]?.split("/")[0]?.toLowerCase() ?? "";
  if (
    wantedFamily &&
    gotFamily &&
    wantedFamily !== "text" &&
    gotFamily === "text" &&
    !ct.startsWith("text/plain") &&
    !ct.startsWith("text/csv")
  ) {
    return { ok: false, reason: `got ${ct} for ${ctx.expectedMime}` };
  }

  // 3. content-length sanity
  const cl = Number(resp.headers.get("content-length") ?? "0");
  if (ctx.expectedBytes > 0 && cl > 0) {
    const diff = Math.abs(cl - ctx.expectedBytes) / ctx.expectedBytes;
    if (diff > 0.1) {
      return {
        ok: false,
        reason: `size mismatch: expected ${ctx.expectedBytes}, got ${cl}`,
      };
    }
  }

  // 4. login redirect
  if (LOGIN_HINT_RE.test(resp.url)) {
    return { ok: false, reason: `redirected to auth flow: ${resp.url}` };
  }

  return { ok: true };
}

// Magic-byte signatures. Keep this list short and high-signal.
// We reject if (a) the expected MIME implies one of these families
// AND (b) the observed magic bytes match none of the allowed families.
const SIGNATURES: Array<{
  name: string;
  match: (bytes: Uint8Array) => boolean;
  mimePrefixes: string[];
}> = [
  {
    name: "pdf",
    match: (b) => b[0] === 0x25 && b[1] === 0x50 && b[2] === 0x44 && b[3] === 0x46,
    mimePrefixes: ["application/pdf"],
  },
  {
    name: "png",
    match: (b) => b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47,
    mimePrefixes: ["image/png"],
  },
  {
    name: "jpeg",
    match: (b) => b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff,
    mimePrefixes: ["image/jpeg", "image/jpg"],
  },
  {
    name: "gif",
    match: (b) => b[0] === 0x47 && b[1] === 0x49 && b[2] === 0x46,
    mimePrefixes: ["image/gif"],
  },
  {
    name: "zip/office/apk/jar",
    match: (b) => b[0] === 0x50 && b[1] === 0x4b && (b[2] === 0x03 || b[2] === 0x05 || b[2] === 0x07),
    mimePrefixes: [
      "application/zip",
      "application/x-zip",
      "application/vnd.openxmlformats",
      "application/java-archive",
      "application/vnd.android.package-archive",
      "application/epub+zip",
    ],
  },
  {
    name: "pe",
    match: (b) => b[0] === 0x4d && b[1] === 0x5a,
    mimePrefixes: [
      "application/vnd.microsoft.portable-executable",
      "application/x-msdownload",
      "application/x-msdos-program",
      "application/octet-stream",
    ],
  },
  {
    name: "elf",
    match: (b) => b[0] === 0x7f && b[1] === 0x45 && b[2] === 0x4c && b[3] === 0x46,
    mimePrefixes: ["application/x-elf", "application/octet-stream"],
  },
  {
    name: "macho",
    match: (b) => {
      const m = (b[0]! << 24) | (b[1]! << 16) | (b[2]! << 8) | b[3]!;
      return (
        m === 0xfeedface || m === 0xfeedfacf || m === 0xcefaedfe || m === 0xcffaedfe
      );
    },
    mimePrefixes: ["application/x-mach-binary", "application/octet-stream"],
  },
  {
    name: "dmg",
    match: (b) => b[0] === 0x78 && b[1] === 0x01, // koly footer only reliable at end
    mimePrefixes: ["application/x-apple-diskimage"],
  },
];

export function validateMagic(
  firstChunk: Uint8Array,
  expectedMime: string,
): ValidationResult {
  if (firstChunk.byteLength < 4) return { ok: true };
  const mime = expectedMime.toLowerCase().split(";")[0]?.trim() ?? "";

  // Only reject if the expected MIME is one we have a signature for.
  const candidates = SIGNATURES.filter((s) =>
    s.mimePrefixes.some((p) => mime.startsWith(p)),
  );
  if (candidates.length === 0) return { ok: true };

  const someMatch = candidates.some((s) => s.match(firstChunk));
  if (someMatch) return { ok: true };

  // Also allow text/html detection so we can report the interstitial case
  // with a nicer message.
  const asText = String.fromCharCode(...firstChunk.slice(0, 16)).toLowerCase();
  if (asText.startsWith("<!doctype") || asText.startsWith("<html")) {
    return { ok: false, reason: "expected binary, got HTML body" };
  }
  return { ok: false, reason: `magic bytes don't match ${mime}` };
}
