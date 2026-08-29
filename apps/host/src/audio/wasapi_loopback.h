// WASAPI loopback capture (PRD section 4.3).
//
// Captures whatever the default render endpoint is playing, which is how you
// record game audio without a virtual cable. Note that a Windows Server VM
// usually has no audio endpoint at all and the Windows Audio service is
// disabled by default - Initialise() reports that case specifically, because
// the raw COM error is unhelpful.

#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace glsplay {

class WasapiLoopback {
 public:
  // Delivers interleaved 16-bit PCM. frames is per channel.
  using DataCallback = std::function<void(const int16_t* samples,
                                          size_t frames,
                                          int sample_rate,
                                          int channels)>;

  WasapiLoopback();
  ~WasapiLoopback();

  WasapiLoopback(const WasapiLoopback&) = delete;
  WasapiLoopback& operator=(const WasapiLoopback&) = delete;

  bool Initialise(int target_sample_rate, int target_channels);
  bool Start(DataCallback callback);
  void Stop();

  bool running() const { return running_.load(); }
  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }
  const std::string& device_name() const { return device_name_; }

 private:
  void CaptureLoop();
  // Converts whatever mix format the endpoint uses into interleaved int16.
  void ConvertAndEmit(const uint8_t* data, uint32_t frames, bool silent);

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<IMMDevice> device_;
  Microsoft::WRL::ComPtr<IAudioClient> client_;
  Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;

  WAVEFORMATEX* mix_format_ = nullptr;
  void* event_ = nullptr;

  std::thread thread_;
  std::atomic<bool> running_{false};
  DataCallback callback_;

  int sample_rate_ = 48000;
  int channels_ = 2;
  bool source_is_float_ = false;
  int source_bits_ = 32;
  int source_channels_ = 2;
  std::string device_name_;

  std::vector<int16_t> scratch_;
  bool com_initialised_ = false;
};

}  // namespace glsplay
