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

  // Rect of the monitor being captured, in virtual-desktop pixels. Relative
  // mouse moves are integrated against the live cursor position and clamped to
  // this, so a fast flick can never fling the pointer onto another head or off
  // the desktop. Called once at startup, before any input arrives.
  void SetCaptureBounds(int left, int top, int width, int height);

  // dx and dy are raw deltas from the browser's Pointer Lock movementX/Y.
  void MouseMoveRelative(int16_t dx, int16_t dy);

  // x and y are normalised 0..32767 across the virtual desktop.
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

  // Tracked so ReleaseAll knows what to lift. Guarded because input arrives on
  // the libwebrtc network thread while ReleaseAll can fire from the signaling
  // thread on disconnect.
  std::mutex mutex_;
  std::unordered_set<uint32_t> held_keys_;
  std::unordered_set<uint8_t> held_buttons_;
};

}  // namespace glsplay
