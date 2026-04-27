import { describe, expect, it } from "vitest";
import { validateHeaders, validateMagic } from "@lib/validator/validate";

function mkResponse(opts: {
  status?: number;
  contentType?: string;
  contentLength?: number;
  url?: string;
}): Response {
  const headers = new Headers();
  if (opts.contentType) headers.set("content-type", opts.contentType);
  if (typeof opts.contentLength === "number")
    headers.set("content-length", String(opts.contentLength));
  // Response.url is read-only; defineProperty fakes it.
  const r = new Response(null, { status: opts.status ?? 200, headers });
  Object.defineProperty(r, "url", {
    value: opts.url ?? "https://example.com/file.bin",
  });
  return r;
}

describe("validateHeaders", () => {
  it("passes happy path", () => {
    const r = mkResponse({
      status: 200,
      contentType: "application/pdf",
      contentLength: 1000,
    });
    expect(
      validateHeaders(r, { expectedMime: "application/pdf", expectedBytes: 1000 }).ok,
    ).toBe(true);
  });

  it("rejects non-2xx", () => {
    const r = mkResponse({ status: 403, contentType: "application/pdf" });
    const v = validateHeaders(r, {
      expectedMime: "application/pdf",
      expectedBytes: 0,
    });
    expect(v.ok).toBe(false);
  });

  it("rejects unexpected text/html when expecting binary", () => {
    const r = mkResponse({ status: 200, contentType: "text/html; charset=utf-8" });
    const v = validateHeaders(r, {
      expectedMime: "application/zip",
      expectedBytes: 0,
    });
    expect(v.ok).toBe(false);
  });

  it("rejects size mismatches > 10%", () => {
    const r = mkResponse({
      status: 200,
      contentType: "application/pdf",
      contentLength: 100,
    });
    const v = validateHeaders(r, {
      expectedMime: "application/pdf",
      expectedBytes: 1000,
    });
    expect(v.ok).toBe(false);
  });

  it("rejects login-redirect URLs", () => {
    const r = mkResponse({
      status: 200,
      contentType: "application/pdf",
      url: "https://example.com/login?next=/file.pdf",
    });
    const v = validateHeaders(r, {
      expectedMime: "application/pdf",
      expectedBytes: 0,
    });
    expect(v.ok).toBe(false);
  });
});

describe("validateMagic", () => {
  it("accepts a real PDF header", () => {
    const bytes = new Uint8Array([0x25, 0x50, 0x44, 0x46, 0x2d, 0x31, 0x2e]);
    expect(validateMagic(bytes, "application/pdf").ok).toBe(true);
  });

  it("rejects HTML body when expecting a PDF", () => {
    const bytes = new TextEncoder().encode("<!DOCTYPE html><html>");
    const v = validateMagic(bytes, "application/pdf");
    expect(v.ok).toBe(false);
  });

  it("accepts ZIP header for office MIME", () => {
    const bytes = new Uint8Array([0x50, 0x4b, 0x03, 0x04, 0, 0, 0, 0]);
    expect(
      validateMagic(
        bytes,
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
      ).ok,
    ).toBe(true);
  });

  it("accepts PE header for Windows executables", () => {
    const bytes = new Uint8Array([0x4d, 0x5a, 0, 0]);
    expect(validateMagic(bytes, "application/x-msdownload").ok).toBe(true);
  });

  it("is permissive for MIMEs without a known signature", () => {
    const bytes = new Uint8Array([0x00, 0x01, 0x02, 0x03]);
    expect(validateMagic(bytes, "application/some-novel-mime").ok).toBe(true);
  });

  it("returns ok for tiny chunks (< 4 bytes) — caller buffers", () => {
    expect(validateMagic(new Uint8Array([1, 2]), "application/pdf").ok).toBe(true);
  });
});
