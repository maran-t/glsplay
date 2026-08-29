#include "util/log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace glsplay {
namespace {

std::atomic<LogLevel> g_level{LogLevel::kInfo};
std::mutex g_mutex;

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO ";
    case LogLevel::kWarn:  return "WARN ";
    case LogLevel::kError: return "ERROR";
  }
  return "?????";
}

std::string Timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto secs = std::chrono::floor<std::chrono::seconds>(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();

  const std::time_t t = std::chrono::system_clock::to_time_t(secs);
  std::tm tm{};
  gmtime_s(&tm, &t);

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
  return buf;
}

// Renders a system error code via FormatMessage, trimming the trailing CRLF
// the API insists on appending.
std::string SystemMessage(unsigned long code) {
  LPSTR text = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&text), 0, nullptr);

  std::string result;
  if (length != 0 && text != nullptr) {
    result.assign(text, length);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
      result.pop_back();
    }
  }
  if (text != nullptr) LocalFree(text);
  return result;
}

}  // namespace

void SetLogLevel(LogLevel level) {
  g_level.store(level, std::memory_order_relaxed);
}

void LogWrite(LogLevel level, std::string_view message) {
  if (level < g_level.load(std::memory_order_relaxed)) return;

  std::ostringstream line;
  line << Timestamp() << ' ' << LevelName(level) << ' ' << message << '\n';
  const std::string text = line.str();

  // One lock around both sinks so interleaved threads never split a line.
  std::lock_guard<std::mutex> guard(g_mutex);
  std::fputs(text.c_str(), level >= LogLevel::kWarn ? stderr : stdout);
  std::fflush(level >= LogLevel::kWarn ? stderr : stdout);
  // Also mirror to the debugger, which is how you read output when the host
  // runs as a service with no attached console.
  OutputDebugStringA(text.c_str());
}

std::string HrToString(long hr) {
  std::ostringstream out;
  out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
      << static_cast<unsigned long>(hr);
  const std::string message = SystemMessage(static_cast<unsigned long>(hr));
  if (!message.empty()) out << " (" << message << ')';
  return out.str();
}

std::string LastErrorToString() {
  return HrToString(static_cast<long>(GetLastError()));
}

}  // namespace glsplay
