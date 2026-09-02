// Parses the binary input DataChannel and drives the injectors.
//
// Wire format is defined once in packages/protocol/include/glsplay_input.h and
// mirrored in input.ts. Nothing here trusts the network: every event is bounds
// checked against the remaining buffer, and an unknown type aborts the batch
// rather than guessing a stride and walking off the end.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "input/gamepad_sink.h"
#include "input/win32_input_injector.h"

namespace glsplay {

struct InputStats {
  uint64_t events = 0;
  uint64_t batches = 0;
  uint64_t malformed = 0;
  uint64_t unknown_type = 0;
  // Mean age of events on arrival, from the client timestamp. Reported to the
  // browser HUD as the one-way input delivery figure in PRD section 2.
  double mean_queue_ms = 0.0;
};

class InputDispatcher {
 public:
  InputDispatcher(std::unique_ptr<Win32InputInjector> injector,
                  std::unique_ptr<IGamepadSink> gamepad);
  ~InputDispatcher();

  // Called from the libwebrtc network thread for every DataChannel message.
  // May contain many coalesced events.
  void HandleMessage(const uint8_t* data, size_t size);

  // Lifts every held key, button and gamepad. Called on peer disconnect.
  void ReleaseAll();

  // Mirrors the client's set-pointer-mode onto the injector; see
  // Win32InputInjector::SetPointerRelative for why the two modes differ.
  void SetPointerRelative(bool relative);

  InputStats stats() const;

  IGamepadSink* gamepad() const { return gamepad_.get(); }

 private:
  void UpdateQueueLatency(uint32_t t_client);

  std::unique_ptr<Win32InputInjector> injector_;
  std::unique_ptr<IGamepadSink> gamepad_;

  // Counters are read by the stats reporter on a different thread. Relaxed
  // ordering is fine - these are diagnostics, not control flow.
  std::atomic<uint64_t> events_{0};
  std::atomic<uint64_t> batches_{0};
  std::atomic<uint64_t> malformed_{0};
  std::atomic<uint64_t> unknown_type_{0};

  // Client and host clocks share no epoch, so absolute difference is
  // meaningless. The minimum observed offset is tracked instead, and queue
  // latency is measured relative to it - the fastest event seen so far is
  // treated as having taken zero time, and everything else is measured
  // against that.
  std::atomic<int64_t> clock_offset_{0};
  std::atomic<bool> clock_primed_{false};
  std::atomic<double> mean_queue_ms_{0.0};
};

}  // namespace glsplay
