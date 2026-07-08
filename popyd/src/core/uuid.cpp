#include "uuid.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "rand.h"

namespace popy::uuid {

std::string v4() {
  std::array<unsigned char, 16> b{};
  popy::rand::fill(b.data(), b.size());

  // RFC 4122 §4.4 — version 4, variant 1.
  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);

  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return std::string(buf, 36);
}

bool is_valid_or_prefix(const std::string& s) {
  if (s.empty() || s.size() > 36) return false;
  static constexpr int kHyphenAt[] = {8, 13, 18, 23};
  for (size_t i = 0; i < s.size(); ++i) {
    bool hyphen_pos = (i == 8 || i == 13 || i == 18 || i == 23);
    (void)kHyphenAt;
    if (hyphen_pos) {
      if (s[i] != '-') return false;
    } else {
      if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
  }
  return true;
}

}  // namespace popy::uuid
