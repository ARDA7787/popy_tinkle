#include "cli/commands.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/sidecar.h"
#include "ipc/status.h"
#include "net/fetch.h"
#include "nlohmann_json.hpp"
#include "store/quarantine.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace popy::cli {

namespace {

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string> options;  // --k v
  std::set<std::string> flags;                 // --k
};

Args parse_args(int argc, char** argv, int start) {
  Args a;
  for (int i = start; i < argc; ++i) {
    std::string s = argv[i];
    if (s.rfind("--", 0) == 0) {
      std::string k = s.substr(2);
      // --k=v form
      auto eq = k.find('=');
      if (eq != std::string::npos) {
        a.options[k.substr(0, eq)] = k.substr(eq + 1);
        continue;
      }
      // --k v form (consume next non-flag); if next is missing or another
      // flag, treat as boolean flag.
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        a.options[k] = argv[++i];
      } else {
        a.flags.insert(k);
      }
    } else {
      a.positional.push_back(std::move(s));
    }
  }
  return a;
}

bool has_flag(const Args& a, const std::string& k) {
  return a.flags.count(k) != 0 ||
         (a.options.count(k) && a.options.at(k) == "true");
}

std::optional<std::string> opt_str(const Args& a, const std::string& k) {
  if (a.options.count(k)) return a.options.at(k);
  return std::nullopt;
}

json record_to_json(const popy::sidecar::Record& r) {
  return {
      {"id",               r.id},
      {"originalFilename", r.originalFilename},
      {"diskPath",         r.diskPath},
      {"sourceUrl",        r.sourceUrl},
      {"sourceHost",       r.sourceHost},
      {"sizeBytes",        r.sizeBytes},
      {"mime",             r.mime},
      {"sha256",           r.sha256},
      {"path",             r.path},
      {"status",           r.status},
      {"createdAt",        r.createdAt},
      {"resolvedAt",       r.resolvedAt ? json(*r.resolvedAt) : json(nullptr)},
      {"note",             r.note ? json(*r.note) : json(nullptr)},
      {"agent",            r.agent},
      {"origin",           r.origin},
  };
}

void print_help() {
  std::cout <<
R"(popy — quarantine CLI

Usage:
  popy fetch <url> [--out <name>] [--mime <type>] [--max-bytes N] [--json]
  popy list  [--json]
  popy read  <file> [--mode raw|text|png] [--page N]
  popy release <file> --to <path> [--force]
  popy delete <file>
  popy status [--json]
  popy pause | popy resume
  popy config [--print] [--path]

`<file>` resolves by full UUID, UUID prefix (≥4 chars), original filename, or
the `<name>_popy` filename.

`popy fetch` is the strong path: bytes stream from the network straight into
<stage>/<uuid>/<name>_popy. The original-extension filename never exists.

Run `popy <command> --help` for command-specific help.
)";
}

// ----------------------------------------------------------------------------
// Subcommands
// ----------------------------------------------------------------------------

int cmd_fetch(const Args& a) {
  if (a.positional.empty()) {
    std::cerr << "popy fetch: missing <url>\n";
    return 2;
  }
  auto cfg = popy::config::load_default_path();

  popy::net::FetchOptions opts;
  opts.url       = a.positional[0];
  opts.stage_dir = cfg.stage_dir;
  opts.out_name  = opt_str(a, "out");
  opts.expected_mime = opt_str(a, "mime");
  opts.max_bytes = cfg.fetch_max_bytes;
  if (auto m = opt_str(a, "max-bytes")) {
    opts.max_bytes = std::stoll(*m);
  }
  opts.verbose = has_flag(a, "verbose");

  auto res = popy::net::fetch(opts);
  if (has_flag(a, "json")) {
    json j = {
        {"id",         res.record.id},
        {"sha256",     res.record.sha256},
        {"path",       res.popy_path.string()},
        {"sidecar",    res.sidecar_path.string()},
        {"sizeBytes",  res.record.sizeBytes},
        {"mime",       res.record.mime},
        {"sourceUrl",  res.record.sourceUrl},
        {"sourceHost", res.record.sourceHost},
    };
    std::cout << j.dump(2) << "\n";
  } else {
    std::cout << "id=" << res.record.id << "\n";
    std::cout << "sha256=" << res.record.sha256 << "\n";
    std::cout << "path=" << res.popy_path.string() << "\n";
    std::cout << "size=" << res.record.sizeBytes << " bytes\n";
    std::cout << "mime=" << res.record.mime << "\n";
  }
  return 0;
}

