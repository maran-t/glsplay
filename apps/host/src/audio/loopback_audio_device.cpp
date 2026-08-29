#include "audio/loopback_audio_device.h"

#include "api/make_ref_counted.h"

#include "util/log.h"

namespace glsplay {

webrtc::scoped_refptr<LoopbackAudioDevice> LoopbackAudioDevice::Create(int sample_rate,
                                                                      int channels) {
  return webrtc::make_ref_counted<LoopbackAudioDevice>(sample_rate, channels);
}

LoopbackAudioDevice::LoopbackAudioDevice(int sample_rate, int channels)
    : sample_rate_(sample_rate), channels_(channels) {}

LoopbackAudioDevice::~LoopbackAudioDevice() {
  StopRecording();
  Terminate();
}

std::string LoopbackAudioDevice::device_name() const {
  return loopback_ ? loopback_->device_name() : std::string("none");
}

int32_t LoopbackAudioDevice::RegisterAudioCallback(webrtc::AudioTransport* transport) {
  std::lock_guard<std::mutex> guard(mutex_);
  transport_ = transport;
  return 0;
}

int32_t LoopbackAudioDevice::Init() {
  if (initialised_.load()) return 0;

  loopback_ = std::make_unique<WasapiLoopback>();
  if (!loopback_->Initialise(sample_rate_, channels_)) {
    // Not fatal. A host with no audio endpoint should still stream video -
    // which on a Windows Server VM is the normal case, not an edge case.
    LOG_WARN << "audio capture unavailable - continuing without audio";
    loopback_.reset();
    loopback_ready_ = false;
    initialised_.store(true);
    return 0;
  }

  // The endpoint dictates the rate; adopt it rather than resampling, since a
  // resampler here would add latency to the one path that is already the
  // tightest in PRD section 4.3.
  sample_rate_ = loopback_->sample_rate();
  channels_ = loopback_->channels();
  frames_per_10ms_ = static_cast<size_t>(sample_rate_) / 100;
  loopback_ready_ = true;
  initialised_.store(true);

  LOG_INFO << "audio device ready: " << loopback_->device_name() << ' ' << sample_rate_
           << "Hz " << channels_ << "ch, 10ms frames";
  return 0;
}

int32_t LoopbackAudioDevice::Terminate() {
  StopRecording();
  loopback_.reset();
  initialised_.store(false);
  loopback_ready_ = false;
  return 0;
}

bool LoopbackAudioDevice::Initialized() const {
  return initialised_.load();
}

int32_t LoopbackAudioDevice::InitRecording() {
  return initialised_.load() ? 0 : -1;
}

bool LoopbackAudioDevice::RecordingIsInitialized() const {
  return loopback_ready_;
}

int32_t LoopbackAudioDevice::StartRecording() {
  if (!loopback_ready_ || !loopback_) return 0;  // nothing to capture, not an error
  if (recording_.exchange(true)) return 0;

  const bool started = loopback_->Start(
      [this](const int16_t* samples, size_t frames, int rate, int channels) {
        Deliver(samples, frames, rate, channels);
      });
  if (!started) {
    recording_.store(false);
    return -1;
  }
  LOG_INFO << "audio capture started";
  return 0;
}

int32_t LoopbackAudioDevice::StopRecording() {
  if (!recording_.exchange(false)) return 0;
  if (loopback_) loopback_->Stop();
  std::lock_guard<std::mutex> guard(mutex_);
  pending_.clear();
  return 0;
}

bool LoopbackAudioDevice::Recording() const {
  return recording_.load();
}

void LoopbackAudioDevice::Deliver(const int16_t* samples, size_t frames, int sample_rate,
                                  int channels) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (transport_ == nullptr) return;

  pending_.insert(pending_.end(), samples,
                  samples + static_cast<size_t>(frames) * channels);

  const size_t samples_per_frame = frames_per_10ms_ * static_cast<size_t>(channels);
  size_t consumed = 0;

  while (pending_.size() - consumed >= samples_per_frame) {
    uint32_t new_mic_level = 0;
    transport_->RecordedDataIsAvailable(
        pending_.data() + consumed,
        frames_per_10ms_,
        sizeof(int16_t) * static_cast<size_t>(channels),
        static_cast<size_t>(channels),
        static_cast<uint32_t>(sample_rate),
        /*total_delay_ms=*/0,
        /*clock_drift=*/0,
        /*current_mic_level=*/0,
        /*key_pressed=*/false,
        new_mic_level);
    consumed += samples_per_frame;
  }

  if (consumed > 0) pending_.erase(pending_.begin(), pending_.begin() + consumed);

  // A backlog means libwebrtc is not draining us; dropping is better than
  // growing an unbounded buffer that would only add latency anyway.
  if (pending_.size() > samples_per_frame * 10) {
    LOG_DEBUG << "audio backlog - dropping " << pending_.size() << " samples";
    pending_.clear();
  }
}

// --- unused interface -------------------------------------------------------
// A capture-only host has no playback path, no mixer, and no hardware effects.
// These return success where libwebrtc treats a failure as fatal, and -1 where
// the call is genuinely unsupported.

