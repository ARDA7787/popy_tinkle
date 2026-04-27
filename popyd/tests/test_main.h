// Minimal zero-dependency test harness.
//
//   POPY_RUN("name") { ...assertions... };
//
// Inside the body, POPY_EXPECT*/POPY_EXPECT_THROW use `return;` to abort the
// test on failure. Because the body is a lambda, that returns from the test
// only — main() keeps running so subsequent tests still execute.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>
#include <utility>

namespace popy::test {

void note_pass();
void note_fail();
int exit_code();

// Glue that lets `POPY_RUN("name") { body };` work cleanly. operator+= takes
// a lambda on its right-hand side, prints the name, marks a pass (a failed
// POPY_EXPECT* will toggle this to a fail), and runs the body.
struct Runner {
  const char* name;
};

template <typename F>
inline void operator+=(Runner r, F&& body) {
  std::fprintf(stderr, "  test %s\n", r.name);
  note_pass();
  std::forward<F>(body)();
}

}  // namespace popy::test

#define POPY_RUN(name) ::popy::test::Runner{name} += [&]

#define POPY_EXPECT(cond)                                                 \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                   #cond);                                                \
      ::popy::test::note_fail();                                          \
      return;                                                             \
    }                                                                     \
  } while (0)

#define POPY_EXPECT_EQ(a, b)                                              \
  do {                                                                    \
    auto _ax = (a);                                                       \
    auto _bx = (b);                                                       \
    if (!(_ax == _bx)) {                                                  \
      std::ostringstream _o;                                              \
      _o << "FAIL " << __FILE__ << ":" << __LINE__                        \
         << ": " << #a << " == " << #b                                    \
         << " (got " << _ax << " vs " << _bx << ")";                      \
      std::fprintf(stderr, "%s\n", _o.str().c_str());                     \
      ::popy::test::note_fail();                                          \
      return;                                                             \
    }                                                                     \
  } while (0)

#define POPY_EXPECT_THROW(stmt)                                           \
  do {                                                                    \
    bool _threw = false;                                                  \
    try { stmt; } catch (const std::exception&) { _threw = true; }        \
    if (!_threw) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: expected exception from %s\n",    \
                   __FILE__, __LINE__, #stmt);                            \
      ::popy::test::note_fail();                                          \
      return;                                                             \
    }                                                                    \
  } while (0)
