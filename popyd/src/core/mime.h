// MIME-type helpers.
//
// Two surfaces:
//   - guess_from_extension(): "report.pdf" -> "application/pdf"
//   - sniff_magic_bytes(): the same magic-byte set the browser extension's
//     validator uses (src/lib/validator/), used as a sanity check on the
//     first 16 bytes of a fetched response when --mime is supplied.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace popy::mime {

// "report.pdf" -> "application/pdf". Returns "application/octet-stream" if
// the extension is unknown.
std::string guess_from_extension(std::string_view filename);

// Returns true if `head` (first up-to-16 bytes of a body) is consistent with
// the family implied by `expected_mime`. Empty/unknown expected_mime → true
// (we make no claim).
bool magic_consistent(std::string_view head, std::string_view expected_mime);

// "image/png; charset=binary" -> "image/png". Lowercases.
std::string normalize(std::string_view mime);

}  // namespace popy::mime
