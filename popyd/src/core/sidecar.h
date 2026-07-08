// Sidecar JSON ↔ struct.
//
// Field names mirror the browser extension's QuarantineRecord interface
// (src/lib/types/index.ts:14-47). The schema is shared on purpose: a future
// sync layer can ingest these sidecars into the extension's IndexedDB with
// zero translation.
#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace popy::sidecar {

inline constexpr int kSchemaVersion = 1;

// Mirrors QuarantineStatus + InterceptPath in the TS.
//   "stored"   → quarantined and waiting
//   "released" → user released, file is gone (sidecar can be GC'd)
//   "deleted"  → user deleted
//   "failed"   → fetch failed, no bytes stored
//   "fetching" → in-flight popy fetch (transient)

struct Record {
  // Required, mirror QuarantineRecord exactly.
  std::string id;                   // UUID v4
  std::string opfsPath;             // empty for popyd
  std::string diskPath;             // path relative to a watch_dir / stage_dir
  std::string originalFilename;
  std::string sourceUrl;
  std::string sourceHost;
  std::optional<std::string> referrer;
  std::optional<std::string> tabUrl;
  std::int64_t sizeBytes{0};
  std::string mime;
  std::string sha256;
  std::string path{"fallback"};     // "opfs" | "fallback"
  std::string status{"stored"};
  std::int64_t createdAt{0};        // unix ms
  std::optional<std::int64_t> resolvedAt;
  std::optional<std::string> note;

  // popyd-only additive fields (extension's idb store accepts unknown keys).
  int schemaVersion{kSchemaVersion};
  std::string agent{"popyd/0.1"};
  std::string origin{"fetch"};      // "fetch" | "watcher"

  // Attestation (additive, optional for pre-upgrade sidecars — see attest.h).
  std::optional<std::string> sig;     // 64 hex chars, HMAC-SHA256
  std::optional<std::string> sigAlg;  // "hmac-sha256-v1"
};

// Read `<file>_popy.meta.json` next to a `_popy` file.
//   - throws std::runtime_error on missing/malformed sidecar.
//   - the JSON file's mode is required to be readable; we don't enforce 0400.
Record read(const std::filesystem::path& sidecar_path);

// Atomic write via "<path>.new" + rename. Sets mode 0400.
// Caller passes the FINAL path (e.g. "<dir>/<name>_popy.meta.json").
void write_atomic(const std::filesystem::path& sidecar_path, const Record&);

// Convenience: <_popy_path>.meta.json
std::filesystem::path sidecar_path_for(const std::filesystem::path& popy_path);

// Convenience: unix ms now()
std::int64_t now_ms();

}  // namespace popy::sidecar
