// Sidecar attestation: HMAC-SHA256 over a canonical record payload.
//
// The payload is length-prefixed text, NOT the sidecar JSON — nlohmann's key
// ordering/escaping is not a stable byte-level contract. The `name` field is
// the actual on-disk `_popy` basename derived from the sidecar's own path
// (never the attacker-editable diskPath JSON field), which pins a signature
// to its file and kills sidecar-transplant attacks.
#pragma once
#include <string>
#include <string_view>

#include "core/sidecar.h"

namespace popy::attest {

inline constexpr const char* kSigAlg = "hmac-sha256-v1";

enum class VerifyResult {
  ok,
  missing_signature,  // pre-upgrade sidecar; `popy resign` adopts it
  unknown_alg,        // sigAlg we don't implement
  bad_signature,      // tamper evidence — never overridable
};

// The exact byte string that gets MACed. `popy_filename` is the on-disk
// basename of the quarantined file (e.g. "report.pdf_popy.1").
std::string canonical_payload(const popy::sidecar::Record& r,
                              std::string_view popy_filename);

// Compute and store sig/sigAlg on `r`. Call again after every mutation
// (e.g. the status change at release).
void sign(popy::sidecar::Record& r, std::string_view popy_filename,
          std::string_view key);

VerifyResult verify(const popy::sidecar::Record& r,
                    std::string_view popy_filename, std::string_view key);

// Throws std::runtime_error with an actionable message on anything but ok.
void verify_or_throw(const popy::sidecar::Record& r,
                     std::string_view popy_filename, std::string_view key);

}  // namespace popy::attest
