#include "render/sandbox.h"

#include <sys/resource.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <seccomp.h>
#include <sys/prctl.h>
#elif defined(__APPLE__)
extern "C" int sandbox_init(const char* profile, unsigned long flags,
                            char** errorbuf);
extern "C" void sandbox_free_error(char* errorbuf);
#endif

namespace popy::render {

namespace {

bool set_limit(int resource, rlim_t value) {
  struct rlimit lim {};
  lim.rlim_cur = value;
  lim.rlim_max = value;
  return ::setrlimit(resource, &lim) == 0;
}

}  // namespace

bool set_limits() {
  constexpr rlim_t kMemory = 512ULL * 1024ULL * 1024ULL;
  bool memory_ok = set_limit(RLIMIT_AS, kMemory);
#if defined(__APPLE__)
  // Some macOS versions reject RLIMIT_AS for hardened/runtime processes. Keep
  // the isolation path available and rely on CPU, output, and fd caps there.
  if (!memory_ok) {
    memory_ok = true;
  }
#endif
  return memory_ok &&
         set_limit(RLIMIT_CPU, 10) &&
         set_limit(RLIMIT_FSIZE, 0) &&
         set_limit(RLIMIT_NOFILE, 16);
}

bool enter_sandbox() {
#if defined(__linux__)
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return false;

  scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);
  if (ctx == nullptr) return false;

  bool ok = true;
  auto allow = [&](int syscall_nr) {
    if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, syscall_nr, 0) != 0) ok = false;
  };

  allow(SCMP_SYS(read));
  allow(SCMP_SYS(write));
  allow(SCMP_SYS(close));
  allow(SCMP_SYS(exit));
  allow(SCMP_SYS(exit_group));
  allow(SCMP_SYS(brk));
  allow(SCMP_SYS(mmap));
  allow(SCMP_SYS(munmap));
  allow(SCMP_SYS(fstat));
  allow(SCMP_SYS(lseek));
  allow(SCMP_SYS(mprotect));

  if (ok && seccomp_load(ctx) != 0) ok = false;
  seccomp_release(ctx);
  return ok;
#elif defined(__APPLE__)
  static constexpr char kProfile[] =
      "(version 1)\n"
      "(allow default)\n"
      "(deny network*)\n"
      "(deny file-write-create)\n"
      "(deny file-write-unlink)\n"
      "(deny file-write-mode)\n"
      "(deny file-write-setugid)\n";
  char* error = nullptr;
  if (sandbox_init(kProfile, 0, &error) != 0) {
    if (error != nullptr) {
      std::fprintf(stderr, "sandbox_init: %s\n", error);
      sandbox_free_error(error);
    } else {
      std::fprintf(stderr, "sandbox_init failed: %s\n", std::strerror(errno));
    }
    return false;
  }
  return true;
#else
  return true;
#endif
}

}  // namespace popy::render
