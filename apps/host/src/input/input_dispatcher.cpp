#include "input/input_dispatcher.h"

#include <chrono>
#include <cstring>

#include "glsplay_input.h"
#include "util/log.h"

namespace glsplay {
namespace {

// Reads a packed struct out of the buffer. The caller has already checked that
// sizeof(T) bytes remain, and the wire layout is little-endian on both sides,
// so a memcpy is both correct and the only way to avoid an unaligned load.
template <typename T>
T Read(const uint8_t* at) {
  T value{};
  std::memcpy(&value, at, sizeof(T));
  return value;
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

InputDispatcher::InputDispatcher(std::unique_ptr<Win32InputInjector> injector,
                                 std::unique_ptr<IGamepadSink> gamepad)
    : injector_(std::move(injector)), gamepad_(std::move(gamepad)) {}

InputDispatcher::~InputDispatcher() {
  ReleaseAll();
}

void InputDispatcher::UpdateQueueLatency(uint32_t t_client) {
  const int64_t now = NowMillis();
  const int64_t offset = now - static_cast<int64_t>(t_client);

  if (!clock_primed_.load(std::memory_order_relaxed)) {
    clock_offset_.store(offset, std::memory_order_relaxed);
    clock_primed_.store(true, std::memory_order_relaxed);
    return;
  }

  // A smaller offset means this event arrived faster than anything before it,
  // so it becomes the new zero point. This tracks clock drift downward but
  // never upward, which is what we want - the baseline should follow the
  // best-case path, not creep along with a congested one.
  int64_t baseline = clock_offset_.load(std::memory_order_relaxed);
  if (offset < baseline) {
    clock_offset_.store(offset, std::memory_order_relaxed);
    baseline = offset;
  }

  const double queue_ms = static_cast<double>(offset - baseline);
  // Exponential moving average; one late packet should not dominate the HUD.
  const double previous = mean_queue_ms_.load(std::memory_order_relaxed);
  mean_queue_ms_.store(previous * 0.95 + queue_ms * 0.05, std::memory_order_relaxed);
}

void InputDispatcher::HandleMessage(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return;
  batches_.fetch_add(1, std::memory_order_relaxed);

  size_t offset = 0;
  while (offset + kInputHeaderSize <= size) {
    const auto header = Read<InputHeader>(data + offset);
    const size_t event_size = InputEventSize(header.type);

    if (event_size == 0) {
      // Unknown type. The stride is unknown too, so the rest of the batch
      // cannot be walked safely - stop rather than guess.
      unknown_type_.fetch_add(1, std::memory_order_relaxed);
      LOG_DEBUG << "unknown input type 0x" << std::hex << static_cast<int>(header.type);
      return;
    }
    if (offset + event_size > size) {
      malformed_.fetch_add(1, std::memory_order_relaxed);
      LOG_DEBUG << "truncated input event: need " << event_size << " have "
                << (size - offset);
      return;
    }

    UpdateQueueLatency(header.t_client);
    events_.fetch_add(1, std::memory_order_relaxed);

    const uint8_t* at = data + offset;
    switch (static_cast<InputType>(header.type)) {
      case InputType::kMouseMoveRelative: {
        const auto e = Read<MouseMoveRelativeEvent>(at);
        injector_->MouseMoveRelative(e.dx, e.dy);
        break;
      }
      case InputType::kMouseMoveAbsolute: {
        const auto e = Read<MouseMoveAbsoluteEvent>(at);
        injector_->MouseMoveAbsolute(e.x, e.y);
        break;
      }
      case InputType::kMouseButton: {
        const auto e = Read<MouseButtonEvent>(at);
        injector_->MouseButton(e.button, e.down != 0);
        break;
      }
      case InputType::kMouseWheel: {
        const auto e = Read<MouseWheelEvent>(at);
        injector_->MouseWheel(e.vertical, e.horizontal);
        break;
      }
      case InputType::kKey: {
        const auto e = Read<KeyEvent>(at);
        injector_->Key(e.scancode, e.down != 0, (header.flags & kFlagExtended) != 0);
        break;
      }
      case InputType::kGamepad: {
        const auto e = Read<GamepadEvent>(at);
        GamepadState state;
        state.buttons = e.buttons;
        state.left_trigger = e.left_trigger;
        state.right_trigger = e.right_trigger;
        state.lx = e.lx;
        state.ly = e.ly;
        state.rx = e.rx;
        state.ry = e.ry;
        gamepad_->Submit(e.index, state);
        break;
      }
      case InputType::kGamepadConnection: {
        const auto e = Read<GamepadConnectionEvent>(at);
        if (e.connected != 0) gamepad_->Connect(e.index);
        else gamepad_->Disconnect(e.index);
        break;
      }
      case InputType::kPing:
        // Timestamp already folded into the latency estimate above.
        break;
    }

    offset += event_size;
  }

  if (offset != size) {
    malformed_.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG << "input batch had " << (size - offset) << " trailing bytes";
  }
}

void InputDispatcher::SetUltraMode(bool enabled) {
  if (injector_) injector_->SetUltraMode(enabled);
}

void InputDispatcher::ReleaseAll() {
  if (injector_) {
    injector_->ReleaseAll();
    // A client that dropped mid-game would otherwise leave the next session's
    // desktop pointer unclamped until it happened to toggle the mode itself.
    injector_->SetUltraMode(false);
  }
  if (gamepad_) gamepad_->ReleaseAll();
}

InputStats InputDispatcher::stats() const {
  InputStats out;
  out.events = events_.load(std::memory_order_relaxed);
  out.batches = batches_.load(std::memory_order_relaxed);
  out.malformed = malformed_.load(std::memory_order_relaxed);
  out.unknown_type = unknown_type_.load(std::memory_order_relaxed);
  out.mean_queue_ms = mean_queue_ms_.load(std::memory_order_relaxed);
  return out;
}

}  // namespace glsplay
