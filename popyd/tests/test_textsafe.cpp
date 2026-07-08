// textsafe: codepoint-level UTF-8 sanitization and the binary probe.

#include <string>

#include "core/textsafe.h"
#include "test_main.h"

using namespace popy::test;
using popy::textsafe::looks_binary;
using popy::textsafe::sanitize_utf8;

namespace {
const std::string kFFFD = "\xEF\xBF\xBD";
}

int main() {
  POPY_RUN("plain ASCII passes through") {
    POPY_EXPECT_EQ(sanitize_utf8("hello world 123 !@#"),
                   std::string("hello world 123 !@#"));
  };

  POPY_RUN("newline and tab survive; CR and other C0 are dropped") {
    POPY_EXPECT_EQ(sanitize_utf8("a\nb\tc"), std::string("a\nb\tc"));
    POPY_EXPECT_EQ(sanitize_utf8("a\r\nb"),  std::string("a\nb"));
    POPY_EXPECT_EQ(sanitize_utf8("a\x1b[31mred\x07"), std::string("a[31mred"));
    POPY_EXPECT_EQ(sanitize_utf8(std::string("a\x7f") + "b"), std::string("ab"));
  };

  POPY_RUN("valid multibyte UTF-8 passes through unchanged") {
    std::string s = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";  // café € 😀
    POPY_EXPECT_EQ(sanitize_utf8(s), s);
  };

  POPY_RUN("C1 codepoints are dropped, not byte-mangled") {
    // U+0085 NEL, U+009B CSI — the terminal-escape smuggling vector.
    POPY_EXPECT_EQ(sanitize_utf8("a\xC2\x85z"), std::string("az"));
    POPY_EXPECT_EQ(sanitize_utf8("a\xC2\x9Bz"), std::string("az"));
    // ...but U+00A0 (just past C1) survives.
    POPY_EXPECT_EQ(sanitize_utf8("a\xC2\xA0z"), std::string("a\xC2\xA0z"));
  };

  POPY_RUN("invalid bytes become U+FFFD") {
    POPY_EXPECT_EQ(sanitize_utf8("a\xFFz"), "a" + kFFFD + "z");
    POPY_EXPECT_EQ(sanitize_utf8("a\x80z"), "a" + kFFFD + "z");  // stray cont.
  };

  POPY_RUN("overlong encodings are rejected") {
    // 0xC0 0xAF is an overlong '/', the classic filter-bypass encoding.
    POPY_EXPECT_EQ(sanitize_utf8("\xC0\xAF"), kFFFD);
    POPY_EXPECT_EQ(sanitize_utf8("\xE0\x80\xAF"), kFFFD);
  };

  POPY_RUN("surrogates and out-of-range are rejected") {
    POPY_EXPECT_EQ(sanitize_utf8("\xED\xA0\x80"), kFFFD);      // U+D800
    POPY_EXPECT_EQ(sanitize_utf8("\xF4\x90\x80\x80"), kFFFD);  // > U+10FFFF
  };

  POPY_RUN("truncated sequence at end of input") {
    POPY_EXPECT_EQ(sanitize_utf8("ab\xE2\x82"), "ab" + kFFFD);
  };

  POPY_RUN("bad continuation consumes only the lead byte") {
    // \xC3 followed by 'x': lead is invalid, 'x' must survive.
    POPY_EXPECT_EQ(sanitize_utf8("\xC3x"), kFFFD + "x");
  };

  POPY_RUN("looks_binary") {
    POPY_EXPECT(looks_binary(std::string_view("ab\0cd", 5)));
    POPY_EXPECT(!looks_binary("plain text"));
    POPY_EXPECT(!looks_binary(""));
  };

  return exit_code();
}
