// Linux inotify-based watcher.
//
// Only watches the top level of each root (no recursive watch). Subdirectory
// support is deferred — it complicates inotify (you have to add watches for
// every new subdir) and the v1 use case is a single staging dir.
//
// IN_CLOSE_WRITE fires after the writer closes the fd, so files are stable
// when we see them. IN_MOVED_TO fires for `mv` into the dir. We treat both
// the same.

#include "watch/watcher.h"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/log.h"

namespace fs = std::filesystem;

namespace popy::watch {

struct Watcher::Impl {
  std::vector<fs::path> roots;
  Callback cb;
  int fd = -1;
  int wake_pipe[2] = {-1, -1};
  std::map<int, fs::path> wd_to_root;  // inotify watch-descriptor → root path
  std::atomic<bool> stop_flag{false};
  std::thread th;

  void loop() {
    constexpr size_t kBuf = 64 * 1024;
    std::vector<char> buf(kBuf);
    while (!stop_flag) {
      pollfd pfds[2] = {
          { fd,           POLLIN, 0 },
          { wake_pipe[0], POLLIN, 0 },
      };
      int rc = ::poll(pfds, 2, -1);
      if (rc < 0) { if (errno == EINTR) continue; break; }
      if (pfds[1].revents & POLLIN) {
        char b; (void)::read(wake_pipe[0], &b, 1);
        if (stop_flag) return;
      }
      if (!(pfds[0].revents & POLLIN)) continue;

      auto n = ::read(fd, buf.data(), buf.size());
      if (n <= 0) continue;

      char* p = buf.data();
      char* end = p + n;
      while (p + sizeof(inotify_event) <= end) {
        auto* ev = reinterpret_cast<inotify_event*>(p);
        if (ev->len > 0 && (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) {
          auto it = wd_to_root.find(ev->wd);
          if (it != wd_to_root.end()) {
            try { cb(it->second / ev->name); }
            catch (const std::exception& e) {
              popy::log::warn(std::string("watcher cb threw: ") + e.what());
            }
          }
        }
        p += sizeof(inotify_event) + ev->len;
      }
    }
  }
};

Watcher::Watcher(std::vector<fs::path> roots) : p_(std::make_unique<Impl>()) {
  p_->roots = std::move(roots);
}

Watcher::~Watcher() { stop(); }

void Watcher::start(Callback cb) {
  p_->cb = std::move(cb);
  p_->stop_flag = false;

  p_->fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (p_->fd < 0) {
    throw std::runtime_error(std::string("watcher: inotify_init1: ") +
                             std::strerror(errno));
  }
  if (::pipe(p_->wake_pipe) != 0) {
    throw std::runtime_error("watcher: pipe failed");
  }

  for (const auto& r : p_->roots) {
    int wd = ::inotify_add_watch(
        p_->fd, r.c_str(),
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_DONT_FOLLOW | IN_EXCL_UNLINK);
    if (wd < 0) {
      popy::log::warn(std::string("watcher: cannot watch ") + r.string() +
                      ": " + std::strerror(errno));
      continue;
    }
    p_->wd_to_root[wd] = r;
  }

  p_->th = std::thread([this]{ p_->loop(); });
  popy::log::info("watcher started (inotify)");
}

void Watcher::stop() {
  if (!p_) return;
  if (p_->stop_flag.exchange(true)) return;  // idempotent
  if (p_->wake_pipe[1] >= 0) {
    char b = 1; (void)::write(p_->wake_pipe[1], &b, 1);
  }
  if (p_->th.joinable()) p_->th.join();
  for (auto& [wd, _] : p_->wd_to_root) ::inotify_rm_watch(p_->fd, wd);
  if (p_->fd >= 0) ::close(p_->fd);
  if (p_->wake_pipe[0] >= 0) ::close(p_->wake_pipe[0]);
  if (p_->wake_pipe[1] >= 0) ::close(p_->wake_pipe[1]);
  p_->fd = p_->wake_pipe[0] = p_->wake_pipe[1] = -1;
  popy::log::info("watcher stopped");
}

}  // namespace popy::watch
