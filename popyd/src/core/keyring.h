// The sidecar-signing key.
//
// A 32-byte random key, auto-created on first use at paths::key_file()
// (mode 0600, O_EXCL). Same-user trust model: the key proves a sidecar was
// written by this user's popy tooling and has not been altered by content
// that came in through a download. It does NOT defend against the user or
// root acting deliberately — see THREAT_MODEL.md.
#pragma once
#include <filesystem>
#include <string>

namespace popy::keyring {

inline constexpr std::size_t kKeyBytes = 32;

// Load the key at `path`, creating it on first use (0600, O_EXCL, fsync;
// a concurrent-create EEXIST race falls back to load). Loading refuses keys
// that are not regular files, not owned by the current euid, group/world-
// accessible, or not exactly 32 bytes — fail closed on anything odd.
// Throws std::runtime_error on failure.
std::string load_or_create(const std::filesystem::path& path);

}  // namespace popy::keyring
