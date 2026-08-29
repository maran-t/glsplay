#include "config.h"

#include <windows.h>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>

#include "util/log.h"

namespace glsplay {
namespace {

std::optional<std::string> GetEnv(const char* name) {
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
  std::string result(value);
  free(value);
  if (result.empty()) return std::nullopt;
  return result;
}

std::optional<int> ParseInt(std::string_view text) {
  int value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  return value;
}

std::string Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

// Loads KEY=VALUE lines into the process environment without overwriting
// anything already set, so a real environment variable always beats the file.
void LoadDotEnv(const std::string& path) {
  std::ifstream file(path);
  if (!file) return;

  std::string line;
  int loaded = 0;
  while (std::getline(file, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    const auto equals = trimmed.find('=');
    if (equals == std::string::npos) continue;

    const std::string key = Trim(trimmed.substr(0, equals));
    std::string value = Trim(trimmed.substr(equals + 1));
    // Strip one layer of surrounding quotes, which .env files commonly carry.
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    if (key.empty() || GetEnv(key.c_str()).has_value()) continue;
    _putenv_s(key.c_str(), value.c_str());
    ++loaded;
  }
  if (loaded > 0) LOG_INFO << "loaded " << loaded << " values from " << path;
}

void PrintUsage() {
  std::cout << R"(glsplay-host - cloud gaming host daemon

Usage: glsplay-host [options]

Connection
  --signaling-url URL     WebSocket broker (default ws://localhost:8080)
  --room ID               Room to join (default poc)
  --secret SECRET         Shared room secret (required)

Video
  --width N               Capture width (default 1920)
  --height N              Capture height (default 1080)
  --fps N                 Target frame rate (default 60)
  --bitrate-kbps N        Start CBR bitrate, kbps (default 25000; GCC drives it live up to 50000)
  --adapter N             DXGI adapter index (default: first NVIDIA adapter)
  --output N              DXGI output index (default 0)

Audio
  --no-audio              Disable WASAPI loopback capture

Input
  --no-gamepad            Disable the ViGEm virtual gamepad
  --no-mouse              Disable mouse injection
  --no-keyboard           Disable keyboard injection

Network
  --min-port N            First UDP port for media (default 50000)
  --max-port N            Last UDP port for media (default 50100)
  --turn-url URL          Optional TURN server
  --turn-user USER
  --turn-pass PASS

Display
  --no-herd-windows       Don't pull windows opened on another monitor onto
                          the captured display

Diagnostics
  --test-pattern          Stream a generated pattern if DXGI capture fails
  --log-level LEVEL       debug | info | warn | error (default info)
  -h, --help              Show this message

Environment variables (also read from .env in the working directory):
  GLSPLAY_SIGNALING_URL, GLSPLAY_ROOM_ID, GLSPLAY_ROOM_SECRET, GLSPLAY_LOG_LEVEL
)";
}

}  // namespace

bool HostConfig::Parse(int argc, char** argv) {
  LoadDotEnv(".env");
  LoadDotEnv("../../.env");

  if (auto v = GetEnv("GLSPLAY_SIGNALING_URL")) signaling.url = *v;
  if (auto v = GetEnv("GLSPLAY_ROOM_ID")) signaling.room_id = *v;
  if (auto v = GetEnv("GLSPLAY_ROOM_SECRET")) signaling.secret = *v;
  if (auto v = GetEnv("GLSPLAY_LOG_LEVEL")) log_level = *v;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    auto next = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        std::cerr << "error: " << flag << " requires a value\n";
        return std::nullopt;
      }
      const std::string_view value = argv[i + 1];
      // An empty shell variable expands to nothing, so the next flag lands
      // here instead. Swallowing it silently produces a baffling error about
      // whatever followed, rather than naming the flag that is actually empty.
      if (value.rfind("--", 0) == 0) {
        std::cerr << "error: " << flag << " requires a value, but got " << value
                  << "\n       (an unset shell variable expands to nothing - "
                     "check your $secret is set)\n";
        return std::nullopt;
      }
      ++i;
      return std::string(value);
    };

    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return false;
    } else if (arg == "--signaling-url") {
      auto v = next("--signaling-url"); if (!v) return false; signaling.url = *v;
    } else if (arg == "--room") {
      auto v = next("--room"); if (!v) return false; signaling.room_id = *v;
    } else if (arg == "--secret") {
      auto v = next("--secret"); if (!v) return false; signaling.secret = *v;
    } else if (arg == "--width") {
      auto v = next("--width"); if (!v) return false;
      if (auto n = ParseInt(*v)) video.width = *n; else return false;
    } else if (arg == "--height") {
      auto v = next("--height"); if (!v) return false;
      if (auto n = ParseInt(*v)) video.height = *n; else return false;
    } else if (arg == "--fps") {
      auto v = next("--fps"); if (!v) return false;
      if (auto n = ParseInt(*v)) video.fps = *n; else return false;
    } else if (arg == "--bitrate-kbps") {
      auto v = next("--bitrate-kbps"); if (!v) return false;
      if (auto n = ParseInt(*v)) video.bitrate_kbps = *n; else return false;
    } else if (arg == "--adapter") {
      auto v = next("--adapter"); if (!v) return false;
      if (auto n = ParseInt(*v)) video.adapter_index = *n; else return false;
    } else if (arg == "--output") {
      auto v = next("--output"); if (!v) return false;
      if (auto n = ParseInt(*v)) video.output_index = *n; else return false;
    } else if (arg == "--min-port") {
      auto v = next("--min-port"); if (!v) return false;
      if (auto n = ParseInt(*v)) min_port = static_cast<uint16_t>(*n); else return false;
    } else if (arg == "--max-port") {
      auto v = next("--max-port"); if (!v) return false;
      if (auto n = ParseInt(*v)) max_port = static_cast<uint16_t>(*n); else return false;
    } else if (arg == "--turn-url") {
      auto v = next("--turn-url"); if (!v) return false; turn_url = *v;
    } else if (arg == "--turn-user") {
      auto v = next("--turn-user"); if (!v) return false; turn_username = *v;
    } else if (arg == "--turn-pass") {
      auto v = next("--turn-pass"); if (!v) return false; turn_credential = *v;
    } else if (arg == "--log-level") {
      auto v = next("--log-level"); if (!v) return false; log_level = *v;
    } else if (arg == "--no-audio") {
      audio.enabled = false;
    } else if (arg == "--no-gamepad") {
      input.enable_gamepad = false;
    } else if (arg == "--no-mouse") {
      input.enable_mouse = false;
    } else if (arg == "--no-keyboard") {
      input.enable_keyboard = false;
    } else if (arg == "--test-pattern") {
      allow_test_pattern = true;
    } else if (arg == "--no-herd-windows") {
      herd_windows = false;
    } else {
      std::cerr << "error: unknown option " << arg << "\n\n";
      PrintUsage();
      return false;
    }
  }

  if (log_level == "debug") SetLogLevel(LogLevel::kDebug);
  else if (log_level == "warn") SetLogLevel(LogLevel::kWarn);
  else if (log_level == "error") SetLogLevel(LogLevel::kError);
  else SetLogLevel(LogLevel::kInfo);

  return true;
}

