// Watcher hardening (quarantine_existing_file) and the startup repair pass.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/attest.h"
#include "core/config.h"
#include "core/keyring.h"
#include "core/sidecar.h"
#include "store/quarantine.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

namespace {

void write_file(const fs::path& p, const std::string& body, mode_t mode = 0644) {
  { std::ofstream f(p, std::ios::binary); f << body; }
  ::chmod(p.c_str(), mode);
}

popy::config::Config test_cfg(const fs::path& watch, const fs::path& stage) {
  popy::config::Config cfg;
  cfg.watch_dirs = {watch};
  cfg.stage_dir  = stage;
  return cfg;
}

void age_file(const fs::path& p, std::int64_t seconds_ago) {
  struct timeval tv[2];
  auto t = ::time(nullptr) - seconds_ago;
  tv[0].tv_sec = t; tv[0].tv_usec = 0;
  tv[1].tv_sec = t; tv[1].tv_usec = 0;
  ::utimes(p.c_str(), tv);
}

}  // namespace

int main() {
  fs::path tmp = fs::temp_directory_path() / "popy_test_repair";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  fs::path keyfile = tmp / "popy.key";

  POPY_RUN("quarantine: happy path locks to 0000 and signs") {
    fs::path dir = tmp / "happy";
    fs::create_directories(dir);
    write_file(dir / "doc.txt", "watched bytes");

    auto r = popy::store::quarantine_existing_file(dir / "doc.txt", keyfile);

    POPY_EXPECT(!fs::exists(dir / "doc.txt"));
    struct stat st{};
    POPY_EXPECT_EQ(::lstat(r.popy_path.c_str(), &st), 0);
    POPY_EXPECT_EQ(static_cast<int>(st.st_mode & 07777), 0);

    auto key = popy::keyring::load_or_create(keyfile);
    POPY_EXPECT(popy::attest::verify(r.record,
                                     r.popy_path.filename().string(), key) ==
                popy::attest::VerifyResult::ok);
  };

  POPY_RUN("quarantine: FIFO refused") {
    fs::path dir = tmp / "fifo";
    fs::create_directories(dir);
    POPY_EXPECT_EQ(::mkfifo((dir / "pipe.txt").c_str(), 0644), 0);
    POPY_EXPECT_THROW(
        popy::store::quarantine_existing_file(dir / "pipe.txt", keyfile));
  };

  POPY_RUN("quarantine: symlink refused") {
    fs::path dir = tmp / "sym";
    fs::create_directories(dir);
    write_file(dir / "real.txt", "x");
    fs::create_symlink(dir / "real.txt", dir / "link.txt");
    POPY_EXPECT_THROW(
        popy::store::quarantine_existing_file(dir / "link.txt", keyfile));
  };

  POPY_RUN("quarantine: rename failure restores the original mode") {
    if (::geteuid() == 0) return;  // root ignores directory write bits
    fs::path dir = tmp / "ro";
    fs::create_directories(dir);
    write_file(dir / "keep.txt", "must stay usable", 0640);
    ::chmod(dir.c_str(), 0500);  // rename inside dir now fails EACCES

    bool threw = false;
    try { popy::store::quarantine_existing_file(dir / "keep.txt", keyfile); }
    catch (const std::exception&) { threw = true; }
    ::chmod(dir.c_str(), 0700);
    POPY_EXPECT(threw);

    struct stat st{};
    POPY_EXPECT_EQ(::lstat((dir / "keep.txt").c_str(), &st), 0);
    POPY_EXPECT_EQ(static_cast<int>(st.st_mode & 07777), 0640);
  };

  POPY_RUN("repair: adopts orphaned _popy files") {
    fs::path watch = tmp / "adopt_watch";
    fs::path stage = tmp / "adopt_stage";
    fs::create_directories(watch);
    fs::create_directories(stage);
    write_file(stage / "orphan.txt_popy", "orphan bytes", 0644);
    write_file(watch / "other.pdf_popy.2", "collision orphan", 0644);

    auto stats = popy::store::repair(test_cfg(watch, stage), keyfile);
    POPY_EXPECT_EQ(stats.adopted, 2);

    auto key = popy::keyring::load_or_create(keyfile);
    {
      auto rec = popy::sidecar::read(stage / "orphan.txt_popy.meta.json");
      POPY_EXPECT_EQ(rec.originalFilename, std::string("orphan.txt"));
      POPY_EXPECT_EQ(rec.sizeBytes, static_cast<std::int64_t>(12));
      POPY_EXPECT(popy::attest::verify(rec, "orphan.txt_popy", key) ==
                  popy::attest::VerifyResult::ok);
      struct stat st{};
      POPY_EXPECT_EQ(::lstat((stage / "orphan.txt_popy").c_str(), &st), 0);
      POPY_EXPECT_EQ(static_cast<int>(st.st_mode & 07777), 0);
    }
    {
      auto rec = popy::sidecar::read(watch / "other.pdf_popy.2.meta.json");
      POPY_EXPECT_EQ(rec.originalFilename, std::string("other.pdf"));
      POPY_EXPECT(popy::attest::verify(rec, "other.pdf_popy.2", key) ==
                  popy::attest::VerifyResult::ok);
    }

    // Idempotent: second run adopts nothing.
    auto again = popy::store::repair(test_cfg(watch, stage), keyfile);
    POPY_EXPECT_EQ(again.adopted, 0);
  };

  POPY_RUN("repair: GCs stale data-less sidecars, keeps young or in-flight") {
    fs::path watch = tmp / "gc_watch";
    fs::path stage = tmp / "gc_stage";
    fs::create_directories(watch);
    fs::create_directories(stage);

    auto mk_sidecar = [&](const fs::path& popy_path) {
      popy::sidecar::Record r;
      r.id               = "00000000-0000-4000-8000-00000000000a";
      r.originalFilename = "gone.bin";
      r.createdAt        = popy::sidecar::now_ms();
      popy::sidecar::write_atomic(
          popy::sidecar::sidecar_path_for(popy_path), r);
    };

    // stale + no data + no .part → GC'd
    mk_sidecar(stage / "gone.bin_popy");
    age_file(stage / "gone.bin_popy.meta.json", 20 * 60);
    // young + no data → kept (fetch may be mid-commit)
    mk_sidecar(stage / "young.bin_popy");
    // stale + .part sibling → kept
    mk_sidecar(stage / "inflight.bin_popy");
    age_file(stage / "inflight.bin_popy.meta.json", 20 * 60);
    write_file(stage / "inflight.bin_popy.part", "partial", 0000);
    // data present → kept
    write_file(stage / "present.bin_popy", "here", 0000);
    mk_sidecar(stage / "present.bin_popy");
    age_file(stage / "present.bin_popy.meta.json", 20 * 60);

    auto stats = popy::store::repair(test_cfg(watch, stage), keyfile);
    POPY_EXPECT_EQ(stats.gc_sidecars, 1);
    POPY_EXPECT(!fs::exists(stage / "gone.bin_popy.meta.json"));
    POPY_EXPECT(fs::exists(stage / "young.bin_popy.meta.json"));
    POPY_EXPECT(fs::exists(stage / "inflight.bin_popy.meta.json"));
    POPY_EXPECT(fs::exists(stage / "present.bin_popy.meta.json"));
  };

  POPY_RUN("list flags data_missing entries") {
    fs::path watch = tmp / "dm_watch";
    fs::path stage = tmp / "dm_stage";
    fs::create_directories(watch);
    fs::create_directories(stage);
    popy::sidecar::Record r;
    r.id               = "00000000-0000-4000-8000-00000000000b";
    r.originalFilename = "lost.bin";
    r.createdAt        = popy::sidecar::now_ms();
    popy::sidecar::write_atomic(
        popy::sidecar::sidecar_path_for(stage / "lost.bin_popy"), r);

    auto entries = popy::store::list(test_cfg(watch, stage));
    POPY_EXPECT_EQ(entries.size(), static_cast<size_t>(1));
    POPY_EXPECT(entries[0].data_missing);
  };

  fs::remove_all(tmp);
  return exit_code();
}
