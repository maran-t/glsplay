#include "input/win32_input_injector.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <vector>

#include "glsplay_input.h"
#include "util/log.h"

namespace glsplay {
namespace {

// Maps our wire button id to the SendInput down/up flag pair, plus the mouseData
// value that X buttons require.
struct ButtonFlags {
  DWORD down;
  DWORD up;
  DWORD data;
};

bool ButtonFlagsFor(uint8_t button, ButtonFlags* out) {
  switch (button) {
    case kMouseLeft:    *out = {MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP,   0}; return true;
    case kMouseRight:   *out = {MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP,  0}; return true;
    case kMouseMiddle:  *out = {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0}; return true;
    case kMouseBack:    *out = {MOUSEEVENTF_XDOWN,      MOUSEEVENTF_XUP, XBUTTON1}; return true;
    case kMouseForward: *out = {MOUSEEVENTF_XDOWN,      MOUSEEVENTF_XUP, XBUTTON2}; return true;
    default: return false;
  }
}

void Send(INPUT* inputs, UINT count) {
  const UINT sent = SendInput(count, inputs, sizeof(INPUT));
  if (sent != count) {
    // The usual cause is a higher-integrity window in the foreground - a UAC
    // prompt or the lock screen - which blocks injection from a normal
    // process. Nothing to do but report it.
    LOG_WARN << "SendInput delivered " << sent << " of " << count
             << ": " << LastErrorToString();
  }
}

}  // namespace

Win32InputInjector::Win32InputInjector(bool mouse_enabled, bool keyboard_enabled)
    : mouse_enabled_(mouse_enabled), keyboard_enabled_(keyboard_enabled) {
  LOG_INFO << "input injector: mouse=" << (mouse_enabled ? "on" : "off")
           << " keyboard=" << (keyboard_enabled ? "on" : "off");

  if (mouse_enabled_) {
    // A relative SendInput passes through the pointer-acceleration curve and
    // the speed slider, so a client delta does NOT map 1:1 - fast moves
    // overshoot, slow ones round away. Turn both off for the session so the
    // delta injects verbatim, and restore on shutdown.
    if (SystemParametersInfoW(SPI_GETMOUSE, 0, saved_mouse_, 0) &&
        SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &saved_speed_, 0)) {
      mouse_prefs_saved_ = true;
      int no_accel[3] = {0, 0, 0};
      SystemParametersInfoW(SPI_SETMOUSE, 0, no_accel, 0);
      SystemParametersInfoW(SPI_SETMOUSESPEED, 0,
                            reinterpret_cast<void*>(static_cast<INT_PTR>(10)), 0);
      LOG_INFO << "mouse acceleration disabled for the session (was accel="
               << saved_mouse_[2] << " speed=" << saved_speed_ << ')';
    }
  }
}

Win32InputInjector::~Win32InputInjector() {
  ReleaseAll();
  if (mouse_prefs_saved_) {
    SystemParametersInfoW(SPI_SETMOUSE, 0, saved_mouse_, 0);
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0,
                          reinterpret_cast<void*>(static_cast<INT_PTR>(saved_speed_)), 0);
  }
}

void Win32InputInjector::SetCaptureBounds(int left, int top, int width, int height) {
  bounds_left_.store(left, std::memory_order_relaxed);
  bounds_top_.store(top, std::memory_order_relaxed);
  bounds_width_.store(width, std::memory_order_relaxed);
  bounds_height_.store(height, std::memory_order_relaxed);
}

void Win32InputInjector::MouseMoveRelative(int16_t dx, int16_t dy) {
  if (!mouse_enabled_) return;
  if (dx == 0 && dy == 0) return;

  // Plain relative SendInput. With acceleration and the speed slider disabled
  // in the constructor this is a verbatim 1:1 move, and unlike SetCursorPos it
  // registers as real mouse activity - so Windows keeps the pointer shown and
  // Desktop Duplication keeps reporting it, which is what the compositor draws.
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = dx;
  input.mi.dy = dy;
  input.mi.dwFlags = MOUSEEVENTF_MOVE;
  Send(&input, 1);

  // Relative motion has no bound of its own; nudge it back onto the captured
  // monitor so a long sweep can't park the pointer on the L4 phantom head.
  const int left = bounds_left_.load(std::memory_order_relaxed);
  const int top = bounds_top_.load(std::memory_order_relaxed);
  const int width = bounds_width_.load(std::memory_order_relaxed);
  const int height = bounds_height_.load(std::memory_order_relaxed);
  if (width <= 0 || height <= 0) return;

  POINT p{};
  if (!GetCursorPos(&p)) return;
  const LONG cx = std::clamp<LONG>(p.x, left, left + width - 1);
  const LONG cy = std::clamp<LONG>(p.y, top, top + height - 1);
  if (cx != p.x || cy != p.y) SetCursorPos(cx, cy);
}

