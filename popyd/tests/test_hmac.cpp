// HMAC-SHA256 against RFC 4231 test vectors (cases 1-4, 6, 7; case 5 is a
// truncated-output vector we don't implement), plus consteq behaviour.

#include <string>

#include "core/hmac.h"
#include "test_main.h"

using namespace popy::test;

namespace {

std::string bytes(unsigned char b, size_t n) {
  return std::string(n, static_cast<char>(b));
}

}  // namespace

int main() {
  POPY_RUN("RFC 4231 test case 1") {
    auto mac = popy::hmac::hmac_sha256_hex(bytes(0x0b, 20), "Hi There");
    POPY_EXPECT_EQ(
        mac,
        std::string("b0344c61d8db38535ca8afceaf0bf12b"
                    "881dc200c9833da726e9376c2e32cff7"));
  };

  POPY_RUN("RFC 4231 test case 2") {
    auto mac = popy::hmac::hmac_sha256_hex(
        "Jefe", "what do ya want for nothing?");
    POPY_EXPECT_EQ(
        mac,
        std::string("5bdcc146bf60754e6a042426089575c7"
                    "5a003f089d2739839dec58b964ec3843"));
  };

  POPY_RUN("RFC 4231 test case 3") {
    auto mac = popy::hmac::hmac_sha256_hex(bytes(0xaa, 20), bytes(0xdd, 50));
    POPY_EXPECT_EQ(
        mac,
        std::string("773ea91e36800e46854db8ebd09181a7"
                    "2959098b3ef8c122d9635514ced565fe"));
  };

  POPY_RUN("RFC 4231 test case 4") {
    std::string key;
    for (int i = 1; i <= 25; ++i) key.push_back(static_cast<char>(i));
    auto mac = popy::hmac::hmac_sha256_hex(key, bytes(0xcd, 50));
    POPY_EXPECT_EQ(
        mac,
        std::string("82558a389a443c0ea4cc819899f2083a"
                    "85f0faa3e578f8077a2e3ff46729665b"));
  };

  POPY_RUN("RFC 4231 test case 6 (key > block size)") {
    auto mac = popy::hmac::hmac_sha256_hex(
        bytes(0xaa, 131),
        "Test Using Larger Than Block-Size Key - Hash Key First");
    POPY_EXPECT_EQ(
        mac,
        std::string("60e431591ee0b67f0d8a26aacbf5b77f"
                    "8e0bc6213728c5140546040f0ee37f54"));
  };

  POPY_RUN("RFC 4231 test case 7 (key and data > block size)") {
    auto mac = popy::hmac::hmac_sha256_hex(
        bytes(0xaa, 131),
        "This is a test using a larger than block-size key and a larger than "
        "block-size data. The key needs to be hashed before being used by "
        "the HMAC algorithm.");
    POPY_EXPECT_EQ(
        mac,
        std::string("9b09ffa71b942fcb27635fbcd5b0e944"
                    "bfdc63644f0713938a7f51535c3a35e2"));
  };

  POPY_RUN("consteq") {
    POPY_EXPECT(popy::hmac::consteq("", ""));
    POPY_EXPECT(popy::hmac::consteq("abcd", "abcd"));
    POPY_EXPECT(!popy::hmac::consteq("abcd", "abce"));
    POPY_EXPECT(!popy::hmac::consteq("abcd", "abc"));
    POPY_EXPECT(!popy::hmac::consteq("", "x"));
    // Differ only in the first byte / only in the last byte.
    POPY_EXPECT(!popy::hmac::consteq("xbcd", "abcd"));
    POPY_EXPECT(!popy::hmac::consteq("abcx", "abcd"));
  };

  return exit_code();
}
