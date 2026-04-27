// Filesystem watcher (PIMPL).
//
// One header, two platform implementations: watcher_fsevents.cpp (macOS) and
// watcher_inotify.cpp (Linux). CMake compiles exactly one of them per build.
//
// The watcher emits paths of files that were just created or finished being
// written. Callers are responsible for filtering against config exclude
// patterns (we don't pull config in here on purpose — keeps the watcher dumb
// and easy to test).
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace popy::watch {

class Watcher {
 public:
  // Called with the absolute path of a file that's just appeared / finished
  // writing. Called from a watcher-internal thread; user code should hand
  // off to its own queue and return quickly.
  using Callback = std::function<void(std::filesystem::path)>;

  explicit Watcher(std::vector<std::filesystem::path> roots);
  ~Watcher();
  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;

  // Begin watching. Spawns whichever threads the impl needs. start() returns
  // immediately; the callback fires asynchronously.
  void start(Callback);

  // Stop watching and join internal threads. Idempotent.
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace popy::watch
