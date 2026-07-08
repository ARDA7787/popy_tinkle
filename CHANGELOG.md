# Changelog

## Unreleased

### Added

- **Zero-trust download pipeline for popyd/popy.**
  - `popy fetch` writes downloads mode `0000` from the very first byte:
    anonymous `O_TMPFILE` on Linux (the in-flight file has no name at all),
    a mode-0000 `.part` file elsewhere. The final `_popy` name appears only
    after the signed sidecar is on disk (fsync data → sign+write sidecar →
    link/rename → fsync dir).
  - HMAC-SHA256-signed sidecars (`sig`/`sigAlg` fields) over a canonical
    length-prefixed payload pinned to the on-disk filename; per-user 32-byte
    key auto-created at `~/.config/popy/popy.key` (0600).
  - Every exit from quarantine is gated by verification: sidecar signature,
    recorded size, and full content SHA-256 (`popy read`, `popy release`,
    MCP tools). Bad signatures are never overridable.
  - `popy verify <file>` re-checks a quarantined file; `popy resign` is the
    explicit one-time migration that signs pre-upgrade (unsigned) sidecars —
    it never overwrites an invalid signature.
  - Symlink-safe verified release: bytes are staged into a same-directory
    `.popy-release.<id>.tmp`, re-hashed, then committed via `linkat` (or
    `rename` for `--force`) — a symlink planted at the destination is never
    followed.
  - `popy read --mode text` sanitizes output (UTF-8-aware C0/C1 stripping,
    U+FFFD for invalid sequences, binary-content refusal) and supports
    `--max-bytes`; the MCP `popy_read_text` tool now uses it instead of raw
    bytes, and a new `popy_verify` MCP tool is exposed.
  - popyd startup repair pass: adopts orphaned `_popy` files (hash, sign,
    lock to 0000) and garbage-collects stale sidecars whose data file is
    missing; `popy list` flags such entries as `data-missing`.
  - Watcher hardening: files are locked to mode 0000 immediately after the
    TOCTOU dev/ino check and before rename+hash; FIFO swap-ins can no longer
    hang the daemon (`O_NONBLOCK`); a failed rename restores the file's
    original mode.
- Sandboxed `popy-render` child for PDF/image/text previews.
- GitHub Actions CI for popyd and the browser extension.
- Weekly and pull-request fuzz workflow for MIME and sidecar parsers.
- Release workflow for tagged binary artifacts.
- SSRF regression coverage for loopback/private-network fetch blocking.

### Fixed

- `popy read` now routes hostile PDF/image parsing through a child process.
- `popy fetch` blocks private-network targets by default and caps redirects at 5.
- `popyd` catches fatal top-level exceptions instead of unwinding through `main`.