int32_t LoopbackAudioDevice::ActiveAudioLayer(AudioLayer* layer) const {
  *layer = AudioLayer::kWindowsCoreAudio2;
  return 0;
}
int16_t LoopbackAudioDevice::PlayoutDevices() { return 0; }
int16_t LoopbackAudioDevice::RecordingDevices() { return loopback_ready_ ? 1 : 0; }
int32_t LoopbackAudioDevice::PlayoutDeviceName(uint16_t, char[webrtc::kAdmMaxDeviceNameSize],
                                               char[webrtc::kAdmMaxGuidSize]) { return -1; }
int32_t LoopbackAudioDevice::RecordingDeviceName(uint16_t, char[webrtc::kAdmMaxDeviceNameSize],
                                                 char[webrtc::kAdmMaxGuidSize]) { return -1; }
int32_t LoopbackAudioDevice::SetPlayoutDevice(uint16_t) { return 0; }
int32_t LoopbackAudioDevice::SetPlayoutDevice(WindowsDeviceType) { return 0; }
int32_t LoopbackAudioDevice::SetRecordingDevice(uint16_t) { return 0; }
int32_t LoopbackAudioDevice::SetRecordingDevice(WindowsDeviceType) { return 0; }
int32_t LoopbackAudioDevice::PlayoutIsAvailable(bool* available) { *available = false; return 0; }
int32_t LoopbackAudioDevice::InitPlayout() { return 0; }
bool LoopbackAudioDevice::PlayoutIsInitialized() const { return false; }
int32_t LoopbackAudioDevice::RecordingIsAvailable(bool* available) {
  *available = loopback_ready_;
  return 0;
}
int32_t LoopbackAudioDevice::StartPlayout() { return 0; }
int32_t LoopbackAudioDevice::StopPlayout() { return 0; }
bool LoopbackAudioDevice::Playing() const { return false; }
int32_t LoopbackAudioDevice::InitSpeaker() { return 0; }
bool LoopbackAudioDevice::SpeakerIsInitialized() const { return false; }
int32_t LoopbackAudioDevice::InitMicrophone() { return 0; }
bool LoopbackAudioDevice::MicrophoneIsInitialized() const { return loopback_ready_; }
int32_t LoopbackAudioDevice::SpeakerVolumeIsAvailable(bool* available) { *available = false; return 0; }
int32_t LoopbackAudioDevice::SetSpeakerVolume(uint32_t) { return -1; }
int32_t LoopbackAudioDevice::SpeakerVolume(uint32_t* volume) const { *volume = 0; return -1; }
int32_t LoopbackAudioDevice::MaxSpeakerVolume(uint32_t* volume) const { *volume = 0; return -1; }
int32_t LoopbackAudioDevice::MinSpeakerVolume(uint32_t* volume) const { *volume = 0; return -1; }
int32_t LoopbackAudioDevice::MicrophoneVolumeIsAvailable(bool* available) { *available = false; return 0; }
int32_t LoopbackAudioDevice::SetMicrophoneVolume(uint32_t) { return -1; }
int32_t LoopbackAudioDevice::MicrophoneVolume(uint32_t* volume) const { *volume = 0; return -1; }
int32_t LoopbackAudioDevice::MaxMicrophoneVolume(uint32_t* volume) const { *volume = 0; return -1; }
int32_t LoopbackAudioDevice::MinMicrophoneVolume(uint32_t* volume) const { *volume = 0; return -1; }
int32_t LoopbackAudioDevice::SpeakerMuteIsAvailable(bool* available) { *available = false; return 0; }
int32_t LoopbackAudioDevice::SetSpeakerMute(bool) { return -1; }
int32_t LoopbackAudioDevice::SpeakerMute(bool* enabled) const { *enabled = false; return -1; }
int32_t LoopbackAudioDevice::MicrophoneMuteIsAvailable(bool* available) { *available = false; return 0; }
int32_t LoopbackAudioDevice::SetMicrophoneMute(bool) { return -1; }
int32_t LoopbackAudioDevice::MicrophoneMute(bool* enabled) const { *enabled = false; return -1; }
int32_t LoopbackAudioDevice::StereoPlayoutIsAvailable(bool* available) const { *available = false; return 0; }
int32_t LoopbackAudioDevice::SetStereoPlayout(bool) { return 0; }
int32_t LoopbackAudioDevice::StereoPlayout(bool* enabled) const { *enabled = false; return 0; }
int32_t LoopbackAudioDevice::StereoRecordingIsAvailable(bool* available) const {
  *available = channels_ >= 2;
  return 0;
}
int32_t LoopbackAudioDevice::SetStereoRecording(bool) { return 0; }
int32_t LoopbackAudioDevice::StereoRecording(bool* enabled) const {
  *enabled = channels_ >= 2;
  return 0;
}
int32_t LoopbackAudioDevice::PlayoutDelay(uint16_t* delay_ms) const { *delay_ms = 0; return 0; }
bool LoopbackAudioDevice::BuiltInAECIsAvailable() const { return false; }
bool LoopbackAudioDevice::BuiltInAGCIsAvailable() const { return false; }
bool LoopbackAudioDevice::BuiltInNSIsAvailable() const { return false; }
int32_t LoopbackAudioDevice::EnableBuiltInAEC(bool) { return -1; }
int32_t LoopbackAudioDevice::EnableBuiltInAGC(bool) { return -1; }
int32_t LoopbackAudioDevice::EnableBuiltInNS(bool) { return -1; }

}  // namespace glsplay
