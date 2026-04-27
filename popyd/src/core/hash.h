// Streaming SHA-256.
//
// Wraps picosha2's hash256_one_by_one so callers don't need the vendored header
// in their own translation units. The wrapper is thin enough that compilers
// inline through it, and it keeps picosha2 confined to one .cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace popy::hash {

class Sha256Streaming {
 public:
  Sha256Streaming();
  ~Sha256Streaming();
  Sha256Streaming(const Sha256Streaming&) = delete;
  Sha256Streaming& operator=(const Sha256Streaming&) = delete;

  void update(const void* data, std::size_t len);
  // Returns lowercase hex (64 chars). May only be called once; further updates
  // after digest() are undefined.
  std::string digest_hex();

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

// Convenience: hash an in-memory buffer.
std::string sha256_hex(std::string_view data);

}  // namespace popy::hash
