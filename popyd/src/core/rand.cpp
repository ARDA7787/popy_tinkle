#include "rand.h"

#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>

#ifdef __APPLE__
#include <stdlib.h>  // arc4random_buf
#endif

namespace popy::rand {

void fill(unsigned char* p, std::size_t n) {
#ifdef __APPLE__
  ::arc4random_buf(p, n);
#else
  // /dev/urandom — same entropy source as glibc's getrandom(2) for our needs.
  int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot open /dev/urandom");
  std::size_t got = 0;
  while (got < n) {
    auto r = ::read(fd, p + got, n - got);
    if (r <= 0) {
      ::close(fd);
      throw std::runtime_error("/dev/urandom read failed");
    }
    got += static_cast<std::size_t>(r);
  }
  ::close(fd);
#endif
}

}  // namespace popy::rand
