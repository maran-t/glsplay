// Keeps top-level windows on the monitor being streamed.
//
// On a headless L4 the phantom display head is the primary, so apps open
// there - off the virtual display glsplay-host captures. This polls for
// visible top-level windows whose centre has landed outside the captured
// rect and nudges them back onto it, so anything the user launches appears
// on the stream without a keyboard shortcut.

#pragma once

#include <atomic>
#include <thread>

namespace glsplay {

class WindowHerder {
 public:
  WindowHerder() = default;
  ~WindowHerder();

  WindowHerder(const WindowHerder&) = delete;
  WindowHerder& operator=(const WindowHerder&) = delete;

  // Rect of the captured monitor in virtual-desktop pixels. Starts a polling
  // thread; a width/height of 0 is a no-op.
  void Start(int left, int top, int width, int height);
  void Stop();

 private:
  void Run();

  std::atomic<bool> running_{false};
  std::thread thread_;
  int left_ = 0;
  int top_ = 0;
  int width_ = 0;
  int height_ = 0;
};

}  // namespace glsplay
