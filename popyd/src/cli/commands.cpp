#include "cli/commands.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
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
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "core/attest.h"
#include "core/config.h"
#include "core/hash.h"
#include "core/keyring.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/sidecar.h"
#include "core/textsafe.h"
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
  popy read  <file> [--mode raw|text|png] [--page N] [--max-bytes N]
  popy release <file> --to <path> [--force]
  popy delete <file>
  popy verify <file> [--json]
  popy resign [--json]
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

bool is_text_mime(const std::string& mime) {
  return mime.rfind("text/", 0) == 0 ||
         mime == "application/json" ||
         mime == "application/xml" ||
         mime == "application/yaml" ||
         mime == "application/toml" ||
         mime == "application/javascript";
}

bool renderable_mime(const std::string& mime) {
  return mime == "application/pdf" || mime.rfind("image/", 0) == 0;
}

fs::path current_exe_dir() {
#if defined(__linux__)
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return fs::path(buf).parent_path();
  }
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) {
    std::error_code ec;
    return fs::canonical(fs::path(buf), ec).parent_path();
  }
#endif
  return fs::current_path();
}

fs::path render_binary_path() {
  fs::path sibling = current_exe_dir() / "popy-render";
  std::error_code ec;
  if (fs::exists(sibling, ec)) return sibling;
  return fs::path("popy-render");
}

void write_all_fd(int fd, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("write failed: ") +
                               std::strerror(errno));
    }
    off += static_cast<size_t>(n);
  }
}

void stream_fd_to_stdout(int fd) {
  char buf[64 * 1024];
  while (true) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("read failed: ") +
                               std::strerror(errno));
    }
    write_all_fd(STDOUT_FILENO, buf, static_cast<size_t>(n));
  }
}