int cmd_list(const Args& a) {
  auto cfg = popy::config::load_default_path();
  auto entries = popy::store::list(cfg);
  if (has_flag(a, "json")) {
    json arr = json::array();
    for (auto& e : entries) arr.push_back(record_to_json(e.record));
    std::cout << arr.dump(2) << "\n";
  } else {
    if (entries.empty()) { std::cout << "(no quarantined files)\n"; return 0; }
    std::printf("%-36s  %-10s  %-20s  %s\n",
                "ID", "STATUS", "SIZE", "ORIGINAL");
    for (auto& e : entries) {
      std::printf("%-36s  %-10s  %-20lld  %s\n",
                  e.record.id.c_str(),
                  e.record.status.c_str(),
                  static_cast<long long>(e.record.sizeBytes),
                  e.record.originalFilename.c_str());
    }
  }
  return 0;
}

int cmd_release(const Args& a) {
  if (a.positional.empty()) {
    std::cerr << "popy release: missing <file>\n"; return 2;
  }
  auto to = opt_str(a, "to");
  if (!to) { std::cerr << "popy release: --to <path> required\n"; return 2; }
  auto cfg = popy::config::load_default_path();
  auto e = popy::store::resolve(cfg, a.positional[0]);
  popy::store::release(e, fs::path(popy::paths::expand_tilde(*to)),
                       has_flag(a, "force"));
  if (has_flag(a, "json")) {
    std::cout << json{{"id", e.record.id}, {"to", *to}, {"status", "released"}}.dump(2)
              << "\n";
  } else {
    std::cout << "released " << e.record.id << " -> " << *to << "\n";
  }
  return 0;
}

int cmd_delete(const Args& a) {
  if (a.positional.empty()) {
    std::cerr << "popy delete: missing <file>\n"; return 2;
  }
  auto cfg = popy::config::load_default_path();
  auto e = popy::store::resolve(cfg, a.positional[0]);
  popy::store::remove(e);
  if (has_flag(a, "json")) {
    std::cout << json{{"id", e.record.id}, {"status", "deleted"}}.dump(2) << "\n";
  } else {
    std::cout << "deleted " << e.record.id << "\n";
  }
  return 0;
}

