// `popyd` — the watcher daemon.
//
// Three threads:
//   - main: holds the pid lock, serves the status socket via StatusServer
//           internally, waits on a self-pipe for SIGTERM/SIGINT
//   - watcher (inside Watcher): emits paths from the OS event source
//   - processor: pulls paths from a queue, calls quarantine_existing_file()
//
// We do not fork or setsid. launchd / systemd run us in the foreground;
// they manage daemonization, log rotation, and restart. KISS.

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/sidecar.h"
#include "ipc/status.h"
#include "store/quarantine.h"
#include "watch/watcher.h"

namespace fs = std::filesystem;

namespace {

// --- self-pipe trick for signal-safe shutdown ------------------------------
int g_wake_pipe[2] = {-1, -1};

extern "C" void signal_handler(int /*sig*/) {
  // async-signal-safe: write a single byte. errno may clobber; that's fine.
  if (g_wake_pipe[1] >= 0) {
    char b = 1;
    ssize_t r = ::write(g_wake_pipe[1], &b, 1);
    (void)r;
  }
}

void install_signal_handlers() {
  if (::pipe(g_wake_pipe) != 0) {
    throw std::runtime_error("popyd: pipe failed");
  }
  ::fcntl(g_wake_pipe[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(g_wake_pipe[1], F_SETFD, FD_CLOEXEC);
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGINT,  &sa, nullptr);
  // Ignore SIGPIPE; we use poll/read returns to detect socket closures.
  ::signal(SIGPIPE, SIG_IGN);
}

// --- pid lock --------------------------------------------------------------
class PidLock {
 public:
  explicit PidLock(fs::path path) : path_(std::move(path)) {
    fs::create_directories(path_.parent_path());
    fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd_ < 0) {
      throw std::runtime_error("popyd: cannot open pid file: " +
                               path_.string());
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd_);
      throw std::runtime_error(
          "popyd: another instance is running (pid file " + path_.string() +
          " is locked)");
    }
    ::ftruncate(fd_, 0);
    auto s = std::to_string(::getpid()) + "\n";
    ::write(fd_, s.data(), s.size());
  }
  ~PidLock() {
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
      ::unlink(path_.c_str());
    }
  }

 private:
  fs::path path_;
  int fd_ = -1;
};

// --- thread-safe path queue -----------------------------------------------
class PathQueue {
 public:
  void push(fs::path p) {
    std::lock_guard<std::mutex> lk(mu_);
    q_.push_back(std::move(p));
    cv_.notify_one();
  }
  // Returns false if the queue has been stopped (caller should exit).
  bool pop(fs::path& out) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&]{ return stopped_ || !q_.empty(); });
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }
  void stop() {
    std::lock_guard<std::mutex> lk(mu_);
    stopped_ = true;
    cv_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<fs::path> q_;
  bool stopped_ = false;
};

// --- helpers ---------------------------------------------------------------
// Has filename matched any exclude glob? `cfg.exclude` patterns are matched
// against the basename and the path-relative-to-watch-root (whichever is more
// informative), to keep "*_popy" matching foo_popy and not subdir/foo_popy.
bool should_skip(const popy::config::Config& cfg, const fs::path& abs) {
  return popy::config::excluded(cfg, abs.filename().string());
}

// Catch-up pass: scan each watch_dir once at startup for files we'd have
// quarantined if we'd been running.
void catchup_scan(const popy::config::Config& cfg, PathQueue& q) {
  std::set<fs::path> seen;
  auto scan = [&](const fs::path& root) {
    if (!seen.insert(fs::weakly_canonical(root)).second) return;
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (auto it = fs::directory_iterator(root, ec); !ec &&
                                                     it != fs::directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (should_skip(cfg, it->path())) continue;
      q.push(it->path());
    }
  };
  for (const auto& w : cfg.watch_dirs) scan(w);
  scan(cfg.stage_dir);
}

}  // namespace

int daemon_main() {
  popy::log::open_file(popy::paths::log_file().string());
  popy::log::info("popyd starting");

  popy::config::Config cfg;
  try {
    // Seed config.toml with the example on first run, then load it.
    popy::config::write_example_if_absent(popy::paths::config_file());
    cfg = popy::config::load_default_path();
  } catch (const std::exception& e) {
    std::cerr << "popyd: " << e.what() << "\n";
    popy::log::error(e.what());
    return 1;
  }

  if (!cfg.enabled) {
    popy::log::info("popyd: enabled=false in config; idling without watcher");
  }

  for (const auto& w : cfg.watch_dirs) {
    std::error_code ec;
    fs::create_directories(w, ec);
  }
  std::error_code ec;
  fs::create_directories(cfg.stage_dir, ec);

  install_signal_handlers();

  std::unique_ptr<PidLock> pid;
  try {
    pid = std::make_unique<PidLock>(popy::paths::pid_file());
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    popy::log::error(e.what());
    return 1;
  }

  popy::ipc::ServerState state;
  state.start_unix_ms = popy::sidecar::now_ms();

  popy::ipc::StatusServer status_srv(popy::paths::status_socket(), &state);
  status_srv.start();

  PathQueue queue;

  std::atomic<bool> shutting_down{false};

  std::thread proc([&]{
    fs::path p;
    while (queue.pop(p)) {
      // While paused, hold the popped path — don't discard. Wake every 200ms
      // to re-check the flag. Shutdown is signalled by `shutting_down`, so
      // we exit the wait promptly even mid-pause.
      while (state.paused.load() && !shutting_down.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      if (shutting_down.load()) break;
      if (should_skip(cfg, p)) continue;
      try {
        auto r = popy::store::quarantine_existing_file(p);
        state.file_count.fetch_add(1);
        state.byte_count.fetch_add(r.record.sizeBytes);
      } catch (const std::exception& e) {
        popy::log::warn(std::string("processor: ") + e.what());
      }
    }
  });

  // Repair pass: adopt orphaned _popy files (hash+sign+sidecar, mode 0000),
  // GC sidecars whose data file is gone. Runs before the watcher so the
  // catch-up scan sees a consistent store.
  try {
    auto stats = popy::store::repair(cfg);
    if (stats.adopted > 0 || stats.gc_sidecars > 0) {
      popy::log::info("repair: adopted=" + std::to_string(stats.adopted) +
                      " gc_sidecars=" + std::to_string(stats.gc_sidecars));
    }
  } catch (const std::exception& e) {
    popy::log::warn(std::string("repair pass failed: ") + e.what());
  }

  std::unique_ptr<popy::watch::Watcher> watcher;
  if (cfg.enabled) {
    watcher = std::make_unique<popy::watch::Watcher>(cfg.watch_dirs);
    watcher->start([&](fs::path p) { queue.push(std::move(p)); });
    catchup_scan(cfg, queue);
  }

  popy::log::info("popyd ready (pid=" + std::to_string(::getpid()) + ")");

  // Block on the self-pipe until SIGTERM/SIGINT.
  char buf;
  while (::read(g_wake_pipe[0], &buf, 1) < 0 && errno == EINTR) {}

  popy::log::info("popyd shutting down");
  shutting_down = true;
  if (watcher) watcher->stop();
  queue.stop();
  if (proc.joinable()) proc.join();
  status_srv.stop();
  return 0;
}

int main(int /*argc*/, char** /*argv*/) {
  try {
    return daemon_main();
  } catch (const std::exception& e) {
    std::cerr << "popyd: " << e.what() << "\n";
    popy::log::error(e.what());
    return 1;
  } catch (...) {
    std::cerr << "popyd: unknown fatal error\n";
    popy::log::error("unknown fatal error");
    return 1;
  }
}
