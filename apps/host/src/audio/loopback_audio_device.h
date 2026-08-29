// AudioDeviceModule that sources audio from WASAPI loopback.
//
// libwebrtc has no "push me some PCM" entry point - audio enters through an
// AudioDeviceModule, so a custom one is the supported way to feed it something
// other than a microphone. Most of this interface is playback and mixer
// control that a capture-only host has no use for, so those methods are
// deliberate no-ops; the parts that matter are RegisterAudioCallback,
// StartRecording, and the 10ms framing in Deliver().

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "api/scoped_refptr.h"
#include "modules/audio_device/include/audio_device.h"

#include "audio/wasapi_loopback.h"

namespace glsplay {

class LoopbackAudioDevice : public webrtc::AudioDeviceModule {
 public:
  static webrtc::scoped_refptr<LoopbackAudioDevice> Create(int sample_rate, int channels);

  LoopbackAudioDevice(int sample_rate, int channels);
  ~LoopbackAudioDevice() override;

  bool loopback_ready() const { return loopback_ready_; }
  std::string device_name() const;

  // --- the methods that actually do something ----------------------------
  int32_t RegisterAudioCallback(webrtc::AudioTransport* transport) override;
  int32_t Init() override;
  int32_t Terminate() override;
  bool Initialized() const override;
  int32_t InitRecording() override;
  bool RecordingIsInitialized() const override;
  int32_t StartRecording() override;
  int32_t StopRecording() override;
  bool Recording() const override;

  // --- everything below is unused by a capture-only host ------------------
  int32_t ActiveAudioLayer(AudioLayer* layer) const override;
  int16_t PlayoutDevices() override;
  int16_t RecordingDevices() override;
  int32_t PlayoutDeviceName(uint16_t, char[webrtc::kAdmMaxDeviceNameSize],
                            char[webrtc::kAdmMaxGuidSize]) override;
  int32_t RecordingDeviceName(uint16_t, char[webrtc::kAdmMaxDeviceNameSize],
                              char[webrtc::kAdmMaxGuidSize]) override;
  int32_t SetPlayoutDevice(uint16_t) override;
  int32_t SetPlayoutDevice(WindowsDeviceType) override;
  int32_t SetRecordingDevice(uint16_t) override;
  int32_t SetRecordingDevice(WindowsDeviceType) override;
  int32_t PlayoutIsAvailable(bool* available) override;
  int32_t InitPlayout() override;
  bool PlayoutIsInitialized() const override;
  int32_t RecordingIsAvailable(bool* available) override;
  int32_t StartPlayout() override;
  int32_t StopPlayout() override;
  bool Playing() const override;
  int32_t InitSpeaker() override;
  bool SpeakerIsInitialized() const override;
  int32_t InitMicrophone() override;
  bool MicrophoneIsInitialized() const override;
  int32_t SpeakerVolumeIsAvailable(bool* available) override;
  int32_t SetSpeakerVolume(uint32_t) override;
  int32_t SpeakerVolume(uint32_t* volume) const override;
  int32_t MaxSpeakerVolume(uint32_t* volume) const override;
  int32_t MinSpeakerVolume(uint32_t* volume) const override;
  int32_t MicrophoneVolumeIsAvailable(bool* available) override;
  int32_t SetMicrophoneVolume(uint32_t) override;
  int32_t MicrophoneVolume(uint32_t* volume) const override;
  int32_t MaxMicrophoneVolume(uint32_t* volume) const override;
  int32_t MinMicrophoneVolume(uint32_t* volume) const override;
  int32_t SpeakerMuteIsAvailable(bool* available) override;
  int32_t SetSpeakerMute(bool) override;
  int32_t SpeakerMute(bool* enabled) const override;
  int32_t MicrophoneMuteIsAvailable(bool* available) override;
  int32_t SetMicrophoneMute(bool) override;
  int32_t MicrophoneMute(bool* enabled) const override;
  int32_t StereoPlayoutIsAvailable(bool* available) const override;
  int32_t SetStereoPlayout(bool) override;
  int32_t StereoPlayout(bool* enabled) const override;
  int32_t StereoRecordingIsAvailable(bool* available) const override;
  int32_t SetStereoRecording(bool) override;
  int32_t StereoRecording(bool* enabled) const override;
  int32_t PlayoutDelay(uint16_t* delay_ms) const override;
  bool BuiltInAECIsAvailable() const override;
  bool BuiltInAGCIsAvailable() const override;
  bool BuiltInNSIsAvailable() const override;
  int32_t EnableBuiltInAEC(bool) override;
  int32_t EnableBuiltInAGC(bool) override;
  int32_t EnableBuiltInNS(bool) override;

 private:
  // Accumulates WASAPI packets and emits exactly 10ms frames, which is the
  // only cadence libwebrtc's AudioTransport accepts.
  void Deliver(const int16_t* samples, size_t frames, int sample_rate, int channels);

  std::unique_ptr<WasapiLoopback> loopback_;
  webrtc::AudioTransport* transport_ = nullptr;

  mutable std::mutex mutex_;
  std::vector<int16_t> pending_;
  size_t frames_per_10ms_ = 480;

  int sample_rate_;
  int channels_;

  std::atomic<bool> initialised_{false};
  std::atomic<bool> recording_{false};
  bool loopback_ready_ = false;
};

}  // namespace glsplay
