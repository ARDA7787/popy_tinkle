#include "render/markdown.h"

#include <unistd.h>

#include <stdexcept>

namespace popy::render {

std::string text_passthrough(int fd, std::size_t max_bytes) {
  std::string out;
  char buf[64 * 1024];
  while (out.size() < max_bytes) {
    std::size_t room = max_bytes - out.size();
    std::size_t want = room < sizeof(buf) ? room : sizeof(buf);
    ssize_t n = ::read(fd, buf, want);
    if (n == 0) return out;
    if (n < 0) throw std::runtime_error("text read failed");
    out.append(buf, static_cast<std::size_t>(n));
  }
  out += "\n[truncated]\n";
  return out;
}

}  // namespace popy::render
