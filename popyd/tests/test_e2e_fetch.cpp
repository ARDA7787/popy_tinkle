// End-to-end test of the Mode A property:
// `popy fetch` writes ONLY the `<name>_popy` file, never the original-extension
// filename, and the SHA-256 in the sidecar matches the served bytes.
//
// We spin up a tiny HTTP/1.1 server on a loopback port in a background thread,
// hand its URL to popy::net::fetch(), and assert filesystem state.

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/attest.h"
#include "core/hash.h"
#include "core/keyring.h"
#include "core/sidecar.h"
#include "net/fetch.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

namespace {

struct TinyServer {
  int srv = -1;
  uint16_t port = 0;
  std::thread th;
  std::atomic<bool> stop{false};
  std::string body;
  std::string content_type = "application/pdf";

  void serve() {
    while (!stop) {
      sockaddr_in cli{};
      socklen_t cl = sizeof(cli);
      int fd = ::accept(srv, reinterpret_cast<sockaddr*>(&cli), &cl);
      if (fd < 0) { if (stop) return; continue; }

      // Drain request (we don't care, we always serve `body`).
      char buf[1024];
      ::recv(fd, buf, sizeof(buf), 0);

      char hdr[256];
      int hn = std::snprintf(
          hdr, sizeof(hdr),
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: %s\r\n"
          "Content-Length: %zu\r\n"
          "Connection: close\r\n\r\n",
          content_type.c_str(), body.size());
      ::send(fd, hdr, static_cast<size_t>(hn), 0);
      size_t off = 0;
      while (off < body.size()) {
        auto n = ::send(fd, body.data() + off, body.size() - off, 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
      }
      ::close(fd);
    }
  }
  void start() {
    srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t al = sizeof(a);
    ::getsockname(srv, reinterpret_cast<sockaddr*>(&a), &al);
    port = ntohs(a.sin_port);
    ::listen(srv, 4);
    th = std::thread([this]{ serve(); });
  }
  void shut() {
    stop = true;
    ::shutdown(srv, SHUT_RDWR);
    ::close(srv);
    if (th.joinable()) th.join();
  }
  std::string url(const std::string& name = "report.pdf") const {
    return "http://127.0.0.1:" + std::to_string(port) + "/" + name;
  }
};

std::string make_random_pdf(size_t size) {
  // Real PDFs start with "%PDF-1.4". popy::mime::magic_consistent will accept
  // the canonical leading form.
  std::string s = "%PDF-1.4\n";
  s.reserve(size);
  std::mt19937 rng(7);
  while (s.size() < size) s.push_back(static_cast<char>(rng()));
  return s;
}

// Serves one request: headers + the first half of the body, then stalls until
// `release` is set, then the rest. Lets the test scan on-disk state while
// hostile bytes are provably still in flight.
struct StallServer {
  int srv = -1;
  uint16_t port = 0;
  std::thread th;
  std::string body;
  std::atomic<bool> half_sent{false};
  std::atomic<bool> release{false};

  void start() {
    srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t al = sizeof(a);
    ::getsockname(srv, reinterpret_cast<sockaddr*>(&a), &al);
    port = ntohs(a.sin_port);
    ::listen(srv, 1);
    th = std::thread([this] {
      sockaddr_in cli{};
      socklen_t cl = sizeof(cli);
      int fd = ::accept(srv, reinterpret_cast<sockaddr*>(&cli), &cl);
      if (fd < 0) return;
      char buf[1024];
      ::recv(fd, buf, sizeof(buf), 0);
      char hdr[256];
      int hn = std::snprintf(hdr, sizeof(hdr),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: application/pdf\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n\r\n",
                             body.size());
      ::send(fd, hdr, static_cast<size_t>(hn), 0);
      size_t half = body.size() / 2;
      size_t off = 0;
      while (off < half) {
        auto n = ::send(fd, body.data() + off, half - off, 0);
        if (n <= 0) { ::close(fd); return; }
        off += static_cast<size_t>(n);
      }
      half_sent = true;
      while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      while (off < body.size()) {
        auto n = ::send(fd, body.data() + off, body.size() - off, 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
      }
      ::close(fd);
    });
  }
  void shut() {
    release = true;
    ::shutdown(srv, SHUT_RDWR);
    ::close(srv);
    if (th.joinable()) th.join();
  }
  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port) + "/report.pdf";
  }
};

}  // namespace

