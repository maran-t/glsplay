// WebSocket client for the signaling broker.
//
// Built on the Windows WinHTTP WebSocket API rather than a third-party
// WebSocket library. WinHTTP has shipped native WebSocket support since
// Windows 8, so this adds no dependency at all - consistent with the rest of
// the host, where every non-media concern is handled by the Windows SDK.
//
// Runs one background thread that owns the connection and pumps receives.
// Callbacks fire on that thread, so handlers must not block.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace glsplay {

enum class SignalingState { kIdle, kConnecting, kRegistered, kClosed, kError };

class SignalingClient {
 public:
  struct Config {
    std::string url;  // ws://host:port or wss://host:port
    std::string room_id;
    std::string secret;
    std::string agent = "glsplay-host";
  };

  using MessageHandler = std::function<void(const std::string& json)>;
  using StateHandler = std::function<void(SignalingState, const std::string& detail)>;

  SignalingClient();
  ~SignalingClient();

  SignalingClient(const SignalingClient&) = delete;
  SignalingClient& operator=(const SignalingClient&) = delete;

  void SetMessageHandler(MessageHandler handler);
  void SetStateHandler(StateHandler handler);

  // Connects and registers as role=host. Reconnects with backoff until Stop().
  bool Start(const Config& config);
  void Stop();

  // Sends a raw JSON string. False if the socket is not currently open.
  bool Send(const std::string& json);

  SignalingState state() const { return state_.load(); }

 private:
  struct Impl;

  void Run();
  bool ConnectOnce();
  void Close();
  void SetState(SignalingState state, const std::string& detail = {});

  Config config_;
  std::unique_ptr<Impl> impl_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<SignalingState> state_{SignalingState::kIdle};

  // Guards the WinHTTP handles, which the sender thread touches while the
  // receive loop is blocked in WinHttpWebSocketReceive.
  std::mutex send_mutex_;

  MessageHandler on_message_;
  StateHandler on_state_;
};

}  // namespace glsplay
