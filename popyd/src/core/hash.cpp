#include "hash.h"

#include "picosha2.h"

namespace popy::hash {

struct Sha256Streaming::Impl {
  picosha2::hash256_one_by_one h;
  Impl() { h.init(); }
};

Sha256Streaming::Sha256Streaming() : p_(std::make_unique<Impl>()) {}
Sha256Streaming::~Sha256Streaming() = default;

void Sha256Streaming::update(const void* data, std::size_t len) {
  const auto* b = static_cast<const unsigned char*>(data);
  p_->h.process(b, b + len);
}

std::string Sha256Streaming::digest_hex() {
  p_->h.finish();
  std::string out;
  picosha2::get_hash_hex_string(p_->h, out);
  return out;
}

std::array<unsigned char, 32> Sha256Streaming::digest_bytes() {
  p_->h.finish();
  std::array<unsigned char, 32> out{};
  p_->h.get_hash_bytes(out.begin(), out.end());
  return out;
}

std::string sha256_hex(std::string_view data) {
  Sha256Streaming s;
  s.update(data.data(), data.size());
  return s.digest_hex();
}

}  // namespace popy::hash