void Win32InputInjector::MouseMoveAbsolute(int16_t x, int16_t y) {
  if (!mouse_enabled_) return;

  INPUT input{};
  input.type = INPUT_MOUSE;
  // The absolute coordinate space is always 0..65535 across the virtual
  // desktop. The wire format carries 0..32767 to fit a signed 16-bit field,
  // so it is scaled back up here.
  input.mi.dx = static_cast<LONG>(x) * 2;
  input.mi.dy = static_cast<LONG>(y) * 2;
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  Send(&input, 1);
}

void Win32InputInjector::MouseButton(uint8_t button, bool down) {
  if (!mouse_enabled_) return;

  ButtonFlags flags{};
  if (!ButtonFlagsFor(button, &flags)) {
    LOG_DEBUG << "unknown mouse button " << static_cast<int>(button);
    return;
  }

  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (down) held_buttons_.insert(button);
    else held_buttons_.erase(button);
  }

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = down ? flags.down : flags.up;
  input.mi.mouseData = flags.data;
  Send(&input, 1);
}

void Win32InputInjector::MouseWheel(int16_t vertical, int16_t horizontal) {
  if (!mouse_enabled_) return;

  std::array<INPUT, 2> inputs{};
  UINT count = 0;

  if (vertical != 0) {
    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dwFlags = MOUSEEVENTF_WHEEL;
    inputs[count].mi.mouseData = static_cast<DWORD>(vertical);
    ++count;
  }
  if (horizontal != 0) {
    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dwFlags = MOUSEEVENTF_HWHEEL;
    inputs[count].mi.mouseData = static_cast<DWORD>(horizontal);
    ++count;
  }
  if (count > 0) Send(inputs.data(), count);
}

void Win32InputInjector::Key(uint16_t scancode, bool down, bool extended) {
  if (!keyboard_enabled_) return;

  const uint32_t token = (static_cast<uint32_t>(extended) << 16) | scancode;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (down) held_keys_.insert(token);
    else held_keys_.erase(token);
  }

  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = 0;
  input.ki.wScan = scancode;
  // KEYEVENTF_SCANCODE is the whole point (PRD section 4.4B): DirectInput
  // titles read the hardware scan code and never see a virtual key, so
  // injecting a VK would work in Notepad and do nothing in a game.
  input.ki.dwFlags = KEYEVENTF_SCANCODE;
  if (extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
  Send(&input, 1);
}

void Win32InputInjector::ReleaseAll() {
  std::vector<uint32_t> keys;
  std::vector<uint8_t> buttons;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    keys.assign(held_keys_.begin(), held_keys_.end());
    buttons.assign(held_buttons_.begin(), held_buttons_.end());
    held_keys_.clear();
    held_buttons_.clear();
  }
  if (keys.empty() && buttons.empty()) return;

  LOG_DEBUG << "releasing " << keys.size() << " key(s) and " << buttons.size()
            << " button(s)";

  std::vector<INPUT> inputs;
  inputs.reserve(keys.size() + buttons.size());

  for (const uint32_t token : keys) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(token & 0xFFFF);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    if ((token >> 16) != 0) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    inputs.push_back(input);
  }

  for (const uint8_t button : buttons) {
    ButtonFlags flags{};
    if (!ButtonFlagsFor(button, &flags)) continue;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags.up;
    input.mi.mouseData = flags.data;
    inputs.push_back(input);
  }

  if (!inputs.empty()) Send(inputs.data(), static_cast<UINT>(inputs.size()));
}

}  // namespace glsplay