int main() {
  fs::path stage = fs::temp_directory_path() / "popy_test_e2e_stage";
  fs::remove_all(stage);
  fs::create_directories(stage);

  TinyServer srv;
  srv.body = make_random_pdf(64 * 1024);
  srv.start();

  POPY_RUN("popy fetch writes only <name>_popy with valid sidecar") {
    popy::net::FetchOptions opts;
    opts.url           = srv.url("report.pdf");
    opts.stage_dir     = stage;
    opts.expected_mime = "application/pdf";
    opts.allow_private_network = true;
    opts.key_file      = stage / "popy.key";

    auto res = popy::net::fetch(opts);

    // The .popy file exists, the original-name file does NOT.
    auto id_dir = stage / res.record.id;
    POPY_EXPECT(fs::exists(id_dir / "report.pdf_popy"));
    POPY_EXPECT(!fs::exists(id_dir / "report.pdf"));

    // Mode is 0000.
    struct stat st{};
    POPY_EXPECT_EQ(::stat((id_dir / "report.pdf_popy").c_str(), &st), 0);
    POPY_EXPECT_EQ(static_cast<int>(st.st_mode & 07777), 0);

    // Sidecar present and well-formed.
    POPY_EXPECT(fs::exists(id_dir / "report.pdf_popy.meta.json"));
    auto rec = popy::sidecar::read(id_dir / "report.pdf_popy.meta.json");
    POPY_EXPECT_EQ(rec.id,               res.record.id);
    POPY_EXPECT_EQ(rec.originalFilename, std::string("report.pdf"));
    POPY_EXPECT_EQ(rec.sizeBytes,        static_cast<std::int64_t>(srv.body.size()));
    POPY_EXPECT_EQ(rec.sha256,           popy::hash::sha256_hex(srv.body));
    POPY_EXPECT_EQ(rec.path,             std::string("fallback"));
    POPY_EXPECT_EQ(rec.status,           std::string("stored"));
    POPY_EXPECT_EQ(rec.origin,           std::string("fetch"));

    // The sidecar carries a valid signature pinned to the on-disk name.
    auto key = popy::keyring::load_or_create(stage / "popy.key");
    POPY_EXPECT(popy::attest::verify(rec, "report.pdf_popy", key) ==
                popy::attest::VerifyResult::ok);
  };

  POPY_RUN("magic-byte mismatch refuses the fetch") {
    TinyServer bad;
    bad.body = std::string("not actually a pdf, just text");
    bad.start();

    popy::net::FetchOptions opts;
    opts.url           = bad.url("report.pdf");
    opts.stage_dir     = stage;
    opts.expected_mime = "application/pdf";
    opts.allow_private_network = true;

    bool threw = false;
    try { popy::net::fetch(opts); }
    catch (const std::exception&) { threw = true; }
    POPY_EXPECT(threw);
    bad.shut();
  };

  POPY_RUN("in-flight bytes are never named _popy nor readable") {
    // Fresh stage dir so the scan can't be satisfied by earlier tests' files.
    fs::path stage2 = fs::temp_directory_path() / "popy_test_e2e_stall";
    fs::remove_all(stage2);
    fs::create_directories(stage2);

    StallServer stall;
    stall.body = make_random_pdf(256 * 1024);
    stall.start();

    popy::net::FetchOptions opts;
    opts.url                   = stall.url();
    opts.stage_dir             = stage2;
    opts.expected_mime         = "application/pdf";
    opts.allow_private_network = true;
    opts.key_file              = stage2 / "popy.key";
    // Force the portable .part branch so this test pins its guarantees on
    // every platform; on Linux the O_TMPFILE branch is strictly stronger
    // (no name at all) and is covered by test_quarantine_file.
    opts.force_part_fallback   = true;

    popy::net::FetchResult res;
    std::exception_ptr fetch_error;
    std::thread fetcher([&] {
      try { res = popy::net::fetch(opts); }
      catch (...) { fetch_error = std::current_exception(); }
    });

    // Wait until the server has provably sent only half the body.
    for (int i = 0; i < 2000 && !stall.half_sent; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    POPY_EXPECT(stall.half_sent.load());

    // Mid-transfer scan: no final _popy name anywhere; the in-flight .part
    // exists and is mode 0000; no sidecar yet.
    bool saw_popy_name = false;
    bool saw_sidecar = false;
    int part_count = 0;
    bool parts_are_0000 = true;
    for (auto& de : fs::recursive_directory_iterator(stage2)) {
      if (!de.is_regular_file()) continue;
      auto name = de.path().filename().string();
      if (name.size() >= 5 && name.substr(name.size() - 5) == "_popy") {
        saw_popy_name = true;
      }
      if (name.size() >= 10 &&
          name.substr(name.size() - 10) == ".meta.json") {
        saw_sidecar = true;
      }
      if (name.size() >= 5 && name.substr(name.size() - 5) == ".part") {
        ++part_count;
        struct stat st{};
        if (::lstat(de.path().c_str(), &st) != 0 ||
            (st.st_mode & 07777) != 0) {
          parts_are_0000 = false;
        }
      }
    }
    POPY_EXPECT(!saw_popy_name);
    POPY_EXPECT(!saw_sidecar);
    POPY_EXPECT_EQ(part_count, 1);
    POPY_EXPECT(parts_are_0000);

    // Unstall; the fetch must complete and commit normally.
    stall.release = true;
    fetcher.join();
    stall.shut();
    POPY_EXPECT(!fetch_error);

    auto id_dir = stage2 / res.record.id;
    struct stat st{};
    POPY_EXPECT_EQ(::stat((id_dir / "report.pdf_popy").c_str(), &st), 0);
    POPY_EXPECT_EQ(static_cast<int>(st.st_mode & 07777), 0);
    POPY_EXPECT(!fs::exists(id_dir / "report.pdf_popy.part"));
    POPY_EXPECT_EQ(res.record.sha256, popy::hash::sha256_hex(stall.body));

    auto rec = popy::sidecar::read(id_dir / "report.pdf_popy.meta.json");
    auto key = popy::keyring::load_or_create(stage2 / "popy.key");
    POPY_EXPECT(popy::attest::verify(rec, "report.pdf_popy", key) ==
                popy::attest::VerifyResult::ok);

    fs::remove_all(stage2);
  };

  POPY_RUN("SSRF guard rejects loopback by default") {
    popy::net::FetchOptions opts;
    opts.url           = srv.url("report.pdf");
    opts.stage_dir     = stage;
    opts.expected_mime = "application/pdf";

    bool threw = false;
    try { popy::net::fetch(opts); }
    catch (const std::exception& e) {
      threw = std::string(e.what()).find("SSRF") != std::string::npos;
    }
    POPY_EXPECT(threw);
  };

  srv.shut();
  fs::remove_all(stage);
  return exit_code();
}
