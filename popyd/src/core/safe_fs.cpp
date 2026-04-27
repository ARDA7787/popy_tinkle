#include "safe_fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace popy::safe_fs {

namespace {

[[noreturn]] void throw_errno(const char* what, std::string_view detail) {
  std::string msg = "safe_fs: ";
  msg += what;
  msg += " (";
  msg.append(detail);
  msg += "): ";
  msg += std::strerror(errno);
  throw std::runtime_error(msg);
}

bool component_safe(std::string_view comp) {
  if (comp.empty()) return false;
  if (comp == ".." || comp == ".") return false;
  return comp.find('\0') == std::string_view::npos;
}

}  // namespace

bool is_safe_rel(std::string_view rel) {
  if (rel.empty() || rel.front() == '/') return false;
  if (rel.find('\0') != std::string_view::npos) return false;
  size_t i = 0;
  while (i < rel.size()) {
    auto j = rel.find('/', i);
    auto end = (j == std::string_view::npos) ? rel.size() : j;
    if (!component_safe(rel.substr(i, end - i))) return false;
    i = (j == std::string_view::npos) ? rel.size() : j + 1;
  }
  return true;
}

int open_root(const fs::path& root) {
  std::error_code ec;
  fs::create_directories(root, ec);  // best-effort; open will fail if missing
  int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw_errno("open_root", root.string());
  return fd;
}

int create_at(int root_fd, std::string_view rel, mode_t mode) {
  if (!is_safe_rel(rel)) {
    throw std::runtime_error("safe_fs: unsafe relative path: " +
                             std::string(rel));
  }
  // We have to walk parent dirs ourselves to apply O_NOFOLLOW per-component;
  // openat with a multi-component `rel` will follow intermediate symlinks.
  std::string r(rel);
  auto last_slash = r.find_last_of('/');
  int parent_fd = root_fd;
  std::string leaf = r;
  bool close_parent = false;

  if (last_slash != std::string::npos) {
    mkdirs_at(root_fd, std::string_view(r).substr(0, last_slash));
    parent_fd = ::openat(root_fd, r.substr(0, last_slash).c_str(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent_fd < 0) throw_errno("openat parent", rel);
    close_parent = true;
    leaf = r.substr(last_slash + 1);
  }

  int fd = ::openat(parent_fd, leaf.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    mode);
  int e = errno;
  if (close_parent) ::close(parent_fd);
  errno = e;
  if (fd < 0) throw_errno("create_at", rel);
  return fd;
}

int open_read_at(int root_fd, std::string_view rel) {
  if (!is_safe_rel(rel)) {
    throw std::runtime_error("safe_fs: unsafe relative path: " +
                             std::string(rel));
  }
  int fd = ::openat(root_fd, std::string(rel).c_str(),
                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) throw_errno("open_read_at", rel);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw_errno("fstat", rel);
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    throw std::runtime_error("safe_fs: not a regular file: " + std::string(rel));
  }
  return fd;
}

void mkdirs_at(int root_fd, std::string_view rel) {
  if (rel.empty()) return;
  if (!is_safe_rel(rel)) {
    throw std::runtime_error("safe_fs: unsafe relative path for mkdir: " +
                             std::string(rel));
  }
  std::string acc;
  size_t i = 0;
  while (i <= rel.size()) {
    auto j = rel.find('/', i);
    auto end = (j == std::string_view::npos) ? rel.size() : j;
    if (end > i) {
      if (!acc.empty()) acc.push_back('/');
      acc.append(rel.substr(i, end - i));
      if (::mkdirat(root_fd, acc.c_str(), 0700) != 0 && errno != EEXIST) {
        throw_errno("mkdirat", acc);
      }
      // Refuse symlinked components: if the path we just ensured is a
      // symlink, abort.
      struct stat st {};
      if (::fstatat(root_fd, acc.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        throw_errno("fstatat", acc);
      }
      if (S_ISLNK(st.st_mode)) {
        throw std::runtime_error("safe_fs: symlink in path: " + acc);
      }
    }
    if (j == std::string_view::npos) break;
    i = j + 1;
  }
}

void rename_at(int root_fd, std::string_view from_rel, std::string_view to_rel) {
  if (!is_safe_rel(from_rel) || !is_safe_rel(to_rel)) {
    throw std::runtime_error("safe_fs: unsafe rel for rename");
  }
  if (::renameat(root_fd, std::string(from_rel).c_str(),
                 root_fd, std::string(to_rel).c_str()) != 0) {
    throw_errno("renameat", from_rel);
  }
}

}  // namespace popy::safe_fs
