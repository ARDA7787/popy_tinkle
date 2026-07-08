# Contributing to Popy

Thanks for considering a contribution. This document tells you how.

## Before you start

Popy is intentionally small. Before adding a feature, check whether it belongs in scope:

**In scope:**

- Making the quarantine more reliable (new failure modes caught by the classifier, better validators).
- Making the UI clearer, faster, or more accessible.
- Making the threat model more honest.
- Making the code smaller.

**Probably out of scope (discuss first):**

- Adding server-side components.
- Adding telemetry or analytics, even "anonymous" ones.
- Adding features that require new permissions.
- Features specific to one browser when the others could support them.

When in doubt, open a discussion before writing code.

## Development setup

```bash
git clone https://github.com/ARDA7787/popy_tinkle.git
cd popy
npm install
npm run dev
```

`npm run dev` builds into `dist/` and watches. Load `dist/` as an unpacked extension in `chrome://extensions`. Changes to source trigger a rebuild; reload the extension manually after each rebuild.

For a production build:

```bash
npm run build
```

## Code layout

See the [Extension layout](README.md#extension-layout) section of the README.

Three rules:

1. **Service-worker listeners register at the top level.** Never inside an `async` function. MV3 re-runs the SW script on wake; listeners registered in async code are lost.
2. **Long work runs in the offscreen document, not the SW.** The SW has a 30-second idle cap.
3. **`showSaveFilePicker()` is called from a user gesture, before the first `await`.** Getting this wrong produces `SecurityError`.

## Style

- **TypeScript, strict.** `noUncheckedIndexedAccess: true`. If `tsc --noEmit` is unhappy, so are we.
- **Prose matters.** Comments explain _why_, not _what_. The code already says what.
- **No dependencies without a reason.** Every `dependencies` entry is scrutinized.

Run `npm run typecheck` before committing.

## Tests

Unit tests for the classifier and validator live in `tests/unit/` (coming in v0.2). For now, manual testing against the failure-mode matrix is expected:

- A normal `<a download>` link to a static file.
- A blob-URL download from `fetch + createObjectURL`.
- A POST-initiated download.
- A download from a Google Drive `/uc?export=download` URL >40 MB.
- A download from a host in your allowlist.
- A download exceeding the large-file threshold.
- A download where re-fetch returns 403 or text/html.

If you add a new failure mode to the classifier, add a manual test case to this list in your PR description.

## Commit messages

Use conventional commits:

- `feat(classifier): detect POST-initiated downloads via webRequest`
- `fix(offscreen): abort writable on validator failure`
- `docs: clarify the fallback path in the threat model`

## Pull requests

1. Fork.
2. Branch from `main`.
3. Small, focused PRs. If a change touches more than ~400 lines, it's usually better as two PRs.
4. In the description, include: what changed, why, and how you tested it.
5. If it changes the threat model, update `THREAT_MODEL.md` in the same PR.

## DCO

By contributing, you agree that your contributions are released under the Apache 2.0 license and that you have the right to contribute the code. There is no formal CLA, but we follow the [Developer Certificate of Origin](https://developercertificate.org/) convention — signing off commits is appreciated:

```
git commit -s -m "feat: add X"
```

## Code of conduct

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Short version: be kind, be patient, assume good faith. If someone isn't, tell a maintainer.
