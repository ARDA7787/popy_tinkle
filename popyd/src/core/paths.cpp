#include "paths.h"

#include <unistd.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace popy::paths {

namespace {

// Cached on first call so we never re-read getenv after the process has
// started: a child shouldn't be able to retroactively change where popyd
// looks for things by re-export'ing $HOME.
const fs::path& cached_home() {
  static const fs::path h = []() {
    const char* h = std::getenv("HOME");
    if (!h || !*h) throw std::runtime_error("$HOME is not set");
    return fs::path(h);
  }();
  return h;
}

fs::path env_or(const char* var, fs::path fallback) {
  const char* v = std::getenv(var);
  if (v && *v) return fs::path(v);
  return fallback;
}

}  // namespace

const fs::path& home() { return cached_home(); }

fs::path config_file() {
  return env_or("XDG_CONFIG_HOME", home() / ".config") / "popy" / "config.toml";
}

fs::path default_stage_dir() { return home() / ".popy-stage"; }

fs::path runtime_dir() {
#ifdef __APPLE__
  return home() / "Library" / "Application Support" / "popy" / "run";
#else
  return env_or("XDG_RUNTIME_DIR",
                fs::path("/tmp") / ("popy-" + std::to_string(::getuid())));
#endif
}

fs::path status_socket() { return runtime_dir() / "popyd.sock"; }
fs::path pid_file()      { return runtime_dir() / "popyd.pid"; }

fs::path log_file() {
#ifdef __APPLE__
  return home() / "Library" / "Logs" / "popyd.log";
#else
  return env_or("XDG_STATE_HOME", home() / ".local" / "state") / "popy" /
         "popyd.log";
#endif
}

fs::path expand_tilde(const std::string& s) {
  if (s.empty() || s[0] != '~') return fs::path(s);
  if (s.size() == 1) return cached_home();
  if (s[1] == '/') return cached_home() / s.substr(2);
  // "~user" or "~something" — not supported; pass through unchanged so the
  // caller fails closed rather than silently mis-resolving.
  return fs::path(s);
}

}  // namespace popy::paths
