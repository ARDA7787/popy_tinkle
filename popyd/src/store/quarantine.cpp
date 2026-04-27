#include "store/quarantine.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

#include "core/hash.h"
#include "core/log.h"
#include "core/mime.h"
#include "core/naming.h"
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

void release(const Entry& e, const fs::path& out, bool force) {
  std::error_code ec;
  if (fs::exists(out, ec) && !force) {
    throw std::runtime_error("popy release: refusing to overwrite " +
                             out.string() + " (use --force)");
  }
  // Make popy_path readable for the copy (we chmod 0000'd it on capture).
  if (::chmod(e.popy_path.c_str(), 0600) != 0) {
    throw std::runtime_error("popy release: chmod 0600 failed: " +
                             std::string(std::strerror(errno)));
  }
  fs::create_directories(out.parent_path(), ec);

  // copy_file with overwrite if --force.
  fs::copy_options copts = fs::copy_options::none;
  if (force) copts = fs::copy_options::overwrite_existing;
  fs::copy_file(e.popy_path, out, copts, ec);
  if (ec) {
    throw std::runtime_error("popy release: copy failed: " + ec.message());
  }
  ::chmod(out.c_str(), 0644);

  // Update sidecar status before unlinking — if the next steps fail, the
  // sidecar reflects what we know.
  popy::sidecar::Record r = e.record;
  r.status     = "released";
  r.resolvedAt = popy::sidecar::now_ms();
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

QuarantineResult quarantine_existing_file(const fs::path& path) {
  // Refuse symlinks and non-regular files. lstat() so we don't follow.
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) {
    throw std::runtime_error("quarantine: lstat " + path.string() + ": " +
                             std::strerror(errno));
  }
  if (!S_ISREG(st.st_mode)) {
    throw std::runtime_error("quarantine: not a regular file: " + path.string());
  }
  // Open read-fd O_NOFOLLOW. We hash through this fd after renaming, so
  // the rename can't change which bytes we hash.
  int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("quarantine: open " + path.string() + ": " +
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
    ::close(fd);
    throw std::runtime_error("quarantine: rename " + path.string() +
                             " -> " + dest.string() + ": " + std::strerror(e));
  }

  // Hash by reading the now-renamed file via the fd we already hold.
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

  // chmod 0000 — even if path-traversal exists, file can't be opened until
  // popy release restores 0644.
  if (::fchmod(fd, 0000) != 0) {
    popy::log::warn(std::string("quarantine: fchmod 0000 failed: ") +
                    std::strerror(errno));
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

}  // namespace popy::store
