// Gamepad injection interface.
//
// ViGEmBus is the only workable way to present a virtual XInput controller on
// Windows - Microsoft exposes XInput as read-only, and a VHF-based HID gamepad
// is invisible to XInput titles. It is also the one community-maintained
// dependency in the stack, so it sits behind this interface: the host builds
// and runs with NullGamepadSink whether or not the driver is installed, and
// PRD phases 1-4 need no gamepad at all.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace glsplay {

struct GamepadState {
  uint16_t buttons = 0;  // XINPUT_GAMEPAD_* bitmask, already in the wire format
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t lx = 0;
  int16_t ly = 0;
  int16_t rx = 0;
  int16_t ry = 0;
};

class IGamepadSink {
 public:
  virtual ~IGamepadSink() = default;

  virtual bool Initialise() = 0;

  // Creates or destroys a virtual pad. The client sends these when a physical
  // controller is connected or removed in the browser.
  virtual bool Connect(uint8_t index) = 0;
  virtual bool Disconnect(uint8_t index) = 0;

  virtual bool Submit(uint8_t index, const GamepadState& state) = 0;

  // Releases every pad. Called when the peer connection drops, so a controller
  // held at disconnect does not stay latched on the host.
  virtual void ReleaseAll() = 0;

  virtual bool available() const = 0;
  virtual std::string description() const = 0;
};

// Accepts and discards everything. Used when ViGEmBus is absent or gamepad
// support is switched off, so callers never need a null check.
class NullGamepadSink final : public IGamepadSink {
 public:
  bool Initialise() override { return true; }
  bool Connect(uint8_t) override { return false; }
  bool Disconnect(uint8_t) override { return false; }
  bool Submit(uint8_t, const GamepadState&) override { return false; }
  void ReleaseAll() override {}
  bool available() const override { return false; }
  std::string description() const override { return "none (ViGEmBus not in use)"; }
};

// Returns a ViGEm sink when the driver is present and enabled, otherwise a
// NullGamepadSink. Never returns null.
std::unique_ptr<IGamepadSink> CreateGamepadSink(bool enabled);

}  // namespace glsplay
