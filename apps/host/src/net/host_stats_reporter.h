// Periodic host telemetry over the control DataChannel.
//
// The browser cannot see encode time, GPU load, or how long input sat in the
// queue - getStats() only reports what the receiver observes. Sending these
// lets the HUD show a full latency breakdown against the PRD section 5 budget
// instead of a receiver-side half of it.

#pragma once

#include <atomic>
#include <thread>

namespace glsplay {

class DesktopCaptureSource;
class InputDispatcher;
class PeerSession;

class HostStatsReporter {
 public:
  HostStatsReporter(DesktopCaptureSource* capture,
                    InputDispatcher* input,
                    PeerSession* session,
                    int interval_ms = 1000);
  ~HostStatsReporter();

  void Start();
  void Stop();

 private:
  void Run();

  DesktopCaptureSource* capture_;
  InputDispatcher* input_;
  PeerSession* session_;
  const int interval_ms_;

  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace glsplay
