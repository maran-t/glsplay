// NVENC encode session (PRD section 4.2).
//
// Wraps NvEncodeAPI directly rather than the SDK's sample wrapper classes, so
// every low-latency parameter is set explicitly and visibly here. The settings
// that matter are all in Configure(): P1 preset, ultra-low-latency tuning, CBR
// with a single-frame VBV, zero B-frames, and infinite GOP with intra-refresh.

#pragma once

#include <d3d11.h>
#include <nvEncodeAPI.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace glsplay {

struct NvencSettings {
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int bitrate_kbps = 20000;
  // Intra-refresh spreads the keyframe cost across this many frames rather
  // than emitting one huge IDR that spikes the bitrate and stalls playback.
  int intra_refresh_period = 240;
  int intra_refresh_count = 60;
};

struct EncodedPacket {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool is_keyframe = false;
  uint64_t timestamp_us = 0;
  // Microseconds from submit to bitstream ready, for the host-stats report.
  double encode_time_ms = 0.0;
};

class NvencSession {
 public:
  NvencSession();
  ~NvencSession();

  NvencSession(const NvencSession&) = delete;
  NvencSession& operator=(const NvencSession&) = delete;

  // Loads nvEncodeAPI64.dll and checks the driver exposes a usable version.
  // Returns false with a specific reason logged if NVENC is unavailable,
  // which on a GCP VM usually means the wrong driver variant is installed.
  static bool IsAvailable();

  bool Initialise(ID3D11Device* device, const NvencSettings& settings);
  void Shutdown();

  // Encodes a texture that already lives on the same device. force_keyframe
  // services RTCP PLI from the browser.
  bool Encode(ID3D11Texture2D* texture,
              uint64_t timestamp_us,
              bool force_keyframe,
              EncodedPacket* out);

  // Applies a new target bitrate without recreating the session, which is how
  // libwebrtc's congestion controller drives us.
  bool Reconfigure(int bitrate_kbps, int fps);

  // SPS/PPS, needed to prime the RTP packetizer.
  const std::vector<uint8_t>& sequence_parameters() const { return sps_pps_; }

  const std::string& encoder_name() const { return encoder_name_; }
  bool initialised() const { return encoder_ != nullptr; }

 private:
  struct InputResource {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    NV_ENC_REGISTERED_PTR registered = nullptr;
    NV_ENC_INPUT_PTR mapped = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
    void* completion_event = nullptr;
    bool in_flight = false;
  };

  bool OpenSession(ID3D11Device* device);
  bool Configure(const NvencSettings& settings);
  bool AllocateResources();
  void ReleaseResources();
  bool FetchSequenceParameters();

  NV_ENCODE_API_FUNCTION_LIST api_{};
  void* encoder_ = nullptr;
  ID3D11Device* device_ = nullptr;

  NV_ENC_INITIALIZE_PARAMS init_params_{};
  NV_ENC_CONFIG encode_config_{};
  NvencSettings settings_;

  // A small ring so the CPU can submit the next frame while the GPU is still
  // producing the previous bitstream. Deeper would add latency for no gain at
  // 60fps, since NVENC on an L4 finishes a 1080p frame in well under 16ms.
  static constexpr size_t kBufferCount = 3;
  std::vector<InputResource> resources_;
  size_t next_resource_ = 0;

  std::vector<uint8_t> sps_pps_;
  std::vector<uint8_t> packet_scratch_;
  std::string encoder_name_;
  uint32_t frame_index_ = 0;
};

}  // namespace glsplay
