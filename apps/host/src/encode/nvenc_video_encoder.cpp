#include "encode/nvenc_video_encoder.h"

#include "api/video/encoded_image.h"
#include "api/video/video_timing.h"
#include "rtc_base/ref_counted_object.h"

#include "capture/d3d11_frame_buffer.h"
#include "util/log.h"

namespace glsplay {

NvencVideoEncoder::NvencVideoEncoder(Microsoft::WRL::ComPtr<ID3D11Device> device,
                                     int playout_delay_max_ms)
    : device_(std::move(device)),
      playout_delay_max_ms_(std::max(0, playout_delay_max_ms)) {}

NvencVideoEncoder::~NvencVideoEncoder() {
  Release();
}

int NvencVideoEncoder::InitEncode(const webrtc::VideoCodec* codec_settings,
                                  const VideoEncoder::Settings& settings) {
  if (codec_settings == nullptr) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  if (codec_settings->codecType != webrtc::kVideoCodecH264) {
    LOG_ERROR << "NvencVideoEncoder asked for a non-H.264 codec";
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  std::lock_guard<std::mutex> guard(mutex_);

  settings_.width = codec_settings->width;
  settings_.height = codec_settings->height;
  settings_.fps = static_cast<int>(codec_settings->maxFramerate);
  // startBitrate is in kbps already. libwebrtc revises this almost immediately
  // through SetRates() once congestion control has an estimate.
  settings_.bitrate_kbps = static_cast<int>(codec_settings->startBitrate);
  if (settings_.bitrate_kbps <= 0) settings_.bitrate_kbps = 25000;
  if (settings_.fps <= 0) settings_.fps = 60;

  session_ = std::make_unique<NvencSession>();
  if (!session_->Initialise(device_.Get(), settings_)) {
    LOG_ERROR << "NVENC session initialisation failed";
    session_.reset();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  initialised_ = true;
  LOG_INFO << "NvencVideoEncoder ready: " << settings_.width << 'x' << settings_.height
           << '@' << settings_.fps << ' ' << settings_.bitrate_kbps << " kbps"
           << " (requested by libwebrtc, cores=" << settings.number_of_cores << ')';
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvencVideoEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> guard(mutex_);
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvencVideoEncoder::Release() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (session_) {
    session_->Shutdown();
    session_.reset();
  }
  callback_ = nullptr;
  initialised_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvencVideoEncoder::Encode(const webrtc::VideoFrame& frame,
                                  const std::vector<webrtc::VideoFrameType>* frame_types) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!initialised_ || !session_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (callback_ == nullptr) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  // A key frame request here is libwebrtc servicing an RTCP PLI or FIR from
  // the browser, or priming the very first frame.
  bool force_keyframe = false;
  if (frame_types != nullptr) {
    for (const auto type : *frame_types) {
      if (type == webrtc::VideoFrameType::kVideoFrameKey) force_keyframe = true;
    }
  }

  auto buffer = frame.video_frame_buffer();
  if (buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    // Something upstream converted our texture to I420. Encoding it would mean
    // uploading pixels back to the GPU, which defeats the entire design, so
    // this is reported rather than silently absorbed.
    LOG_ERROR << "frame arrived as a non-native buffer - zero-copy path broken";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  auto* d3d_buffer = static_cast<D3D11FrameBuffer*>(buffer.get());
  ID3D11Texture2D* texture = d3d_buffer->texture();
  if (texture == nullptr) return WEBRTC_VIDEO_CODEC_ERROR;

  EncodedPacket packet;
  if (!session_->Encode(texture, static_cast<uint64_t>(frame.timestamp_us()),
                        force_keyframe, &packet)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (packet.size == 0) return WEBRTC_VIDEO_CODEC_OK;  // nothing emitted this call

  mean_encode_ms_ = mean_encode_ms_ * 0.9 + packet.encode_time_ms * 0.1;
  ++frames_encoded_;

  webrtc::EncodedImage image;
  image.SetEncodedData(webrtc::EncodedImageBuffer::Create(packet.data, packet.size));
  image._encodedWidth = static_cast<uint32_t>(settings_.width);
  image._encodedHeight = static_cast<uint32_t>(settings_.height);
  image.SetRtpTimestamp(frame.rtp_timestamp());
  image.capture_time_ms_ = frame.render_time_ms();
  image.ntp_time_ms_ = frame.ntp_time_ms();
  image.rotation_ = frame.rotation();
  image._frameType = packet.is_keyframe ? webrtc::VideoFrameType::kVideoFrameKey
                                        : webrtc::VideoFrameType::kVideoFrameDelta;

  // Playout-delay RTP header extension (PRD section 5). This is the only way to
  // bound the receiver's jitter buffer that the receiver must obey: the
  // client-side jitterBufferTarget=0 hint is advisory, and Chrome was observed
  // overriding it and holding 100-170ms. min=0 means "render as soon as you
  // can"; the max leaves a small window so genuine network jitter is still
  // absorbed rather than juddering. Requires the extension to be negotiated -
  // PeerSession enables it on the video transceiver; without that this is
  // silently dropped rather than sent.
  image.SetPlayoutDelay(webrtc::VideoPlayoutDelay(
      webrtc::TimeDelta::Zero(),
      webrtc::TimeDelta::Millis(playout_delay_max_ms_)));

  webrtc::CodecSpecificInfo codec_specific;
  codec_specific.codecType = webrtc::kVideoCodecH264;
  // Single NAL per packet is impossible at 1080p, so non-interleaved mode with
  // FU-A fragmentation is the only workable choice - and it is what the client
  // negotiates via packetization-mode=1.
  codec_specific.codecSpecific.H264.packetization_mode =
      webrtc::H264PacketizationMode::NonInterleaved;
  codec_specific.codecSpecific.H264.idr_frame = packet.is_keyframe;

  const auto result = callback_->OnEncodedImage(image, &codec_specific);
  if (result.error != webrtc::EncodedImageCallback::Result::OK) {
    LOG_WARN << "OnEncodedImage rejected a frame";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

void NvencVideoEncoder::SetRates(const RateControlParameters& parameters) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!session_) return;

  // This is Google Congestion Control talking. Following it is the entire
  // reason for using libwebrtc rather than pushing RTP ourselves - ignoring it
  // would mean blasting CBR into a link that has already told us it is full.
  const int bitrate_kbps = static_cast<int>(parameters.bitrate.get_sum_kbps());
  const int fps = static_cast<int>(parameters.framerate_fps);
  if (bitrate_kbps <= 0) return;

  if (bitrate_kbps != settings_.bitrate_kbps || (fps > 0 && fps != settings_.fps)) {
    if (session_->Reconfigure(bitrate_kbps, fps)) {
      settings_.bitrate_kbps = bitrate_kbps;
      if (fps > 0) settings_.fps = fps;
    }
  }
}

webrtc::VideoEncoder::EncoderInfo NvencVideoEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name = "NVENC-H264";
  info.is_hardware_accelerated = true;
  // NVENC applies its own rate control, so libwebrtc should not also try to
  // drop frames or adjust quantisers on our behalf.
  info.has_trusted_rate_controller = true;
  info.supports_native_handle = true;
  // Never let libwebrtc substitute a software encoder. If NVENC is broken we
  // want a loud failure, not a silent 3fps CPU fallback that looks like a
  // network problem.
  info.supports_simulcast = false;
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  return info;
}

double NvencVideoEncoder::mean_encode_ms() const {
  return mean_encode_ms_;
}

uint64_t NvencVideoEncoder::frames_encoded() const {
  return frames_encoded_;
}

}  // namespace glsplay
