#include "naming.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <string_view>

namespace popy::naming {

namespace {

// Same character class as ILLEGAL_RE in src/lib/opfs/fs.ts:13.
// Literal: [<>:"/\\|?*\x00-\x1f]
bool is_illegal(unsigned char c) {
  return c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
         c == '/' || c == '\\' || c == '|' || c == '?' || c == '*';
}

// Matches /^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)/i — same as the TS.
bool is_windows_reserved(std::string_view s) {
  static const std::regex re(
      R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$))",
      std::regex::icase | std::regex::optimize);
  return std::regex_search(s.begin(), s.end(), re);
}

}  // namespace

std::string sanitize(std::string_view raw) {
  // Strip path components — TS does .split(/[\\/]/).pop(). Take everything
  // after the last separator.
  std::string base;
  if (auto p = raw.find_last_of("\\/"); p != std::string_view::npos) {
    base.assign(raw.substr(p + 1));
  } else {
    base.assign(raw);
  }

  // Replace illegal chars with '_'; collapse runs of whitespace into single
  // spaces; trim leading/trailing space. The TS does .replace(/\s+/g, " ").trim()
  std::string out;
  out.reserve(base.size());
  bool prev_space = false;
  for (char ch : base) {
    auto c = static_cast<unsigned char>(ch);
    if (is_illegal(c)) {
      out.push_back('_');
      prev_space = false;
    } else if (std::isspace(c)) {
      if (!prev_space) out.push_back(' ');
      prev_space = true;
    } else {
      out.push_back(static_cast<char>(c));
      prev_space = false;
    }
  }
  // Trim leading and trailing whitespace.
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front())))
    out.erase(out.begin());
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
    out.pop_back();

  if (is_windows_reserved(out)) out.insert(out.begin(), '_');

  // Cap at 230 chars — leaves room for "_popy" plus the per-file UUID dir.
  if (out.size() > 230) out.resize(230);

  if (out.empty()) out = "download";
  return out;
}

std::string popy_name(std::string_view original) {
  std::string s = sanitize(original);
  s.append(kSuffix);
  return s;
}

std::string strip_suffix(std::string_view popy_name) {
  if (popy_name.size() >= kSuffix.size() &&
      popy_name.substr(popy_name.size() - kSuffix.size()) == kSuffix) {
    return std::string(popy_name.substr(0, popy_name.size() - kSuffix.size()));
  }
  return std::string(popy_name);
}

// --- glob_match -------------------------------------------------------------
// '*' matches zero or more chars within a path segment (no '/').
// '?' matches one char (not '/').
// '**/' prefix interpreted as "any depth"; remainder matched against basename.
// This is intentionally smaller than fnmatch — we own the patterns from
// config.toml, so we don't need character classes or escapes.

namespace {

bool match_segment(std::string_view pat, std::string_view text) {
  // Classic recursive glob. Bounded by len(pat)*len(text); patterns are short.
  // '*' / '?' never match '/' — keeps semantics predictable when patterns
  // have multiple path segments (e.g. "**/popy-released/**").
  size_t pi = 0, ti = 0;
  size_t star_pi = std::string_view::npos, star_ti = 0;
  while (ti < text.size()) {
    if (pi < pat.size() && pat[pi] == '*') {
      star_pi = pi++;
      star_ti = ti;
    } else if (pi < pat.size() && pat[pi] == '?' && text[ti] != '/') {
      ++pi;
      ++ti;
    } else if (pi < pat.size() && pat[pi] == text[ti]) {
      // Literal match — including '/', so multi-segment patterns work.
      ++pi;
      ++ti;
    } else if (star_pi != std::string_view::npos && text[ti] != '/') {
      // Extend a previous '*' by one more char of text, but never cross '/'.
      pi = star_pi + 1;
      ti = ++star_ti;
    } else {
      return false;
    }
  }
  while (pi < pat.size() && pat[pi] == '*') ++pi;
  return pi == pat.size();
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) {
  // The two recursive forms in our exclude defaults are "**/X" and
  // "**/X/**". We strip the leading "**/" and trailing "/**" once each,
  // then match the remaining "core" pattern against either the whole text
  // (if no leading **) or any subpath beginning after a '/' (with **).

  bool leading_dstar  = pattern.size() >= 3 && pattern.substr(0, 3) == "**/";
  if (leading_dstar) pattern.remove_prefix(3);

  bool trailing_dstar = pattern.size() >= 3 &&
                        pattern.substr(pattern.size() - 3) == "/**";
  if (trailing_dstar) pattern.remove_suffix(3);

  // try_at(t): does the (possibly-prefix) pattern match `t`?
  auto try_at = [&](std::string_view t) -> bool {
    if (trailing_dstar) {
      // Match core against t[0..i] where i is end-of-string or a '/'.
      for (size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == '/') {
          if (match_segment(pattern, t.substr(0, i))) return true;
        }
      }
      return false;
    }
    return match_segment(pattern, t);
  };

  if (try_at(text)) return true;
  if (leading_dstar) {
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '/' && try_at(text.substr(i + 1))) return true;
    }
  }
  return false;
}

}  // namespace popy::naming
