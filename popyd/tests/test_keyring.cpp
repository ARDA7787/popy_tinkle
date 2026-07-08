// Keyring: auto-create semantics and the fail-closed load checks.

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/keyring.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

int main() {
  fs::path tmp = fs::temp_directory_path() / "popy_test_keyring";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  POPY_RUN("create on first use: 0600, 32 bytes, stable across loads") {
    auto p = tmp / "popy.key";
    auto k1 = popy::keyring::load_or_create(p);
    POPY_EXPECT_EQ(k1.size(), static_cast<size_t>(32));

    struct stat st {};
    POPY_EXPECT_EQ(::stat(p.c_str(), &st), 0);
    POPY_EXPECT_EQ(static_cast<int>(st.st_mode & 07777), 0600);
    POPY_EXPECT_EQ(static_cast<long long>(st.st_size), 32LL);

    auto k2 = popy::keyring::load_or_create(p);
    POPY_EXPECT(k1 == k2);
  };

  POPY_RUN("distinct paths get distinct keys") {
    auto k1 = popy::keyring::load_or_create(tmp / "a.key");
    auto k2 = popy::keyring::load_or_create(tmp / "b.key");
    POPY_EXPECT(k1 != k2);
  };

  POPY_RUN("reject group/world-accessible key") {
    auto p = tmp / "loose.key";
    popy::keyring::load_or_create(p);
    ::chmod(p.c_str(), 0644);
    POPY_EXPECT_THROW(popy::keyring::load_or_create(p));
  };

  POPY_RUN("reject wrong-size key") {
    auto p = tmp / "short.key";
    { std::ofstream f(p); f << "too short"; }
    ::chmod(p.c_str(), 0600);
    POPY_EXPECT_THROW(popy::keyring::load_or_create(p));
  };

  POPY_RUN("reject symlinked key") {
    auto real = tmp / "real.key";
    popy::keyring::load_or_create(real);
    auto link = tmp / "link.key";
    fs::create_symlink(real, link);
    POPY_EXPECT_THROW(popy::keyring::load_or_create(link));
  };

  fs::remove_all(tmp);
  return exit_code();
}
