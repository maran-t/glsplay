#include "net/host_stats_reporter.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "capture/desktop_capture_source.h"
#include "capture/dxgi_duplicator.h"
#include "encode/nvenc_encoder_factory.h"
#include "encode/nvenc_video_encoder.h"
#include "input/input_dispatcher.h"
#include "net/peer_session.h"
#include "util/log.h"

#ifdef GLSPLAY_HAVE_NVML
#include <nvml.h>
#endif

namespace glsplay {
namespace {

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Formats with a fixed precision so the HUD does not jitter between widths.
std::string Fixed(double value, int digits = 2) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
  return buffer;
}

#ifdef GLSPLAY_HAVE_NVML
class NvmlProbe {
 public:
  NvmlProbe() {
    if (nvmlInit_v2() != NVML_SUCCESS) return;
    if (nvmlDeviceGetHandleByIndex_v2(0, &device_) != NVML_SUCCESS) return;
    ready_ = true;
  }
  ~NvmlProbe() {
    if (ready_) nvmlShutdown();
  }

  void Sample(double* gpu_percent, double* encoder_percent) {
    if (!ready_) return;
    nvmlUtilization_t util{};
    if (nvmlDeviceGetUtilizationRates(device_, &util) == NVML_SUCCESS) {
      *gpu_percent = util.gpu;
    }
    unsigned int encoder_util = 0;
    unsigned int sampling_us = 0;
    if (nvmlDeviceGetEncoderUtilization(device_, &encoder_util, &sampling_us) ==
        NVML_SUCCESS) {
      *encoder_percent = encoder_util;
    }
  }

 private:
  nvmlDevice_t device_{};
  bool ready_ = false;
};
#endif

}  // namespace

HostStatsReporter::HostStatsReporter(DesktopCaptureSource* capture,
                                     InputDispatcher* input,
                                     PeerSession* session,
                                     int interval_ms)
    : capture_(capture), input_(input), session_(session), interval_ms_(interval_ms) {}

HostStatsReporter::~HostStatsReporter() {
  Stop();
}

void HostStatsReporter::Start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread(&HostStatsReporter::Run, this);
}

void HostStatsReporter::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void HostStatsReporter::Run() {
#ifdef GLSPLAY_HAVE_NVML
  NvmlProbe nvml;
#endif

  // Logged locally every 10 seconds too, so a session with no browser attached
  // still leaves evidence of whether capture and encode were healthy.
  int ticks = 0;

  // The cursor is composited into the video by the capture path, so there is
  // nothing to push here - this loop only emits the 1 Hz host-stats message.
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    if (!running_.load()) break;

    const auto capture_stats = capture_->stats();
    const auto input_stats = input_->stats();

    double encode_ms = 0.0;
    double encoded_fps = 0.0;
    double encoder_bitrate_kbps = 0.0;
    if (auto* factory = session_->encoder_factory()) {
      if (auto* encoder = factory->active_encoder()) {
        encode_ms = encoder->mean_encode_ms();
        // Encoded frame count is cumulative; the browser derives its own rate
        // from getStats(), so this is reported as the capture rate less drops.
        encoded_fps = capture_stats.captured_fps;
      }
    }

    double gpu_percent = 0.0;
    double encoder_percent = 0.0;
#ifdef GLSPLAY_HAVE_NVML
    nvml.Sample(&gpu_percent, &encoder_percent);
#endif

    std::string message = "{\"type\":\"host-stats\"";
    message += ",\"t\":" + std::to_string(NowMillis());
    message += ",\"capturedFps\":" + Fixed(capture_stats.captured_fps);
    message += ",\"encodedFps\":" + Fixed(encoded_fps);
    message += ",\"captureMs\":" + Fixed(capture_stats.mean_capture_ms);
    message += ",\"encodeMs\":" + Fixed(encode_ms);
    message += ",\"encoderBitrateKbps\":" + Fixed(encoder_bitrate_kbps, 0);
    message += ",\"gpuUtilPercent\":" + Fixed(gpu_percent, 0);
    message += ",\"encoderUtilPercent\":" + Fixed(encoder_percent, 0);
    message += ",\"droppedFrames\":" + std::to_string(capture_stats.dropped);
    message += ",\"inputQueueMs\":" + Fixed(input_stats.mean_queue_ms);
    message += '}';

    session_->SendControl(message);

    if (++ticks % 10 == 0) {
      LOG_INFO << "capture " << Fixed(capture_stats.captured_fps, 1) << "fps"
               << "  capture " << Fixed(capture_stats.mean_capture_ms) << "ms"
               << "  encode " << Fixed(encode_ms) << "ms"
               << "  dropped " << capture_stats.dropped
               << "  repeats " << capture_stats.repeats
               << "  input " << input_stats.events << " ev";
    }
  }
}

}  // namespace glsplay
