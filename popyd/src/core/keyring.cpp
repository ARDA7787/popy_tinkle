#include "keyring.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "rand.h"

namespace fs = std::filesystem;

namespace popy::keyring {

namespace {

std::string load(const fs::path& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("keyring: cannot open " + path.string() + ": " +
                             std::strerror(errno));
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("keyring: fstat failed: " +
                             std::string(std::strerror(errno)));
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    throw std::runtime_error("keyring: " + path.string() +
                             " is not a regular file");
  }
  if (st.st_uid != ::geteuid()) {
    ::close(fd);
    throw std::runtime_error("keyring: " + path.string() +
                             " is not owned by the current user");
  }
  if ((st.st_mode & 077) != 0) {
    ::close(fd);
    throw std::runtime_error("keyring: " + path.string() +
                             " is group/world-accessible; run chmod 600 on it");
  }
  if (st.st_size != static_cast<off_t>(kKeyBytes)) {
    ::close(fd);
    throw std::runtime_error("keyring: " + path.string() +
                             " has the wrong size (expected 32 bytes)");
  }
  std::string key(kKeyBytes, '\0');
  std::size_t got = 0;
  while (got < kKeyBytes) {
    auto n = ::read(fd, key.data() + got, kKeyBytes - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      throw std::runtime_error("keyring: read failed: " +
                               std::string(std::strerror(errno)));
    }
    if (n == 0) {
      ::close(fd);
      throw std::runtime_error("keyring: short read from " + path.string());
    }
    got += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return key;
}

}  // namespace

std::string load_or_create(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);

  int fd = ::open(path.c_str(),
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (errno == EEXIST) return load(path);
    throw std::runtime_error("keyring: cannot create " + path.string() + ": " +
                             std::strerror(errno));
  }

  unsigned char key[kKeyBytes];
  try {
    popy::rand::fill(key, kKeyBytes);
  } catch (...) {
    ::close(fd);
    ::unlink(path.c_str());
    throw;
  }
  std::size_t off = 0;
  while (off < kKeyBytes) {
    auto n = ::write(fd, key + off, kKeyBytes - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      ::unlink(path.c_str());
      throw std::runtime_error("keyring: write failed: " +
                               std::string(std::strerror(e)));
    }
    off += static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) {
    int e = errno;
    ::close(fd);
    ::unlink(path.c_str());
    throw std::runtime_error("keyring: fsync failed: " +
                             std::string(std::strerror(e)));
  }
  ::close(fd);
  return std::string(reinterpret_cast<const char*>(key), kKeyBytes);
}

}  // namespace popy::keyring
