#include "capture/desktop_capture_source.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

#include "api/video/video_frame.h"
#include "rtc_base/time_utils.h"

#include "capture/d3d11_frame_buffer.h"
#include "util/log.h"

namespace glsplay {
namespace {

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

DesktopCaptureSource::DesktopCaptureSource() = default;

DesktopCaptureSource::~DesktopCaptureSource() {
  Stop();
}

bool DesktopCaptureSource::Start(int adapter_index, int output_index, int target_fps) {
  if (running_.load()) return true;

  // Log what is present before choosing, so a wrong-adapter problem is visible
  // in the first few lines of output rather than three layers down.
  const auto adapters = DxgiDuplicator::EnumerateAdapters();
  LOG_INFO << "DXGI adapters:";
  for (const auto& a : adapters) {
    LOG_INFO << "  [" << a.index << "] " << a.description
             << (a.is_nvidia ? "  (NVIDIA)" : "")
             << "  outputs=" << a.output_count
             << "  vram=" << (a.dedicated_vram / (1024 * 1024)) << "MB";
  }

  duplicator_ = std::make_unique<DxgiDuplicator>();
  if (!duplicator_->Initialise(adapter_index, output_index)) {
    duplicator_.reset();
    state_ = webrtc::MediaSourceInterface::kEnded;
    return false;
  }

  // The cursor is streamed as metadata over the control channel and drawn by
  // the client, so nothing is composited into the video here.

  running_.store(true);
  state_ = webrtc::MediaSourceInterface::kLive;
  NotifyObservers();
  thread_ = std::thread(&DesktopCaptureSource::CaptureLoop, this, target_fps);

  // Capture is the head of the pipeline; if it is starved by a background
  // thread the whole latency budget slips. Above normal, not time-critical -
  // starving the encoder thread would be worse.
  SetThreadPriority(thread_.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);

  LOG_INFO << "capture started: " << duplicator_->width() << 'x'
           << duplicator_->height() << " target " << target_fps << "fps";
  return true;
}

void DesktopCaptureSource::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  duplicator_.reset();
  state_ = webrtc::MediaSourceInterface::kEnded;
  NotifyObservers();
  LOG_INFO << "capture stopped after " << frames_.load() << " frames";
}

Microsoft::WRL::ComPtr<ID3D11Device> DesktopCaptureSource::device() const {
  if (!duplicator_) return nullptr;
  return Microsoft::WRL::ComPtr<ID3D11Device>(duplicator_->device());
}

void DesktopCaptureSource::CaptureLoop(int target_fps) {
  const auto frame_interval_us = std::chrono::microseconds(1000000 / std::max(1, target_fps));
  auto next_deadline = std::chrono::steady_clock::now();

  int64_t fps_window_start = NowUs();
  uint64_t fps_window_frames = 0;
  int consecutive_failures = 0;
  int recovery_attempts = 0;

  Microsoft::WRL::ComPtr<ID3D11Device> device(duplicator_->device());

  while (running_.load()) {
    const int64_t capture_start_us = NowUs();

    CapturedFrame captured;
    // A short timeout rather than blocking indefinitely, so a completely
    // static desktop still produces frames. WebRTC needs a steady cadence:
    // going silent makes the browser's jitter buffer grow, and the first
    // frame after the pause then arrives visibly late.
    const CaptureStatus status = duplicator_->AcquireFrame(
        static_cast<uint32_t>(frame_interval_us.count() / 1000), &captured);

    if (status == CaptureStatus::kAccessLost) {
      reinits_.fetch_add(1, std::memory_order_relaxed);
      if (!duplicator_->Reinitialise()) {
        // Do not give up. The usual cause is a secure desktop - a lock screen
        // or UAC prompt - which Windows forbids duplicating but which goes
        // away on its own. Exiting here leaves the host running with a dead
        // capture thread, connected to a browser it can never send a frame
        // to, which is the least useful possible failure mode.
        ++recovery_attempts;
        if (recovery_attempts == 1 || recovery_attempts % 20 == 0) {
          LOG_WARN << "duplication unavailable (attempt " << recovery_attempts
                   << ") - retrying. If this persists, the console session is "
                      "probably at the lock screen; enable auto-logon.";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      recovery_attempts = 0;
      LOG_INFO << "duplication recovered";
      device = Microsoft::WRL::ComPtr<ID3D11Device>(duplicator_->device());
      continue;
    }
    if (status == CaptureStatus::kFailed) {
      if (++consecutive_failures > 30) {
        // Rebuild rather than give up. Stopping here leaves the host connected
        // to a browser it can never send a frame to, and the only way back is
        // a restart; a rebuild costs a few milliseconds and usually works.
        LOG_WARN << "capture failed 30 times consecutively - rebuilding duplication";
        consecutive_failures = 0;
        if (duplicator_->Reinitialise()) {
          device = Microsoft::WRL::ComPtr<ID3D11Device>(duplicator_->device());
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
      }
      continue;
    }
    consecutive_failures = 0;

    if (status == CaptureStatus::kTimeout) {
      // Nothing changed on screen. The previous texture is still valid and is
      // resent so the cadence holds.
      repeats_.fetch_add(1, std::memory_order_relaxed);
    }

    if (captured.texture == nullptr) {
      std::this_thread::sleep_until(next_deadline);
      next_deadline += frame_interval_us;
      continue;
    }

    const int width = duplicator_->width();
    const int height = duplicator_->height();

    // Snapshot the pointer state, applying a short "keep the last one" grace
    // period so a momentary Visible=false does not blink the cursor out.
    CursorState cursor = duplicator_->cursor();
    if (cursor.visible) {
      sticky_cursor_ = cursor;
      cursor_hide_frames_ = 0;
    } else if (!sticky_cursor_.shape.empty() && cursor_hide_frames_ < 90) {
      // DXGI reports Visible=false in bursts on this headless VDD even while
      // the pointer is plainly in use, which would strobe the client's pointer.
      // Report the last good one for up to ~1.5s (the pointer is clamped to the
      // captured monitor, so it cannot really have left the frame).
      ++cursor_hide_frames_;
      cursor = sticky_cursor_;
      cursor.visible = true;
    }
    {
      std::lock_guard<std::mutex> guard(cursor_mutex_);
      cursor_state_ = cursor;
    }

    // The cursor is never blended into the frame; `captured.texture` is handed
    // to the encoder as a clean desktop image and the client draws the pointer.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture(captured.texture);
    auto buffer = D3D11FrameBuffer::Create(texture, device, width, height);

    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                   .set_video_frame_buffer(buffer)
                                   .set_timestamp_us(capture_start_us)
                                   .set_rotation(webrtc::kVideoRotation_0)
                                   .build();
    Broadcast(frame);

    const double capture_ms = static_cast<double>(NowUs() - capture_start_us) / 1000.0;
    const double previous = mean_capture_ms_.load(std::memory_order_relaxed);
    mean_capture_ms_.store(previous * 0.9 + capture_ms * 0.1, std::memory_order_relaxed);

    frames_.fetch_add(1, std::memory_order_relaxed);
    ++fps_window_frames;

    const int64_t now_us = NowUs();
    if (now_us - fps_window_start >= 1000000) {
      const double seconds = static_cast<double>(now_us - fps_window_start) / 1000000.0;
      captured_fps_.store(static_cast<double>(fps_window_frames) / seconds,
                          std::memory_order_relaxed);
      fps_window_frames = 0;
      fps_window_start = now_us;
    }

    // Pace to the target rate. If a frame took longer than its slot, reset the
    // deadline rather than trying to catch up - bunching frames together after
    // a stall makes the jitter worse, not better.
    next_deadline += frame_interval_us;
    const auto now = std::chrono::steady_clock::now();
    if (next_deadline < now) next_deadline = now;
    else std::this_thread::sleep_until(next_deadline);
  }

  running_.store(false);
  state_ = webrtc::MediaSourceInterface::kEnded;
  NotifyObservers();
}

void DesktopCaptureSource::Broadcast(const webrtc::VideoFrame& frame) {
  std::lock_guard<std::mutex> guard(sink_mutex_);
  for (auto* sink : sinks_) sink->OnFrame(frame);
}

void DesktopCaptureSource::AddOrUpdateSink(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
    const webrtc::VideoSinkWants& wants) {
  // VideoSinkWants carries resolution and frame-rate requests from congestion
  // control. We cannot rescale a D3D11 texture without breaking the zero-copy
  // path, so the requests are logged and ignored - NVENC keeps the native size
  // and adapts bitrate instead, which is the right trade for a game stream.
  (void)wants;
  std::lock_guard<std::mutex> guard(sink_mutex_);
  if (std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end()) {
    sinks_.push_back(sink);
    LOG_DEBUG << "video sink attached (" << sinks_.size() << " total)";
  }
}

void DesktopCaptureSource::RemoveSink(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) {
  std::lock_guard<std::mutex> guard(sink_mutex_);
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
  LOG_DEBUG << "video sink detached (" << sinks_.size() << " remaining)";
}

void DesktopCaptureSource::RegisterObserver(webrtc::ObserverInterface* observer) {
  std::lock_guard<std::mutex> guard(observer_mutex_);
  observers_.push_back(observer);
}

void DesktopCaptureSource::UnregisterObserver(webrtc::ObserverInterface* observer) {
  std::lock_guard<std::mutex> guard(observer_mutex_);
  observers_.erase(std::remove(observers_.begin(), observers_.end(), observer),
                   observers_.end());
}

void DesktopCaptureSource::NotifyObservers() {
  std::lock_guard<std::mutex> guard(observer_mutex_);
  for (auto* observer : observers_) observer->OnChanged();
}

bool DesktopCaptureSource::GetStats(Stats* stats) {
  if (stats == nullptr || !duplicator_) return false;
  stats->input_width = duplicator_->width();
  stats->input_height = duplicator_->height();
  return true;
}

CaptureStats DesktopCaptureSource::stats() const {
  CaptureStats out;
  out.captured_fps = captured_fps_.load(std::memory_order_relaxed);
  out.mean_capture_ms = mean_capture_ms_.load(std::memory_order_relaxed);
  out.frames = frames_.load(std::memory_order_relaxed);
  out.dropped = dropped_.load(std::memory_order_relaxed);
  out.repeats = repeats_.load(std::memory_order_relaxed);
  out.reinits = reinits_.load(std::memory_order_relaxed);
  return out;
}

}  // namespace glsplay
