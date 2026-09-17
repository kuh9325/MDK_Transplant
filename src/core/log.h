// Minimal runtime logging. Deliberately not a framework: three levels,
// stderr output, printf-style formatting.

#ifndef MDK_CORE_LOG_H
#define MDK_CORE_LOG_H

#include <cstdarg>
#include <cstdio>

namespace mdk::log {

enum class Level { info, warn, error };

inline void write(Level level, const char* tag, const char* fmt, ...) {
  const char* name = level == Level::info  ? "info"
                     : level == Level::warn ? "warn"
                                            : "error";
  std::fprintf(stderr, "[%s][%s] ", name, tag);
  std::va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
}

inline void info(const char* tag, const char* fmt, ...) {
  std::va_list args;
  va_start(args, fmt);
  std::fprintf(stderr, "[info][%s] ", tag);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
}

inline void warn(const char* tag, const char* fmt, ...) {
  std::va_list args;
  va_start(args, fmt);
  std::fprintf(stderr, "[warn][%s] ", tag);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
}

inline void error(const char* tag, const char* fmt, ...) {
  std::va_list args;
  va_start(args, fmt);
  std::fprintf(stderr, "[error][%s] ", tag);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
}

} // namespace mdk::log

#endif // MDK_CORE_LOG_H
