#include "core/naming.h"
#include "test_main.h"

using namespace popy::naming;
using namespace popy::test;

int main() {
  POPY_RUN("suffix append") {
    POPY_EXPECT_EQ(popy_name("report.pdf"), std::string("report.pdf_popy"));
    POPY_EXPECT_EQ(popy_name("a"),          std::string("a_popy"));
  };

  POPY_RUN("strip suffix") {
    POPY_EXPECT_EQ(strip_suffix("report.pdf_popy"), std::string("report.pdf"));
    POPY_EXPECT_EQ(strip_suffix("no-suffix"),       std::string("no-suffix"));
  };

  POPY_RUN("sanitize illegal chars") {
    POPY_EXPECT_EQ(sanitize("foo/bar.txt"),         std::string("bar.txt"));
    POPY_EXPECT_EQ(sanitize("a<b>c.txt"),           std::string("a_b_c.txt"));
    POPY_EXPECT_EQ(sanitize("a:b\"c|d?e*f.txt"),
                   std::string("a_b_c_d_e_f.txt"));
  };

  POPY_RUN("sanitize collapses whitespace") {
    // Regular ASCII space (0x20) is collapsed/trimmed.
    POPY_EXPECT_EQ(sanitize("  many   spaces  .txt  "),
                   std::string("many spaces .txt"));
    // Control whitespace (\t \n \r etc, < 0x20) is "illegal" per ILLEGAL_RE
    // in src/lib/opfs/fs.ts and gets replaced with '_' BEFORE the \s+ collapse
    // ever sees it. Mirrors the TS impl exactly.
    POPY_EXPECT_EQ(sanitize("\t\nweird\twhitespace\n"),
                   std::string("__weird_whitespace_"));
  };

  POPY_RUN("windows reserved names") {
    POPY_EXPECT_EQ(sanitize("CON"),     std::string("_CON"));
    POPY_EXPECT_EQ(sanitize("nul.txt"), std::string("_nul.txt"));
    POPY_EXPECT_EQ(sanitize("com1"),    std::string("_com1"));
  };

  POPY_RUN("empty fallback") {
    POPY_EXPECT_EQ(sanitize(""),            std::string("download"));
    POPY_EXPECT_EQ(sanitize("///"),         std::string("download"));
    POPY_EXPECT_EQ(sanitize("   "),         std::string("download"));
  };

  POPY_RUN("length cap at 230") {
    std::string huge(500, 'a');
    POPY_EXPECT(sanitize(huge).size() == 230);
  };

  POPY_RUN("glob match basics") {
    POPY_EXPECT(glob_match("*_popy",  "report.pdf_popy"));
    POPY_EXPECT(!glob_match("*_popy", "report_popy/x"));
    POPY_EXPECT(glob_match("*.tmp",   "x.tmp"));
    POPY_EXPECT(!glob_match("*.tmp",  "x.tmpfoo"));
    POPY_EXPECT(glob_match(".*",      ".popy-ignore"));
    POPY_EXPECT(!glob_match(".*",     "no-leading-dot"));
  };

  POPY_RUN("glob match recursive **") {
    POPY_EXPECT(glob_match("**/popy-released/**",
                           "subdir/popy-released/x.txt"));
    POPY_EXPECT(glob_match("**/popy-released/**",
                           "deep/path/popy-released/y/z.bin"));
    POPY_EXPECT(!glob_match("**/popy-released/**",
                            "released/x"));
  };

  return exit_code();
}