bool HostConfig::Validate() const {
  bool ok = true;

  if (signaling.secret.empty()) {
    LOG_ERROR << "no room secret. Pass --secret or set GLSPLAY_ROOM_SECRET.";
    ok = false;
  } else if (signaling.secret.rfind("change-me", 0) == 0) {
    LOG_ERROR << "room secret is still the placeholder from .env.example.";
    ok = false;
  }

  if (video.width <= 0 || video.height <= 0) {
    LOG_ERROR << "invalid capture size " << video.width << 'x' << video.height;
    ok = false;
  }
  // NVENC encodes in 16x16 macroblocks. A non-multiple-of-2 dimension is
  // rejected outright by the encoder, with an error that does not say so.
  if (video.width % 2 != 0 || video.height % 2 != 0) {
    LOG_ERROR << "capture dimensions must be even; got "
              << video.width << 'x' << video.height;
    ok = false;
  }

  if (video.fps <= 0 || video.fps > 240) {
    LOG_ERROR << "invalid fps " << video.fps;
    ok = false;
  }

  if (video.bitrate_kbps <= 0) {
    LOG_ERROR << "invalid bitrate " << video.bitrate_kbps;
    ok = false;
  }

  if (min_port >= max_port) {
    LOG_ERROR << "min-port must be below max-port (" << min_port << ".." << max_port << ')';
    ok = false;
  }
  // One port per ICE component, and libwebrtc allocates several during
  // gathering. A range this small will intermittently fail to connect.
  if (max_port - min_port < 10) {
    LOG_WARN << "port range " << min_port << ".." << max_port
             << " is very narrow; ICE gathering may fail under BUNDLE";
  }

  if (!turn_url.empty() && (turn_username.empty() || turn_credential.empty())) {
    LOG_ERROR << "TURN url given without credentials";
    ok = false;
  }

  return ok;
}

void HostConfig::LogSummary() const {
  LOG_INFO << "glsplay-host " << GLSPLAY_VERSION;
  LOG_INFO << "  signaling : " << signaling.url << " room=" << signaling.room_id;
  LOG_INFO << "  video     : " << video.width << 'x' << video.height << '@' << video.fps
           << "  " << video.bitrate_kbps << " kbps CBR";
  LOG_INFO << "  audio     : "
           << (audio.enabled ? std::to_string(audio.sample_rate) + "Hz stereo Opus" : "disabled");
  LOG_INFO << "  input     : mouse=" << (input.enable_mouse ? "on" : "off")
           << " keyboard=" << (input.enable_keyboard ? "on" : "off")
           << " gamepad=" << (input.enable_gamepad ? "on" : "off");
  LOG_INFO << "  media UDP : " << min_port << '-' << max_port;
}

}  // namespace glsplay
