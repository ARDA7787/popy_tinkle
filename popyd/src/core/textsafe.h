// Text sanitization for content leaving quarantine as "text".
//
// UTF-8-aware at the codepoint level: byte-level C1 stripping would corrupt
// UTF-8 continuation bytes (every codepoint U+0080..U+07FF has a continuation
// byte in 0x80..0xBF). Invalid sequences become U+FFFD; C0 controls are
// dropped except \n and \t; C1 codepoints (U+0080..U+009F) are dropped.
#pragma once
#include <string>
#include <string_view>

namespace popy::textsafe {

// True when `head` (the first bytes of a file) looks like binary content —
// i.e. contains a NUL byte. Callers refuse "text" reads of such files.
bool looks_binary(std::string_view head);

// Sanitize arbitrary bytes into safe UTF-8 text:
//   - valid multi-byte UTF-8 passes through unchanged
//   - invalid bytes/sequences (incl. overlong forms, surrogates, > U+10FFFF)
//     become U+FFFD
//   - C0 controls are dropped except '\n' and '\t' (CRLF becomes LF); DEL too
//   - C1 codepoints U+0080..U+009F are dropped (terminal escape smuggling)
std::string sanitize_utf8(std::string_view in);

}  // namespace popy::textsafe
