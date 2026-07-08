// Cryptographically strong random bytes.
//
// arc4random_buf on macOS, /dev/urandom elsewhere — the same entropy source
// uuid::v4() has always used, extracted so keyring can share it.
#pragma once
#include <cstddef>

namespace popy::rand {

// Fill p[0..n) with random bytes. Throws std::runtime_error on entropy
// source failure; never returns partially-filled buffers.
void fill(unsigned char* p, std::size_t n);

}  // namespace popy::rand
