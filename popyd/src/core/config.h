// Config (~/.config/popy/config.toml) parsing.
//
// Defaults match plan.md §6.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace popy::config {

struct Config {
  bool enabled = true;
  std::vector<std::filesystem::path> watch_dirs;        // expanded ~
  std::filesystem::path stage_dir;                       // expanded ~
  std::vector<std::string> exclude;
  int stability_ms = 750;
  std::int64_t fetch_max_bytes   = 5LL * 1024 * 1024 * 1024;  // 5 GiB
  std::int64_t preview_max_bytes = 512LL * 1024 * 1024;       // 512 MiB
};

// Defaults; safe to use without any config file.
Config defaults();

// Read from disk; falls back to defaults() if file does not exist.
// Throws std::runtime_error on parse error or on a watch_dir resolving outside
// $HOME (defence against misconfig pointing at /).
Config load(const std::filesystem::path& path);

// Same, using paths::config_file().
Config load_default_path();

// Returns true if `name` (relative path within a watch_dir, forward-slashes)
// matches any exclude glob.
bool excluded(const Config&, std::string_view rel_name);

// Helper used at install time / first daemon start: write the example config
// to `path` if it doesn't exist. Returns true if a file was created.
bool write_example_if_absent(const std::filesystem::path& path);

}  // namespace popy::config
