#pragma once
// Host stub: the device's Logging.h writes through logPrintf(); here every line is captured so a
// test can assert on the state machine's own account of what it did.
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

inline std::vector<std::string> hostLogLines;
inline bool hostLogEcho = false;

inline void hostLog(const char* level, const char* origin, const char* format, ...) {
  char buffer[768];
  std::va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  const std::string line = std::string(level) + " " + origin + " " + buffer;
  hostLogLines.push_back(line);
  if (hostLogEcho) std::printf("%s\n", line.c_str());
}

#define LOG_ERR(origin, ...) hostLog("ERR", origin, __VA_ARGS__)
#define LOG_INF(origin, ...) hostLog("INF", origin, __VA_ARGS__)
#define LOG_DBG(origin, ...) hostLog("DBG", origin, __VA_ARGS__)
