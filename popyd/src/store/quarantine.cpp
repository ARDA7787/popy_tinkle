#include "store/quarantine.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/attest.h"
#include "core/hash.h"
#include "core/keyring.h"
#include "core/log.h"
#include "core/mime.h"
#include "core/naming.h"
#include "core/paths.h"
#include "core/sidecar.h"
#include "core/uuid.h"

namespace fs = std::filesystem;

namespace popy::store {

namespace {

bool ends_with(std::string_view a, std::string_view b) {
  return a.size() >= b.size() &&
         a.substr(a.size() - b.size()) == b;
}

// Walk `root` recursively, collect every `*.meta.json` (skipping `.new`).
void collect_in(const fs::path& root, std::vector<Entry>& out) {
  std::error_code ec;
  if (!fs::exists(root, ec)) return;
  for (auto it = fs::recursive_directory_iterator(
           root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& p = it->path();
    if (!it->is_regular_file(ec)) continue;
    auto name = p.filename().string();
    if (!ends_with(name, ".meta.json")) continue;
    if (ends_with(name, ".meta.json.new")) continue;
    try {
      auto rec = popy::sidecar::read(p);
      Entry e;
      e.sidecar_path = p;
      // popy_path is sidecar_path without the trailing ".meta.json".
      auto s = p.string();
      e.popy_path = fs::path(s.substr(0, s.size() - std::string(".meta.json").size()));
      e.record = std::move(rec);
      struct stat dst {};
      e.data_missing = (::lstat(e.popy_path.c_str(), &dst) != 0);
      out.push_back(std::move(e));
    } catch (const std::exception& e) {
      popy::log::warn(std::string("skipping malformed sidecar ") +
                      p.string() + ": " + e.what());
    }
  }
}

}  // namespace

std::vector<Entry> list(const popy::config::Config& cfg) {
  std::vector<Entry> out;
  std::set<fs::path> seen;
  auto add_root = [&](const fs::path& r) {
    fs::path canon = fs::weakly_canonical(r);
    if (seen.insert(canon).second) collect_in(canon, out);
  };
  for (const auto& w : cfg.watch_dirs) add_root(w);
  add_root(cfg.stage_dir);

  std::sort(out.begin(), out.end(),
            [](const Entry& a, const Entry& b) {
              return a.record.createdAt > b.record.createdAt;
            });
  return out;
}

Entry resolve(const popy::config::Config& cfg, const std::string& query) {
  if (query.empty()) {
    throw std::runtime_error("popy: empty <file> argument");
  }
  auto all = list(cfg);

  // Pass 1: exact UUID match.
  for (const auto& e : all) if (e.record.id == query) return e;

  // Pass 2: UUID prefix match (>= 4 chars).
  if (query.size() >= 4 && query.find('/') == std::string::npos) {
    std::vector<const Entry*> hits;
    for (const auto& e : all) {
      if (e.record.id.compare(0, query.size(), query) == 0) hits.push_back(&e);
    }
    if (hits.size() == 1) return *hits[0];
    if (hits.size() > 1) {
      throw std::runtime_error("popy: ambiguous UUID prefix '" + query + "'");
    }
  }

  // Pass 3 & 4: basename matches.
  std::vector<const Entry*> by_orig, by_popy;
  for (const auto& e : all) {
    if (e.record.originalFilename == query) by_orig.push_back(&e);
    if (e.popy_path.filename().string() == query) by_popy.push_back(&e);
  }
  if (by_orig.size() == 1) return *by_orig[0];
  if (by_popy.size() == 1) return *by_popy[0];
  if (by_orig.size() + by_popy.size() > 1) {
    throw std::runtime_error("popy: ambiguous filename '" + query + "'");
  }

  throw std::runtime_error("popy: no match for '" + query + "'");
}

void release(const Entry& e, const fs::path& out, bool force,
             const fs::path& key_file) {
  auto key = popy::keyring::load_or_create(
      key_file.empty() ? popy::paths::key_file() : key_file);

  // Gate the exit: sidecar HMAC + size now; the content hash is checked on
  // the very bytes we copy (prehash=false — hash once, during the copy).
  auto readable = open_verified(e, key, /*prehash=*/false);

  std::error_code ec;
  fs::path out_dir = out.parent_path().empty() ? fs::path(".")
                                               : out.parent_path();
  fs::create_directories(out_dir, ec);

  // Stage into a same-directory tmp so the final step is atomic on one
  // filesystem. O_EXCL|O_NOFOLLOW: nobody can pre-plant this name usefully.
  fs::path tmp = out_dir / (".popy-release." + e.record.id + ".tmp");
  int tfd = ::open(tmp.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (tfd < 0) {
    throw std::runtime_error("popy release: cannot create " + tmp.string() +
                             ": " + std::strerror(errno));
  }
  auto fail_tmp = [&](const std::string& msg) {
    ::close(tfd);
    ::unlink(tmp.c_str());
    throw std::runtime_error("popy release: " + msg);
  };

  // Copy + re-hash the copied bytes.
  popy::hash::Sha256Streaming hasher;
  char buf[64 * 1024];
  while (true) {
    auto n = ::read(readable.fd(), buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      fail_tmp(std::string("read failed: ") + std::strerror(errno));
    }
    hasher.update(buf, static_cast<size_t>(n));
    size_t off = 0;
    while (off < static_cast<size_t>(n)) {
      auto w = ::write(tfd, buf + off, static_cast<size_t>(n) - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        fail_tmp(std::string("write failed: ") + std::strerror(errno));
      }
      off += static_cast<size_t>(w);
    }
  }
  if (hasher.digest_hex() != e.record.sha256) {
    fail_tmp("content hash mismatch — the quarantined file was modified; "
             "refusing to release (quarantine left intact)");
  }
  if (::fsync(tfd) != 0) {
    fail_tmp(std::string("fsync failed: ") + std::strerror(errno));
  }
  if (::fchmod(tfd, 0644) != 0) {
    fail_tmp(std::string("fchmod 0644 failed: ") + std::strerror(errno));
  }
  ::close(tfd);

  // Commit. Neither linkat(flags=0) nor rename() follows a symlink planted
  // at the destination — the confused-deputy arbitrary-overwrite is dead.
  if (force) {
    if (::rename(tmp.c_str(), out.c_str()) != 0) {
      int err = errno;
      ::unlink(tmp.c_str());
      throw std::runtime_error(std::string("popy release: rename failed: ") +
                               std::strerror(err));
    }
  } else {
    if (::linkat(AT_FDCWD, tmp.c_str(), AT_FDCWD, out.c_str(), 0) != 0) {
      int err = errno;
      ::unlink(tmp.c_str());
      if (err == EEXIST) {
        throw std::runtime_error("popy release: refusing to overwrite " +
                                 out.string() + " (use --force)");
      }
      throw std::runtime_error(std::string("popy release: linkat failed: ") +
                               std::strerror(err));
    }
    ::unlink(tmp.c_str());  // drop the second name; `out` holds the inode
  }
  // Best-effort durability for the new directory entry.
  int dfd = ::open(out_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }

  // Update + re-sign sidecar status before unlinking — if removal fails,
  // the sidecar still verifies and reflects what we know.
  popy::sidecar::Record r = e.record;
  r.status     = "released";
  r.resolvedAt = popy::sidecar::now_ms();
  popy::attest::sign(r, e.popy_path.filename().string(), key);
  popy::sidecar::write_atomic(e.sidecar_path, r);

  // Now remove the quarantined file + sidecar + per-id dir.
  fs::remove(e.popy_path, ec);
  fs::remove(e.sidecar_path, ec);
  fs::remove(e.popy_path.parent_path(), ec);  // best-effort empty-dir cleanup

  popy::log::info("released " + e.record.id + " -> " + out.string());
}

void remove(const Entry& e) {
  std::error_code ec;
  // unlink file (chmod 0000'd files unlink fine — we own them).
  fs::remove(e.popy_path, ec);
  fs::remove(e.sidecar_path, ec);
  fs::remove(e.popy_path.parent_path(), ec);
  popy::log::info("deleted " + e.record.id);
}

// ---------------------------------------------------------------------------
// Mode B: in-place quarantine of a file that already exists on disk.
// ---------------------------------------------------------------------------

namespace {

// Pick "<dir>/<name>_popy", or "<dir>/<name>_popy.1", "...2" on collision.
fs::path choose_dest(const fs::path& dir, std::string_view base_popy_name) {
  fs::path p = dir / std::string(base_popy_name);
  std::error_code ec;
  if (!fs::exists(p, ec)) return p;
  for (int i = 1; i < 1000; ++i) {
    fs::path q = dir / (std::string(base_popy_name) + "." + std::to_string(i));
    if (!fs::exists(q, ec)) return q;
  }
  throw std::runtime_error("quarantine: too many collisions for " +
                           std::string(base_popy_name));
}

}  // namespace

QuarantineResult quarantine_existing_file(const fs::path& path,
                                          const fs::path& key_file) {
  // Refuse symlinks and non-regular files. lstat() so we don't follow.
  struct stat lst {};
  if (::lstat(path.c_str(), &lst) != 0) {
    throw std::runtime_error("quarantine: lstat " + path.string() + ": " +
                             std::strerror(errno));
  }
  if (!S_ISREG(lst.st_mode)) {
    throw std::runtime_error("quarantine: not a regular file: " + path.string());
  }
  // Open read-fd O_NOFOLLOW|O_NONBLOCK: O_NONBLOCK means a FIFO swapped in
  // between lstat and open can't hang the daemon; the fstat below catches it.
  int fd = ::open(path.c_str(),
                  O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("quarantine: open " + path.string() + ": " +
                             std::strerror(errno));
  }
  // TOCTOU check: the thing we opened must be the exact inode we lstat'd.
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error(std::string("quarantine: fstat failed: ") +
                             std::strerror(errno));
  }
  if (!S_ISREG(st.st_mode) || st.st_dev != lst.st_dev ||
      st.st_ino != lst.st_ino) {
    ::close(fd);
    throw std::runtime_error("quarantine: file changed identity during "
                             "capture, refusing: " + path.string());
  }

  // Lockdown FIRST: from here no other process can newly open these bytes.
  // We keep reading through our already-open fd (permissions are checked at
  // open time only).
  bool locked = (::fchmod(fd, 0000) == 0);
  if (!locked) {
    popy::log::warn(std::string("quarantine: fchmod 0000 failed: ") +
                    std::strerror(errno));
  }

  fs::path dir = path.parent_path();
  std::string original = path.filename().string();
  std::string popy_name = popy::naming::popy_name(original);
  fs::path dest = choose_dest(dir, popy_name);

  // The atomic commit: from this point onward the file's on-disk name is
  // "_popy" and OS handlers cannot dispatch on the original extension.
  if (::rename(path.c_str(), dest.c_str()) != 0) {
    int e = errno;
    // Don't brick the user's file: restore its original mode before bailing.
    if (locked && ::fchmod(fd, lst.st_mode & 07777) != 0) {
      popy::log::warn(std::string("quarantine: mode restore failed: ") +
                      std::strerror(errno));
    }
    ::close(fd);
    throw std::runtime_error("quarantine: rename " + path.string() +
                             " -> " + dest.string() + ": " + std::strerror(e));
  }

  // Hash by reading the now-renamed, already-0000 file via the fd we hold.
  popy::hash::Sha256Streaming hasher;
  char buf[64 * 1024];
  std::int64_t total = 0;
  while (true) {
    auto n = ::read(fd, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      throw std::runtime_error(std::string("quarantine: read failed: ") +
                               std::strerror(errno));
    }
    hasher.update(buf, static_cast<size_t>(n));
    total += n;
  }
  ::close(fd);

  popy::sidecar::Record r;
  r.id               = popy::uuid::v4();
  r.diskPath         = dest.string();
  r.originalFilename = popy::naming::sanitize(original);
  r.sizeBytes        = total;
  r.mime             = popy::mime::guess_from_extension(original);
  r.sha256           = hasher.digest_hex();
  r.path             = "fallback";
  r.status           = "stored";
  r.createdAt        = popy::sidecar::now_ms();
  r.note             = std::string("captured by popyd watcher");
  r.agent            = "popyd/0.1";
  r.origin           = "watcher";

  auto key = popy::keyring::load_or_create(
      key_file.empty() ? popy::paths::key_file() : key_file);
  popy::attest::sign(r, dest.filename().string(), key);

  fs::path sidecar_path = popy::sidecar::sidecar_path_for(dest);
  popy::sidecar::write_atomic(sidecar_path, r);

  popy::log::info("quarantined " + path.filename().string() + " -> " +
                  dest.string());

  QuarantineResult out;
  out.popy_path    = dest;
  out.sidecar_path = sidecar_path;
  out.record       = std::move(r);
  return out;
}

// ---------------------------------------------------------------------------
// Startup repair pass.
// ---------------------------------------------------------------------------

namespace {

// "report.pdf_popy" / "report.pdf_popy.3" → "report.pdf"; nullopt when the
// name is not a quarantine data-file name.
std::optional<std::string> original_from_popy_name(std::string_view name) {
  constexpr std::string_view kSuffix = "_popy";
  if (ends_with(name, kSuffix)) {
    return std::string(name.substr(0, name.size() - kSuffix.size()));
  }
  auto pos = name.rfind("_popy.");
  if (pos == std::string_view::npos) return std::nullopt;
  auto digits = name.substr(pos + 6);
  if (digits.empty()) return std::nullopt;
  for (char c : digits) {
    if (c < '0' || c > '9') return std::nullopt;
  }
  return std::string(name.substr(0, pos));
}

// Adopt one orphaned data file: lock to 0000, hash through the fd, sign,
// write the sidecar. Best-effort — throws propagate to the caller's catch.
void adopt_orphan(const fs::path& path, const std::string& original,
                  const std::string& key) {
  struct stat lst {};
  if (::lstat(path.c_str(), &lst) != 0 || !S_ISREG(lst.st_mode)) return;

  int fd = ::open(path.c_str(),
                  O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("repair: open " + path.string() + ": " +
                             std::strerror(errno));
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_dev != lst.st_dev || st.st_ino != lst.st_ino) {
    ::close(fd);
    throw std::runtime_error("repair: file changed identity: " + path.string());
  }
  if (::fchmod(fd, 0000) != 0) {
    popy::log::warn(std::string("repair: fchmod 0000 failed: ") +
                    std::strerror(errno));
  }

  popy::hash::Sha256Streaming hasher;
  char buf[64 * 1024];
  std::int64_t total = 0;
  while (true) {
    auto n = ::read(fd, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      throw std::runtime_error(std::string("repair: read failed: ") +
                               std::strerror(errno));
    }
    hasher.update(buf, static_cast<size_t>(n));
    total += n;
  }
  ::close(fd);

  popy::sidecar::Record r;
  r.id               = popy::uuid::v4();
  r.diskPath         = path.string();
  r.originalFilename = popy::naming::sanitize(original);
  r.sizeBytes        = total;
  r.mime             = popy::mime::guess_from_extension(original);
  r.sha256           = hasher.digest_hex();
  r.path             = "fallback";
  r.status           = "stored";
  r.createdAt        = popy::sidecar::now_ms();
  r.note             = std::string("adopted by popyd repair pass");
  r.agent            = "popyd/0.1";
  r.origin           = "watcher";
  popy::attest::sign(r, path.filename().string(), key);
  popy::sidecar::write_atomic(popy::sidecar::sidecar_path_for(path), r);
}

}  // namespace

// ---------------------------------------------------------------------------
// Verified reads out of quarantine.
// ---------------------------------------------------------------------------

QuarantineReadable::QuarantineReadable(const fs::path& path) : path_(path) {
  struct stat lst {};
  if (::lstat(path_.c_str(), &lst) != 0) {
    throw std::runtime_error("popy read: lstat " + path_.string() + ": " +
                             std::strerror(errno));
  }
  if (!S_ISREG(lst.st_mode)) {
    throw std::runtime_error("popy read: not a regular file: " +
                             path_.string());
  }
  if (::chmod(path_.c_str(), 0400) != 0) {
    throw std::runtime_error("popy read: chmod 0400 failed: " +
                             std::string(std::strerror(errno)));
  }
  fd_ = ::open(path_.c_str(),
               O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    int e = errno;
    ::chmod(path_.c_str(), 0000);
    throw std::runtime_error("popy read: open failed: " +
                             std::string(std::strerror(e)));
  }
  // Close the readable window immediately — we keep access via the fd.
  if (::fchmod(fd_, 0000) != 0) {
    int e = errno;
    ::close(fd_);
    fd_ = -1;
    ::chmod(path_.c_str(), 0000);
    throw std::runtime_error("popy read: fchmod 0000 failed: " +
                             std::string(std::strerror(e)));
  }
  // TOCTOU check: the fd must be the exact inode we lstat'd, and regular.
  struct stat st {};
  if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_dev != lst.st_dev || st.st_ino != lst.st_ino) {
    ::close(fd_);
    fd_ = -1;
    ::chmod(path_.c_str(), 0000);
    throw std::runtime_error(
        "popy read: file changed identity during open, refusing: " +
        path_.string());
  }
}

QuarantineReadable::QuarantineReadable(QuarantineReadable&& other) noexcept
    : path_(std::move(other.path_)), fd_(other.fd_) {
  other.fd_ = -1;
  other.path_.clear();
}

QuarantineReadable::~QuarantineReadable() {
  if (fd_ >= 0) {
    // Already fchmod(0000)'d at open; nothing to restore. Just release.
    ::close(fd_);
    fd_ = -1;
  }
}

QuarantineReadable open_verified(const Entry& e, const std::string& key,
                                 bool prehash) {
  // 1. Sidecar integrity, pinned to the actual on-disk name.
  popy::attest::verify_or_throw(e.record, e.popy_path.filename().string(),
                                key);

  // 2. Minimal-window open.
  QuarantineReadable readable(e.popy_path);

  // 3. Size gate.
  struct stat st {};
  if (::fstat(readable.fd(), &st) != 0) {
    throw std::runtime_error(std::string("popy read: fstat failed: ") +
                             std::strerror(errno));
  }
  if (st.st_size != e.record.sizeBytes) {
    throw std::runtime_error(
        "popy: content size mismatch for '" + e.record.id + "' (sidecar says " +
        std::to_string(e.record.sizeBytes) + " bytes, file has " +
        std::to_string(st.st_size) + ") — the file was modified while "
        "quarantined; refusing");
  }

  // 4. Content hash gate.
  if (prehash) {
    popy::hash::Sha256Streaming hasher;
    char buf[64 * 1024];
    while (true) {
      auto n = ::read(readable.fd(), buf, sizeof(buf));
      if (n == 0) break;
      if (n < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error(std::string("popy read: read failed: ") +
                                 std::strerror(errno));
      }
      hasher.update(buf, static_cast<size_t>(n));
    }
    if (hasher.digest_hex() != e.record.sha256) {
      throw std::runtime_error(
          "popy: content hash mismatch for '" + e.record.id +
          "' — the file was modified while quarantined; refusing");
    }
    if (::lseek(readable.fd(), 0, SEEK_SET) != 0) {
      throw std::runtime_error("popy read: rewind failed");
    }
  }
  return readable;
}

RepairStats repair(const popy::config::Config& cfg, const fs::path& key_file) {
  RepairStats stats;
  auto key = popy::keyring::load_or_create(
      key_file.empty() ? popy::paths::key_file() : key_file);

  std::set<fs::path> seen;
  std::vector<fs::path> roots;
  for (const auto& w : cfg.watch_dirs) {
    if (seen.insert(fs::weakly_canonical(w)).second) roots.push_back(w);
  }
  if (seen.insert(fs::weakly_canonical(cfg.stage_dir)).second) {
    roots.push_back(cfg.stage_dir);
  }

  const std::int64_t now_s = static_cast<std::int64_t>(::time(nullptr));
  constexpr std::int64_t kStaleSeconds = 10 * 60;

  for (const auto& root : roots) {
    std::error_code ec;
    if (!fs::exists(root, ec)) continue;
    // Collect first, mutate after — writing sidecars while iterating the
    // same tree is asking for iterator surprises.
    std::vector<fs::path> sidecars, datafiles;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec)) continue;
      auto name = it->path().filename().string();
      if (ends_with(name, ".meta.json.new")) continue;
      if (ends_with(name, ".meta.json")) { sidecars.push_back(it->path()); continue; }
      if (ends_with(name, ".part")) continue;
      if (original_from_popy_name(name)) datafiles.push_back(it->path());
    }

    for (const auto& sc : sidecars) {
      auto s = sc.string();
      fs::path data(s.substr(0, s.size() - std::string(".meta.json").size()));
      struct stat st {};
      if (::lstat(data.c_str(), &st) == 0) continue;  // data present — fine
      fs::path part = data;
      part += ".part";
      if (::lstat(part.c_str(), &st) == 0) continue;  // fetch in flight
      struct stat scs {};
      if (::lstat(sc.c_str(), &scs) != 0) continue;
      if (now_s - static_cast<std::int64_t>(scs.st_mtime) < kStaleSeconds) {
        continue;  // young — may be a fetch between sidecar-write and commit
      }
      std::error_code ec2;
      fs::remove(sc, ec2);
      if (!ec2) {
        ++stats.gc_sidecars;
        popy::log::info("repair: GC'd stale sidecar " + sc.string());
        fs::remove(sc.parent_path(), ec2);  // best-effort empty-dir cleanup
      }
    }

    for (const auto& df : datafiles) {
      std::error_code ec2;
      if (fs::exists(popy::sidecar::sidecar_path_for(df), ec2)) continue;
      auto original = original_from_popy_name(df.filename().string());
      if (!original || original->empty()) continue;
      try {
        adopt_orphan(df, *original, key);
        ++stats.adopted;
        popy::log::info("repair: adopted orphan " + df.string());
      } catch (const std::exception& e) {
        popy::log::warn(std::string("repair: ") + e.what());
      }
    }
  }
  return stats;
}

}  // namespace popy::store
