// Zero-readable-window writer for quarantined downloads.
//
// POSIX checks permissions at open(2) time and an fd keeps its access mode
// regardless of later file-mode changes — so the file can be mode 0000 from
// its very first byte while we keep writing through the held fd:
//
//   Linux:   open(parent_dir, O_TMPFILE|O_WRONLY, 0000) — the file has no
//            name at all while bytes flow. commit() links it into place via
//            linkat(AT_FDCWD, "/proc/self/fd/<fd>", ..., AT_SYMLINK_FOLLOW)
//            (the unprivileged form; AT_EMPTY_PATH needs CAP_DAC_READ_SEARCH).
//            O_EXCL must NOT accompany O_TMPFILE — that forbids linking.
//   macOS /
//   fallback: "<final>.part" created O_EXCL|O_NOFOLLOW mode 0000; commit()
//            rename()s it to the final name. "*.part" is in the default
//            watcher exclude globs.
//
// Caller ordering for fetch: sync() → sign+write sidecar → commit(). A crash
// leaves either nothing (O_TMPFILE), a mode-0000 .part, or a sidecar without
// a data file — never a named, readable, unverified _popy file.
#pragma once
#include <filesystem>

namespace popy::store {

class QuarantineWriter {
 public:
  // Opens the writer for `final_path` (parent directory must exist).
  // `force_part_fallback` is a test hook that skips the O_TMPFILE branch so
  // CI exercises the portable path on Linux too.
  explicit QuarantineWriter(const std::filesystem::path& final_path,
                            bool force_part_fallback = false);
  ~QuarantineWriter();  // aborts (removes any .part) unless committed

  QuarantineWriter(const QuarantineWriter&) = delete;
  QuarantineWriter& operator=(const QuarantineWriter&) = delete;

  int fd() const { return fd_; }
  // True on the O_TMPFILE branch — the in-flight file has no name at all.
  bool anonymous() const { return part_path_.empty(); }

  // fsync the data fd. Throws on failure.
  void sync();
  // Give the data its final name (linkat or rename) and fsync the parent
  // directory. Closes the fd. Throws on failure (writer stays abortable).
  void commit();
  // Remove the in-flight file (if it has a name) and close the fd. Safe to
  // call multiple times; no-op after commit().
  void abort() noexcept;

 private:
  std::filesystem::path final_path_;
  std::filesystem::path part_path_;  // empty on the O_TMPFILE branch
  int fd_ = -1;
  bool committed_ = false;
};

}  // namespace popy::store
