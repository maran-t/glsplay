// Minimal logging. Deliberately not libwebrtc's RTC_LOG: the host must be able
// to report a failure before the libwebrtc factory exists, and during the
// early DXGI/NVENC probing that is exactly when most failures happen.

#pragma once

#include <string>
#include <string_view>

namespace glsplay {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError };

void SetLogLevel(LogLevel level);
void LogWrite(LogLevel level, std::string_view message);

// Formats a Windows HRESULT into "0x80070005 (Access is denied)". Every DXGI
// and NVENC failure path uses this - a bare hex code sends you to a search
// engine, and the system message usually names the actual problem.
std::string HrToString(long hr);

// GetLastError() rendered the same way.
std::string LastErrorToString();

}  // namespace glsplay

// Stream-style macros so call sites read naturally and argument formatting is
// skipped entirely when the level is filtered out.
#define GLSPLAY_LOG_AT(level)                                       \
  for (bool once = true; once; once = false)                        \
    ::glsplay::detail::LogStream(level).stream()

#define LOG_DEBUG GLSPLAY_LOG_AT(::glsplay::LogLevel::kDebug)
#define LOG_INFO  GLSPLAY_LOG_AT(::glsplay::LogLevel::kInfo)
#define LOG_WARN  GLSPLAY_LOG_AT(::glsplay::LogLevel::kWarn)
#define LOG_ERROR GLSPLAY_LOG_AT(::glsplay::LogLevel::kError)

#include <sstream>

namespace glsplay::detail {

class LogStream {
 public:
  explicit LogStream(LogLevel level) : level_(level) {}
  ~LogStream() { LogWrite(level_, buffer_.str()); }

  LogStream(const LogStream&) = delete;
  LogStream& operator=(const LogStream&) = delete;

  std::ostringstream& stream() { return buffer_; }

 private:
  LogLevel level_;
  std::ostringstream buffer_;
};

}  // namespace glsplay::detail
