// `popy fetch` — Mode A primary path.
//
// libcurl streaming GET that writes directly to <stage>/<uuid>/<name>_popy
// with O_CREAT|O_EXCL|O_NOFOLLOW. The original-extension filename never
// exists. SHA-256 + 16-byte magic-byte probe in the same write loop.
//
// Mirrors src/offscreen/offscreen.ts:81-143 of the browser extension as
// closely as a libcurl write callback allows.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "core/sidecar.h"

namespace popy::net {

struct FetchOptions {
  std::string url;
  std::filesystem::path stage_dir;
  std::optional<std::string> out_name;       // overrides server-suggested name
  std::optional<std::string> expected_mime;  // verifies magic bytes if set
  std::int64_t max_bytes = 5LL * 1024 * 1024 * 1024;
  bool verbose = false;
};

struct FetchResult {
  popy::sidecar::Record record;
  std::filesystem::path popy_path;     // absolute on-disk path of <name>_popy
  std::filesystem::path sidecar_path;  // absolute on-disk path of meta.json
};

// Performs the fetch. Throws std::runtime_error on any failure (HTTP non-2xx,
// magic mismatch, oversize, network error). On error the in-progress
// _popy file and sidecar are unlinked.
FetchResult fetch(const FetchOptions&);

}  // namespace popy::net
