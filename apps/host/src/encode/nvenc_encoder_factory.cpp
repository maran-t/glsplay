#include "encode/nvenc_encoder_factory.h"

#include "api/video_codecs/h264_profile_level_id.h"

#include "encode/nvenc_video_encoder.h"
#include "util/log.h"

namespace glsplay {
namespace {

// These were cricket::kH264* constants, but the cricket namespace was folded
// into webrtc and media_constants.h is not shipped in every prebuilt package.
// The values are fixed by RFC 6184, so using the literals removes a header
// dependency that has moved twice.
constexpr char kH264CodecName[] = "H264";
constexpr char kProfileLevelId[] = "profile-level-id";
constexpr char kPacketizationMode[] = "packetization-mode";
constexpr char kLevelAsymmetryAllowed[] = "level-asymmetry-allowed";

// PRD section 4.2 and the client's preferH264(): High profile, level 4.0,
// non-interleaved packetisation. The client scores offered payload types
// against exactly this fmtp, so the two must agree or it falls back to
// baseline.
webrtc::SdpVideoFormat MakeH264Format() {
  webrtc::SdpVideoFormat format(kH264CodecName);
  format.parameters[kProfileLevelId] = "640028";
  format.parameters[kPacketizationMode] = "1";
  // Games render progressive frames only; advertising level asymmetry allowed
  // lets the browser accept a lower level than it offers without renegotiating.
  format.parameters[kLevelAsymmetryAllowed] = "1";
  return format;
}

}  // namespace

NvencEncoderFactory::NvencEncoderFactory(Microsoft::WRL::ComPtr<ID3D11Device> device)
    : device_(std::move(device)) {}

NvencEncoderFactory::~NvencEncoderFactory() = default;

std::vector<webrtc::SdpVideoFormat> NvencEncoderFactory::GetSupportedFormats() const {
  // Deliberately one entry. Offering VP8/VP9 as well would let the browser
  // negotiate a codec NVENC is not configured for, and libwebrtc would then
  // quietly build a software encoder to satisfy it.
  return {MakeH264Format()};
}

std::unique_ptr<webrtc::VideoEncoder> NvencEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  (void)env;

  if (!absl::EqualsIgnoreCase(format.name, kH264CodecName)) {
    LOG_ERROR << "encoder requested for unsupported format: " << format.name;
    return nullptr;
  }

  const auto profile = webrtc::ParseSdpForH264ProfileLevelId(format.parameters);
  if (profile.has_value() && profile->profile != webrtc::H264Profile::kProfileHigh) {
    // Not fatal: NVENC is configured for High, and a browser that negotiated
    // a lower profile will still decode a High stream in practice. Worth
    // knowing about though, because it usually means the client's codec
    // preference ordering did not apply.
    LOG_WARN << "negotiated H.264 profile is not High - client codec "
                "preferences may not have been applied";
  }

  auto encoder = std::make_unique<NvencVideoEncoder>(device_);
  active_ = encoder.get();
  LOG_INFO << "created NVENC encoder for " << format.ToString();
  return encoder;
}

}  // namespace glsplay
