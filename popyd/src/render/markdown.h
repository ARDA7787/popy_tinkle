#pragma once

#include <cstddef>
#include <string>

namespace popy::render {

std::string text_passthrough(int fd, std::size_t max_bytes = 1024U * 1024U);

}  // namespace popy::render
