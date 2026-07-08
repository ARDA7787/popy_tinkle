#include "textsafe.h"

#include <cstdint>

namespace popy::textsafe {

bool looks_binary(std::string_view head) {
  return head.find('\0') != std::string_view::npos;
}

std::string sanitize_utf8(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  static constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD

  std::size_t i = 0;
  while (i < in.size()) {
    unsigned char c = static_cast<unsigned char>(in[i]);

    if (c < 0x80) {
      if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F)) {
        out.push_back(static_cast<char>(c));
      }
      // other C0 controls and DEL are dropped
      ++i;
      continue;
    }

    std::size_t len;
    std::uint32_t v;
    if ((c & 0xE0) == 0xC0)      { len = 2; v = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07u; }
    else {  // stray continuation byte or invalid lead (0xFE/0xFF)
      out += kReplacement;
      ++i;
      continue;
    }

    if (i + len > in.size()) {  // truncated sequence at end of input:
      out += kReplacement;      // one U+FFFD for the maximal subpart (WHATWG)
      ++i;
      while (i < in.size() &&
             (static_cast<unsigned char>(in[i]) & 0xC0) == 0x80) {
        ++i;
      }
      continue;
    }

    bool valid = true;
    for (std::size_t k = 1; k < len; ++k) {
      unsigned char cc = static_cast<unsigned char>(in[i + k]);
      if ((cc & 0xC0) != 0x80) { valid = false; break; }
      v = (v << 6) | (cc & 0x3Fu);
    }
    if (!valid) {  // bad continuation: consume the lead byte only (W3C style)
      out += kReplacement;
      ++i;
      continue;
    }

    bool overlong = (len == 2 && v < 0x80) || (len == 3 && v < 0x800) ||
                    (len == 4 && v < 0x10000);
    bool surrogate = (v >= 0xD800 && v <= 0xDFFF);
    if (overlong || surrogate || v > 0x10FFFF) {
      out += kReplacement;
      i += len;
      continue;
    }

    if (v >= 0x80 && v <= 0x9F) {  // C1 control — drop
      i += len;
      continue;
    }

    out.append(in.substr(i, len));
    i += len;
  }
  return out;
}

}  // namespace popy::textsafe
