// AF_UNIX line-protocol status socket.
//
// Server (popyd) accepts one client at a time, reads one line, writes a
// response, closes. No keep-alive, no JSON-RPC — keeps the wire simple.
//
// Client (popy CLI) connects, writes a single line "STATUS\n" / "PAUSE\n" /
// "RESUME\n" / "PING\n", reads the response, closes.
//
// Wire format:
//   request:  "<COMMAND>\n"
//   response: success → "OK [json|text...]\n"
//             failure → "ERR <reason>\n"
//
// The status command's response payload is a single JSON object on one line.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace popy::ipc {

// State the server reports through STATUS. Server reads it under no lock —
// individual fields are atomic.
struct ServerState {
  std::atomic<bool>         paused{false};
  std::atomic<std::int64_t> file_count{0};
  std::atomic<std::int64_t> byte_count{0};
  std::int64_t              start_unix_ms = 0;
};

// Server: bind + listen + accept loop on its own thread.
class StatusServer {
 public:
  StatusServer(std::filesystem::path socket_path, ServerState* state);
  ~StatusServer();
  StatusServer(const StatusServer&) = delete;
  StatusServer& operator=(const StatusServer&) = delete;

  void start();
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

// Client: open, write `command\n`, read one line, close. Returns the line
// without the trailing newline, or std::nullopt if no daemon is reachable.
std::string client_send(const std::filesystem::path& socket_path,
                        const std::string& command);

}  // namespace popy::ipc
