// Tiny line-based logger. Writes to stderr by default; popyd switches it to
// the daemon log file at startup.
#pragma once
#include <string>
#include <string_view>

namespace popy::log {

enum class Level { Debug, Info, Warn, Error };

// Open a log file (appended to, line-buffered). Existing fd is closed.
// Pass empty path to revert to stderr.
void open_file(const std::string& path);

void set_level(Level);

void write(Level, std::string_view msg);

inline void info (std::string_view m) { write(Level::Info,  m); }
inline void warn (std::string_view m) { write(Level::Warn,  m); }
inline void error(std::string_view m) { write(Level::Error, m); }
inline void debug(std::string_view m) { write(Level::Debug, m); }

}  // namespace popy::log
