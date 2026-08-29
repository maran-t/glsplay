// Binary input wire format - C++ mirror of packages/protocol/src/input.ts.
//
// These two files are one contract. Change one, change the other. The
// static_asserts at the bottom are what stop a silent field-offset drift from
// becoming "the mouse moves diagonally and nobody knows why".
//
// Everything is little-endian, the only byte order both x86 hosts and every
// browser we target actually use. No ntoh conversion is performed anywhere.

#pragma once

#include <cstddef>
#include <cstdint>

namespace glsplay {

inline constexpr std::size_t kInputHeaderSize = 8;
inline constexpr std::size_t kMaxEventSize = 24;

enum class InputType : std::uint8_t {
  kMouseMoveRelative = 0x01,
  kMouseButton = 0x02,
  kMouseWheel = 0x03,
  kKey = 0x04,
  kGamepad = 0x05,
  kPing = 0x06,
  kMouseMoveAbsolute = 0x07,
  kGamepadConnection = 0x08,
};

enum InputFlags : std::uint8_t {
  kFlagExtended = 1u << 0,  // key needs the 0xE0 scan-code prefix
};

enum MouseButtonId : std::uint8_t {
  kMouseLeft = 0,
  kMouseRight = 1,
  kMouseMiddle = 2,
  kMouseBack = 3,
  kMouseForward = 4,
};

inline constexpr std::int16_t kWheelDelta = 120;

#pragma pack(push, 1)

struct InputHeader {
  std::uint8_t type;
  std::uint8_t flags;
  std::uint16_t seq;
  std::uint32_t t_client;  // client clock in ms; wraps roughly every 49 days
};

struct MouseMoveRelativeEvent {
  InputHeader header;
  std::int16_t dx;
  std::int16_t dy;
};

struct MouseMoveAbsoluteEvent {
  InputHeader header;
  std::int16_t x;  // normalised 0..32767 across the virtual desktop
  std::int16_t y;
};

struct MouseButtonEvent {
  InputHeader header;
  std::uint8_t button;
  std::uint8_t down;
};

struct MouseWheelEvent {
  InputHeader header;
  std::int16_t vertical;  // multiples of kWheelDelta
  std::int16_t horizontal;
};

struct KeyEvent {
  InputHeader header;
  std::uint16_t scancode;  // PS/2 set 1, low byte only; see kFlagExtended
  std::uint8_t down;
  std::uint8_t reserved;
};

struct GamepadEvent {
  InputHeader header;
  std::uint8_t index;
  std::uint8_t reserved;
  std::uint16_t buttons;  // XINPUT_GAMEPAD_* bitmask, forwarded to ViGEm as-is
  std::uint8_t left_trigger;
  std::uint8_t right_trigger;
  std::int16_t lx;
  std::int16_t ly;
  std::int16_t rx;
  std::int16_t ry;
  // The fields above total 22 bytes, but INPUT_SIZES in input.ts declares 24
  // and the encoder advances by that much. Without this tail padding the
  // parser would step 22 and desync from the second gamepad event onward.
  std::uint16_t reserved2;
};

struct GamepadConnectionEvent {
  InputHeader header;
  std::uint8_t index;
  std::uint8_t connected;
};

struct PingEvent {
  InputHeader header;
};

#pragma pack(pop)

// Wire size for a type, or 0 if unknown. The parser uses this to walk a
// coalesced batch without trusting any length field from the network.
constexpr std::size_t InputEventSize(std::uint8_t type) {
  switch (static_cast<InputType>(type)) {
    case InputType::kMouseMoveRelative: return sizeof(MouseMoveRelativeEvent);
    case InputType::kMouseButton:       return sizeof(MouseButtonEvent);
    case InputType::kMouseWheel:        return sizeof(MouseWheelEvent);
    case InputType::kKey:               return sizeof(KeyEvent);
    case InputType::kGamepad:           return sizeof(GamepadEvent);
    case InputType::kPing:              return sizeof(PingEvent);
    case InputType::kMouseMoveAbsolute: return sizeof(MouseMoveAbsoluteEvent);
    case InputType::kGamepadConnection: return sizeof(GamepadConnectionEvent);
    default:                            return 0;
  }
}

// Sizes must match INPUT_SIZES in input.ts exactly.
static_assert(sizeof(InputHeader) == kInputHeaderSize, "header size drift");
static_assert(sizeof(MouseMoveRelativeEvent) == 12, "size drift vs input.ts");
static_assert(sizeof(MouseMoveAbsoluteEvent) == 12, "size drift vs input.ts");
static_assert(sizeof(MouseButtonEvent) == 10, "size drift vs input.ts");
static_assert(sizeof(MouseWheelEvent) == 12, "size drift vs input.ts");
static_assert(sizeof(KeyEvent) == 12, "size drift vs input.ts");
static_assert(sizeof(GamepadEvent) == 24, "size drift vs input.ts");
static_assert(sizeof(GamepadConnectionEvent) == 10, "size drift vs input.ts");
static_assert(sizeof(PingEvent) == 8, "size drift vs input.ts");
static_assert(sizeof(GamepadEvent) == kMaxEventSize, "kMaxEventSize is stale");

// Offsets the TypeScript encoder hard-codes when writing payload fields.
static_assert(offsetof(GamepadEvent, buttons) == 10, "offset drift vs input.ts");
static_assert(offsetof(GamepadEvent, left_trigger) == 12, "offset drift vs input.ts");
static_assert(offsetof(GamepadEvent, lx) == 14, "offset drift vs input.ts");
static_assert(offsetof(KeyEvent, down) == 10, "offset drift vs input.ts");

}  // namespace glsplay
