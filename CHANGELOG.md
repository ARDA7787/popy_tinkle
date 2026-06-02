# Changelog

## Unreleased

### Added

- Sandboxed `popy-render` child for PDF/image/text previews.
- GitHub Actions CI for popyd and the browser extension.
- Weekly and pull-request fuzz workflow for MIME and sidecar parsers.
- Release workflow for tagged binary artifacts.
- SSRF regression coverage for loopback/private-network fetch blocking.

### Fixed

- `popy read` now routes hostile PDF/image parsing through a child process.
- `popy fetch` blocks private-network targets by default and caps redirects at 5.
- `popyd` catches fatal top-level exceptions instead of unwinding through `main`.

