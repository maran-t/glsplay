#include "encode/nvenc_session.h"

#include <windows.h>

#include <chrono>
#include <cstring>

#include "util/log.h"

namespace glsplay {
namespace {

using NvEncodeAPICreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using NvEncodeAPIGetMaxSupportedVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);

const char* StatusName(NVENCSTATUS status) {
  switch (status) {
    case NV_ENC_SUCCESS: return "SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE: return "INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST: return "DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR: return "INVALID_PTR";
    case NV_ENC_ERR_INVALID_PARAM: return "INVALID_PARAM";
    case NV_ENC_ERR_INVALID_VERSION: return "INVALID_VERSION";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED: return "ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM: return "UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY: return "LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER: return "NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_ENCODER_BUSY: return "ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD: return "EVENT_NOT_REGISTERED";
    case NV_ENC_ERR_GENERIC: return "GENERIC";
    case NV_ENC_ERR_UNIMPLEMENTED: return "UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED: return "RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED: return "RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED: return "RESOURCE_NOT_MAPPED";
    default: return "UNKNOWN";
  }
}

HMODULE LoadNvencLibrary() {
  static HMODULE module = LoadLibraryW(L"nvEncodeAPI64.dll");
  return module;
}

int64_t NowMicroseconds() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

#define NVENC_CHECK(expr, what)                                        \
  do {                                                                 \
    const NVENCSTATUS status_ = (expr);                                \
    if (status_ != NV_ENC_SUCCESS) {                                   \
      LOG_ERROR << what << " failed: " << StatusName(status_)          \
                << " (" << static_cast<int>(status_) << ')';           \
      return false;                                                    \
    }                                                                  \
  } while (false)

NvencSession::NvencSession() = default;

NvencSession::~NvencSession() {
  Shutdown();
}

bool NvencSession::IsAvailable() {
  HMODULE module = LoadNvencLibrary();
  if (module == nullptr) {
    LOG_ERROR << "nvEncodeAPI64.dll not found. Install the NVIDIA driver - on a "
                 "GCP L4 this ships with the data-center or vWS driver package.";
    return false;
  }

  auto get_version = reinterpret_cast<NvEncodeAPIGetMaxSupportedVersionFn>(
      GetProcAddress(module, "NvEncodeAPIGetMaxSupportedVersion"));
  if (get_version == nullptr) {
    LOG_ERROR << "NvEncodeAPIGetMaxSupportedVersion missing from nvEncodeAPI64.dll";
    return false;
  }

  uint32_t driver_version = 0;
  if (get_version(&driver_version) != NV_ENC_SUCCESS) {
    LOG_ERROR << "NvEncodeAPIGetMaxSupportedVersion call failed";
    return false;
  }

  const uint32_t header_version =
      (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
  if (driver_version < header_version) {
    LOG_ERROR << "NVIDIA driver supports NVENC API " << (driver_version >> 4) << '.'
              << (driver_version & 0xf) << " but this build needs "
              << NVENCAPI_MAJOR_VERSION << '.' << NVENCAPI_MINOR_VERSION
              << ". Update the driver or build against an older Video Codec SDK.";
    return false;
  }

  return true;
}

bool NvencSession::Initialise(ID3D11Device* device, const NvencSettings& settings) {
  if (!IsAvailable()) return false;

  settings_ = settings;
  device_ = device;

  if (!OpenSession(device)) return false;
  if (!Configure(settings)) return false;
  if (!AllocateResources()) return false;
  if (!FetchSequenceParameters()) return false;

  LOG_INFO << "NVENC ready: " << encoder_name_ << ' ' << settings.width << 'x'
           << settings.height << '@' << settings.fps << ' ' << settings.bitrate_kbps
           << " kbps CBR, 0 B-frames, intra-refresh";
  return true;
}

bool NvencSession::OpenSession(ID3D11Device* device) {
  HMODULE module = LoadNvencLibrary();
  if (module == nullptr) return false;

  auto create_instance = reinterpret_cast<NvEncodeAPICreateInstanceFn>(
      GetProcAddress(module, "NvEncodeAPICreateInstance"));
  if (create_instance == nullptr) {
    LOG_ERROR << "NvEncodeAPICreateInstance missing from nvEncodeAPI64.dll";
    return false;
  }

  std::memset(&api_, 0, sizeof(api_));
  api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  NVENC_CHECK(create_instance(&api_), "NvEncodeAPICreateInstance");

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
  params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  // DirectX rather than CUDA: the frame is already a D3D11 texture on this
  // device, so this is what keeps the whole path inside VRAM with no CUDA
  // context and no interop copy.
  params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
  params.device = device;
  params.apiVersion = NVENCAPI_VERSION;

  NVENC_CHECK(api_.nvEncOpenEncodeSessionEx(&params, &encoder_),
              "nvEncOpenEncodeSessionEx");
  return true;
}

bool NvencSession::Configure(const NvencSettings& settings) {
  // Query the driver's preset defaults, then override only what matters. The
  // SDK explicitly requires starting from a preset config rather than a
  // zeroed struct, since new fields appear between versions.
  NV_ENC_PRESET_CONFIG preset{};
  preset.version = NV_ENC_PRESET_CONFIG_VER;
  preset.presetCfg.version = NV_ENC_CONFIG_VER;

  // P1 is the fastest preset. PRD section 4.2 pins this: quality presets add
  // multi-pass analysis that costs more latency than the bitrate saves.
  NVENC_CHECK(api_.nvEncGetEncodePresetConfigEx(
                  encoder_, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
                  NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset),
              "nvEncGetEncodePresetConfigEx");

  encode_config_ = preset.presetCfg;
  encode_config_.version = NV_ENC_CONFIG_VER;
  encode_config_.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;

  // Infinite GOP. Combined with intra-refresh below, this removes periodic IDR
  // frames entirely - no bitrate spike, no visible hitch every few seconds.
  encode_config_.gopLength = NVENC_INFINITE_GOPLENGTH;
  encode_config_.frameIntervalP = 1;  // IPPP, no B-frames

  auto& rc = encode_config_.rcParams;
  rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  rc.averageBitRate = static_cast<uint32_t>(settings.bitrate_kbps) * 1000u;
  rc.maxBitRate = rc.averageBitRate;
  // A single-frame VBV is the whole trick for low latency: it forbids the
  // encoder from borrowing bits across frames, so no frame can arrive late
  // because an earlier one overspent.
  rc.vbvBufferSize = rc.averageBitRate / static_cast<uint32_t>(settings.fps);
  rc.vbvInitialDelay = rc.vbvBufferSize;
  rc.enableAQ = 1;  // spatial AQ; cheap and visibly better on dark game scenes
  rc.zeroReorderDelay = 1;
  rc.enableLookahead = 0;  // lookahead buffers frames, which is pure added delay

  auto& h264 = encode_config_.encodeCodecConfig.h264Config;
  h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
  // Intra-refresh: a moving band of intra-coded macroblocks that fully
  // refreshes the picture over intra_refresh_count frames.
  h264.enableIntraRefresh = 1;
  h264.intraRefreshPeriod = static_cast<uint32_t>(settings.intra_refresh_period);
  h264.intraRefreshCnt = static_cast<uint32_t>(settings.intra_refresh_count);
  // Repeat SPS/PPS so a client joining mid-stream, or recovering from loss,
  // can decode without waiting for the next parameter set.
  h264.repeatSPSPPS = 1;
  h264.outputAUD = 0;
  h264.sliceMode = 3;       // slices per frame
  h264.sliceModeData = 1;   // one slice, minimising packetisation overhead
  h264.chromaFormatIDC = 1; // 4:2:0
  // Fill in the VUI so the browser applies the right colour matrix instead of
  // guessing, which is what causes washed-out or oversaturated output.
  h264.h264VUIParameters.videoSignalTypePresentFlag = 1;
  h264.h264VUIParameters.videoFullRangeFlag = 0;
  h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
  // Value 1 is BT.709 in all three tables. Recent SDK versions made these
  // strongly-typed enums, so the cast is required; casting the literal rather
  // than naming the constant keeps this working across SDK versions that spell
  // the enumerator differently.
  h264.h264VUIParameters.colourPrimaries =
      static_cast<NV_ENC_VUI_COLOR_PRIMARIES>(1);
  h264.h264VUIParameters.transferCharacteristics =
      static_cast<NV_ENC_VUI_TRANSFER_CHARACTERISTIC>(1);
  h264.h264VUIParameters.colourMatrix =
      static_cast<NV_ENC_VUI_MATRIX_COEFFS>(1);

  std::memset(&init_params_, 0, sizeof(init_params_));
  init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
  init_params_.encodeGUID = NV_ENC_CODEC_H264_GUID;
  init_params_.presetGUID = NV_ENC_PRESET_P1_GUID;
  init_params_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  init_params_.encodeWidth = static_cast<uint32_t>(settings.width);
  init_params_.encodeHeight = static_cast<uint32_t>(settings.height);
  init_params_.darWidth = static_cast<uint32_t>(settings.width);
  init_params_.darHeight = static_cast<uint32_t>(settings.height);
  init_params_.frameRateNum = static_cast<uint32_t>(settings.fps);
  init_params_.frameRateDen = 1;
  init_params_.enablePTD = 1;  // let NVENC decide picture types
  init_params_.encodeConfig = &encode_config_;
  // Asynchronous mode needs completion events; the synchronous path is simpler
  // and on an L4 the encode finishes fast enough that the difference is noise.
  init_params_.enableEncodeAsync = 0;

  NVENC_CHECK(api_.nvEncInitializeEncoder(encoder_, &init_params_),
              "nvEncInitializeEncoder");

  encoder_name_ = "NVENC H.264 High (P1, ultra-low-latency)";
  return true;
}

bool NvencSession::AllocateResources() {
  resources_.resize(kBufferCount);

  for (size_t i = 0; i < resources_.size(); ++i) {
    auto& resource = resources_[i];

    // Each slot owns a texture that captured frames are copied into, so a
    // frame still being encoded is never overwritten by the next capture.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(settings_.width);
    desc.Height = static_cast<UINT>(settings_.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &resource.texture);
    if (FAILED(hr)) {
      LOG_ERROR << "input texture " << i << " creation failed: " << HrToString(hr);
      return false;
    }

    // This is the call PRD section 4.2 describes: the texture is handed to
    // NVENC by pointer, and no pixel ever crosses the PCIe bus.
    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = static_cast<uint32_t>(settings_.width);
    reg.height = static_cast<uint32_t>(settings_.height);
    reg.resourceToRegister = resource.texture.Get();
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENC_CHECK(api_.nvEncRegisterResource(encoder_, &reg), "nvEncRegisterResource");
    resource.registered = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
    bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    NVENC_CHECK(api_.nvEncCreateBitstreamBuffer(encoder_, &bitstream),
                "nvEncCreateBitstreamBuffer");
    resource.bitstream = bitstream.bitstreamBuffer;
  }

  return true;
}

bool NvencSession::FetchSequenceParameters() {
  uint8_t buffer[1024] = {};
  uint32_t size = 0;

  NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
  payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
  payload.inBufferSize = sizeof(buffer);
  payload.spsppsBuffer = buffer;
  payload.outSPSPPSPayloadSize = &size;

  NVENC_CHECK(api_.nvEncGetSequenceParams(encoder_, &payload),
              "nvEncGetSequenceParams");

  sps_pps_.assign(buffer, buffer + size);
  LOG_DEBUG << "SPS/PPS is " << size << " bytes";
  return true;
}

bool NvencSession::Encode(ID3D11Texture2D* texture,
                          uint64_t timestamp_us,
                          bool force_keyframe,
                          EncodedPacket* out) {
  if (encoder_ == nullptr || texture == nullptr) return false;

  auto& resource = resources_[next_resource_];
  next_resource_ = (next_resource_ + 1) % resources_.size();

  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  device_->GetImmediateContext(&context);
  context->CopyResource(resource.texture.Get(), texture);

  NV_ENC_MAP_INPUT_RESOURCE map{};
  map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  map.registeredResource = resource.registered;
  NVENC_CHECK(api_.nvEncMapInputResource(encoder_, &map), "nvEncMapInputResource");
  resource.mapped = map.mappedResource;

  const int64_t submit_us = NowMicroseconds();

  NV_ENC_PIC_PARAMS pic{};
  pic.version = NV_ENC_PIC_PARAMS_VER;
  pic.inputBuffer = resource.mapped;
  pic.bufferFmt = map.mappedBufferFmt;
  pic.inputWidth = static_cast<uint32_t>(settings_.width);
  pic.inputHeight = static_cast<uint32_t>(settings_.height);
  pic.outputBitstream = resource.bitstream;
  pic.inputTimeStamp = timestamp_us;
  pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic.frameIdx = frame_index_++;

  if (force_keyframe) {
    // Servicing an RTCP PLI. With intra-refresh running this is rare, but a
    // decoder that has genuinely lost sync needs a real IDR to recover.
    pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
  }

  const NVENCSTATUS encode_status = api_.nvEncEncodePicture(encoder_, &pic);
  if (encode_status != NV_ENC_SUCCESS && encode_status != NV_ENC_ERR_NEED_MORE_INPUT) {
    LOG_ERROR << "nvEncEncodePicture failed: " << StatusName(encode_status);
    api_.nvEncUnmapInputResource(encoder_, resource.mapped);
    resource.mapped = nullptr;
    return false;
  }

  NV_ENC_LOCK_BITSTREAM lock{};
  lock.version = NV_ENC_LOCK_BITSTREAM_VER;
  lock.outputBitstream = resource.bitstream;
  lock.doNotWait = 0;

  const NVENCSTATUS lock_status = api_.nvEncLockBitstream(encoder_, &lock);
  if (lock_status != NV_ENC_SUCCESS) {
    LOG_ERROR << "nvEncLockBitstream failed: " << StatusName(lock_status);
    api_.nvEncUnmapInputResource(encoder_, resource.mapped);
    resource.mapped = nullptr;
    return false;
  }

  // Copy out before unlocking - NVENC reclaims the buffer immediately, and
  // handing libwebrtc a pointer into it would be a use-after-free.
  packet_scratch_.assign(static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
                         static_cast<const uint8_t*>(lock.bitstreamBufferPtr) +
                             lock.bitstreamSizeInBytes);

  out->data = packet_scratch_.data();
  out->size = packet_scratch_.size();
  out->is_keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                     lock.pictureType == NV_ENC_PIC_TYPE_I;
  out->timestamp_us = lock.outputTimeStamp;
  out->encode_time_ms = static_cast<double>(NowMicroseconds() - submit_us) / 1000.0;

  api_.nvEncUnlockBitstream(encoder_, resource.bitstream);
  api_.nvEncUnmapInputResource(encoder_, resource.mapped);
  resource.mapped = nullptr;

  return true;
}

bool NvencSession::Reconfigure(int bitrate_kbps, int fps) {
  if (encoder_ == nullptr) return false;
  if (bitrate_kbps <= 0) return false;

  settings_.bitrate_kbps = bitrate_kbps;
  if (fps > 0) settings_.fps = fps;

  auto& rc = encode_config_.rcParams;
  rc.averageBitRate = static_cast<uint32_t>(bitrate_kbps) * 1000u;
  rc.maxBitRate = rc.averageBitRate;
  rc.vbvBufferSize = rc.averageBitRate / static_cast<uint32_t>(settings_.fps);
  rc.vbvInitialDelay = rc.vbvBufferSize;

  init_params_.frameRateNum = static_cast<uint32_t>(settings_.fps);

  NV_ENC_RECONFIGURE_PARAMS params{};
  params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  params.reInitEncodeParams = init_params_;
  params.reInitEncodeParams.encodeConfig = &encode_config_;
  // Not forcing an IDR: intra-refresh already keeps the stream recoverable,
  // and a keyframe on every bandwidth adjustment would spike exactly when the
  // network is already struggling.
  params.forceIDR = 0;
  params.resetEncoder = 0;

  NVENC_CHECK(api_.nvEncReconfigureEncoder(encoder_, &params),
              "nvEncReconfigureEncoder");

  LOG_DEBUG << "NVENC retargeted to " << bitrate_kbps << " kbps";
  return true;
}

void NvencSession::ReleaseResources() {
  for (auto& resource : resources_) {
    if (resource.mapped != nullptr) {
      api_.nvEncUnmapInputResource(encoder_, resource.mapped);
      resource.mapped = nullptr;
    }
    if (resource.registered != nullptr) {
      api_.nvEncUnregisterResource(encoder_, resource.registered);
      resource.registered = nullptr;
    }
    if (resource.bitstream != nullptr) {
      api_.nvEncDestroyBitstreamBuffer(encoder_, resource.bitstream);
      resource.bitstream = nullptr;
    }
    resource.texture.Reset();
  }
  resources_.clear();
}

void NvencSession::Shutdown() {
  if (encoder_ == nullptr) return;

  // Flush. Skipping this leaks the driver-side session until process exit,
  // which matters when the encoder is rebuilt on a resolution change.
  NV_ENC_PIC_PARAMS eos{};
  eos.version = NV_ENC_PIC_PARAMS_VER;
  eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
  api_.nvEncEncodePicture(encoder_, &eos);

  ReleaseResources();
  api_.nvEncDestroyEncoder(encoder_);
  encoder_ = nullptr;
  device_ = nullptr;
  LOG_DEBUG << "NVENC session destroyed";
}

#undef NVENC_CHECK

}  // namespace glsplay
