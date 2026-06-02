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

#include "core/hash.h"
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
