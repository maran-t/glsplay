// ViGEm gamepad sink. Compiled only when ViGEmClient is available; see
// CMakeLists.txt and GLSPLAY_HAVE_VIGEM.
//
// ViGEmBus emulates a USB bus convincingly enough that Microsoft's own
// xusb22.sys binds to it, which is why XInput titles see the pad. A HID
// gamepad built on Microsoft's VHF would not be visible to XInput at all.

#include "input/gamepad_sink.h"

#include "util/log.h"

#ifdef GLSPLAY_HAVE_VIGEM

#include <windows.h>
#include <ViGEm/Client.h>

#include <array>
#include <mutex>

namespace glsplay {
namespace {

constexpr uint8_t kMaxPads = 4;

const char* ErrorName(VIGEM_ERROR error) {
  switch (error) {
    case VIGEM_ERROR_NONE: return "NONE";
    case VIGEM_ERROR_BUS_NOT_FOUND: return "BUS_NOT_FOUND";
    case VIGEM_ERROR_NO_FREE_SLOT: return "NO_FREE_SLOT";
    case VIGEM_ERROR_INVALID_TARGET: return "INVALID_TARGET";
    case VIGEM_ERROR_REMOVAL_FAILED: return "REMOVAL_FAILED";
    case VIGEM_ERROR_ALREADY_CONNECTED: return "ALREADY_CONNECTED";
    case VIGEM_ERROR_TARGET_UNINITIALIZED: return "TARGET_UNINITIALIZED";
    case VIGEM_ERROR_TARGET_NOT_PLUGGED_IN: return "TARGET_NOT_PLUGGED_IN";
    case VIGEM_ERROR_BUS_VERSION_MISMATCH: return "BUS_VERSION_MISMATCH";
    case VIGEM_ERROR_BUS_ACCESS_FAILED: return "BUS_ACCESS_FAILED";
    default: return "UNKNOWN";
  }
}

class ViGEmGamepadSink final : public IGamepadSink {
 public:
  ~ViGEmGamepadSink() override {
    ReleaseAll();
    if (client_ != nullptr) {
      vigem_disconnect(client_);
      vigem_free(client_);
      client_ = nullptr;
    }
  }

  bool Initialise() override {
    client_ = vigem_alloc();
    if (client_ == nullptr) {
      LOG_ERROR << "vigem_alloc failed (out of memory)";
      return false;
    }

    const VIGEM_ERROR result = vigem_connect(client_);
    if (!VIGEM_SUCCESS(result)) {
      if (result == VIGEM_ERROR_BUS_NOT_FOUND) {
        LOG_WARN << "ViGEmBus driver not installed - gamepad injection disabled";
        LOG_WARN << "Install from https://github.com/nefarius/ViGEmBus/releases "
                    "(only needed for PRD phase 5)";
      } else {
        LOG_WARN << "vigem_connect failed: " << ErrorName(result);
      }
      vigem_free(client_);
      client_ = nullptr;
      return false;
    }

    available_ = true;
    LOG_INFO << "ViGEmBus connected - virtual XInput gamepads available";
    return true;
  }

  bool Connect(uint8_t index) override {
    if (!available_ || index >= kMaxPads) return false;
    std::lock_guard<std::mutex> guard(mutex_);
    return ConnectLocked(index);
  }

  bool Disconnect(uint8_t index) override {
    if (!available_ || index >= kMaxPads) return false;
    std::lock_guard<std::mutex> guard(mutex_);
    return RemoveLocked(index);
  }

  bool Submit(uint8_t index, const GamepadState& state) override {
    if (!available_ || index >= kMaxPads) return false;
    std::lock_guard<std::mutex> guard(mutex_);

    // The browser can send state before its connection event arrives, since
    // the input channel is unordered. Plugging in on demand avoids dropping
    // that first frame of input.
    if (pads_[index] == nullptr && !ConnectLocked(index)) return false;

    XUSB_REPORT report{};
    // The wire format already uses the XINPUT_GAMEPAD_* bit layout, so the
    // button mask passes straight through with no translation table.
    report.wButtons = state.buttons;
    report.bLeftTrigger = state.left_trigger;
    report.bRightTrigger = state.right_trigger;
    report.sThumbLX = state.lx;
    report.sThumbLY = state.ly;
    report.sThumbRX = state.rx;
    report.sThumbRY = state.ry;

    const VIGEM_ERROR result = vigem_target_x360_update(client_, pads_[index], report);
    if (!VIGEM_SUCCESS(result)) {
      LOG_DEBUG << "vigem_target_x360_update failed: " << ErrorName(result);
      return false;
    }
    return true;
  }

  void ReleaseAll() override {
    if (!available_) return;
    std::lock_guard<std::mutex> guard(mutex_);
    for (uint8_t i = 0; i < kMaxPads; ++i) RemoveLocked(i);
  }

  bool available() const override { return available_; }

  std::string description() const override {
    return available_ ? "ViGEmBus virtual Xbox 360 controller" : "unavailable";
  }

 private:
  // Caller holds mutex_.
  bool ConnectLocked(uint8_t index) {
    if (pads_[index] != nullptr) return true;  // already plugged in

    PVIGEM_TARGET pad = vigem_target_x360_alloc();
    if (pad == nullptr) return false;

    const VIGEM_ERROR result = vigem_target_add(client_, pad);
    if (!VIGEM_SUCCESS(result)) {
      LOG_WARN << "vigem_target_add(" << static_cast<int>(index)
               << ") failed: " << ErrorName(result);
      vigem_target_free(pad);
      return false;
    }

    pads_[index] = pad;
    LOG_INFO << "virtual gamepad " << static_cast<int>(index) << " connected";
    return true;
  }

  // Caller holds mutex_.
  bool RemoveLocked(uint8_t index) {
    if (pads_[index] == nullptr) return false;
    vigem_target_remove(client_, pads_[index]);
    vigem_target_free(pads_[index]);
    pads_[index] = nullptr;
    LOG_DEBUG << "virtual gamepad " << static_cast<int>(index) << " removed";
    return true;
  }

  PVIGEM_CLIENT client_ = nullptr;
  std::array<PVIGEM_TARGET, kMaxPads> pads_{};
  std::mutex mutex_;
  bool available_ = false;
};

}  // namespace

std::unique_ptr<IGamepadSink> CreateGamepadSink(bool enabled) {
  if (!enabled) {
    LOG_INFO << "gamepad support disabled by configuration";
    return std::make_unique<NullGamepadSink>();
  }

  auto sink = std::make_unique<ViGEmGamepadSink>();
  if (sink->Initialise()) return sink;

  // Falling back rather than failing: PRD phases 1-4 do not need a gamepad,
  // and refusing to start would block the whole POC on an optional driver.
  return std::make_unique<NullGamepadSink>();
}

}  // namespace glsplay

#else  // !GLSPLAY_HAVE_VIGEM

namespace glsplay {

std::unique_ptr<IGamepadSink> CreateGamepadSink(bool enabled) {
  if (enabled) {
    LOG_INFO << "built without ViGEmClient - gamepad injection unavailable";
  }
  return std::make_unique<NullGamepadSink>();
}

}  // namespace glsplay

#endif  // GLSPLAY_HAVE_VIGEM
