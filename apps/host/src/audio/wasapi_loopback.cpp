#include "audio/wasapi_loopback.h"

#include <windows.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>

#include "util/log.h"

namespace glsplay {
namespace {

// 10ms buffer. PRD section 4.3 asks for 5-10ms frames to keep encoder delay
// down, and this is the smallest period WASAPI shared mode reliably services.
constexpr REFERENCE_TIME kBufferDuration = 100000;  // 10ms in 100ns units

std::string DeviceFriendlyName(IMMDevice* device) {
  Microsoft::WRL::ComPtr<IPropertyStore> props;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return "unknown";

  PROPVARIANT value;
  PropVariantInit(&value);
  std::string name = "unknown";
  if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
      value.vt == VT_LPWSTR) {
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, nullptr, nullptr);
    if (length > 1) {
      name.assign(static_cast<size_t>(length - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, name.data(), length, nullptr,
                          nullptr);
    }
  }
  PropVariantClear(&value);
  return name;
}

int16_t FloatToPcm16(float sample) {
  const float clamped = std::max(-1.0f, std::min(1.0f, sample));
  return static_cast<int16_t>(clamped * 32767.0f);
}

}  // namespace

WasapiLoopback::WasapiLoopback() = default;

WasapiLoopback::~WasapiLoopback() {
  Stop();
  if (mix_format_ != nullptr) {
    CoTaskMemFree(mix_format_);
    mix_format_ = nullptr;
  }
  if (event_ != nullptr) {
    CloseHandle(event_);
    event_ = nullptr;
  }
  if (com_initialised_) CoUninitialize();
}

bool WasapiLoopback::Initialise(int target_sample_rate, int target_channels) {
  sample_rate_ = target_sample_rate;
  channels_ = target_channels;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) com_initialised_ = true;
  else if (hr != RPC_E_CHANGED_MODE) {
    LOG_ERROR << "CoInitializeEx failed: " << HrToString(hr);
    return false;
  }

  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        IID_PPV_ARGS(&enumerator_));
  if (FAILED(hr)) {
    LOG_ERROR << "MMDeviceEnumerator creation failed: " << HrToString(hr);
    return false;
  }

  // eRender + loopback is the whole trick: we open the *playback* endpoint and
  // ask for a capture stream from it.
  hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
  if (FAILED(hr)) {
    if (hr == E_NOTFOUND) {
      LOG_ERROR << "No audio render endpoint. Windows Server VMs ship without one.";
      LOG_ERROR << "Install a virtual audio device, or run with --no-audio.";
    } else {
      LOG_ERROR << "GetDefaultAudioEndpoint failed: " << HrToString(hr);
    }
    return false;
  }

  device_name_ = DeviceFriendlyName(device_.Get());

  hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
  if (FAILED(hr)) {
    LOG_ERROR << "IAudioClient activation failed: " << HrToString(hr);
    return false;
  }

  hr = client_->GetMixFormat(&mix_format_);
  if (FAILED(hr)) {
    LOG_ERROR << "GetMixFormat failed: " << HrToString(hr);
    return false;
  }

  // The endpoint dictates the format in loopback mode - we cannot request one,
  // so whatever it hands back gets converted downstream.
  source_channels_ = mix_format_->nChannels;
  source_bits_ = mix_format_->wBitsPerSample;
  source_is_float_ = false;

  if (mix_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    source_is_float_ = true;
  } else if (mix_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_format_);
    source_is_float_ = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  }

  sample_rate_ = static_cast<int>(mix_format_->nSamplesPerSec);
  channels_ = std::min(source_channels_, 2);

  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event_ == nullptr) {
    LOG_ERROR << "CreateEvent failed: " << LastErrorToString();
    return false;
  }

  hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                           AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                           kBufferDuration, 0, mix_format_, nullptr);
  if (FAILED(hr)) {
    LOG_ERROR << "IAudioClient::Initialize failed: " << HrToString(hr);
    return false;
  }

  hr = client_->SetEventHandle(static_cast<HANDLE>(event_));
  if (FAILED(hr)) {
    LOG_ERROR << "SetEventHandle failed: " << HrToString(hr);
    return false;
  }

  hr = client_->GetService(IID_PPV_ARGS(&capture_));
  if (FAILED(hr)) {
    LOG_ERROR << "IAudioCaptureClient unavailable: " << HrToString(hr);
    return false;
  }

  LOG_INFO << "WASAPI loopback ready: " << device_name_ << ' ' << sample_rate_ << "Hz "
           << source_channels_ << "ch " << (source_is_float_ ? "float" : "int")
           << source_bits_;
  return true;
}

