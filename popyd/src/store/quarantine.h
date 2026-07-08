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
  bool data_missing = false;  // sidecar exists but the _popy data file is gone
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

// Verified, symlink-safe release:
//   1. open_verified (sidecar HMAC + size; hash happens during the copy)
//   2. copy into ".popy-release.<id>.tmp" (O_EXCL, 0600) in the destination
//      dir, re-hashing the copied bytes
//   3. hash mismatch → tmp deleted, quarantine intact, throw
//   4. fsync → fchmod 0644 → commit: linkat (no-replace; EEXIST → throw
//      "use --force") or rename for --force (atomic, never follows a
//      symlink planted at the destination)
//   5. re-sign sidecar as status="released", then remove file+sidecar+dir.
// `key_file` overrides the signing key location (tests); empty → default.
void release(const Entry&, const std::filesystem::path& out, bool force,
             const std::filesystem::path& key_file = {});

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

// `key_file` overrides the sidecar-signing key location (tests); empty →
// paths::key_file().
QuarantineResult quarantine_existing_file(
    const std::filesystem::path& path,
    const std::filesystem::path& key_file = {});

// Startup repair pass (also useful from tests):
//   - adopt orphaned `*_popy` / `*_popy.N` data files that have no sidecar:
//     lock to 0000, hash, sign, write the sidecar;
//   - GC sidecars whose data file is missing, has no in-flight `.part`
//     sibling, and whose sidecar is older than 10 minutes (younger ones may
//     belong to a fetch that is between sidecar-write and commit).
struct RepairStats {
  int adopted = 0;
  int gc_sidecars = 0;
};
RepairStats repair(const popy::config::Config&,
                   const std::filesystem::path& key_file = {});

// Minimal-window fd on a mode-0000 quarantined file.
//
// Sequence: lstat (require S_ISREG) → chmod(path, 0400) → open(O_RDONLY|
// O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC) → fstat must match the lstat's dev/ino and
// be S_ISREG → IMMEDIATELY fchmod(fd, 0000). The on-disk readable window is
// only the chmod→open gap; the held fd keeps read access (POSIX checks
// permissions at open time). O_NONBLOCK means a FIFO swapped in can't hang us.
class QuarantineReadable {
 public:
  explicit QuarantineReadable(const std::filesystem::path& path);
  ~QuarantineReadable();

  QuarantineReadable(QuarantineReadable&& other) noexcept;
  QuarantineReadable& operator=(QuarantineReadable&&) = delete;
  QuarantineReadable(const QuarantineReadable&) = delete;
  QuarantineReadable& operator=(const QuarantineReadable&) = delete;

  int fd() const { return fd_; }

 private:
  std::filesystem::path path_;
  int fd_ = -1;
};

// EVERY exit from quarantine goes through this gate:
//   1. verify the sidecar HMAC (throws on unsigned/bad — no override)
//   2. minimal-window open (QuarantineReadable)
//   3. fstat size must equal record.sizeBytes
//   4. if `prehash`: stream-hash the entire fd, must equal record.sha256,
//      then rewind to offset 0. release() passes prehash=false and hashes
//      once during the copy instead.
// Throws std::runtime_error on any mismatch; the file stays mode 0000.
QuarantineReadable open_verified(const Entry&, const std::string& key,
                                 bool prehash = true);

}  // namespace popy::store
