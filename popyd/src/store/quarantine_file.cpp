#include "store/quarantine_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include "core/log.h"

namespace fs = std::filesystem;

namespace popy::store {

namespace {

void fsync_dir(const fs::path& dir) {
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);  // best-effort; a failure here doesn't lose committed data
    ::close(dfd);
  }
}

}  // namespace

QuarantineWriter::QuarantineWriter(const fs::path& final_path,
                                   bool force_part_fallback)
    : final_path_(final_path) {
#if defined(__linux__) && defined(O_TMPFILE)
  if (!force_part_fallback) {
    // linkat's unprivileged form goes through /proc/self/fd — preflight it
    // so we fail over NOW rather than at commit time with data written.
    struct stat ps {};
    if (::stat("/proc/self/fd", &ps) == 0 && S_ISDIR(ps.st_mode)) {
      int fd = ::open(final_path_.parent_path().c_str(),
                      O_TMPFILE | O_WRONLY | O_CLOEXEC, 0000);
      if (fd >= 0) {
        fd_ = fd;
        return;
      }
      // EOPNOTSUPP (filesystem), EISDIR/EINVAL (old kernel) → .part fallback.
    }
  }
#else
  (void)force_part_fallback;
#endif

  part_path_ = final_path_;
  part_path_ += ".part";
  fd_ = ::open(part_path_.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0000);
  if (fd_ < 0) {
    throw std::runtime_error("quarantine writer: cannot create " +
                             part_path_.string() + ": " +
                             std::strerror(errno));
  }
}

QuarantineWriter::~QuarantineWriter() {
  if (!committed_) abort();
}

void QuarantineWriter::sync() {
  if (fd_ < 0) {
    throw std::runtime_error("quarantine writer: sync on closed writer");
  }
  if (::fsync(fd_) != 0) {
    throw std::runtime_error(std::string("quarantine writer: fsync failed: ") +
                             std::strerror(errno));
  }
}

void QuarantineWriter::commit() {
  if (committed_) {
    throw std::runtime_error("quarantine writer: double commit");
  }
  if (fd_ < 0) {
    throw std::runtime_error("quarantine writer: commit on closed writer");
  }

#if defined(__linux__) && defined(O_TMPFILE)
  if (part_path_.empty()) {
    std::string proc = "/proc/self/fd/" + std::to_string(fd_);
    if (::linkat(AT_FDCWD, proc.c_str(), AT_FDCWD, final_path_.c_str(),
                 AT_SYMLINK_FOLLOW) != 0) {
      throw std::runtime_error(
          std::string("quarantine writer: linkat failed: ") +
          std::strerror(errno));
    }
    committed_ = true;
    ::close(fd_);
    fd_ = -1;
    fsync_dir(final_path_.parent_path());
    return;
  }
#endif

  if (::rename(part_path_.c_str(), final_path_.c_str()) != 0) {
    throw std::runtime_error(std::string("quarantine writer: rename ") +
                             part_path_.string() + " -> " +
                             final_path_.string() + " failed: " +
                             std::strerror(errno));
  }
  committed_ = true;
  ::close(fd_);
  fd_ = -1;
  fsync_dir(final_path_.parent_path());
}

void QuarantineWriter::abort() noexcept {
  if (committed_) return;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!part_path_.empty()) {
    ::unlink(part_path_.c_str());  // ENOENT is fine
  }
}

}  // namespace popy::store