bool WasapiLoopback::Start(DataCallback callback) {
  if (!client_ || !capture_) return false;
  if (running_.exchange(true)) return true;

  callback_ = std::move(callback);

  const HRESULT hr = client_->Start();
  if (FAILED(hr)) {
    LOG_ERROR << "IAudioClient::Start failed: " << HrToString(hr);
    running_.store(false);
    return false;
  }

  thread_ = std::thread(&WasapiLoopback::CaptureLoop, this);
  return true;
}

void WasapiLoopback::Stop() {
  if (!running_.exchange(false)) return;
  if (event_ != nullptr) SetEvent(static_cast<HANDLE>(event_));
  if (thread_.joinable()) thread_.join();
  if (client_) client_->Stop();
}

void WasapiLoopback::ConvertAndEmit(const uint8_t* data, uint32_t frames, bool silent) {
  if (!callback_ || frames == 0) return;

  const int out_channels = channels_;
  scratch_.resize(static_cast<size_t>(frames) * out_channels);

  if (silent || data == nullptr) {
    // WASAPI signals a silent buffer rather than filling it. Emitting real
    // zeroes keeps the Opus encoder's timeline continuous - skipping the
    // buffer instead would show up as a gap and a resync.
    std::fill(scratch_.begin(), scratch_.end(), static_cast<int16_t>(0));
  } else {
    for (uint32_t frame = 0; frame < frames; ++frame) {
      for (int ch = 0; ch < out_channels; ++ch) {
        // Downmix by taking the first channels rather than averaging; game
        // audio on a VM is stereo already, and averaging a 5.1 mix would need
        // proper coefficients to avoid sounding wrong.
        const int source_ch = std::min(ch, source_channels_ - 1);
        const size_t index = static_cast<size_t>(frame) * source_channels_ + source_ch;
        int16_t value = 0;

        if (source_is_float_ && source_bits_ == 32) {
          value = FloatToPcm16(reinterpret_cast<const float*>(data)[index]);
        } else if (source_bits_ == 16) {
          value = reinterpret_cast<const int16_t*>(data)[index];
        } else if (source_bits_ == 32) {
          value = static_cast<int16_t>(reinterpret_cast<const int32_t*>(data)[index] >> 16);
        }
        scratch_[static_cast<size_t>(frame) * out_channels + ch] = value;
      }
    }
  }

  callback_(scratch_.data(), frames, sample_rate_, out_channels);
}

void WasapiLoopback::CaptureLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  while (running_.load()) {
    // 100ms guard rather than INFINITE so Stop() is always responsive, even if
    // the endpoint stalls.
    const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(event_), 100);
    if (!running_.load()) break;
    if (wait != WAIT_OBJECT_0) continue;

    UINT32 packet_frames = 0;
    while (SUCCEEDED(capture_->GetNextPacketSize(&packet_frames)) && packet_frames > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;

      const HRESULT hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hr)) {
        LOG_DEBUG << "IAudioCaptureClient::GetBuffer failed: " << HrToString(hr);
        break;
      }

      ConvertAndEmit(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
      capture_->ReleaseBuffer(frames);
    }
  }

  CoUninitialize();
}

}  // namespace glsplay
