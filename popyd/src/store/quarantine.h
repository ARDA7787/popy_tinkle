// Operations against the sidecar set.
//
// Mostly trivial filesystem walking; centralised here so the CLI commands and
// (later) the MCP server share one implementation.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/sidecar.h"

namespace popy::store {

struct Entry {
  std::filesystem::path popy_path;     // <name>_popy
  std::filesystem::path sidecar_path;  // <name>_popy.meta.json
  popy::sidecar::Record record;
};

// Walk every watch_dir AND the stage_dir for *.meta.json sidecars.
// Newest first (by record.createdAt).
std::vector<Entry> list(const popy::config::Config&);

// Resolve `query` to a single entry. Match priorities:
//   1. exact UUID
//   2. UUID prefix (≥ 4 chars; ambiguous → throw)
//   3. exact basename of original
//   4. exact basename of <name>_popy
// Throws std::runtime_error if no match or ambiguous.
Entry resolve(const popy::config::Config&, const std::string& query);

// Release: copy popy_path → out (without _popy suffix), update sidecar
// status="released", restore mode 0644 on the released file, then remove the
// quarantined file + sidecar + (now empty) per-id dir.
void release(const Entry&, const std::filesystem::path& out, bool force);

// Delete: unlink file + sidecar + per-id dir. No overwrite (see plan.md §12).
void remove(const Entry&);

// Mode B (watcher) — in-place quarantine of an already-existing file.
//
// Sequence (see plan.md §1):
//   1. open(path, O_RDONLY|O_NOFOLLOW)   — refuses symlinks
//   2. rename(path -> <name>_popy)        — atomic; original name no longer
//                                           exists on disk
//   3. read fd through SHA-256 hasher     — no copy, no doubled disk usage
//   4. fchmod(fd, 0000)
//   5. write sidecar atomically
//
// `path` must be an absolute path inside one of `watch_dirs`. Returns the
// final on-disk path of the `_popy` file. Throws on failure; on partial
// failure mid-sequence, the file may be left at `<name>_popy` mode 0644 with
// no sidecar — orphaned but harmless (next watcher pass skips it).
struct QuarantineResult {
  std::filesystem::path popy_path;
  std::filesystem::path sidecar_path;
  popy::sidecar::Record record;
};

QuarantineResult quarantine_existing_file(const std::filesystem::path& path);

}  // namespace popy::store
