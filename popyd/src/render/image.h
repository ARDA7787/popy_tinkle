#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace popy::render {

std::vector<std::uint8_t> image_to_png(int fd, const std::string& mime_hint);

}  // namespace popy::render
