#include "mime.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace popy::mime {

namespace {

std::string lower(std::string_view s) {
  std::string r(s);
  std::transform(r.begin(), r.end(), r.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return r;
}

// Small extension table — covers what we'd actually fetch.
const std::unordered_map<std::string, std::string>& ext_table() {
  static const std::unordered_map<std::string, std::string> t = {
      {"pdf",  "application/pdf"},
      {"png",  "image/png"},
      {"jpg",  "image/jpeg"},
      {"jpeg", "image/jpeg"},
      {"gif",  "image/gif"},
      {"webp", "image/webp"},
      {"svg",  "image/svg+xml"},
      {"txt",  "text/plain"},
      {"md",   "text/markdown"},
      {"json", "application/json"},
      {"yaml", "application/yaml"},
      {"yml",  "application/yaml"},
      {"toml", "application/toml"},
      {"zip",  "application/zip"},
      {"gz",   "application/gzip"},
      {"tar",  "application/x-tar"},
      {"html", "text/html"},
      {"htm",  "text/html"},
      {"csv",  "text/csv"},
      {"xml",  "application/xml"},
      {"wasm", "application/wasm"},
      {"js",   "application/javascript"},
      {"css",  "text/css"},
  };
  return t;
}

bool starts_with(std::string_view a, std::string_view b) {
  return a.size() >= b.size() && a.substr(0, b.size()) == b;
}

}  // namespace

std::string guess_from_extension(std::string_view filename) {
  auto dot = filename.find_last_of('.');
  if (dot == std::string_view::npos) return "application/octet-stream";
  auto ext = lower(filename.substr(dot + 1));
  if (auto it = ext_table().find(ext); it != ext_table().end()) {
    return it->second;
  }
  return "application/octet-stream";
}

std::string normalize(std::string_view mime) {
  auto semi = mime.find(';');
  auto core = (semi == std::string_view::npos) ? mime : mime.substr(0, semi);
  // trim
  while (!core.empty() && std::isspace(static_cast<unsigned char>(core.front())))
    core.remove_prefix(1);
  while (!core.empty() && std::isspace(static_cast<unsigned char>(core.back())))
    core.remove_suffix(1);
  return lower(core);
}

bool magic_consistent(std::string_view head, std::string_view expected_mime) {
  if (expected_mime.empty()) return true;
  std::string m = normalize(expected_mime);
  if (m.empty() || m == "application/octet-stream") return true;

  auto eq = [&](const unsigned char* sig, size_t n) -> bool {
    return head.size() >= n &&
           std::memcmp(head.data(), sig, n) == 0;
  };

  // PDF: "%PDF-" within the first 1024 bytes by spec, but in our 16-byte
  // probe we accept the canonical leading form.
  if (m == "application/pdf") {
    static const unsigned char k[] = {'%', 'P', 'D', 'F', '-'};
    return eq(k, sizeof(k));
  }
  if (m == "image/png") {
    static const unsigned char k[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return eq(k, sizeof(k));
  }
  if (m == "image/jpeg") {
    static const unsigned char k[] = {0xFF, 0xD8, 0xFF};
    return eq(k, sizeof(k));
  }
  if (m == "image/gif") {
    return eq(reinterpret_cast<const unsigned char*>("GIF87a"), 6) ||
           eq(reinterpret_cast<const unsigned char*>("GIF89a"), 6);
  }
  if (m == "image/webp") {
    return head.size() >= 12 &&
           std::memcmp(head.data(), "RIFF", 4) == 0 &&
           std::memcmp(head.data() + 8, "WEBP", 4) == 0;
  }
  if (m == "application/zip") {
    static const unsigned char k[] = {'P', 'K', 0x03, 0x04};
    static const unsigned char e[] = {'P', 'K', 0x05, 0x06};  // empty archive
    return eq(k, sizeof(k)) || eq(e, sizeof(e));
  }
  if (m == "application/gzip") {
    static const unsigned char k[] = {0x1F, 0x8B};
    return eq(k, sizeof(k));
  }
  // Text-ish: accept anything ASCII-printable in the probe window.
  if (starts_with(m, "text/") || m == "application/json" ||
      m == "application/yaml" || m == "application/toml" ||
      m == "application/xml" || m == "application/javascript") {
    for (char ch : head) {
      auto c = static_cast<unsigned char>(ch);
      if (c < 0x09) return false;
      if (c >= 0x0E && c < 0x20 && c != 0x1B) return false;
    }
    return true;
  }
  // Unknown family: accept (we make no claim).
  return true;
}

}  // namespace popy::mime
