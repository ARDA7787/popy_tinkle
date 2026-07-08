// open_verified: the single gate every quarantine exit passes through.
// Tampered data or sidecar → refuse; unsigned → refuse with resign hint;
// on-disk mode stays/returns 0000 in every outcome including throws.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/attest.h"
#include "core/config.h"
#include "core/keyring.h"
#include "core/sidecar.h"
#include "store/quarantine.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

namespace {

fs::path g_tmp;
fs::path g_keyfile;

// Build a fresh signed quarantine entry containing `body`.
popy::store::Entry make_entry(const std::string& name,
                              const std::string& body) {
  fs::path dir = g_tmp / name;
  fs::create_directories(dir);
  fs::path plain = dir / (name + ".txt");
  { std::ofstream f(plain, std::ios::binary); f << body; }
  auto r = popy::store::quarantine_existing_file(plain, g_keyfile);
  popy::store::Entry e;
  e.popy_path    = r.popy_path;
  e.sidecar_path = r.sidecar_path;
  e.record       = r.record;
  return e;
}

int mode_of(const fs::path& p) {
  struct stat st {};
  if (::lstat(p.c_str(), &st) != 0) return -1;
  return static_cast<int>(st.st_mode & 07777);
}

// Overwrite the data file's bytes without changing anything else.
void tamper_data(const fs::path& popy_path, const std::string& new_body) {
  ::chmod(popy_path.c_str(), 0600);
  { std::ofstream f(popy_path, std::ios::binary | std::ios::trunc); f << new_body; }
  ::chmod(popy_path.c_str(), 0000);
}

std::string read_all(int fd) {
  std::string out;
  char buf[4096];
  while (true) {
    auto n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

}  // namespace

int main() {
  g_tmp = fs::temp_directory_path() / "popy_test_vread";
  fs::remove_all(g_tmp);
  fs::create_directories(g_tmp);
  g_keyfile = g_tmp / "popy.key";
  auto key = popy::keyring::load_or_create(g_keyfile);

  POPY_RUN("happy path: verified fd reads the bytes; mode stays 0000") {
    auto e = make_entry("happy", "trusted-ish bytes\n");
    auto readable = popy::store::open_verified(e, key);
    POPY_EXPECT_EQ(mode_of(e.popy_path), 0);  // 0000 even while fd is open
    POPY_EXPECT_EQ(read_all(readable.fd()), std::string("trusted-ish bytes\n"));
  };

  POPY_RUN("mode is 0000 after the readable is destroyed") {
    auto e = make_entry("after", "x");
    { auto readable = popy::store::open_verified(e, key); }
    POPY_EXPECT_EQ(mode_of(e.popy_path), 0);
  };

  POPY_RUN("tampered data (same size) → hash mismatch, mode back to 0000") {
    auto e = make_entry("tamperdata", "AAAAAAAAAA");
    tamper_data(e.popy_path, "BBBBBBBBBB");  // same length, different bytes
    bool threw = false;
    try { popy::store::open_verified(e, key); }
    catch (const std::exception& ex) {
      threw = std::string(ex.what()).find("hash mismatch") != std::string::npos;
    }
    POPY_EXPECT(threw);
    POPY_EXPECT_EQ(mode_of(e.popy_path), 0);
  };

  POPY_RUN("tampered data (grown) → size mismatch") {
    auto e = make_entry("tampersize", "short");
    tamper_data(e.popy_path, "much longer body than before");
    bool threw = false;
    try { popy::store::open_verified(e, key); }
    catch (const std::exception& ex) {
      threw = std::string(ex.what()).find("size mismatch") != std::string::npos;
    }
    POPY_EXPECT(threw);
    POPY_EXPECT_EQ(mode_of(e.popy_path), 0);
  };

  POPY_RUN("tampered sidecar field → bad signature, no override") {
    auto e = make_entry("tampersc", "body");
    e.record.sha256 = std::string(64, '0');  // attacker fixes up the hash
    bool threw = false;
    try { popy::store::open_verified(e, key); }
    catch (const std::exception& ex) {
      threw = std::string(ex.what()).find("FAILED") != std::string::npos;
    }
    POPY_EXPECT(threw);
    POPY_EXPECT_EQ(mode_of(e.popy_path), 0);  // never opened
  };

  POPY_RUN("sidecar transplant onto another file is refused") {
    auto a = make_entry("transA", "contents A");
    auto b = make_entry("transB", "contents A");  // same bytes, same size!
    // Graft b's (valid) record onto a's path: name pinning must catch it.
    popy::store::Entry grafted = a;
    grafted.record = b.record;
    POPY_EXPECT_THROW(popy::store::open_verified(grafted, key));
  };

  POPY_RUN("unsigned sidecar is refused with a resign hint") {
    auto e = make_entry("unsigned", "old bytes");
    e.record.sig.reset();
    e.record.sigAlg.reset();
    bool threw = false;
    try { popy::store::open_verified(e, key); }
    catch (const std::exception& ex) {
      threw = std::string(ex.what()).find("popy resign") != std::string::npos;
    }
    POPY_EXPECT(threw);
  };

  POPY_RUN("prehash=false skips the content gate but keeps sig+size gates") {
    auto e = make_entry("nopre", "0123456789");
    tamper_data(e.popy_path, "9876543210");  // same size
    // No prehash: opens fine (release() hashes during the copy instead).
    auto readable = popy::store::open_verified(e, key, /*prehash=*/false);
    POPY_EXPECT_EQ(read_all(readable.fd()), std::string("9876543210"));
    // But a bad signature still refuses.
    e.record.mime = "application/x-evil";
    POPY_EXPECT_THROW(popy::store::open_verified(e, key, false));
  };

  POPY_RUN("FIFO in place of the data file is refused, not hung") {
    auto e = make_entry("fifo", "will be replaced");
    fs::remove(e.popy_path);
    POPY_EXPECT_EQ(::mkfifo(e.popy_path.c_str(), 0000), 0);
    POPY_EXPECT_THROW(popy::store::open_verified(e, key));
  };

  POPY_RUN("symlinked data file is refused") {
    auto e = make_entry("symread", "real");
    fs::path victim = g_tmp / "victim.txt";
    { std::ofstream f(victim); f << "victim data"; }
    fs::remove(e.popy_path);
    fs::create_symlink(victim, e.popy_path);
    POPY_EXPECT_THROW(popy::store::open_verified(e, key));
  };

  fs::remove_all(g_tmp);
  return exit_code();
}
