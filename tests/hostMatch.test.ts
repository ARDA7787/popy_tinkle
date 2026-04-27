import { describe, expect, it } from "vitest";
import { matchHost, normalizePattern } from "@lib/interceptor/hostMatch";

describe("matchHost", () => {
  it("matches exact hosts", () => {
    expect(matchHost("github.com", ["github.com"])).toBe(true);
    expect(matchHost("github.com", ["example.com"])).toBe(false);
  });

  it("treats *. as base + any subdomain (one or more labels)", () => {
    const p = ["*.github.com"];
    expect(matchHost("github.com", p)).toBe(true);
    expect(matchHost("release.github.com", p)).toBe(true);
    expect(matchHost("a.b.github.com", p)).toBe(true);
    expect(matchHost("notgithub.com", p)).toBe(false);
    expect(matchHost("github.com.evil.com", p)).toBe(false);
  });

  it("does not match subdomains for non-wildcard patterns", () => {
    expect(matchHost("api.example.com", ["example.com"])).toBe(false);
  });

  it("is case-insensitive", () => {
    expect(matchHost("GitHub.com", ["github.com"])).toBe(true);
    expect(matchHost("API.example.com", ["*.EXAMPLE.com"])).toBe(true);
  });

  it("ignores empty pattern lists", () => {
    expect(matchHost("github.com", [])).toBe(false);
  });
});

describe("normalizePattern", () => {
  it("accepts plain hosts", () => {
    expect(normalizePattern("github.com")).toBe("github.com");
    expect(normalizePattern("  GitHub.COM  ")).toBe("github.com");
  });

  it("accepts wildcard hosts", () => {
    expect(normalizePattern("*.github.com")).toBe("*.github.com");
  });

  it("strips protocol, path, and port", () => {
    expect(normalizePattern("https://github.com/foo")).toBe("github.com");
    expect(normalizePattern("github.com:8080")).toBe("github.com");
  });

  it("rejects garbage", () => {
    expect(normalizePattern("")).toBeNull();
    expect(normalizePattern("not a host")).toBeNull();
    expect(normalizePattern("*.")).toBeNull();
    expect(normalizePattern("github")).toBeNull(); // single label, no TLD
    expect(normalizePattern("**.github.com")).toBeNull();
  });
});
