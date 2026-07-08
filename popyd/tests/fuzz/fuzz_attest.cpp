// attest::verify must never crash and never falsely accept.
//
// The fuzzer drives record fields, the filename, and the stored signature
// from the input bytes. Any verify() result other than ok is fine; ok is
// only legitimate when the signature actually equals the HMAC over the
// canonical payload — which we recompute and assert.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/attest.h"
#include "core/hmac.h"
#include "core/sidecar.h"

namespace {

const std::string kKey(32, 'K');

std::string take(const std::uint8_t*& data, std::size_t& size,
                 std::size_t max_len) {
  if (size == 0) return {};
  std::size_t len = data[0] % (max_len + 1);
  ++data;
  --size;
  if (len > size) len = size;
  std::string out(reinterpret_cast<const char*>(data), len);
  data += len;
  size -= len;
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  popy::sidecar::Record r;
  r.id               = take(data, size, 64);
  r.originalFilename = take(data, size, 64);
  r.sha256           = take(data, size, 80);
  r.mime             = take(data, size, 48);
  r.sourceUrl        = take(data, size, 96);
  r.sourceHost       = take(data, size, 48);
  r.status           = take(data, size, 16);
  r.origin           = take(data, size, 16);
  if (size >= 8) {
    std::int64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | data[i];
    r.sizeBytes = v;
    data += 8;
    size -= 8;
  }
  std::string name = take(data, size, 64);
  r.sig    = take(data, size, 128);
  r.sigAlg = take(data, size, 32);

  auto result = popy::attest::verify(r, name, kKey);
  if (result == popy::attest::VerifyResult::ok) {
    // ok must imply the signature really is the HMAC of the canonical
    // payload under the right algorithm tag — anything else is a false
    // accept and must abort the fuzzer.
    auto expected = popy::hmac::hmac_sha256_hex(
        kKey, popy::attest::canonical_payload(r, name));
    if (!r.sig || *r.sig != expected ||
        !r.sigAlg || *r.sigAlg != popy::attest::kSigAlg) {
      __builtin_trap();
    }
  }
  return 0;
}
