#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/config.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

int main() {
  fs::path tmp = fs::temp_directory_path() / "popy_test_config";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  POPY_RUN("defaults are sane") {
    auto c = popy::config::defaults();
    POPY_EXPECT(c.enabled);
    POPY_EXPECT(!c.watch_dirs.empty());
    POPY_EXPECT(!c.exclude.empty());
  };

  POPY_RUN("excluded patterns work") {
    auto c = popy::config::defaults();
    POPY_EXPECT(popy::config::excluded(c, "report.pdf_popy"));
    POPY_EXPECT(popy::config::excluded(c, "x.crdownload"));
    POPY_EXPECT(popy::config::excluded(c, ".popy-ignore"));
    POPY_EXPECT(!popy::config::excluded(c, "report.pdf"));
  };

  POPY_RUN("missing file returns defaults") {
    auto c = popy::config::load(tmp / "nope.toml");
    POPY_EXPECT(c.enabled);
  };

  POPY_RUN("parse minimal toml") {
    auto p = tmp / "ok.toml";
    {
      std::ofstream f(p);
      f << "enabled = false\n"
           "watch_dirs = [\"~/.popy-stage\"]\n"
           "stage_dir  = \"~/.popy-stage\"\n"
           "exclude    = [\"*.tmp\"]\n";
    }
    auto c = popy::config::load(p);
    POPY_EXPECT(!c.enabled);
    POPY_EXPECT_EQ(c.exclude.size(), size_t{1});
  };

  POPY_RUN("parse error throws") {
    auto p = tmp / "broken.toml";
    {
      std::ofstream f(p);
      f << "not = valid = toml";
    }
    POPY_EXPECT_THROW(popy::config::load(p));
  };

  POPY_RUN("watch_dir outside home is rejected") {
    auto p = tmp / "evil.toml";
    {
      std::ofstream f(p);
      f << "watch_dirs = [\"/\"]\n"
           "stage_dir  = \"~/.popy-stage\"\n";
    }
    POPY_EXPECT_THROW(popy::config::load(p));
  };

  fs::remove_all(tmp);
  return exit_code();
}
