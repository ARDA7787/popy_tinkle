#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/sidecar.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  static int counter = 0;
  std::filesystem::path p =
      std::filesystem::temp_directory_path() /
      ("popy_fuzz_sidecar_" + std::to_string(counter++) + ".json");
  try {
    {
      std::ofstream out(p, std::ios::binary);
      out.write(reinterpret_cast<const char*>(data),
                static_cast<std::streamsize>(size));
    }
    auto r = popy::sidecar::read(p);
    (void)r;
  } catch (const std::exception&) {
  }
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return 0;
}
