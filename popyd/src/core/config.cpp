#include "config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include "naming.h"
#include "paths.h"
#include "toml.hpp"

namespace fs = std::filesystem;

namespace popy::config {

namespace {

constexpr const char* kExampleToml = R"(# popy config — see plan.md §6 for the full reference.

enabled = true

# Default is the staging dir, indexer-excluded by the installer.
# Add ~/Downloads only if you accept that watcher mode is best-effort against
# index/AV races.
watch_dirs = ["~/.popy-stage"]
stage_dir  = "~/.popy-stage"

exclude = [
  "*_popy",
  "*.meta.json", "*.meta.json.new",
  "*.crdownload", "*.part", "*.download", "*.tmp",
  ".*",
  "**/popy-released/**",
]

stability_ms      = 750
fetch_max_bytes   = 5368709120
preview_max_bytes = 536870912
)";

bool starts_with(std::string_view a, std::string_view b) {
  return a.size() >= b.size() && a.substr(0, b.size()) == b;
}

void require_within_home(const fs::path& p) {
  // Resolve the parent (path itself may not exist yet if we're about to
  // create it). Use weakly_canonical to handle non-existent leaves.
  std::error_code ec;
  fs::path resolved = fs::weakly_canonical(p, ec);
  if (ec) resolved = p;  // best-effort; refusal below covers the dangerous case
  fs::path home_canon = fs::weakly_canonical(paths::home(), ec);
  if (ec) home_canon = paths::home();

  auto a = resolved.string();
  auto b = home_canon.string();
  if (b.empty() || b.back() != '/') b.push_back('/');
  if (!starts_with(a, b) && resolved != home_canon) {
    throw std::runtime_error("config: path " + p.string() +
                             " resolves outside $HOME (" +
                             home_canon.string() + ")");
  }
}

}  // namespace

Config defaults() {
  Config c;
  c.watch_dirs = {paths::default_stage_dir()};
  c.stage_dir  = paths::default_stage_dir();
  c.exclude = {
      "*_popy",      "*.meta.json",  "*.meta.json.new",
      "*.crdownload", "*.part",      "*.download",
      "*.tmp",       ".*",           "**/popy-released/**",
  };
  return c;
}

Config load(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return defaults();

  toml::table tbl;
  try {
    tbl = toml::parse_file(path.string());
  } catch (const toml::parse_error& e) {
    throw std::runtime_error("config: parse error: " +
                             std::string(e.description()));
  }

  Config c = defaults();
  if (auto v = tbl["enabled"].value<bool>())                 c.enabled = *v;
  if (auto v = tbl["stability_ms"].value<int>())             c.stability_ms = *v;
  if (auto v = tbl["fetch_max_bytes"].value<std::int64_t>()) c.fetch_max_bytes = *v;
  if (auto v = tbl["preview_max_bytes"].value<std::int64_t>())
    c.preview_max_bytes = *v;

  if (auto* arr = tbl["watch_dirs"].as_array()) {
    c.watch_dirs.clear();
    for (auto& el : *arr) {
      if (auto s = el.value<std::string>()) {
        c.watch_dirs.push_back(paths::expand_tilde(*s));
      }
    }
  }
  if (auto v = tbl["stage_dir"].value<std::string>())
    c.stage_dir = paths::expand_tilde(*v);

  if (auto* arr = tbl["exclude"].as_array()) {
    c.exclude.clear();
    for (auto& el : *arr) {
      if (auto s = el.value<std::string>()) c.exclude.push_back(*s);
    }
  }

  for (const auto& w : c.watch_dirs) require_within_home(w);
  require_within_home(c.stage_dir);

  return c;
}

Config load_default_path() { return load(paths::config_file()); }

bool excluded(const Config& c, std::string_view rel_name) {
  for (const auto& pat : c.exclude) {
    if (popy::naming::glob_match(pat, rel_name)) return true;
  }
  return false;
}

bool write_example_if_absent(const fs::path& path) {
  std::error_code ec;
  if (fs::exists(path, ec)) return false;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!out) return false;
  out << kExampleToml;
  return out.good();
}

}  // namespace popy::config
