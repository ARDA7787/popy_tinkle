import { describe, expect, it } from "vitest";
import { classify } from "@lib/interceptor/classifier";

const base = {
  finalUrl: "https://example.com/file.bin",
  referrer: "",
  mime: "application/octet-stream",
  allowlistPatterns: [] as string[],
  sizeBytes: 1024,
  freeOpfsBytes: 1e12,
  largeSizeThreshold: 2 * 1024 * 1024 * 1024,
};

describe("classify", () => {
  it("routes a normal small GET to OPFS", () => {
    expect(classify(base).kind).toBe("opfs");
  });

  it("allowlists exact hosts", () => {
    const r = classify({ ...base, allowlistPatterns: ["example.com"] });
    expect(r.kind).toBe("allow");
  });

  it("allowlists wildcard subdomains", () => {
    const r = classify({
      ...base,
      finalUrl: "https://api.example.com/file.bin",
      allowlistPatterns: ["*.example.com"],
    });
    expect(r.kind).toBe("allow");
  });

  it("does not allowlist subdomains under exact-host pattern", () => {
    const r = classify({
      ...base,
      finalUrl: "https://api.example.com/file.bin",
      allowlistPatterns: ["example.com"],
    });
    expect(r.kind).toBe("opfs");
  });

  it("falls back on blob: URLs", () => {
    const r = classify({ ...base, finalUrl: "blob:https://example.com/abc" });
    expect(r.kind).toBe("fallback");
  });

  it("falls back on non-GET methods", () => {
    const r = classify({ ...base, method: "POST" });
    expect(r.kind).toBe("fallback");
  });

  it("falls back on unparseable URLs", () => {
    const r = classify({ ...base, finalUrl: "not a url" });
    expect(r.kind).toBe("fallback");
  });

  it("falls back over the large-file threshold", () => {
    const r = classify({
      ...base,
      sizeBytes: 5 * 1024 * 1024 * 1024,
    });
    expect(r.kind).toBe("fallback");
  });

  it("falls back when OPFS quota is too small", () => {
    const r = classify({
      ...base,
      sizeBytes: 1_000_000_000,
      freeOpfsBytes: 100_000_000,
    });
    expect(r.kind).toBe("fallback");
  });

  it("falls back on the Google Drive confirmation interstitial", () => {
    const r = classify({
      ...base,
      finalUrl: "https://drive.usercontent.google.com/download?id=abc&export=download",
    });
    expect(r.kind).toBe("fallback");
  });
});