int cmd_read(const Args& a) {
  if (a.positional.empty()) {
    std::cerr << "popy read: missing <file>\n"; return 2;
  }
  auto cfg = popy::config::load_default_path();
  auto e = popy::store::resolve(cfg, a.positional[0]);
  std::string mode = opt_str(a, "mode").value_or("raw");

  if (mode == "raw" || mode == "text") {
    // Open the popy file (chmod 0000) — we own it, opening read still works
    // on macOS/Linux because mode-checks happen on open(O_RDONLY) but the
    // owner ignores its own permissions on macOS HFS+/APFS. To be safe and
    // portable, briefly chmod 0400, dump, then chmod 0000 back.
    if (::chmod(e.popy_path.c_str(), 0400) != 0) {
      std::cerr << "popy read: chmod 0400 failed\n"; return 1;
    }
    int fd = ::open(e.popy_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { std::cerr << "popy read: open failed\n"; return 1; }
    char buf[64 * 1024];
    while (true) {
      auto n = ::read(fd, buf, sizeof(buf));
      if (n == 0) break;
      if (n < 0) { ::close(fd); ::chmod(e.popy_path.c_str(), 0000); return 1; }
      ::write(STDOUT_FILENO, buf, static_cast<size_t>(n));
    }
    ::close(fd);
    ::chmod(e.popy_path.c_str(), 0000);
    return 0;
  }

  if (mode == "png") {
    std::cerr << "popy read --mode png: rendering requires popy-render "
                 "(install libmupdf and rebuild)\n";
    return 3;  // unsupported_type
  }

  std::cerr << "popy read: unknown --mode '" << mode << "'\n";
  return 2;
}

int cmd_config(const Args& a) {
  if (has_flag(a, "path")) {
    std::cout << popy::paths::config_file().string() << "\n";
    return 0;
  }
  if (has_flag(a, "print")) {
    auto p = popy::paths::config_file();
    std::error_code ec;
    if (fs::exists(p, ec)) {
      std::ifstream in(p);
      std::cout << in.rdbuf();
    } else {
      std::cout << "(config file does not exist; using defaults)\n";
    }
    return 0;
  }
  std::cout << "popy config: --print | --path\n";
  return 0;
}

int cmd_status(const Args& a) {
  // Without popyd running, we still report stage-dir contents from the
  // sidecar walk — useful for CI / scripted use where no daemon is up.
  auto cfg = popy::config::load_default_path();
  auto entries = popy::store::list(cfg);
  std::int64_t total = 0;
  for (auto& e : entries) total += e.record.sizeBytes;

  // If the daemon is up, fold its live state in.
  json daemon_obj = nullptr;
  auto reply = popy::ipc::client_send(popy::paths::status_socket(), "STATUS");
  if (reply.rfind("OK ", 0) == 0) {
    try { daemon_obj = json::parse(reply.substr(3)); }
    catch (...) { /* malformed; ignore */ }
  }

  json j = {
      {"daemonRunning", !daemon_obj.is_null()},
      {"daemon",        daemon_obj},
      {"stageDir",      cfg.stage_dir.string()},
      {"watchDirs",     [&] { json arr; for (auto& w : cfg.watch_dirs) arr.push_back(w.string()); return arr; }()},
      {"fileCount",     entries.size()},
      {"totalBytes",    total},
  };
  if (has_flag(a, "json")) {
    std::cout << j.dump(2) << "\n";
  } else {
    std::cout << "stage_dir=" << cfg.stage_dir.string() << "\n";
    std::cout << "files=" << entries.size() << "  bytes=" << total << "\n";
    if (daemon_obj.is_null()) {
      std::cout << "daemon=not running\n";
    } else {
      std::cout << "daemon=running pid=" << daemon_obj["pid"]
                << " paused=" << (daemon_obj["paused"].get<bool>() ? "yes" : "no")
                << "\n";
    }
  }
  return (daemon_obj.is_null() && has_flag(a, "require-daemon")) ? 1 : 0;
}

int cmd_pause_resume(const std::string& sub, const Args& a) {
  auto reply = popy::ipc::client_send(popy::paths::status_socket(),
                                      sub == "pause" ? "PAUSE" : "RESUME");
  if (reply.empty()) {
    std::cerr << "popy " << sub << ": daemon is not running\n";
    return 1;
  }
  if (reply.rfind("OK", 0) != 0) {
    std::cerr << "popy " << sub << ": " << reply << "\n";
    return 1;
  }
  if (has_flag(a, "json")) {
    std::cout << json{{"daemon", sub == "pause" ? "paused" : "resumed"}}.dump(2)
              << "\n";
  } else {
    std::cout << "daemon " << (sub == "pause" ? "paused" : "resumed") << "\n";
  }
  return 0;
}

}  // namespace

int run(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) == "--help" ||
      std::string(argv[1]) == "-h") {
    print_help();
    return (argc < 2) ? 2 : 0;
  }
  std::string sub = argv[1];
  auto a = parse_args(argc, argv, 2);

  try {
    if (sub == "fetch")   return cmd_fetch(a);
    if (sub == "list")    return cmd_list(a);
    if (sub == "release") return cmd_release(a);
    if (sub == "delete")  return cmd_delete(a);
    if (sub == "read")    return cmd_read(a);
    if (sub == "status")  return cmd_status(a);
    if (sub == "config")  return cmd_config(a);
    if (sub == "pause" || sub == "resume") return cmd_pause_resume(sub, a);
    std::cerr << "popy: unknown subcommand '" << sub << "'\n";
    print_help();
    return 2;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}

}  // namespace popy::cli
