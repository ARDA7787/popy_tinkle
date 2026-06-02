#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/mime.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (size < 1) return 0;
  static constexpr const char* kMimes[] = {
      "application/pdf", "image/png", "image/jpeg",
      "application/zip", "application/octet-stream",
      "application/x-msdos-program", "application/x-elf",
  };
  std::size_t mime_idx = data[size - 1] % std::size(kMimes);
  std::string_view body(reinterpret_cast<const char*>(data), size - 1);
  popy::mime::magic_consistent(body, kMimes[mime_idx]);
  return 0;
}
