#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/safe_fs.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

int main() {
  fs::path tmp = fs::temp_directory_path() / "popy_test_safe_fs";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  POPY_RUN("is_safe_rel basics") {
    using popy::safe_fs::is_safe_rel;
    POPY_EXPECT(is_safe_rel("foo.txt"));
    POPY_EXPECT(is_safe_rel("a/b/c.txt"));
    POPY_EXPECT(!is_safe_rel("/abs"));
    POPY_EXPECT(!is_safe_rel("../escape"));
    POPY_EXPECT(!is_safe_rel("a/../b"));
    POPY_EXPECT(!is_safe_rel(""));
    POPY_EXPECT(!is_safe_rel(std::string_view("nul\0byte", 8)));
  };

  POPY_RUN("create_at within root") {
    int rfd = popy::safe_fs::open_root(tmp);
    int fd  = popy::safe_fs::create_at(rfd, "a/b/file.txt", 0600);
    ::write(fd, "hi", 2);
    ::close(fd);
    POPY_EXPECT(fs::exists(tmp / "a" / "b" / "file.txt"));
    ::close(rfd);
  };

  POPY_RUN("symlink at leaf is rejected") {
    fs::path link_dir = tmp / "linkdir";
    fs::create_directories(link_dir);
    fs::path target = tmp / "outside.txt";
    {
      std::ofstream f(target);
      f << "do not open me";
    }
    fs::create_symlink(target, link_dir / "leak");

    int rfd = popy::safe_fs::open_root(link_dir);
    bool threw = false;
    try {
      int fd = popy::safe_fs::open_read_at(rfd, "leak");
      (void)fd;
    } catch (const std::exception&) { threw = true; }
    POPY_EXPECT(threw);
    ::close(rfd);
  };

  POPY_RUN("rejects ../ traversal") {
    int rfd = popy::safe_fs::open_root(tmp);
    bool threw = false;
    try { popy::safe_fs::create_at(rfd, "../escape.txt"); }
    catch (const std::exception&) { threw = true; }
    POPY_EXPECT(threw);
    ::close(rfd);
  };

  fs::remove_all(tmp);
  return exit_code();
}
