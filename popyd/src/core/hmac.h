// HMAC-SHA256 (RFC 2104) built on the vendored picosha2 via Sha256Streaming.
//
// Used to sign sidecar records with the same-user key from keyring. No new
// dependency: Ed25519/libsodium buys nothing in a single-user trust model
// where the verifier and the signer are the same account.
#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace popy::hmac {

// HMAC-SHA256 over `msg` with `key` (any length; keys longer than the 64-byte
// block are hashed first, per RFC 2104).
std::array<unsigned char, 32> hmac_sha256(std::string_view key,
                                          std::string_view msg);

// Same, as 64 lowercase hex chars.
std::string hmac_sha256_hex(std::string_view key, std::string_view msg);

// Constant-time equality for MAC comparison. Lengths are compared first
// (length is not secret); equal-length inputs compare in time independent
// of where they differ.
bool consteq(std::string_view a, std::string_view b);

}  // namespace popy::hmac