int render_via_child(const popy::store::Entry& e, const std::string& mode,
                     int page_num, const std::string& key) {
  // Verified gate: sidecar HMAC + size + full content hash, then a minimal-
  // window fd (on-disk mode stays 0000 while the renderer works).
  auto readable = popy::store::open_verified(e, key);

  int child_stdin[2] = {-1, -1};
  int child_stdout[2] = {-1, -1};
  if (::pipe(child_stdin) != 0 || ::pipe(child_stdout) != 0) {
    throw std::runtime_error("popy read: pipe failed");
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    throw std::runtime_error("popy read: fork failed");
  }

  if (pid == 0) {
    ::dup2(child_stdin[0], STDIN_FILENO);
    ::dup2(child_stdout[1], STDOUT_FILENO);
    ::dup2(readable.fd(), 3);
    ::close(child_stdin[0]);
    ::close(child_stdin[1]);
    ::close(child_stdout[0]);
    ::close(child_stdout[1]);
    fs::path render = render_binary_path();
    ::execl(render.c_str(), render.c_str(), static_cast<char*>(nullptr));
    std::string error = json{{"ok", false},
                             {"error", std::string("exec popy-render failed: ") +
                                           std::strerror(errno)}}.dump() + "\n";
    write_all_fd(STDOUT_FILENO, error.data(), error.size());
    _exit(127);
  }

  ::close(child_stdin[0]);
  ::close(child_stdout[1]);

  json command = {
      {"mode", mode},
      {"mime", e.record.mime},
      {"page", page_num},
      {"fd", 3},
  };
  std::string line = command.dump() + "\n";
  write_all_fd(child_stdin[1], line.data(), line.size());
  ::close(child_stdin[1]);

  std::string header;
  bool saw_header = false;
  bool child_ok = false;
  std::string child_error;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

  char buf[16 * 1024];
  while (true) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      ::kill(pid, SIGKILL);
      ::close(child_stdout[0]);
      ::waitpid(pid, nullptr, 0);
      std::cerr << "popy read: renderer timed out\n";
      return 1;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    struct pollfd pfd {};
    pfd.fd = child_stdout[0];
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
    if (pr < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("popy read: poll failed");
    }
    if (pr == 0) continue;

    ssize_t n = ::read(child_stdout[0], buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("popy read: renderer read failed");
    }

    std::string_view chunk(buf, static_cast<size_t>(n));
    if (!saw_header) {
      auto nl = chunk.find('\n');
      if (nl == std::string_view::npos) {
        header.append(chunk);
        continue;
      }
      header.append(chunk.substr(0, nl));
      json reply = json::parse(header);
      child_ok = reply.value("ok", false);
      child_error = reply.value("error", "");
      saw_header = true;
      if (!child_ok) continue;
      auto payload = chunk.substr(nl + 1);
      if (!payload.empty()) write_all_fd(STDOUT_FILENO, payload.data(), payload.size());
    } else if (child_ok) {
      write_all_fd(STDOUT_FILENO, chunk.data(), chunk.size());
    }
  }

  ::close(child_stdout[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (!saw_header) {
    std::cerr << "popy read: renderer produced no response\n";
    return 1;
  }
  if (!child_ok) {
    std::cerr << "popy read: " << child_error << "\n";
    return 1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::cerr << "popy read: renderer exited abnormally\n";
    return 1;
  }
  return 0;
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
    for (const auto& e : entries) {
      auto j = record_to_json(e.record);
      j["dataMissing"] = e.data_missing;
      arr.push_back(std::move(j));
    }
    std::cout << arr.dump(2) << "\n";
  } else {
    if (entries.empty()) { std::cout << "(no quarantined files)\n"; return 0; }
    std::printf("%-36s  %-12s  %-20s  %s\n",
                "ID", "STATUS", "SIZE", "ORIGINAL");
    for (auto& e : entries) {
      std::printf("%-36s  %-12s  %-20lld  %s\n",
                  e.record.id.c_str(),
                  e.data_missing ? "data-missing" : e.record.status.c_str(),
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
  int page_num = 0;
  if (auto page = opt_str(a, "page")) {
    page_num = std::stoi(*page);
  }
  auto key = popy::keyring::load_or_create(popy::paths::key_file());

  if (mode == "png") {
    if (!renderable_mime(e.record.mime)) {
      std::cerr << "popy read --mode png: unsupported mime '" << e.record.mime
                << "'\n";
      return 3;
    }
    return render_via_child(e, mode, page_num, key);
  }

  if (mode == "text" && e.record.mime == "application/pdf") {
    return render_via_child(e, mode, page_num, key);
  }

  if (mode == "text" && !is_text_mime(e.record.mime)) {
    std::cerr << "popy read --mode text: unsupported binary mime '"
              << e.record.mime << "'\n";
    return 3;
  }

  if (mode == "raw") {
    // Raw stays available as an explicit debug path — verified (sidecar HMAC
    // + size + content hash) but NOT sanitized.
    auto readable = popy::store::open_verified(e, key);
    stream_fd_to_stdout(readable.fd());
    return 0;
  }

  if (mode == "text") {
    std::int64_t cap = cfg.preview_max_bytes;
    if (auto m = opt_str(a, "max-bytes")) {
      cap = std::min(cap, static_cast<std::int64_t>(std::stoll(*m)));
    }
    if (cap < 0) cap = 0;

    auto readable = popy::store::open_verified(e, key);
    std::string body;
    body.reserve(static_cast<size_t>(
        std::min<std::int64_t>(cap, e.record.sizeBytes)));
    char buf[64 * 1024];
    while (static_cast<std::int64_t>(body.size()) < cap) {
      auto want = std::min<std::int64_t>(
          static_cast<std::int64_t>(sizeof(buf)),
          cap - static_cast<std::int64_t>(body.size()));
      ssize_t n = ::read(readable.fd(), buf, static_cast<size_t>(want));
      if (n == 0) break;
      if (n < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error(std::string("popy read: read failed: ") +
                                 std::strerror(errno));
      }
      body.append(buf, static_cast<size_t>(n));
    }

    // Defence against a lying mime: a NUL in the head means binary content.
    if (popy::textsafe::looks_binary(
            std::string_view(body).substr(0, 4096))) {
      std::cerr << "popy read --mode text: content looks binary "
                   "(NUL in probe); use --mode raw if you really mean it\n";
      return 3;
    }

    auto safe = popy::textsafe::sanitize_utf8(body);
    write_all_fd(STDOUT_FILENO, safe.data(), safe.size());
    return 0;
  }

  std::cerr << "popy read: unknown --mode '" << mode << "'\n";
  return 2;
}

// `popy verify <file>` — verify the sidecar HMAC and re-hash the content.
// Exit 0 only when both check out; unsigned sidecars exit 1 with a resign
// hint; a bad signature or hash mismatch is reported as tampering.
int cmd_verify(const Args& a) {
  if (a.positional.empty()) {
    std::cerr << "popy verify: missing <file>\n"; return 2;
  }
  auto cfg = popy::config::load_default_path();
  auto e = popy::store::resolve(cfg, a.positional[0]);
  auto key = popy::keyring::load_or_create(popy::paths::key_file());

  std::string popy_name = e.popy_path.filename().string();
  auto sig = popy::attest::verify(e.record, popy_name, key);
  std::string sig_status;
  switch (sig) {
    case popy::attest::VerifyResult::ok:                sig_status = "ok"; break;
    case popy::attest::VerifyResult::missing_signature: sig_status = "unsigned"; break;
    case popy::attest::VerifyResult::unknown_alg:       sig_status = "unknown-alg"; break;
    case popy::attest::VerifyResult::bad_signature:     sig_status = "bad-signature"; break;
  }

  bool data_ok = false;
  std::string data_status;
  struct stat st {};
  if (::lstat(e.popy_path.c_str(), &st) != 0) {
    data_status = "missing";
  } else if (!S_ISREG(st.st_mode)) {
    data_status = "not-a-regular-file";
  } else if (st.st_size != e.record.sizeBytes) {
    data_status = "size-mismatch";
  } else {
    popy::store::QuarantineReadable readable(e.popy_path);
    popy::hash::Sha256Streaming hasher;
    char buf[64 * 1024];
    while (true) {
      ssize_t n = ::read(readable.fd(), buf, sizeof(buf));
      if (n == 0) break;
      if (n < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error(std::string("popy verify: read failed: ") +
                                 std::strerror(errno));
      }
      hasher.update(buf, static_cast<size_t>(n));
    }
    if (hasher.digest_hex() == e.record.sha256) {
      data_ok = true;
      data_status = "ok";
    } else {
      data_status = "sha256-mismatch";
    }
  }

  bool ok = (sig == popy::attest::VerifyResult::ok) && data_ok;
  if (has_flag(a, "json")) {
    std::cout << json{{"id", e.record.id},
                      {"file", popy_name},
                      {"signature", sig_status},
                      {"content", data_status},
                      {"ok", ok}}.dump(2) << "\n";
  } else {
    std::cout << "id=" << e.record.id << "\n";
    std::cout << "signature=" << sig_status << "\n";
    std::cout << "content=" << data_status << "\n";
    std::cout << (ok ? "OK" : "FAILED") << "\n";
    if (sig == popy::attest::VerifyResult::missing_signature) {
      std::cout << "hint: run `popy resign` to sign pre-upgrade sidecars\n";
    }
  }
  return ok ? 0 : 1;
}

// `popy resign` — one-time TOFU migration: sign every UNSIGNED sidecar.
// An invalid signature is never overwritten — that is tamper evidence and is
// reported with exit 1.
int cmd_resign(const Args& a) {
  auto cfg = popy::config::load_default_path();
  auto key = popy::keyring::load_or_create(popy::paths::key_file());
  auto entries = popy::store::list(cfg);

  int signed_count = 0, ok_count = 0, tampered = 0;
  json results = json::array();
  for (const auto& e : entries) {
    std::string popy_name = e.popy_path.filename().string();
    auto v = popy::attest::verify(e.record, popy_name, key);
    if (v == popy::attest::VerifyResult::ok) {
      ++ok_count;
      continue;
    }
    if (v == popy::attest::VerifyResult::missing_signature) {
      popy::sidecar::Record r = e.record;
      popy::attest::sign(r, popy_name, key);
      popy::sidecar::write_atomic(e.sidecar_path, r);
      ++signed_count;
      results.push_back({{"id", e.record.id}, {"action", "signed"}});
      continue;
    }
    ++tampered;
    results.push_back({{"id", e.record.id}, {"action", "tamper-detected"}});
    std::cerr << "popy resign: NOT overwriting invalid signature on "
              << e.record.id << " (" << popy_name
              << ") — possible tampering\n";
  }

  if (has_flag(a, "json")) {
    std::cout << json{{"signed", signed_count},
                      {"alreadyValid", ok_count},
                      {"tamperDetected", tampered},
                      {"results", results}}.dump(2) << "\n";
  } else {
    std::cout << "signed=" << signed_count
              << " already-valid=" << ok_count
              << " tamper-detected=" << tampered << "\n";
  }
  return tampered > 0 ? 1 : 0;
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
  for (const auto& e : entries) total += e.record.sizeBytes;

  // If the daemon is up, fold its live state in.
  json daemon_obj = nullptr;
  auto reply = popy::ipc::client_send(popy::paths::status_socket(), "STATUS");
  if (reply.rfind("OK ", 0) == 0) {
    try { daemon_obj = json::parse(reply.substr(3)); }
    catch (...) { /* malformed; ignore */ }
  }

  // Overall enforcement posture. `popy fetch` (Mode A) is always a guaranteed
  // per-invocation path regardless of the daemon; the daemon (Mode B) widens
  // coverage to arbitrary writes into the watch dirs, but only best-effort
  // (there is a create-to-rename race window). Report both so agents/users
  // can tell which guarantee applies to *this* environment right now.
  const bool daemon_running = !daemon_obj.is_null();
  const std::string enforcement_mode = daemon_running ? "strict" : "popy_only";
  const std::string guarantee_level =
      daemon_running ? "best_effort" : "guaranteed";
  json bypass_surface = json::array();
  bypass_surface.push_back(
      "native downloads/writes outside popy fetch are unenforced when the "
      "daemon is off");
  bypass_surface.push_back(
      "new files outside configured watchDirs are not quarantined by the "
      "watcher");

  json j = {
      {"daemonRunning",    daemon_running},
      {"daemon",           daemon_obj},
      {"stageDir",         cfg.stage_dir.string()},
      {"watchDirs",        [&] { json arr; for (auto& w : cfg.watch_dirs) arr.push_back(w.string()); return arr; }()},
      {"fileCount",        entries.size()},
      {"totalBytes",       total},
      {"enforcementMode",  enforcement_mode},
      {"guaranteeLevel",   guarantee_level},
      {"bypassSurface",    bypass_surface},
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
    std::cout << "enforcement_mode=" << enforcement_mode
              << "  guarantee_level=" << guarantee_level << "\n";
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
    if (sub == "verify")  return cmd_verify(a);
    if (sub == "resign")  return cmd_resign(a);
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
