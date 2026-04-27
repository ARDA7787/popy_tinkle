import { describe, expect, it } from "vitest";
import { opfsPathFor, popyNameFor, sanitizeFilename } from "@lib/opfs/fs";
import { POPY_SUFFIX } from "@lib/types";

describe("sanitizeFilename", () => {
  it("strips path components", () => {
    expect(sanitizeFilename("../etc/passwd")).toBe("passwd");
    expect(sanitizeFilename("C:\\evil\\foo.exe")).toBe("foo.exe");
  });

  it("replaces illegal characters", () => {
    expect(sanitizeFilename('a<b>c:d"e/f\\g|h?i*j.txt')).toMatch(/_/);
  });

  it("collapses whitespace and trims", () => {
    expect(sanitizeFilename("  many   spaces.bin  ")).toBe("many spaces.bin");
  });

  it("guards Windows reserved names", () => {
    expect(sanitizeFilename("CON")).toBe("_CON");
    expect(sanitizeFilename("CON.txt")).toBe("_CON.txt");
    expect(sanitizeFilename("NUL.bin")).toBe("_NUL.bin");
    expect(sanitizeFilename("COM1.dat")).toBe("_COM1.dat");
  });

  it("falls back when filename is empty", () => {
    expect(sanitizeFilename("")).toBe("download");
    expect(sanitizeFilename("   ")).toBe("download");
  });

  it("caps very long filenames", () => {
    const huge = "x".repeat(1000) + ".pdf";
    expect(sanitizeFilename(huge).length).toBeLessThanOrEqual(230);
  });
});

describe("popyNameFor / opfsPathFor", () => {
  it("appends the _popy suffix verbatim", () => {
    expect(popyNameFor("invoice.pdf")).toBe("invoice.pdf" + POPY_SUFFIX);
    expect(popyNameFor("setup.exe")).toBe("setup.exe" + POPY_SUFFIX);
  });

  it("composes a stable OPFS path", () => {
    expect(opfsPathFor("uuid-123", "doc.pdf")).toBe("uuid-123/doc.pdf" + POPY_SUFFIX);
  });
});
