#include "render/image.h"

#include <unistd.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "stb_image.h"
#include "stb_image_write.h"

namespace popy::render {

namespace {

constexpr std::size_t kMaxInput = 32U * 1024U * 1024U;
constexpr std::size_t kMaxPng = 2U * 1024U * 1024U;

std::vector<std::uint8_t> read_all_capped(int fd) {
  std::vector<std::uint8_t> data;
  std::uint8_t buf[64 * 1024];
  while (true) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) throw std::runtime_error("image: read failed");
    if (data.size() + static_cast<std::size_t>(n) > kMaxInput) {
      throw std::runtime_error("image: input exceeded 32MB cap");
    }
    data.insert(data.end(), buf, buf + n);
  }
  return data;
}

void png_write(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<std::uint8_t>*>(context);
  if (size < 0 || out->size() + static_cast<std::size_t>(size) > kMaxPng) {
    return;
  }
  auto* bytes = static_cast<std::uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

std::vector<std::uint8_t> image_to_png(int fd, const std::string& mime_hint) {
  (void)mime_hint;
  auto encoded = read_all_capped(fd);

  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(encoded.data(),
                                          static_cast<int>(encoded.size()),
                                          &width, &height, &channels, 4);
  if (pixels == nullptr) {
    throw std::runtime_error(std::string("image decode failed: ") +
                             stbi_failure_reason());
  }

  std::vector<std::uint8_t> png;
  int ok = stbi_write_png_to_func(png_write, &png, width, height, 4, pixels,
                                  width * 4);
  stbi_image_free(pixels);
  if (ok == 0) throw std::runtime_error("image encode failed");
  if (png.size() > kMaxPng) throw std::runtime_error("PNG exceeded 2MB output cap");
  return png;
}

}  // namespace popy::render
