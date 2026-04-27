#include "net/fetch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "core/hash.h"
#include "core/log.h"
#include "core/mime.h"
#include "core/naming.h"
#include "core/safe_fs.h"
#include "core/sidecar.h"
#include "core/uuid.h"

namespace fs = std::filesystem;

namespace popy::net {

namespace {

constexpr size_t kMagicProbe = 16;

struct Sink {
  int fd = -1;
  popy::hash::Sha256Streaming hasher;
  std::int64_t received = 0;
  std::int64_t max_bytes = 0;
  std::array<unsigned char, kMagicProbe> probe{};
  size_t probe_len = 0;
  bool probe_done = false;
  std::string expected_mime;
  std::string error;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* s = static_cast<Sink*>(userdata);
  size_t n = size * nmemb;
  if (n == 0) return 0;

  // Magic-byte probe — accumulate up to kMagicProbe bytes across however many
  // transport chunks libcurl feeds us, then verify against expected_mime.
  if (!s->probe_done && s->probe_len < kMagicProbe) {
    size_t take = std::min(n, kMagicProbe - s->probe_len);
    std::memcpy(s->probe.data() + s->probe_len, ptr, take);
    s->probe_len += take;
    if (s->probe_len >= kMagicProbe) {
      auto head = std::string_view(
          reinterpret_cast<const char*>(s->probe.data()), s->probe_len);
      if (!popy::mime::magic_consistent(head, s->expected_mime)) {
        s->error = "magic-byte mismatch for expected mime '" +
                   s->expected_mime + "'";
        return 0;  // signals abort to libcurl
      }
      s->probe_done = true;
    }
  }

  s->received += static_cast<std::int64_t>(n);
  if (s->max_bytes > 0 && s->received > s->max_bytes) {
    s->error = "response exceeded max_bytes";
    return 0;
  }

  s->hasher.update(ptr, n);

