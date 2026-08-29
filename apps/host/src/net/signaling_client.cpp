#include "net/signaling_client.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <random>
#include <vector>

#include "util/log.h"

namespace glsplay {
namespace {

struct ParsedUrl {
  std::wstring host;
  INTERNET_PORT port = 80;
  std::wstring path = L"/";
  bool secure = false;
};

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

std::string Narrow(const wchar_t* text) {
  if (text == nullptr) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) return {};
  std::string out(static_cast<size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
  return out;
}

// Minimal ws:// and wss:// parser. WinHttpCrackUrl does not recognise the ws
// schemes, so the scheme is stripped and the remainder treated as http.
bool ParseUrl(const std::string& url, ParsedUrl* out) {
  std::string rest = url;

  if (rest.rfind("wss://", 0) == 0) {
    out->secure = true;
    out->port = 443;
    rest = rest.substr(6);
  } else if (rest.rfind("ws://", 0) == 0) {
    out->secure = false;
    out->port = 80;
    rest = rest.substr(5);
  } else {
    LOG_ERROR << "signaling url must start with ws:// or wss:// - got " << url;
    return false;
  }

  const auto slash = rest.find('/');
  std::string authority = rest;
  if (slash != std::string::npos) {
    authority = rest.substr(0, slash);
    out->path = Widen(rest.substr(slash));
  }

  const auto colon = authority.rfind(':');
  if (colon != std::string::npos) {
    const std::string port_text = authority.substr(colon + 1);
    authority = authority.substr(0, colon);
    try {
      out->port = static_cast<INTERNET_PORT>(std::stoi(port_text));
    } catch (...) {
      LOG_ERROR << "invalid port in signaling url: " << port_text;
      return false;
    }
  }

  if (authority.empty()) {
    LOG_ERROR << "no host in signaling url: " << url;
    return false;
  }
  out->host = Widen(authority);
  return true;
}

// Escapes the few characters that must not appear raw inside a JSON string.
// The values we send are ids and secrets, so the full escape table is not
// needed - but a secret with a quote in it would otherwise produce invalid
// JSON and a confusing bad-json rejection from the broker.
std::string EscapeJson(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

struct SignalingClient::Impl {
  HINTERNET session = nullptr;
  HINTERNET connection = nullptr;
  HINTERNET request = nullptr;
  HINTERNET websocket = nullptr;

  void Reset() {
    if (websocket) { WinHttpCloseHandle(websocket); websocket = nullptr; }
    if (request) { WinHttpCloseHandle(request); request = nullptr; }
    if (connection) { WinHttpCloseHandle(connection); connection = nullptr; }
    if (session) { WinHttpCloseHandle(session); session = nullptr; }
  }
};

SignalingClient::SignalingClient() : impl_(std::make_unique<Impl>()) {}

SignalingClient::~SignalingClient() {
  Stop();
}

void SignalingClient::SetMessageHandler(MessageHandler handler) {
  on_message_ = std::move(handler);
}

void SignalingClient::SetStateHandler(StateHandler handler) {
  on_state_ = std::move(handler);
}

void SignalingClient::SetState(SignalingState state, const std::string& detail) {
  state_.store(state);
  if (on_state_) on_state_(state, detail);
}

bool SignalingClient::Start(const Config& config) {
  if (running_.load()) return true;
  config_ = config;

  ParsedUrl parsed;
  if (!ParseUrl(config.url, &parsed)) return false;

  running_.store(true);
  thread_ = std::thread(&SignalingClient::Run, this);
  return true;
}

void SignalingClient::Stop() {
  if (!running_.exchange(false)) return;
  // Closing the handles unblocks WinHttpWebSocketReceive in the run loop.
  {
    std::lock_guard<std::mutex> guard(send_mutex_);
    if (impl_->websocket) {
      WinHttpWebSocketClose(impl_->websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                            nullptr, 0);
    }
  }
  if (thread_.joinable()) thread_.join();
  impl_->Reset();
  SetState(SignalingState::kClosed);
}

bool SignalingClient::ConnectOnce() {
  ParsedUrl parsed;
  if (!ParseUrl(config_.url, &parsed)) return false;

  SetState(SignalingState::kConnecting);
  impl_->Reset();

  impl_->session = WinHttpOpen(L"glsplay-host/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (impl_->session == nullptr) {
    SetState(SignalingState::kError, "WinHttpOpen: " + LastErrorToString());
    return false;
  }

  impl_->connection = WinHttpConnect(impl_->session, parsed.host.c_str(), parsed.port, 0);
  if (impl_->connection == nullptr) {
    SetState(SignalingState::kError, "WinHttpConnect: " + LastErrorToString());
    return false;
  }

  impl_->request = WinHttpOpenRequest(
      impl_->connection, L"GET", parsed.path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, parsed.secure ? WINHTTP_FLAG_SECURE : 0);
  if (impl_->request == nullptr) {
    SetState(SignalingState::kError, "WinHttpOpenRequest: " + LastErrorToString());
    return false;
  }

  // Tells WinHTTP this request is a WebSocket upgrade; without it the
  // CompleteUpgrade call below fails with ERROR_INVALID_OPERATION.
  if (!WinHttpSetOption(impl_->request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
    SetState(SignalingState::kError, "upgrade option: " + LastErrorToString());
    return false;
  }

  if (!WinHttpSendRequest(impl_->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    SetState(SignalingState::kError, "WinHttpSendRequest: " + LastErrorToString());
    return false;
  }

  if (!WinHttpReceiveResponse(impl_->request, nullptr)) {
    SetState(SignalingState::kError, "WinHttpReceiveResponse: " + LastErrorToString());
    return false;
  }

  impl_->websocket = WinHttpWebSocketCompleteUpgrade(impl_->request, 0);
  if (impl_->websocket == nullptr) {
    SetState(SignalingState::kError, "WebSocket upgrade failed: " + LastErrorToString());
    return false;
  }

  // The request handle is no longer needed once the socket is up.
  WinHttpCloseHandle(impl_->request);
  impl_->request = nullptr;

  LOG_INFO << "signaling connected to " << config_.url;

  const std::string register_msg =
      std::string("{\"type\":\"register\",\"role\":\"host\",\"roomId\":\"") +
      EscapeJson(config_.room_id) + "\",\"secret\":\"" + EscapeJson(config_.secret) +
      "\",\"agent\":\"" + EscapeJson(config_.agent) + "\"}";
  if (!Send(register_msg)) {
    SetState(SignalingState::kError, "register send failed");
    return false;
  }

  return true;
}

bool SignalingClient::Send(const std::string& json) {
  std::lock_guard<std::mutex> guard(send_mutex_);
  if (impl_->websocket == nullptr) return false;

  const DWORD result = WinHttpWebSocketSend(
      impl_->websocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
      const_cast<char*>(json.data()), static_cast<DWORD>(json.size()));
  if (result != NO_ERROR) {
    LOG_WARN << "WinHttpWebSocketSend failed: " << HrToString(static_cast<long>(result));
    return false;
  }
  return true;
}

void SignalingClient::Run() {
  std::mt19937 rng{std::random_device{}()};
  int attempt = 0;

  // Exponential backoff to 10s with jitter, so a broker restart - or a broker
  // that accepts the upgrade and then drops us - is not met by a tight
  // reconnect loop. Sleeps in slices so Stop() stays responsive.
  const auto backoff_sleep = [&]() {
    const int base_ms = (std::min)(10000, 500 * (1 << (std::min)(attempt, 5)));
    std::uniform_int_distribution<int> jitter(base_ms / 2, base_ms);
    const int delay_ms = jitter(rng);
    LOG_WARN << "signaling reconnect in " << delay_ms << "ms";
    ++attempt;
    for (int slept = 0; slept < delay_ms && running_.load(); slept += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  };

  while (running_.load()) {
    if (!ConnectOnce()) {
      if (!running_.load()) break;
      backoff_sleep();
      continue;
    }

    // Do not clear backoff yet. A socket that upgrades and is then closed
    // straight away (a rejected secret, or a newer connection taking our seat)
    // must keep backing off instead of spinning - so only a session that
    // actually lasted a few seconds counts as healthy.
    const auto session_start = std::chrono::steady_clock::now();
    std::vector<char> buffer(64 * 1024);
    std::string accumulated;

    while (running_.load()) {
      DWORD received = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};

      const DWORD result = WinHttpWebSocketReceive(
          impl_->websocket, buffer.data(), static_cast<DWORD>(buffer.size()),
          &received, &type);
      if (result != NO_ERROR) {
        if (running_.load()) {
          LOG_WARN << "signaling receive failed: " << HrToString(static_cast<long>(result));
        }
        break;
      }

      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        LOG_INFO << "signaling closed by broker";
        break;
      }

      accumulated.append(buffer.data(), received);

      // A large SDP can arrive split across several fragments; only dispatch
      // once the final fragment of the message has landed.
      const bool complete = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
      if (!complete) continue;

      if (state_.load() != SignalingState::kRegistered &&
          accumulated.find("\"registered\"") != std::string::npos) {
        SetState(SignalingState::kRegistered);
      }

      if (on_message_) on_message_(accumulated);
      accumulated.clear();
    }

    {
      std::lock_guard<std::mutex> guard(send_mutex_);
      impl_->Reset();
    }
    if (!running_.load()) break;
    SetState(SignalingState::kClosed, "reconnecting");

    const auto session_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - session_start)
                                .count();
    if (session_ms >= 5000) {
      attempt = 0;  // Healthy session; next drop retries immediately.
    } else {
      backoff_sleep();
    }
  }
}

}  // namespace glsplay
