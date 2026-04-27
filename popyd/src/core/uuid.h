// UUID v4 generated from /dev/urandom (or arc4random on macOS).
//
// We deliberately do not depend on libuuid — it's not in the macOS base toolchain
// and the algorithm is small.
#pragma once
#include <string>

namespace popy::uuid {

// Returns a 36-char canonical lowercase UUID v4 (8-4-4-4-12 hex).
// Throws std::runtime_error on entropy source failure.
std::string v4();

// True iff `s` is a valid 36-char canonical UUID v4 (or a strict
// case-insensitive prefix of one, used by `popy <id>` resolution).
bool is_valid_or_prefix(const std::string& s);

}  // namespace popy::uuid
