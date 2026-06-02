#include "render/image.h"
#include "render/markdown.h"
#include "render/pdf.h"
#include "render/sandbox.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann_json.hpp"

using nlohmann::json;

namespace {

constexpr int kRenderFd = 3;

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

void write_all(const std::vector<std::uint8_t>& bytes) {
  std::size_t off = 0;
  while (off < bytes.size()) {
    ssize_t n = ::write(STDOUT_FILENO, bytes.data() + off, bytes.size() - off);
    if (n < 0) throw std::runtime_error("stdout write failed");
    off += static_cast<std::size_t>(n);
  }
}

void write_all(const std::string& text) {
  std::size_t off = 0;
  while (off < text.size()) {
    ssize_t n = ::write(STDOUT_FILENO, text.data() + off, text.size() - off);
    if (n < 0) throw std::runtime_error("stdout write failed");
    off += static_cast<std::size_t>(n);
  }
}

}  // namespace

int main() {
  if (!popy::render::set_limits() || !popy::render::enter_sandbox()) {
    std::cout << json{{"ok", false}, {"error", "sandbox setup failed"}}.dump()
              << "\n";
    return 2;
  }

  try {
    std::string line;
    if (!std::getline(std::cin, line)) {
      throw std::runtime_error("missing render command");
    }
    json command = json::parse(line);
    std::string mode = command.value("mode", "");
    std::string mime = command.value("mime", "");
    int page = command.value("page", 0);
    int fd = command.value("fd", kRenderFd);
    if (fd != kRenderFd) throw std::runtime_error("unexpected fd");

    struct stat st {};
    if (::fstat(kRenderFd, &st) != 0 || !S_ISREG(st.st_mode)) {
      throw std::runtime_error("render fd is not a regular file");
    }

    if (mode == "text") {
      if (mime == "application/pdf") {
        auto info = popy::render::extract_text(kRenderFd);
        std::cout << json{{"ok", true}, {"pages", info.page_count},
                          {"bytes", info.text_sample.size()}}.dump()
                  << "\n";
        std::cout.flush();
        write_all(info.text_sample);
      } else {
        auto text = popy::render::text_passthrough(kRenderFd);
        std::cout << json{{"ok", true}, {"bytes", text.size()}}.dump() << "\n";
        std::cout.flush();
        write_all(text);
      }
      return 0;
    }

    if (mode == "png") {
      std::vector<std::uint8_t> png;
      if (mime == "application/pdf") {
        png = popy::render::render_page_png(kRenderFd, page);
      } else if (starts_with(mime, "image/")) {
        png = popy::render::image_to_png(kRenderFd, mime);
      } else {
        throw std::runtime_error("unsupported mime for png render: " + mime);
      }
      std::cout << json{{"ok", true}, {"bytes", png.size()}}.dump() << "\n";
      std::cout.flush();
      write_all(png);
      return 0;
    }

    throw std::runtime_error("unknown render mode: " + mode);
  } catch (const std::exception& e) {
    std::cout << json{{"ok", false}, {"error", e.what()}}.dump() << "\n";
    return 1;
  }
}
