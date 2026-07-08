// Verified symlink-safe release.

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/keyring.h"
#include "core/sidecar.h"
#include "store/quarantine.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

namespace {

fs::path g_tmp;
fs::path g_keyfile;

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

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void tamper_data(const fs::path& popy_path, const std::string& new_body) {
  ::chmod(popy_path.c_str(), 0600);
  { std::ofstream f(popy_path, std::ios::binary | std::ios::trunc); f << new_body; }
  ::chmod(popy_path.c_str(), 0000);
}

bool no_tmp_left(const fs::path& dir) {
  for (auto& de : fs::directory_iterator(dir)) {
    auto n = de.path().filename().string();
    if (n.rfind(".popy-release.", 0) == 0) return false;
  }
  return true;
}

}  // namespace

int main() {
  g_tmp = fs::temp_directory_path() / "popy_test_release";
  fs::remove_all(g_tmp);
  fs::create_directories(g_tmp);
  g_keyfile = g_tmp / "popy.key";
  popy::keyring::load_or_create(g_keyfile);

  POPY_RUN("happy path: dest 0644, bytes identical, quarantine gone") {
    auto e = make_entry("happy", "release me\n");
    fs::path dest_dir = g_tmp / "happy_out";
    fs::create_directories(dest_dir);
    fs::path dest = dest_dir / "released.txt";

    popy::store::release(e, dest, /*force=*/false, g_keyfile);

    POPY_EXPECT_EQ(mode_of(dest), 0644);
    POPY_EXPECT_EQ(slurp(dest), std::string("release me\n"));
    POPY_EXPECT(!fs::exists(e.popy_path));
    POPY_EXPECT(!fs::exists(e.sidecar_path));
    POPY_EXPECT(no_tmp_left(dest_dir));
  };

  POPY_RUN("tamper → throw, no dest, quarantine intact at 0000") {
    auto e = make_entry("tamper", "AAAAAAAAAA");
    tamper_data(e.popy_path, "BBBBBBBBBB");  // same size, hash gate must fire
    fs::path dest_dir = g_tmp / "tamper_out";
    fs::create_directories(dest_dir);
    fs::path dest = dest_dir / "evil.txt";

    POPY_EXPECT_THROW(popy::store::release(e, dest, false, g_keyfile));
    POPY_EXPECT(!fs::exists(dest));
    POPY_EXPECT(fs::exists(e.popy_path));
    POPY_EXPECT(fs::exists(e.sidecar_path));
    POPY_EXPECT_EQ(mode_of(e.popy_path), 0);
    POPY_EXPECT(no_tmp_left(dest_dir));
  };

  POPY_RUN("bad sidecar signature refuses the release") {
    auto e = make_entry("badsig", "bytes");
    e.record.originalFilename = "renamed.txt";  // breaks the HMAC
    POPY_EXPECT_THROW(
        popy::store::release(e, g_tmp / "badsig_out.txt", false, g_keyfile));
    POPY_EXPECT(fs::exists(e.popy_path));
  };

  POPY_RUN("existing destination refused without --force, tmp cleaned") {
    auto e = make_entry("exists", "new content");
    fs::path dest_dir = g_tmp / "exists_out";
    fs::create_directories(dest_dir);
    fs::path dest = dest_dir / "already.txt";
    { std::ofstream f(dest); f << "precious"; }

    bool threw = false;
    try { popy::store::release(e, dest, false, g_keyfile); }
    catch (const std::exception& ex) {
      threw = std::string(ex.what()).find("--force") != std::string::npos;
    }
    POPY_EXPECT(threw);
    POPY_EXPECT_EQ(slurp(dest), std::string("precious"));
    POPY_EXPECT(fs::exists(e.popy_path));  // quarantine intact
    POPY_EXPECT(no_tmp_left(dest_dir));
  };

  POPY_RUN("--force replaces an existing destination atomically") {
    auto e = make_entry("force", "the new bytes");
    fs::path dest_dir = g_tmp / "force_out";
    fs::create_directories(dest_dir);
    fs::path dest = dest_dir / "clobber.txt";
    { std::ofstream f(dest); f << "old"; }

    popy::store::release(e, dest, /*force=*/true, g_keyfile);
    POPY_EXPECT_EQ(slurp(dest), std::string("the new bytes"));
    POPY_EXPECT_EQ(mode_of(dest), 0644);
    POPY_EXPECT(no_tmp_left(dest_dir));
  };

  POPY_RUN("symlink planted at destination is not followed") {
    fs::path victim = g_tmp / "victim.txt";
    { std::ofstream f(victim); f << "victim data"; }

    // Without --force: refused outright.
    auto e1 = make_entry("symlink1", "attacker bytes");
    fs::path dest_dir = g_tmp / "sym_out";
    fs::create_directories(dest_dir);
    fs::path dest = dest_dir / "link.txt";
    fs::create_symlink(victim, dest);
    POPY_EXPECT_THROW(popy::store::release(e1, dest, false, g_keyfile));
    POPY_EXPECT_EQ(slurp(victim), std::string("victim data"));

    // With --force: the SYMLINK is replaced; the victim file is untouched.
    auto e2 = make_entry("symlink2", "forced bytes");
    popy::store::release(e2, dest, /*force=*/true, g_keyfile);
    POPY_EXPECT_EQ(slurp(victim), std::string("victim data"));
    POPY_EXPECT(!fs::is_symlink(dest));
    POPY_EXPECT_EQ(slurp(dest), std::string("forced bytes"));
  };

  fs::remove_all(g_tmp);
  return exit_code();
}
