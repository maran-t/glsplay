// Mouse and keyboard injection via Win32 SendInput (PRD section 4.4 A and B).
//
// No third-party dependency - this is entirely Windows SDK.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace glsplay {

class Win32InputInjector {
 public:
  Win32InputInjector(bool mouse_enabled, bool keyboard_enabled);
  ~Win32InputInjector();

  // Rect of the monitor being captured, in virtual-desktop pixels. After a
  // relative move the pointer is nudged back inside this so a fast sweep can
  // never leave it on another head. Called once at startup, before any input.
  void SetCaptureBounds(int left, int top, int width, int height);

  // Ultra mode, toggled by the client's set-ultra-mode control message when the
  // user starts playing. While on, relative deltas are injected verbatim
  // instead of being clamped to the captured monitor.
  //
  // The clamp exists so the desktop pointer cannot wander onto a head the user
  // cannot see, and for desktop use it is right. For mouselook it is fatal: at
  // a screen edge the clamped delta is zero, the zero is filtered out before
  // SendInput is ever called, and so nothing reaches Windows' raw input stream
  // at all - a game reading WM_INPUT stops turning about one screen-width into
  // a sweep, even though the motion would have been delivered regardless of
  // where the cursor sat.
  //
  // Deliberately a manual toggle rather than something derived from Pointer
  // Lock. Control messages travel on a different DataChannel from input, and
  // SCTP orders nothing between two streams; a mode that flipped on every lock
  // transition raced the deltas it applied to. Flipped twice a session by a
  // button press, while the mouse is not being swept, that race has no window
  // that matters.
  void SetUltraMode(bool enabled);

  // dx and dy are raw deltas from the browser's Pointer Lock movementX/Y.
  void MouseMoveRelative(int16_t dx, int16_t dy);

  // x and y are normalised 0..32767 within the captured output (0 = left/top
  // edge, 32767 = right/bottom edge of the monitor being streamed).
  void MouseMoveAbsolute(int16_t x, int16_t y);

  void MouseButton(uint8_t button, bool down);
  void MouseWheel(int16_t vertical, int16_t horizontal);

  // scancode is PS/2 set 1; extended selects the 0xE0 prefix range.
  void Key(uint16_t scancode, bool down, bool extended);

  // Releases every key and button currently held. Called when the peer drops,
  // so a key held at disconnect does not stay down on the remote desktop
  // forever - which on a game means the character keeps walking.
  void ReleaseAll();

 private:
  const bool mouse_enabled_;
  const bool keyboard_enabled_;

  // Captured-monitor rect, set once before input starts, then read-only.
  std::atomic<int32_t> bounds_left_{0};
  std::atomic<int32_t> bounds_top_{0};
  std::atomic<int32_t> bounds_width_{0};   // 0 until SetCaptureBounds: fall
  std::atomic<int32_t> bounds_height_{0};  // back to the whole virtual desktop

  // Where we believe the pointer is, in virtual-desktop pixels, integrated from
  // the deltas we inject. Relative moves are clamped against this *before*
  // injection, so the pointer never crosses the captured-monitor edge and no
  // absolute SetCursorPos snap is needed in the hot path. Re-anchored from
  // GetCursorPos out of band (not right after an async SendInput, which races
  // the OS applying the move) every kReanchorEvery events, or when it has
  // diverged far enough that something else must have moved the pointer.
  // Only MouseMoveRelative touches these, and all input arrives on the one
  // libwebrtc network thread, so no lock is needed.
  static constexpr int kReanchorEvery = 30;
  static constexpr int64_t kReanchorDivergePx = 24;
  bool have_pos_ = false;
  int64_t pos_x_ = 0;
  int64_t pos_y_ = 0;
  int reanchor_countdown_ = 0;

  // Set from any thread (the control message arrives on the network thread,
  // ReleaseAll can come from the signaling thread on disconnect), so atomic.
  std::atomic<bool> ultra_mode_{false};
  // Read only by MouseMoveRelative, to spot the moment ultra mode turns off:
  // pos_ tracks nothing while deltas bypass the clamped path, so it is stale by
  // however far the pointer travelled in game and must be re-anchored once
  // before it is trusted again.
  bool was_ultra_ = false;

  // Windows mouse settings saved at construction and restored on destruction:
  // acceleration curve (SPI_*MOUSE, 3 ints) and pointer speed (SPI_*MOUSESPEED).
  // Zeroed / set to 1:1 while we run so a client delta injects verbatim.
  int saved_mouse_[3] = {0, 0, 0};
  int saved_speed_ = 10;
  bool mouse_prefs_saved_ = false;

  // Tracked so ReleaseAll knows what to lift. Guarded because input arrives on
  // the libwebrtc network thread while ReleaseAll can fire from the signaling
  // thread on disconnect.
  std::mutex mutex_;
  std::unordered_set<uint32_t> held_keys_;
  std::unordered_set<uint8_t> held_buttons_;
};

}  // namespace glsplay
