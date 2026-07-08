#include "hmac.h"

#include <cstring>

#include "hash.h"

namespace popy::hmac {

namespace {

constexpr std::size_t kBlock = 64;  // SHA-256 block size

}  // namespace

std::array<unsigned char, 32> hmac_sha256(std::string_view key,
                                          std::string_view msg) {
  unsigned char k[kBlock] = {0};
  if (key.size() > kBlock) {
    popy::hash::Sha256Streaming kh;
    kh.update(key.data(), key.size());
    auto d = kh.digest_bytes();
    std::memcpy(k, d.data(), d.size());
  } else if (!key.empty()) {
    std::memcpy(k, key.data(), key.size());
  }

  unsigned char ipad[kBlock];
  unsigned char opad[kBlock];
  for (std::size_t i = 0; i < kBlock; ++i) {
    ipad[i] = static_cast<unsigned char>(k[i] ^ 0x36);
    opad[i] = static_cast<unsigned char>(k[i] ^ 0x5c);
  }

  popy::hash::Sha256Streaming inner;
  inner.update(ipad, kBlock);
  inner.update(msg.data(), msg.size());
  auto inner_digest = inner.digest_bytes();

  popy::hash::Sha256Streaming outer;
  outer.update(opad, kBlock);
  outer.update(inner_digest.data(), inner_digest.size());
  return outer.digest_bytes();
}

std::string hmac_sha256_hex(std::string_view key, std::string_view msg) {
  auto mac = hmac_sha256(key, msg);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(mac.size() * 2);
  for (unsigned char b : mac) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

bool consteq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  volatile unsigned char acc = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    acc = static_cast<unsigned char>(
        acc | (static_cast<unsigned char>(a[i]) ^
               static_cast<unsigned char>(b[i])));
  }
  return acc == 0;
}

}  // namespace popy::hmac
