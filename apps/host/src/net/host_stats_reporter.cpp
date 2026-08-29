#include "net/host_stats_reporter.h"

#include <windows.h>

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

// NVML, loaded from the driver's nvml.dll at runtime - no SDK header or import
// library needed (the CUDA toolkit is not on the VM). Only the five entry
// points and two structs we use are declared here.
class NvmlProbe {
  using Device = void*;
  struct Util {
    unsigned int gpu;
    unsigned int memory;
  };
  using InitFn = int(__cdecl*)();
  using ShutdownFn = int(__cdecl*)();
  using HandleFn = int(__cdecl*)(unsigned int, Device*);
  using UtilFn = int(__cdecl*)(Device, Util*);
  using EncUtilFn = int(__cdecl*)(Device, unsigned int*, unsigned int*);

 public:
  NvmlProbe() {
    dll_ = LoadLibraryW(L"nvml.dll");
    if (!dll_) return;
    auto init = reinterpret_cast<InitFn>(GetProcAddress(dll_, "nvmlInit_v2"));
    shutdown_ = reinterpret_cast<ShutdownFn>(GetProcAddress(dll_, "nvmlShutdown"));
    auto handle = reinterpret_cast<HandleFn>(
        GetProcAddress(dll_, "nvmlDeviceGetHandleByIndex_v2"));
    util_ = reinterpret_cast<UtilFn>(
        GetProcAddress(dll_, "nvmlDeviceGetUtilizationRates"));
    enc_util_ = reinterpret_cast<EncUtilFn>(
        GetProcAddress(dll_, "nvmlDeviceGetEncoderUtilization"));
    if (!init || !handle || !util_) return;
    if (init() != 0 || handle(0, &device_) != 0) return;
    ready_ = true;
    LOG_INFO << "NVML: GPU / encoder utilisation telemetry enabled";
  }

  ~NvmlProbe() {
    if (ready_ && shutdown_) shutdown_();
    if (dll_) FreeLibrary(dll_);
  }

  void Sample(double* gpu_percent, double* encoder_percent) {
    if (!ready_) return;
    Util u{};
    if (util_(device_, &u) == 0) *gpu_percent = u.gpu;
    unsigned int enc = 0, sampling_us = 0;
    if (enc_util_ && enc_util_(device_, &enc, &sampling_us) == 0) {
      *encoder_percent = enc;
    }
  }

 private:
  HMODULE dll_ = nullptr;
  ShutdownFn shutdown_ = nullptr;
  UtilFn util_ = nullptr;
  EncUtilFn enc_util_ = nullptr;
  Device device_ = nullptr;
  bool ready_ = false;
};

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
  NvmlProbe nvml;

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
    nvml.Sample(&gpu_percent, &encoder_percent);

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
