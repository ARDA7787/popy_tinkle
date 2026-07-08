// QuarantineWriter: mode 0000 from the first byte, no final name before
// commit, nothing left behind after abort. On Linux both the O_TMPFILE and
// the forced .part branch run; elsewhere .part is the only branch.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "store/quarantine_file.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

namespace {

void write_all(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    auto n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) { if (errno == EINTR) continue; return; }
    off += static_cast<size_t>(n);
  }
}

std::string slurp_via_fd(const fs::path& p) {
  // The committed file is mode 0000; read it the way the store does — chmod
  // up, open, chmod back (we own it; this is a test helper, not the product
  // path).
  ::chmod(p.c_str(), 0400);
  std::ifstream in(p, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
  ::chmod(p.c_str(), 0000);
  return s;
}

void run_branch(const fs::path& tmp, bool force_part, const char* label) {
  POPY_RUN((std::string("commit: 0000 final file, no leftovers [") + label +
            "]").c_str()) {
    fs::path dir = tmp / (std::string("commit_") + label);
    fs::create_directories(dir);
    fs::path final_path = dir / "file.bin_popy";

    popy::store::QuarantineWriter w(final_path, force_part);
    write_all(w.fd(), "hello quarantine");

    // Pre-commit: the final name must not exist; if the in-flight file has a
    // name at all it is the .part and it is mode 0000.
    POPY_EXPECT(!fs::exists(final_path));
    fs::path part = final_path;
    part += ".part";
    if (w.anonymous()) {
      POPY_EXPECT(!fs::exists(part));
    } else {
      struct stat st {};
      POPY_EXPECT_EQ(::lstat(part.c_str(), &st), 0);
      POPY_EXPECT_EQ(static_cast<int>(st.st_mode & 07777), 0);
    }

    w.sync();
    w.commit();

    struct stat st {};
    POPY_EXPECT_EQ(::lstat(final_path.c_str(), &st), 0);
    POPY_EXPECT(S_ISREG(st.st_mode));
    POPY_EXPECT_EQ(static_cast<int>(st.st_mode & 07777), 0);
    POPY_EXPECT(!fs::exists(part));
    POPY_EXPECT_EQ(slurp_via_fd(final_path), std::string("hello quarantine"));
  };

  POPY_RUN((std::string("abort leaves nothing [") + label + "]").c_str()) {
    fs::path dir = tmp / (std::string("abort_") + label);
    fs::create_directories(dir);
    fs::path final_path = dir / "gone.bin_popy";
    {
      popy::store::QuarantineWriter w(final_path, force_part);
      write_all(w.fd(), "doomed bytes");
      w.abort();
    }
    POPY_EXPECT(fs::is_empty(dir));
  };

  POPY_RUN((std::string("destructor without commit leaves nothing [") + label +
            "]").c_str()) {
    fs::path dir = tmp / (std::string("dtor_") + label);
    fs::create_directories(dir);
    {
      popy::store::QuarantineWriter w(dir / "x_popy", force_part);
      write_all(w.fd(), "abc");
      // no commit — throw path
    }
    POPY_EXPECT(fs::is_empty(dir));
  };

  POPY_RUN((std::string("double commit throws [") + label + "]").c_str()) {
    fs::path dir = tmp / (std::string("dbl_") + label);
    fs::create_directories(dir);
    popy::store::QuarantineWriter w(dir / "y_popy", force_part);
    write_all(w.fd(), "z");
    w.commit();
    POPY_EXPECT_THROW(w.commit());
  };
}

}  // namespace

int main() {
  fs::path tmp = fs::temp_directory_path() / "popy_test_qwriter";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  run_branch(tmp, /*force_part=*/true, "part");
#if defined(__linux__)
  run_branch(tmp, /*force_part=*/false, "tmpfile");
#endif

  POPY_RUN("refuses to clobber an existing .part") {
    fs::path dir = tmp / "excl";
    fs::create_directories(dir);
    { std::ofstream f(dir / "z_popy.part"); f << "stale"; }
    POPY_EXPECT_THROW(popy::store::QuarantineWriter(dir / "z_popy", true));
  };

  fs::remove_all(tmp);
  return exit_code();
}
