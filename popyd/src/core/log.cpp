#include "log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace popy::log {

namespace {

std::mutex g_mu;
std::FILE* g_out = stderr;
Level g_min = Level::Info;

const char* level_str(Level l) {
  switch (l) {
    case Level::Debug: return "debug";
    case Level::Info:  return "info";
    case Level::Warn:  return "warn";
    case Level::Error: return "error";
  }
  return "?";
}

}  // namespace

void open_file(const std::string& path) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_out && g_out != stderr) {
    std::fclose(g_out);
    g_out = stderr;
  }
  if (!path.empty()) {
    std::FILE* f = std::fopen(path.c_str(), "ae");  // 'a' append, 'e' O_CLOEXEC
    if (f) {
      std::setvbuf(f, nullptr, _IOLBF, 0);  // line buffered
      g_out = f;
    }
  }
}

void set_level(Level l) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_min = l;
}

void write(Level l, std::string_view msg) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (l < g_min) return;
  using namespace std::chrono;
  auto now = system_clock::now();
  auto t = system_clock::to_time_t(now);
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
#if defined(__APPLE__) || defined(_POSIX_VERSION)
  ::localtime_r(&t, &tm);
#else
  tm = *std::localtime(&t);
#endif
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);
  std::fprintf(g_out, "%s.%03lld %-5s %.*s\n",
               ts, static_cast<long long>(ms), level_str(l),
               static_cast<int>(msg.size()), msg.data());
}

}  // namespace popy::log
