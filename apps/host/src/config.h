// Host configuration, resolved from a .env file, environment variables and
// command-line flags in that order of increasing precedence.

#pragma once

#include <cstdint>
#include <string>

namespace glsplay {

struct VideoConfig {
  // PRD section 4.1. Must match a mode the IDD virtual display actually
  // publishes, or DXGI will report a different resolution than requested.
  int width = 1920;
  int height = 1080;
  int fps = 60;

  // Start value only - Google Congestion Control revises this within a second
  // of the first RTCP report and then drives it continuously (SetRates), never
  // exceeding the link estimate or max_bitrate_kbps. A higher start just means
  // the first ~1s is less soft; the ceiling is what lets a good wired link
  // actually reach a crisp 1080p60.
  int bitrate_kbps = 25000;
  int max_bitrate_kbps = 50000;

  // Zero B-frames is not tunable. PRD section 4.2 eliminates them because
  // reordering costs a full 16-33ms of latency, which is most of the budget.
  int gop_length = -1;  // -1 selects infinite GOP with intra-refresh

  // Upper bound written into the playout-delay RTP header extension, which the
  // receiver is obliged to honour (unlike the client-side jitterBufferTarget
  // hint, which Chrome overrode - it was observed holding 100-170ms). The
  // minimum is always 0: render as soon as possible. Leaving a small window
  // rather than pinning 0/0 lets Chrome still absorb genuine network jitter
  // instead of juddering on every late packet.
  //
  // 50ms was the first ceiling tried; Chrome settled at 33ms inside it, so the
  // window itself was the binding constraint rather than the network. 20ms is
  // ~1 frame at 60fps, which is where Moonlight and Parsec effectively sit.
  int playout_delay_max_ms = 20;

  // Which DXGI adapter and output to duplicate. Defaults pick the first
  // NVIDIA adapter and its first attached output.
  int adapter_index = -1;
  int output_index = 0;
};

struct AudioConfig {
  // PRD section 4.3.
  int sample_rate = 48000;
  int channels = 2;
  int bitrate_bps = 128000;
  bool enabled = true;
};

struct SignalingConfig {
  std::string url = "ws://localhost:8080";
  std::string room_id = "poc";
  std::string secret;
};

struct InputConfig {
  bool enable_mouse = true;
  bool enable_keyboard = true;
  bool enable_gamepad = true;
};

struct HostConfig {
  VideoConfig video;
  AudioConfig audio;
  SignalingConfig signaling;
  InputConfig input;

  // ICE. The host has a public IP so its host candidate should win outright;
  // STUN is configured anyway so the browser side can discover its own
  // reflexive address.
  std::string stun_url = "stun:stun.l.google.com:19302";
  std::string turn_url;
  std::string turn_username;
  std::string turn_credential;

  // PRD section 4.5 pins the media port range so a single firewall rule
  // covers it. 0 means let the OS choose, which makes the rule impossible.
  uint16_t min_port = 50000;
  uint16_t max_port = 50100;

  // Falls back to a generated test pattern when DXGI duplication is
  // unavailable. Lets phases 1 and 4 be validated on a machine with no GPU,
  // and makes an RDP-session misconfiguration obvious rather than fatal.
  bool allow_test_pattern = false;

  // Poll for top-level windows that opened on another monitor (the L4 phantom
  // head is the primary on a headless box) and pull them onto the captured
  // display, so launched apps appear on the stream.
  bool herd_windows = true;

  std::string log_level = "info";

  // Parses argv and the environment. Returns false and prints usage on error.
  bool Parse(int argc, char** argv);

  // Validates the resolved values, logging anything that will not work.
  bool Validate() const;

  void LogSummary() const;
};

}  // namespace glsplay
