#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace popy::render {

struct PdfInfo {
  int page_count = 0;
  std::string text_sample;
};

PdfInfo extract_text(int fd);
std::vector<std::uint8_t> render_page_png(int fd, int page_num);

}  // namespace popy::render
