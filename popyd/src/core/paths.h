// Resolved filesystem locations used across popyd, popy, and popy-render.
//
// Computed once on first call from $HOME and $XDG_CONFIG_HOME / $XDG_RUNTIME_DIR
// and cached. Never reads them from a process the user does not own.
#pragma once
#include <filesystem>

namespace popy::paths {

// $HOME, must exist. Throws std::runtime_error if unset.
const std::filesystem::path& home();

// ~/.config/popy/config.toml (honours $XDG_CONFIG_HOME).
std::filesystem::path config_file();

// ~/.config/popy/popy.key (honours $XDG_CONFIG_HOME) — the 32-byte HMAC key
// that signs sidecars. Lives in the config dir, NOT $XDG_RUNTIME_DIR: the
// runtime dir is tmpfs on most Linux systems and a reboot would invalidate
// every existing signature.
std::filesystem::path key_file();

// ~/.popy-stage — default stage_dir / first watch_dir, indexer-excluded.
std::filesystem::path default_stage_dir();

// $XDG_RUNTIME_DIR/popy or, on macOS, ~/Library/Application Support/popy/run.
// Used for the AF_UNIX status socket and the daemon pid file.
std::filesystem::path runtime_dir();

std::filesystem::path status_socket();
std::filesystem::path pid_file();
std::filesystem::path log_file();

// Expand a leading "~/" or "~" — no shell metacharacters interpreted, no
// $VAR substitution. Anything else is returned as-is.
std::filesystem::path expand_tilde(const std::string& s);

}  // namespace popy::paths
