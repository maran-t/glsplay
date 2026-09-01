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
#include <windows.h>

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

std::string Base64(const std::vector<uint8_t>& in) {
  static const char t[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t n = (static_cast<uint32_t>(in[i]) << 16) |
                       (static_cast<uint32_t>(in[i + 1]) << 8) | in[i + 2];
    out += t[(n >> 18) & 63];
    out += t[(n >> 12) & 63];
    out += t[(n >> 6) & 63];
    out += t[n & 63];
  }
  if (i < in.size()) {
    uint32_t n = static_cast<uint32_t>(in[i]) << 16;
    const bool two = (i + 1) < in.size();
    if (two) n |= static_cast<uint32_t>(in[i + 1]) << 8;
    out += t[(n >> 18) & 63];
    out += t[(n >> 12) & 63];
    out += two ? t[(n >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

#ifdef GLSPLAY_HAVE_NVML
// nvml.dll ships with the NVIDIA driver, but nvml.lib (the import library) only
// comes with the full CUDA Toolkit. Resolve the handful of entry points we need
// at runtime instead - same pattern as nvenc_session.cpp with nvEncodeAPI64.dll.
class NvmlProbe {
 public:
  NvmlProbe() {
    module_ = LoadLibraryW(L"nvml.dll");
    if (!module_) return;
    init_ = reinterpret_cast<PfnInit>(GetProcAddress(module_, "nvmlInit_v2"));
    shutdown_ =
        reinterpret_cast<PfnShutdown>(GetProcAddress(module_, "nvmlShutdown"));
    get_handle_ = reinterpret_cast<PfnGetHandle>(
        GetProcAddress(module_, "nvmlDeviceGetHandleByIndex_v2"));
    get_util_ = reinterpret_cast<PfnGetUtil>(
        GetProcAddress(module_, "nvmlDeviceGetUtilizationRates"));
    get_encoder_util_ = reinterpret_cast<PfnGetEncoderUtil>(
        GetProcAddress(module_, "nvmlDeviceGetEncoderUtilization"));
    if (!init_ || !get_handle_ || !get_util_) return;
    if (init_() != NVML_SUCCESS) return;
    if (get_handle_(0, &device_) != NVML_SUCCESS) return;
    ready_ = true;
  }
  ~NvmlProbe() {
    if (ready_ && shutdown_) shutdown_();
    if (module_) FreeLibrary(module_);
  }

  void Sample(double* gpu_percent, double* encoder_percent) {
    if (!ready_) return;
    nvmlUtilization_t util{};
    if (get_util_(device_, &util) == NVML_SUCCESS) {
      *gpu_percent = util.gpu;
    }
    unsigned int encoder_util = 0;
    unsigned int sampling_us = 0;
    if (get_encoder_util_ &&
        get_encoder_util_(device_, &encoder_util, &sampling_us) == NVML_SUCCESS) {
      *encoder_percent = encoder_util;
    }
  }

 private:
  using PfnInit = nvmlReturn_t (*)();
  using PfnShutdown = nvmlReturn_t (*)();
  using PfnGetHandle = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
  using PfnGetUtil = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
  using PfnGetEncoderUtil =
      nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, unsigned int*);

  HMODULE module_ = nullptr;
  PfnInit init_ = nullptr;
  PfnShutdown shutdown_ = nullptr;
  PfnGetHandle get_handle_ = nullptr;
  PfnGetUtil get_util_ = nullptr;
  PfnGetEncoderUtil get_encoder_util_ = nullptr;
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

  // The client draws the cursor itself, so shape and visibility changes are
  // pushed as they happen rather than on the 1 Hz stats cadence.
  std::vector<uint8_t> last_shape;
  uint32_t last_shape_type = 0xFFFFFFFFu;
  uint32_t last_w = 0;
  uint32_t last_h = 0;
  int last_visible = -1;
  int32_t last_x = INT32_MIN;
  int32_t last_y = INT32_MIN;

  auto emit_cursor = [&]() {
    if (!capture_) return;
    const CursorState cur = capture_->cursor_snapshot();
    const bool shape_changed =
        cur.shape != last_shape || cur.shape_type != last_shape_type;
    const bool vis_changed = static_cast<int>(cur.visible) != last_visible;
    const bool pos_changed = cur.x != last_x || cur.y != last_y;
    if (!shape_changed && !vis_changed && !pos_changed) return;

    std::string msg = "{\"type\":\"cursor\",\"visible\":";
    msg += cur.visible ? "true" : "false";
    msg += ",\"x\":" + std::to_string(cur.x);
    msg += ",\"y\":" + std::to_string(cur.y);
    msg += ",\"hotspotX\":" + std::to_string(cur.hotspot_x);
    msg += ",\"hotspotY\":" + std::to_string(cur.hotspot_y);
    if (shape_changed) {
      std::vector<uint8_t> rgba;
      uint32_t w = 0;
      uint32_t h = 0;
      if (DecodeCursorRgba(cur, &rgba, &w, &h)) {
        msg += ",\"width\":" + std::to_string(w);
        msg += ",\"height\":" + std::to_string(h);
        msg += ",\"rgbaBase64\":\"" + Base64(rgba) + "\"";
        last_w = w;
        last_h = h;
      } else {
        msg += ",\"width\":0,\"height\":0";
      }
      last_shape = cur.shape;
      last_shape_type = cur.shape_type;
    } else {
      msg += ",\"width\":" + std::to_string(last_w);
      msg += ",\"height\":" + std::to_string(last_h);
    }
    msg += '}';
    session_->SendControl(msg);
    last_visible = static_cast<int>(cur.visible);
    last_x = cur.x;
    last_y = cur.y;
  };

  int elapsed_ms = 0;
  // The cursor overlay is drawn client-side from these position updates, so it
  // is only as smooth as this loop is fast. Poll at ~60Hz to match the video;
  // emit_cursor() early-returns when nothing moved, so a still pointer costs
  // nothing and a moving one sends ~120-byte position messages.
  constexpr int kTickMs = 16;

  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
    if (!running_.load()) break;

    emit_cursor();

    elapsed_ms += kTickMs;
    if (elapsed_ms < interval_ms_) continue;
    elapsed_ms = 0;

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
