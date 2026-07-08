#include "sidecar.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "nlohmann_json.hpp"

using nlohmann::json;
namespace fs = std::filesystem;

namespace popy::sidecar {

namespace {

template <typename T>
T require(const json& j, const char* key) {
  if (!j.contains(key)) {
    throw std::runtime_error(std::string("sidecar: missing key '") + key + "'");
  }
  return j.at(key).get<T>();
}

template <typename T>
T optional_or(const json& j, const char* key, T fallback) {
  if (!j.contains(key) || j.at(key).is_null()) return fallback;
  return j.at(key).get<T>();
}

std::optional<std::string> opt_str(const json& j, const char* key) {
  if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
  return j.at(key).get<std::string>();
}

std::optional<std::int64_t> opt_i64(const json& j, const char* key) {
  if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
  return j.at(key).get<std::int64_t>();
}

}  // namespace

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

fs::path sidecar_path_for(const fs::path& popy_path) {
  return popy_path.string() + ".meta.json";
}

Record read(const fs::path& sidecar_path) {
  std::ifstream in(sidecar_path);
  if (!in) {
    throw std::runtime_error("sidecar: cannot open " + sidecar_path.string());
  }
  json j;
  try {
    in >> j;
  } catch (const std::exception& e) {
    throw std::runtime_error("sidecar: invalid JSON in " +
                             sidecar_path.string() + ": " + e.what());
  }

  Record r;
  r.id               = require<std::string>(j, "id");
  r.opfsPath         = optional_or<std::string>(j, "opfsPath", "");
  r.diskPath         = optional_or<std::string>(j, "diskPath", "");
  r.originalFilename = require<std::string>(j, "originalFilename");
  r.sourceUrl        = optional_or<std::string>(j, "sourceUrl", "");
  r.sourceHost       = optional_or<std::string>(j, "sourceHost", "");
  r.referrer         = opt_str(j, "referrer");
  r.tabUrl           = opt_str(j, "tabUrl");
  r.sizeBytes        = optional_or<std::int64_t>(j, "sizeBytes", 0);
  r.mime             = optional_or<std::string>(j, "mime", "application/octet-stream");
  r.sha256           = optional_or<std::string>(j, "sha256", "");
  r.path             = optional_or<std::string>(j, "path", "fallback");
  r.status           = optional_or<std::string>(j, "status", "stored");
  r.createdAt        = require<std::int64_t>(j, "createdAt");
  r.resolvedAt       = opt_i64(j, "resolvedAt");
  r.note             = opt_str(j, "note");

  r.schemaVersion    = optional_or<int>(j, "schemaVersion", kSchemaVersion);
  r.agent            = optional_or<std::string>(j, "agent",  "popyd/0.1");
  r.origin           = optional_or<std::string>(j, "origin", "fetch");
  r.sig              = opt_str(j, "sig");
  r.sigAlg           = opt_str(j, "sigAlg");
  return r;
}

void write_atomic(const fs::path& sidecar_path, const Record& r) {
  json j = {
      {"schemaVersion",    r.schemaVersion},
      {"id",               r.id},
      {"opfsPath",         r.opfsPath},
      {"diskPath",         r.diskPath},
      {"originalFilename", r.originalFilename},
      {"sourceUrl",        r.sourceUrl},
      {"sourceHost",       r.sourceHost},
      {"referrer",         r.referrer ? json(*r.referrer) : json(nullptr)},
      {"tabUrl",           r.tabUrl   ? json(*r.tabUrl)   : json(nullptr)},
      {"sizeBytes",        r.sizeBytes},
      {"mime",             r.mime},
      {"sha256",           r.sha256},
      {"path",             r.path},
      {"status",           r.status},
      {"createdAt",        r.createdAt},
      {"resolvedAt",       r.resolvedAt ? json(*r.resolvedAt) : json(nullptr)},
      {"note",             r.note     ? json(*r.note)     : json(nullptr)},
      {"agent",            r.agent},
      {"origin",           r.origin},
      {"sig",              r.sig    ? json(*r.sig)    : json(nullptr)},
      {"sigAlg",           r.sigAlg ? json(*r.sigAlg) : json(nullptr)},
  };
  const auto serialized = j.dump(2);

  fs::path tmp = sidecar_path;
  tmp += ".new";

  // O_EXCL: refuse to clobber a stale .new from a crashed write.
  ::unlink(tmp.c_str());  // best-effort cleanup; safe on ENOENT
  int fd = ::open(tmp.c_str(),
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0400);
  if (fd < 0) {
    throw std::runtime_error("sidecar: open " + tmp.string() + " failed");
  }
  size_t off = 0;
  while (off < serialized.size()) {
    auto n = ::write(fd, serialized.data() + off, serialized.size() - off);
    if (n < 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      throw std::runtime_error("sidecar: write failed");
    }
    off += static_cast<size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), sidecar_path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    throw std::runtime_error("sidecar: rename to final failed");
  }
}

}  // namespace popy::sidecar
