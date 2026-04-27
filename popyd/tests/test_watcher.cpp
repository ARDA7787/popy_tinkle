// Watcher integration test.
//
// Spins up a popy::watch::Watcher, drops a file in a temp dir, waits for the
// callback, and asserts (a) the path is right, (b) it arrives within the
// 750ms+250ms stability window plus generous margin.
//
// This exercises the FSEvents/inotify code paths end-to-end without the
// full daemon machinery — that's covered by manual smoke tests in the
// README, since launching a real popyd from a unit test is fragile (pid
// file conflicts, status socket conflicts on shared CI runners).

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "test_main.h"
#include "watch/watcher.h"

namespace fs = std::filesystem;
using namespace popy::test;

int main() {
  fs::path tmp = fs::temp_directory_path() / "popy_test_watcher";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  POPY_RUN("watcher emits a path for a newly written file") {
    popy::watch::Watcher w({tmp});
    std::mutex                   mu;
    std::condition_variable      cv;
    std::vector<fs::path>        seen;

    w.start([&](fs::path p) {
      std::lock_guard<std::mutex> lk(mu);
      seen.push_back(std::move(p));
      cv.notify_all();
    });

    // Brief delay to let FSEvents settle on its initial scan.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Drop a file. On macOS, FSEvents+stability gives us the path ~1s later.
    auto target = tmp / "watched.txt";
    {
      std::ofstream f(target);
      f << "hello watcher";
    }

    {
      std::unique_lock<std::mutex> lk(mu);
      bool got = cv.wait_for(lk, std::chrono::seconds(5),
                             [&]{ return !seen.empty(); });
      POPY_EXPECT(got);
      // FSEvents returns canonical paths (/private/var/...); compare canon.
      POPY_EXPECT(fs::weakly_canonical(seen.front()) ==
                  fs::weakly_canonical(target));
    }

    w.stop();
  };

  fs::remove_all(tmp);
  return exit_code();
}
