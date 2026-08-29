// Adapts NvencSession to libwebrtc's VideoEncoder interface.
//
// This is what makes the zero-copy path work end to end. libwebrtc hands us a
// VideoFrame; when its buffer is a D3D11FrameBuffer we pull the texture
// straight out and feed NVENC, so no pixel ever leaves VRAM. Because we are a
// real VideoEncoder rather than an RTP injector, libwebrtc keeps ownership of
// pacing, NACK, FEC and - critically - Google Congestion Control, which drives
// SetRates() as the network changes.

#pragma once

#include <memory>
#include <mutex>

#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
// WEBRTC_VIDEO_CODEC_* moved out of video_codec_interface.h.
#include "modules/video_coding/include/video_error_codes.h"

#include "encode/nvenc_session.h"

namespace glsplay {

class NvencVideoEncoder : public webrtc::VideoEncoder {
 public:
  // The D3D11 device must be the same one the capture source produces textures
  // on; NVENC cannot read a texture from a different device.
  explicit NvencVideoEncoder(Microsoft::WRL::ComPtr<ID3D11Device> device);
  ~NvencVideoEncoder() override;

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const VideoEncoder::Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const webrtc::VideoFrame& frame,
                 const std::vector<webrtc::VideoFrameType>* frame_types) override;
  void SetRates(const RateControlParameters& parameters) override;
  EncoderInfo GetEncoderInfo() const override;

  // Sampled by the stats reporter for the browser HUD.
  double mean_encode_ms() const;
  uint64_t frames_encoded() const;

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;

  std::mutex mutex_;
  std::unique_ptr<NvencSession> session_;
  webrtc::EncodedImageCallback* callback_ = nullptr;

  NvencSettings settings_;
  bool initialised_ = false;

  double mean_encode_ms_ = 0.0;
  uint64_t frames_encoded_ = 0;
};

}  // namespace glsplay
