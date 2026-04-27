#include "ipc/status.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/log.h"
#include "nlohmann_json.hpp"

using nlohmann::json;
namespace fs = std::filesystem;

namespace popy::ipc {

namespace {

void bind_unix_socket(int fd, const fs::path& path) {
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  if (path.string().size() >= sizeof(a.sun_path)) {
    throw std::runtime_error("ipc: socket path too long: " + path.string());
  }
  std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
  // Stale socket from a crashed predecessor? Unlink and retry.
  ::unlink(path.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    throw std::runtime_error(std::string("ipc: bind: ") + std::strerror(errno));
  }
  // 0700 on parent dir is the access control; relax socket itself to 0600.
  ::chmod(path.c_str(), 0600);
}

std::string read_line(int fd) {
  std::string out;
  char c;
  while (out.size() < 256) {
    auto n = ::read(fd, &c, 1);
    if (n <= 0) break;
    if (c == '\n') break;
    out.push_back(c);
  }
  return out;
}

void write_all(int fd, std::string_view s) {
  size_t off = 0;
  while (off < s.size()) {
    auto n = ::write(fd, s.data() + off, s.size() - off);
    if (n < 0) { if (errno == EINTR) continue; return; }
    off += static_cast<size_t>(n);
  }
}

}  // namespace

struct StatusServer::Impl {
  fs::path                socket_path;
  ServerState*            state = nullptr;
  int                     fd = -1;
  int                     wake_pipe[2] = {-1, -1};
  std::atomic<bool>       stop_flag{false};
  std::thread             th;

  void serve_one(int cfd) {
    auto cmd = read_line(cfd);
    std::string resp;
    if (cmd == "PING") {
      resp = "OK\n";
    } else if (cmd == "PAUSE") {
      state->paused = true;
      resp = "OK\n";
    } else if (cmd == "RESUME") {
      state->paused = false;
      resp = "OK\n";
    } else if (cmd == "STATUS") {
      using namespace std::chrono;
      auto now = duration_cast<milliseconds>(
                     system_clock::now().time_since_epoch())
                     .count();
      json j = {
          {"pid",       static_cast<long long>(::getpid())},
          {"paused",    state->paused.load()},
          {"files",     state->file_count.load()},
          {"bytes",     state->byte_count.load()},
          {"uptime_ms", static_cast<long long>(now - state->start_unix_ms)},
      };
      resp = "OK " + j.dump() + "\n";
    } else {
      resp = "ERR unknown command\n";
    }
    write_all(cfd, resp);
    ::close(cfd);
  }

  void loop() {
    while (!stop_flag) {
      pollfd pfds[2] = {
          { fd,           POLLIN, 0 },
          { wake_pipe[0], POLLIN, 0 },
      };
      int rc = ::poll(pfds, 2, -1);
      if (rc < 0) { if (errno == EINTR) continue; return; }
      if (pfds[1].revents & POLLIN) {
        char b; (void)::read(wake_pipe[0], &b, 1);
        return;  // shutdown
      }
      if (!(pfds[0].revents & POLLIN)) continue;
      int cfd = ::accept(fd, nullptr, nullptr);
      if (cfd < 0) continue;
      try { serve_one(cfd); }
      catch (const std::exception& e) {
        popy::log::warn(std::string("status server: ") + e.what());
      }
    }
  }
};

StatusServer::StatusServer(fs::path socket_path, ServerState* state)
    : p_(std::make_unique<Impl>()) {
  p_->socket_path = std::move(socket_path);
  p_->state       = state;
}

StatusServer::~StatusServer() { stop(); }

void StatusServer::start() {
  fs::create_directories(p_->socket_path.parent_path());
  p_->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (p_->fd < 0) {
    throw std::runtime_error(std::string("ipc: socket: ") +
                             std::strerror(errno));
  }
  ::fcntl(p_->fd, F_SETFD, FD_CLOEXEC);
  bind_unix_socket(p_->fd, p_->socket_path);
  if (::listen(p_->fd, 4) != 0) {
    throw std::runtime_error("ipc: listen failed");
  }
  if (::pipe(p_->wake_pipe) != 0) {
    throw std::runtime_error("ipc: pipe failed");
  }
  p_->stop_flag = false;
  p_->th = std::thread([this]{ p_->loop(); });
  popy::log::info("status socket: " + p_->socket_path.string());
}

void StatusServer::stop() {
  if (!p_) return;
  if (p_->stop_flag.exchange(true)) return;
  if (p_->wake_pipe[1] >= 0) {
    char b = 1; (void)::write(p_->wake_pipe[1], &b, 1);
  }
  if (p_->th.joinable()) p_->th.join();
  if (p_->fd >= 0) ::close(p_->fd);
  if (p_->wake_pipe[0] >= 0) ::close(p_->wake_pipe[0]);
  if (p_->wake_pipe[1] >= 0) ::close(p_->wake_pipe[1]);
  ::unlink(p_->socket_path.c_str());
  p_->fd = p_->wake_pipe[0] = p_->wake_pipe[1] = -1;
}

std::string client_send(const fs::path& socket_path,
                        const std::string& command) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return "";
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  if (socket_path.string().size() >= sizeof(a.sun_path)) {
    ::close(fd);
    return "";
  }
  std::strncpy(a.sun_path, socket_path.c_str(), sizeof(a.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd); return "";
  }
  std::string c = command + "\n";
  write_all(fd, c);
  auto line = read_line(fd);
  ::close(fd);
  return line;
}

}  // namespace popy::ipc
