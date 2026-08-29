// VideoEncoderFactory that advertises only NVENC H.264.
//
// Installed on the PeerConnectionFactory so libwebrtc never builds its own
// software encoder for our track. Advertising a single codec also keeps the
// SDP offer narrow, which is what lets the browser reliably negotiate the
// High profile the L4 is producing.

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

#include "api/video_codecs/video_encoder_factory.h"

namespace glsplay {

class NvencVideoEncoder;

class NvencEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  explicit NvencEncoderFactory(Microsoft::WRL::ComPtr<ID3D11Device> device);
  ~NvencEncoderFactory() override;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

  // The most recently created encoder, for telemetry. Null before the first
  // negotiation completes.
  NvencVideoEncoder* active_encoder() const { return active_; }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  // Non-owning: libwebrtc owns the encoder it was handed.
  mutable NvencVideoEncoder* active_ = nullptr;
};

}  // namespace glsplay
