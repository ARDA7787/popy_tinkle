#include "test_main.h"

namespace popy::test {

namespace {
int g_pass = 0;
int g_fail = 0;
}  // namespace

void note_pass() { ++g_pass; }
void note_fail() { ++g_fail; }

int exit_code() {
  std::fprintf(stderr, "  %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace popy::test
