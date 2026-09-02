// libwebrtc video source backed by DXGI Desktop Duplication.
//
// Runs its own capture thread rather than being driven by libwebrtc, because
// Desktop Duplication is a blocking pull API - AcquireNextFrame waits until
// the desktop actually changes. Frames are handed to libwebrtc as native
// D3D11FrameBuffers so they stay in VRAM all the way to NVENC.
//
// This implements VideoTrackSourceInterface directly rather than deriving from
// AdaptedVideoTrackSource. That base class lives under media/, which prebuilt
// libwebrtc packages do not always ship, and the only feature we would use is
// resolution adaptation - which we cannot honour anyway, since rescaling a
// D3D11 texture on the capture thread would defeat the zero-copy path.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

#include "capture/dxgi_duplicator.h"

namespace glsplay {

struct CaptureStats {
  double captured_fps = 0.0;
  double mean_capture_ms = 0.0;
  uint64_t frames = 0;
  uint64_t dropped = 0;   // captured but not sent, to hold the outbound rate at
                          // target_fps when the display presents faster
  uint64_t repeats = 0;   // idle frames resent because nothing changed
  uint64_t reinits = 0;   // recoveries from DXGI_ERROR_ACCESS_LOST
};

class DesktopCaptureSource : public webrtc::VideoTrackSourceInterface {
 public:
  DesktopCaptureSource();
  ~DesktopCaptureSource() override;

  // adapter_index of -1 picks the first NVIDIA adapter with an attached output.
  bool Start(int adapter_index, int output_index, int target_fps);
  void Stop();

  // The device NVENC must encode on. Valid only after a successful Start().
  Microsoft::WRL::ComPtr<ID3D11Device> device() const;

  int width() const { return duplicator_ ? duplicator_->width() : 0; }
  int height() const { return duplicator_ ? duplicator_->height() : 0; }
  int desktop_left() const { return duplicator_ ? duplicator_->desktop_left() : 0; }
  int desktop_top() const { return duplicator_ ? duplicator_->desktop_top() : 0; }
  std::string adapter_description() const {
    return duplicator_ ? duplicator_->adapter_description() : std::string();
  }

  CaptureStats stats() const;

  // A thread-safe copy of the latest pointer state. The cursor is not drawn
  // into the video; callers ship this over the control channel so the client
  // can render it locally.
  CursorState cursor_snapshot() const {
    std::lock_guard<std::mutex> guard(cursor_mutex_);
    return cursor_state_;
  }

  // --- MediaSourceInterface ----------------------------------------------
  webrtc::MediaSourceInterface::SourceState state() const override { return state_; }
  bool remote() const override { return false; }
  void RegisterObserver(webrtc::ObserverInterface* observer) override;
  void UnregisterObserver(webrtc::ObserverInterface* observer) override;

  // --- VideoSourceInterface<VideoFrame> ----------------------------------
  void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const webrtc::VideoSinkWants& wants) override;
  void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override;

  // --- VideoTrackSourceInterface -----------------------------------------
  bool is_screencast() const override { return true; }
  // Games are full-motion; letting libwebrtc treat this as static screen
  // content would make it drop the frame rate to save bandwidth.
  std::optional<bool> needs_denoising() const override { return false; }
  bool GetStats(Stats* stats) override;
  bool SupportsEncodedOutput() const override { return false; }
  void GenerateKeyFrame() override {}
  void AddEncodedSink(
      webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*) override {}
  void RemoveEncodedSink(
      webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>*) override {}

 private:
  void CaptureLoop(int target_fps);
  // Fans a frame out to every attached sink.
  void Broadcast(const webrtc::VideoFrame& frame);
  void NotifyObservers();

  std::unique_ptr<DxgiDuplicator> duplicator_;
  std::thread thread_;

  // The cursor is NOT drawn into the video (RDP / Parsec / Moonlight model). It
  // is snapshotted here and shipped over the control channel for the client to
  // render locally - zero round-trip lag, no encoder ghosting, and it tracks
  // the client refresh rate rather than the capture rate.
  //
  // PointerPosition.Visible flaps false briefly on this headless VDD even mid
  // use; keep reporting the last real cursor for a short grace period so the
  // client-side pointer doesn't strobe.
  CursorState sticky_cursor_{};
  int cursor_hide_frames_ = 0;

  mutable std::mutex cursor_mutex_;
  CursorState cursor_state_;
  std::atomic<bool> running_{false};
  webrtc::MediaSourceInterface::SourceState state_ =
      webrtc::MediaSourceInterface::kInitializing;

  // In practice there is exactly one sink - the encoder path - but the
  // interface permits several, and a plain vector under a mutex is cheaper
  // than pulling VideoBroadcaster in from media/.
  mutable std::mutex sink_mutex_;
  std::vector<webrtc::VideoSinkInterface<webrtc::VideoFrame>*> sinks_;

  mutable std::mutex observer_mutex_;
  std::vector<webrtc::ObserverInterface*> observers_;

  std::atomic<double> captured_fps_{0.0};
  std::atomic<double> mean_capture_ms_{0.0};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> repeats_{0};
  std::atomic<uint64_t> reinits_{0};
};

}  // namespace glsplay
