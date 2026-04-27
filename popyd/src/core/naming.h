// Filename suffix and sanitization.
//
// Mirrors src/lib/opfs/fs.ts:13-25 of the browser extension byte-for-byte so
// a sidecar produced here, ingested into the extension's IndexedDB, and
// released back to disk produces the same name we would have produced.
#pragma once

#include <string>
#include <string_view>

namespace popy::naming {

inline constexpr std::string_view kSuffix = "_popy";

// Replace illegal chars, collapse whitespace, cap to 230 chars, refuse empty.
// Windows-reserved names get a leading underscore. Same algorithm as the TS.
std::string sanitize(std::string_view raw);

// "report.pdf" -> "report.pdf_popy"
std::string popy_name(std::string_view original);

// Reverse of popy_name(): if `s` ends in "_popy", drop it; otherwise return
// as-is. Used by `popy release`.
std::string strip_suffix(std::string_view popy_name);

// Simple O(n) glob matcher: '*' matches any run within a path segment, '?'
// matches one char, '/' is literal. We avoid wildcards spanning '/' to keep
// the matcher predictable and to honour user-intent on patterns like "*_popy"
// (which must NOT match "subdir/foo_popy" — the user wrote one segment).
//
// Special-case "**/x" — interpreted as "match x in any subdirectory".
bool glob_match(std::string_view pattern, std::string_view text);

}  // namespace popy::naming