  size_t off = 0;
  while (off < n) {
    auto w = ::write(s->fd, ptr + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      s->error = std::string("write failed: ") + std::strerror(errno);
      return 0;
    }
    off += static_cast<size_t>(w);
  }
  return n;
}

// Extract a likely filename from URL path (after last /), stripping query.
std::string filename_from_url(const std::string& url) {
  auto q = url.find_first_of("?#");
  std::string path = (q == std::string::npos) ? url : url.substr(0, q);
  auto slash = path.find_last_of('/');
  std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  if (name.empty()) name = "download";
  return name;
}

std::string host_from_url(const std::string& url) {
  // Tiny manual parse — we don't link a URL lib for this.
  auto scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  auto host_start = scheme + 3;
  auto host_end = url.find_first_of("/?#", host_start);
  std::string host = url.substr(
      host_start, (host_end == std::string::npos)
                      ? std::string::npos
                      : host_end - host_start);
  // Strip user:pass@
  auto at = host.find('@');
  if (at != std::string::npos) host = host.substr(at + 1);
  // Strip :port
  auto colon = host.find(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  return host;
}

[[noreturn]] void cleanup_and_throw(const fs::path& popy_path,
                                    const fs::path& sidecar_path,
                                    const std::string& msg) {
  std::error_code ec;
  fs::remove(popy_path, ec);
  fs::remove(sidecar_path, ec);
  // also try to remove the now-empty <uuid> dir
  fs::remove(popy_path.parent_path(), ec);
  throw std::runtime_error("popy fetch: " + msg);
}

}  // namespace

FetchResult fetch(const FetchOptions& opts) {
  if (opts.url.empty()) {
    throw std::runtime_error("popy fetch: empty URL");
  }
  if (opts.url.find("https://") != 0 && opts.url.find("http://") != 0) {
    throw std::runtime_error("popy fetch: only http/https URLs are accepted");
  }

  // Resolve original filename: --out wins, else URL basename.
  std::string original = opts.out_name.value_or(filename_from_url(opts.url));
  std::string popy_basename = popy::naming::popy_name(original);

  // Build the destination path: <stage>/<uuid>/<name>_popy
  std::string id = popy::uuid::v4();
  fs::path target_dir = opts.stage_dir / id;
  fs::path popy_path = target_dir / popy_basename;
  fs::path sidecar_path = popy::sidecar::sidecar_path_for(popy_path);

  // Create dirs (mkdir -p), open fd with O_EXCL|O_NOFOLLOW.
  std::error_code ec;
  fs::create_directories(opts.stage_dir, ec);
  fs::create_directories(target_dir, ec);
  // 0700 on the per-id dir so peer users can't read in-flight bytes.
  ::chmod(target_dir.c_str(), 0700);

  int fd = ::open(popy_path.c_str(),
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw std::runtime_error("popy fetch: cannot create " + popy_path.string() +
                             ": " + std::strerror(errno));
  }

  Sink sink;
  sink.fd = fd;
  sink.max_bytes = opts.max_bytes;
  if (opts.expected_mime) sink.expected_mime = *opts.expected_mime;

  CURL* curl = curl_easy_init();
  if (!curl) {
    ::close(fd);
    cleanup_and_throw(popy_path, sidecar_path, "curl_easy_init failed");
  }

  curl_easy_setopt(curl, CURLOPT_URL, opts.url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);  // 4xx/5xx → error
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "popy/0.1");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, opts.verbose ? 1L : 0L);
  // Cap response size at the libcurl level too (defence in depth).
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                   static_cast<curl_off_t>(opts.max_bytes));

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  char* effective_url_cstr = nullptr;
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url_cstr);
  std::string effective_url = effective_url_cstr ? effective_url_cstr : opts.url;
  char* server_mime_cstr = nullptr;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &server_mime_cstr);
  std::string server_mime = server_mime_cstr ? server_mime_cstr : "";
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    std::string what = sink.error.empty() ? curl_easy_strerror(rc) : sink.error;
    ::close(fd);
    cleanup_and_throw(popy_path, sidecar_path,
                      "transport error: " + what +
                          (http_code ? " (HTTP " + std::to_string(http_code) + ")"
                                     : ""));
  }

  // If we hit EOF before the probe completed, validate whatever we have.
  if (!sink.probe_done && sink.probe_len > 0 && !sink.expected_mime.empty()) {
    auto head = std::string_view(
        reinterpret_cast<const char*>(sink.probe.data()), sink.probe_len);
    if (!popy::mime::magic_consistent(head, sink.expected_mime)) {
      ::close(fd);
      cleanup_and_throw(popy_path, sidecar_path,
                        "magic-byte mismatch (short body) for expected mime '" +
                            sink.expected_mime + "'");
    }
  }

  if (::fsync(fd) != 0) {
    ::close(fd);
    cleanup_and_throw(popy_path, sidecar_path,
                      std::string("fsync failed: ") + std::strerror(errno));
  }
  // chmod 0000 — even if a path traversal exists later, the file can't be
  // opened until popy release restores 0644.
  if (::fchmod(fd, 0000) != 0) {
    popy::log::warn(std::string("fchmod 0000 failed: ") + std::strerror(errno));
  }
  ::close(fd);

  // Build sidecar.
  popy::sidecar::Record r;
  r.id               = id;
  r.opfsPath         = "";
  r.diskPath         = popy_path.string();
  r.originalFilename = popy::naming::sanitize(original);
  r.sourceUrl        = effective_url;
  r.sourceHost       = host_from_url(effective_url);
  r.sizeBytes        = sink.received;
  r.mime = !server_mime.empty()
               ? popy::mime::normalize(server_mime)
               : popy::mime::guess_from_extension(original);
  r.sha256    = sink.hasher.digest_hex();
  r.path      = "fallback";
  r.status    = "stored";
  r.createdAt = popy::sidecar::now_ms();
  r.note      = std::string("fetched via popy fetch");
  r.agent     = "popyd/0.1";
  r.origin    = "fetch";

  try {
    popy::sidecar::write_atomic(sidecar_path, r);
  } catch (const std::exception& e) {
    cleanup_and_throw(popy_path, sidecar_path,
                      std::string("sidecar write failed: ") + e.what());
  }

  popy::log::info("fetched " + opts.url + " -> " + popy_path.string());

  FetchResult out;
  out.record       = std::move(r);
  out.popy_path    = popy_path;
  out.sidecar_path = sidecar_path;
  return out;
}

}  // namespace popy::net
