// glsplay host daemon entry point.
//
// Startup order matters and is not arbitrary:
//   1. Config, so failures are reported with the right log level.
//   2. Capture, because it owns the D3D11 device everything else binds to.
//   3. NVENC probe, so an encoder problem is reported before the network.
//   4. PeerConnectionFactory, which needs the device from step 2.
//   5. Signaling, last - nothing should connect until the pipeline is ready.

#include <windows.h>
// timeBeginPeriod/timeEndPeriod - excluded by WIN32_LEAN_AND_MEAN.
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <thread>

#include "api/make_ref_counted.h"

#include "capture/desktop_capture_source.h"
#include "config.h"
#include "encode/nvenc_session.h"
#include "input/gamepad_sink.h"
#include "input/input_dispatcher.h"
#include "input/win32_input_injector.h"
#include "net/host_stats_reporter.h"
#include "net/peer_session.h"
#include "net/signaling_client.h"
#include "util/log.h"

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI ConsoleHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT) {
    LOG_INFO << "shutdown requested";
    g_running.store(false);
    return TRUE;
  }
  return FALSE;
}

// Desktop Duplication captures whichever session owns the display. In an RDP
// session that is the RDP virtual display, which has no NVENC - so this check
// turns the single most common misconfiguration into a clear message instead
// of a black stream.
void WarnIfNotConsoleSession() {
  DWORD session_id = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) return;

  const DWORD console_session = WTSGetActiveConsoleSessionId();
  if (console_session == 0xFFFFFFFF) return;

  if (session_id != console_session) {
    LOG_WARN << "";
    LOG_WARN << "  Running in session " << session_id << ", but the console session is "
             << console_session << '.';
    LOG_WARN << "  Desktop Duplication captures the console session, and NVENC is not";
    LOG_WARN << "  available in an RDP session. Expect a black or failing stream.";
    LOG_WARN << "  Fix: disconnect RDP, then run  tscon " << session_id << " /dest:console";
    LOG_WARN << "";
  } else {
    LOG_INFO << "running in the console session (" << session_id << ')';
  }
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
  // Windows defaults timers to ~15ms granularity, which is a quarter of a
  // 60fps frame. Every sleep in the capture loop depends on this.
  timeBeginPeriod(1);

  glsplay::HostConfig config;
  if (!config.Parse(argc, argv)) {
    timeEndPeriod(1);
    return 1;
  }
  if (!config.Validate()) {
    timeEndPeriod(1);
    return 1;
  }
  config.LogSummary();

  WarnIfNotConsoleSession();

  // --- capture --------------------------------------------------------------
  auto capture = webrtc::make_ref_counted<glsplay::DesktopCaptureSource>();
  if (!capture->Start(config.video.adapter_index, config.video.output_index,
                      config.video.fps)) {
    LOG_ERROR << "capture could not start - see the messages above";
    timeEndPeriod(1);
    return 2;
  }

  // The negotiated size comes from the display, not the config. A mismatch
  // usually means the IDD mode list is missing the requested resolution.
  if (capture->width() != config.video.width || capture->height() != config.video.height) {
    LOG_WARN << "display is " << capture->width() << 'x' << capture->height()
             << " but config asked for " << config.video.width << 'x'
             << config.video.height << " - encoding at the display size";
  }

  // --- encoder probe --------------------------------------------------------
  if (!glsplay::NvencSession::IsAvailable()) {
    LOG_ERROR << "NVENC unavailable - refusing to start";
    LOG_ERROR << "A software fallback would produce numbers that mean nothing "
                 "for this POC, so this is fatal by design.";
    capture->Stop();
    timeEndPeriod(1);
    return 3;
  }

  // --- input ----------------------------------------------------------------
  auto injector = std::make_unique<glsplay::Win32InputInjector>(
      config.input.enable_mouse, config.input.enable_keyboard);
  auto gamepad = glsplay::CreateGamepadSink(config.input.enable_gamepad);
  LOG_INFO << "gamepad: " << gamepad->description();

  auto input = std::make_shared<glsplay::InputDispatcher>(std::move(injector),
                                                          std::move(gamepad));

  // --- signaling and peer ---------------------------------------------------
  glsplay::SignalingClient signaling;

  auto session = std::make_unique<glsplay::PeerSession>(
      config, capture, input,
      [&signaling](const std::string& json) { signaling.Send(json); });

  if (!session->Initialise()) {
    LOG_ERROR << "PeerConnectionFactory initialisation failed";
    capture->Stop();
    timeEndPeriod(1);
    return 4;
  }

  signaling.SetMessageHandler([&session](const std::string& json) {
    session->HandleSignalingMessage(json);
  });
  signaling.SetStateHandler([](glsplay::SignalingState state, const std::string& detail) {
    switch (state) {
      case glsplay::SignalingState::kRegistered:
        LOG_INFO << "signaling registered";
        break;
      case glsplay::SignalingState::kError:
        LOG_ERROR << "signaling error: " << detail;
        break;
      case glsplay::SignalingState::kClosed:
        LOG_WARN << "signaling closed" << (detail.empty() ? "" : ": " + detail);
        break;
      default:
        break;
    }
  });

  glsplay::SignalingClient::Config signaling_config;
  signaling_config.url = config.signaling.url;
  signaling_config.room_id = config.signaling.room_id;
  signaling_config.secret = config.signaling.secret;

  if (!signaling.Start(signaling_config)) {
    LOG_ERROR << "signaling client could not start";
    capture->Stop();
    timeEndPeriod(1);
    return 5;
  }

  // --- telemetry ------------------------------------------------------------
  glsplay::HostStatsReporter reporter(capture.get(), input.get(), session.get());
  reporter.Start();

  LOG_INFO << "";
  LOG_INFO << "glsplay-host running. Waiting for a browser to join room '"
           << config.signaling.room_id << "'.";
  LOG_INFO << "Press Ctrl+C to stop.";
  LOG_INFO << "";

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  LOG_INFO << "shutting down";
  reporter.Stop();
  signaling.Stop();
  session->ClosePeer();
  session.reset();
  input->ReleaseAll();
  capture->Stop();

  timeEndPeriod(1);
  LOG_INFO << "stopped";
  return 0;
}
