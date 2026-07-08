#include "attest.h"

#include <stdexcept>

#include "core/hmac.h"

namespace popy::attest {

std::string canonical_payload(const popy::sidecar::Record& r,
                              std::string_view popy_filename) {
  std::string p = "popy-attest-v1\n";
  auto add = [&p](std::string_view k, std::string_view v) {
    p += k;
    p += ':';
    p += std::to_string(v.size());
    p += ':';
    p += v;
    p += '\n';
  };
  add("schema",  std::to_string(r.schemaVersion));
  add("id",      r.id);
  add("name",    popy_filename);
  add("orig",    r.originalFilename);
  add("sha256",  r.sha256);
  add("size",    std::to_string(r.sizeBytes));
  add("mime",    r.mime);
  add("url",     r.sourceUrl);
  add("host",    r.sourceHost);
  add("created", std::to_string(r.createdAt));
  add("status",  r.status);
  add("origin",  r.origin);
  return p;
}

void sign(popy::sidecar::Record& r, std::string_view popy_filename,
          std::string_view key) {
  r.sig    = popy::hmac::hmac_sha256_hex(key, canonical_payload(r, popy_filename));
  r.sigAlg = kSigAlg;
}

VerifyResult verify(const popy::sidecar::Record& r,
                    std::string_view popy_filename, std::string_view key) {
  if (!r.sig || r.sig->empty()) return VerifyResult::missing_signature;
  if (!r.sigAlg || *r.sigAlg != kSigAlg) return VerifyResult::unknown_alg;
  auto expected =
      popy::hmac::hmac_sha256_hex(key, canonical_payload(r, popy_filename));
  return popy::hmac::consteq(expected, *r.sig) ? VerifyResult::ok
                                               : VerifyResult::bad_signature;
}

void verify_or_throw(const popy::sidecar::Record& r,
                     std::string_view popy_filename, std::string_view key) {
  switch (verify(r, popy_filename, key)) {
    case VerifyResult::ok:
      return;
    case VerifyResult::missing_signature:
      throw std::runtime_error(
          "popy: sidecar for '" + std::string(popy_filename) +
          "' is unsigned (created before signing was enabled); run "
          "`popy resign` once to adopt it");
    case VerifyResult::unknown_alg:
      throw std::runtime_error(
          "popy: sidecar for '" + std::string(popy_filename) +
          "' uses an unknown signature algorithm; refusing");
    case VerifyResult::bad_signature:
      throw std::runtime_error(
          "popy: sidecar signature verification FAILED for '" +
          std::string(popy_filename) +
          "' — the sidecar was modified outside popy (possible tampering); "
          "refusing");
  }
  throw std::runtime_error("popy: unreachable verify state");
}

}  // namespace popy::attest
