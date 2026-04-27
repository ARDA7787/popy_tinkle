// macOS FSEvents-based watcher.
//
// FSEvents fires per-file event callbacks (kFSEventStreamCreateFlagFileEvents).
// When a path arrives, we put it in a "pending" map; a stability thread polls
// each entry's size every 250ms and emits the path to the user callback once
// the size has stayed unchanged for 750ms.
//
// We use the modern dispatch-queue API (FSEventStreamSetDispatchQueue),
// available since macOS 10.6 and the only non-deprecated path on macOS 13+.
// GCD owns the FSEvents callback thread, so we don't need a CFRunLoop.

#include "watch/watcher.h"

#include <CoreServices/CoreServices.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"

namespace fs = std::filesystem;

namespace popy::watch {

namespace {

constexpr auto kPollEvery = std::chrono::milliseconds(250);
constexpr auto kStableFor = std::chrono::milliseconds(750);

struct PendingFile {
  off_t size = -1;
  std::chrono::steady_clock::time_point last_change{};
  std::chrono::steady_clock::time_point first_seen{};
};

}  // namespace

struct Watcher::Impl {
  std::vector<fs::path> roots;
  Callback cb;

  // FSEvents stream + the dispatch queue it runs its callbacks on.
  FSEventStreamRef    stream = nullptr;
  dispatch_queue_t    queue  = nullptr;

  // Stability state shared between FSEvents callback and stability thread.
  std::mutex                       mu;
  std::condition_variable          cv;
  std::map<std::string, PendingFile> pending;
  std::atomic<bool>                stop_flag{false};
  std::thread                      stability_thread;

  static void fsevents_cb(ConstFSEventStreamRef /*stream*/,
                          void* userdata,
                          size_t count,
                          void* paths_ptr,
                          const FSEventStreamEventFlags flags[],
                          const FSEventStreamEventId* /*ids*/);

  void stability_loop();
};

void Watcher::Impl::stability_loop() {
  using clock = std::chrono::steady_clock;
  while (!stop_flag) {
    std::vector<std::string> ready;
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait_for(lk, kPollEvery, [&]{ return stop_flag.load(); });
      if (stop_flag) return;
      auto now = clock::now();
      for (auto it = pending.begin(); it != pending.end();) {
        struct stat st {};
        if (::lstat(it->first.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
          it = pending.erase(it);
          continue;
        }
        if (st.st_size != it->second.size) {
          // Size changed since we last looked → still being written.
          it->second.size = st.st_size;
          it->second.last_change = now;
          ++it;
          continue;
        }
        if (now - it->second.last_change >= kStableFor) {
          ready.push_back(it->first);
          it = pending.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (const auto& p : ready) {
      try { cb(fs::path(p)); }
      catch (const std::exception& e) {
        popy::log::warn(std::string("watcher cb threw: ") + e.what());
      }
    }
  }
}

void Watcher::Impl::fsevents_cb(ConstFSEventStreamRef /*stream*/,
                                void* userdata,
                                size_t count,
                                void* paths_ptr,
                                const FSEventStreamEventFlags flags[],
                                const FSEventStreamEventId* /*ids*/) {
  auto* self = static_cast<Impl*>(userdata);
  auto** paths = static_cast<char**>(paths_ptr);
  auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> lk(self->mu);
  for (size_t i = 0; i < count; ++i) {
    if (flags[i] & (kFSEventStreamEventFlagItemRemoved |
                    kFSEventStreamEventFlagItemRenamed)) {
      // Renamed *away* or removed: drop any pending entry; its old path will
      // never stabilize.
      self->pending.erase(paths[i]);
    }
    // We treat creates and modifies the same: enter pending and let the
    // stability loop confirm.
    if (flags[i] & (kFSEventStreamEventFlagItemCreated |
                    kFSEventStreamEventFlagItemModified |
                    kFSEventStreamEventFlagItemRenamed)) {
      auto& e = self->pending[paths[i]];
      if (e.first_seen.time_since_epoch().count() == 0) e.first_seen = now;
      e.last_change = now;
      e.size = -1;  // force re-stat next pass
    }
  }
  self->cv.notify_all();
}

Watcher::Watcher(std::vector<fs::path> roots)
    : p_(std::make_unique<Impl>()) {
  p_->roots = std::move(roots);
}

Watcher::~Watcher() { stop(); }

void Watcher::start(Callback cb) {
  p_->cb = std::move(cb);
  p_->stop_flag = false;

  // Build the CFArray of paths.
  std::vector<CFStringRef> cf_paths;
  for (const auto& r : p_->roots) {
    cf_paths.push_back(CFStringCreateWithCString(
        nullptr, r.c_str(), kCFStringEncodingUTF8));
  }
  CFArrayRef path_array = CFArrayCreate(
      nullptr, reinterpret_cast<const void**>(cf_paths.data()),
      static_cast<CFIndex>(cf_paths.size()), &kCFTypeArrayCallBacks);

  FSEventStreamContext ctx{};
  ctx.info = p_.get();

  p_->stream = FSEventStreamCreate(
      nullptr,
      &Impl::fsevents_cb,
      &ctx,
      path_array,
      kFSEventStreamEventIdSinceNow,
      0.1 /* latency seconds */,
      kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);

  CFRelease(path_array);
  for (auto s : cf_paths) CFRelease(s);

  if (!p_->stream) {
    throw std::runtime_error("watcher: FSEventStreamCreate failed");
  }

  // Run callbacks on a serial dispatch queue. GCD owns the thread; we just
  // attach. No CFRunLoop, no extra thread.
  p_->queue = dispatch_queue_create("com.popy.fsevents", DISPATCH_QUEUE_SERIAL);
  FSEventStreamSetDispatchQueue(p_->stream, p_->queue);
  FSEventStreamStart(p_->stream);

  p_->stability_thread = std::thread([this]{ p_->stability_loop(); });

  popy::log::info("watcher started (FSEvents)");
}

void Watcher::stop() {
  if (!p_) return;
  if (p_->stop_flag.exchange(true)) return;  // idempotent

  p_->cv.notify_all();
  if (p_->stability_thread.joinable()) p_->stability_thread.join();

  if (p_->stream) {
    FSEventStreamStop(p_->stream);
    FSEventStreamInvalidate(p_->stream);
    FSEventStreamRelease(p_->stream);
    p_->stream = nullptr;
  }
  if (p_->queue) {
    dispatch_release(p_->queue);
    p_->queue = nullptr;
  }
  popy::log::info("watcher stopped");
}

}  // namespace popy::watch
