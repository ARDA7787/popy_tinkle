#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>

#include "core/hash.h"
#include "test_main.h"

using namespace popy::test;

namespace {

// SHA-256 via system openssl, returned as lowercase hex.
std::string sha256_via_openssl(const std::string& path) {
  std::string cmd = "openssl dgst -sha256 -r '" + path + "' 2>/dev/null";
  std::FILE* p = ::popen(cmd.c_str(), "r");
  if (!p) return "";
  char buf[256];
  std::string line;
  if (std::fgets(buf, sizeof(buf), p)) line = buf;
  ::pclose(p);
  // openssl -r emits "<hex> *<path>\n"
  auto sp = line.find(' ');
  return (sp == std::string::npos) ? "" : line.substr(0, sp);
}

}  // namespace

int main() {
  POPY_RUN("known vectors") {
    POPY_EXPECT_EQ(
        popy::hash::sha256_hex(""),
        std::string(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    POPY_EXPECT_EQ(
        popy::hash::sha256_hex("abc"),
        std::string(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  };

  POPY_RUN("streaming chunked == one-shot") {
    std::string s(1 << 20, '\0');
    std::mt19937 rng(42);
    for (auto& c : s) c = static_cast<char>(rng());
    auto whole = popy::hash::sha256_hex(s);

    popy::hash::Sha256Streaming st;
    for (size_t i = 0; i < s.size(); i += 7919 /* odd chunk */) {
      auto n = std::min<size_t>(7919, s.size() - i);
      st.update(s.data() + i, n);
    }
    POPY_EXPECT_EQ(st.digest_hex(), whole);
  };

  POPY_RUN("cross-check against openssl") {
    std::string tmp = "/tmp/popy_hash_test.bin";
    {
      std::ofstream f(tmp, std::ios::binary);
      std::mt19937 rng(7);
      for (int i = 0; i < 1024 * 100; ++i) f.put(static_cast<char>(rng()));
    }
    auto ours = [&]() {
      std::ifstream f(tmp, std::ios::binary);
      popy::hash::Sha256Streaming st;
      char buf[8192];
      while (f) {
        f.read(buf, sizeof(buf));
        st.update(buf, static_cast<size_t>(f.gcount()));
      }
      return st.digest_hex();
    }();
    auto theirs = sha256_via_openssl(tmp);
    if (theirs.empty()) {
      std::fprintf(stderr, "  (openssl unavailable, skipping cross-check)\n");
    } else {
      POPY_EXPECT_EQ(ours, theirs);
    }
    std::remove(tmp.c_str());
  };

  return exit_code();
}
