#include "input/win32_input_injector.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
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
      SystemParametersInfoW(SPI_SETMOUSE, 0, no_accel, SPIF_SENDCHANGE);
      SystemParametersInfoW(SPI_SETMOUSESPEED, 0,
                            reinterpret_cast<void*>(static_cast<INT_PTR>(10)),
                            SPIF_SENDCHANGE);
      int now_accel[3] = {0, 0, 0};
      int now_speed = 0;
      SystemParametersInfoW(SPI_GETMOUSE, 0, now_accel, 0);
      SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &now_speed, 0);
      LOG_INFO << "mouse ballistics: was accel=" << saved_mouse_[2]
               << " speed=" << saved_speed_ << ", now accel=" << now_accel[2]
               << " speed=" << now_speed;
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

void Win32InputInjector::SetUltraMode(bool enabled) {
  if (ultra_mode_.exchange(enabled, std::memory_order_relaxed) == enabled) return;
  LOG_INFO << "ultra mode " << (enabled ? "on - relative deltas injected verbatim"
                                        : "off - relative deltas clamped to the capture");
}

void Win32InputInjector::MouseMoveRelative(int16_t dx, int16_t dy) {
  if (!mouse_enabled_) return;
  if (dx == 0 && dy == 0) return;

  auto inject = [](int mx, int my) {
    if (mx == 0 && my == 0) return;
    // Relative SendInput. With acceleration and the speed slider disabled in the
    // constructor this is a verbatim 1:1 move, and unlike SetCursorPos it
    // registers as real mouse activity - so Windows keeps the pointer shown and
    // Desktop Duplication keeps reporting it.
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = mx;
    input.mi.dy = my;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    Send(&input, 1);
  };

  // Ultra mode: verbatim, never clamped. See SetUltraMode for why the clamp is
  // fatal to mouselook. Everything below this point is the desktop path and is
  // untouched by the toggle.
  const bool ultra = ultra_mode_.load(std::memory_order_relaxed);
  if (ultra != was_ultra_) {
    was_ultra_ = ultra;
    // Coming back out, pos_ is stale by the whole in-game sweep. Drop it so the
    // block below re-anchors from GetCursorPos on this very event rather than
    // clamping against a value that has not tracked anything for minutes.
    if (!ultra) have_pos_ = false;
  }
  if (ultra) {
    inject(dx, dy);
    return;
  }

  const int left = bounds_left_.load(std::memory_order_relaxed);
  const int top = bounds_top_.load(std::memory_order_relaxed);
  const int width = bounds_width_.load(std::memory_order_relaxed);
  const int height = bounds_height_.load(std::memory_order_relaxed);

  // No captured-monitor rect yet: fall back to a plain verbatim relative move.
  if (width <= 0 || height <= 0) {
    inject(dx, dy);
    return;
  }

  // Re-anchor to the real pointer out of band - on first use, every
  // kReanchorEvery events, or whenever it has diverged far enough that
  // something else (a game recentre, another input path) must have moved it.
  // Reading GetCursorPos here rather than straight after a SendInput avoids
  // racing the OS as it applies the injected move.
  if (!have_pos_ || --reanchor_countdown_ <= 0) {
    POINT p{};
    if (GetCursorPos(&p)) {
      if (!have_pos_ || std::llabs(p.x - pos_x_) > kReanchorDivergePx ||
          std::llabs(p.y - pos_y_) > kReanchorDivergePx) {
        pos_x_ = p.x;
        pos_y_ = p.y;
      }
      have_pos_ = true;
    }
    reanchor_countdown_ = kReanchorEvery;
  }

  // Clamp the target against our believed position *before* injecting, so the
  // pointer never crosses the edge - no stale-read decision, no absolute snap.
  const int64_t min_x = left;
  const int64_t max_x = static_cast<int64_t>(left) + width - 1;
  const int64_t min_y = top;
  const int64_t max_y = static_cast<int64_t>(top) + height - 1;

  const int64_t want_x = std::clamp<int64_t>(pos_x_ + dx, min_x, max_x);
  const int64_t want_y = std::clamp<int64_t>(pos_y_ + dy, min_y, max_y);

  const int inject_dx = static_cast<int>(want_x - pos_x_);
  const int inject_dy = static_cast<int>(want_y - pos_y_);
  pos_x_ = want_x;
  pos_y_ = want_y;

  inject(inject_dx, inject_dy);
}

void Win32InputInjector::MouseMoveAbsolute(int16_t x, int16_t y) {
  if (!mouse_enabled_) return;

  // Wire x/y are 0..32767 normalised *within the captured output* - the client
  // maps its pointer into the letterboxed video, not the whole virtual desktop.
  // Turn that into a pixel on the captured monitor, then into the 0..65535
  // virtual-desktop space SendInput's ABSOLUTE|VIRTUALDESK mode expects.
  const double nx = std::clamp(static_cast<double>(x) / 32767.0, 0.0, 1.0);
  const double ny = std::clamp(static_cast<double>(y) / 32767.0, 0.0, 1.0);

  const int vs_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vs_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vs_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vs_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (vs_w <= 1 || vs_h <= 1) return;

  const int bl = bounds_left_.load(std::memory_order_relaxed);
  const int bt = bounds_top_.load(std::memory_order_relaxed);
  const int bw = bounds_width_.load(std::memory_order_relaxed);
  const int bh = bounds_height_.load(std::memory_order_relaxed);

  // Fall back to the whole virtual desktop if the capture bounds were never set.
  double px;
  double py;
  if (bw > 0 && bh > 0) {
    px = bl + nx * (bw - 1);
    py = bt + ny * (bh - 1);
  } else {
    px = vs_left + nx * (vs_w - 1);
    py = vs_top + ny * (vs_h - 1);
  }

  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = static_cast<LONG>((px - vs_left) * 65535.0 / (vs_w - 1) + 0.5);
  input.mi.dy = static_cast<LONG>((py - vs_top) * 65535.0 / (vs_h - 1) + 0.5);
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
