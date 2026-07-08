# Security Policy

## Reporting a vulnerability

Popy handles security-sensitive operations. Please report potential vulnerabilities **privately** before disclosing them publicly.

**Preferred channel:** GitHub's [private vulnerability reporting](https://github.com/ARDA7787/popy_tinkle/security/advisories/new). This is the supported private disclosure path.

Include a description, a proof-of-concept if possible, and the version of Popy, popyd, and Chromium you tested against.

### What you can expect

| Milestone               | Target                               |
| ----------------------- | ------------------------------------ |
| Acknowledgement         | Within 72 hours                      |
| Initial triage          | Within 7 days                        |
| Fix proposed            | Within 30 days for critical issues   |
| Coordinated disclosure  | 90 days from report, or when fix ships to users, whichever is sooner |
| Public credit           | In the release notes and a CVE if applicable, unless you prefer anonymity |

### Scope

**In scope:**

- Bypasses of the quarantine model (any path by which a downloaded file reaches a location where the OS can interact with it, other than the documented `showSaveFilePicker()` flow).
- popyd/popy pipeline breaks: any window where in-flight `popy fetch` bytes are readable by another process, a way to make `popy read`/`popy release` accept content whose sidecar signature or SHA-256 does not verify, a sidecar-forgery or sidecar-transplant path that does not require reading the user's `popy.key`, or a `popy release` that follows a symlink planted at the destination.
- Extension-context escalation (an attacker with only a website's privileges gaining Popy's privileges).
- Data-exfiltration paths (any way Popy could send file bytes or metadata outside your browser).
- Denial-of-service against the extension that leaves downloads unprotected without your knowledge.
- Supply-chain concerns with pinned dependencies.

**Out of scope:**

- Issues requiring a compromised browser (Chromium's own bugs — report to Google).
- Issues requiring local shell access.
- "Popy cannot protect against X" where X is explicitly listed in [THREAT_MODEL.md](THREAT_MODEL.md).
- Theoretical quota-exhaustion attacks requiring hundreds of GB of downloads.
- Missing hardening that would not prevent any known attack (we welcome these as regular issues, just not as security reports).

### What not to do

- Do not test against other people's browsers.
- Do not attempt to exfiltrate data from the maintainers' infrastructure. We have none worth taking, and unauthorized access is illegal regardless.
- Do not run automated scanners against our GitHub issues — they produce mostly noise.

## Supported versions

Popy is in public preview. Until 1.0, only the latest release is supported. When 1.0 ships, the most recent two minor versions will receive security fixes.

## Verification and reproducibility

Reproducible-build support is on the roadmap. In the meantime, you can verify the code you're running by:

1. Cloning the repository at the release tag.
2. Running `npm ci && npm run build`.
3. Diffing the resulting `dist/` against the extension unpacked from the Chrome Web Store.

If these differ in anything other than signed metadata, file a security advisory immediately.
