#include "util/window_herder.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "util/log.h"

namespace glsplay {
namespace {

struct EnumContext {
  int left, top, width, height;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
  const auto* ctx = reinterpret_cast<const EnumContext*>(lparam);

  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
  // Owned windows (dialogs, tooltips) follow their owner; tool windows are
  // palettes. Leave both alone.
  if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
  const LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
  if (ex_style & WS_EX_TOOLWINDOW) return TRUE;

  // UWP keeps invisible cloaked top-level windows around.
  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                      sizeof(cloaked))) &&
      cloaked) {
    return TRUE;
  }

  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  // The desktop, the taskbar, and Task View are not ours to move.
  if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
      !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"MultitaskingViewFrame")) {
    return TRUE;
  }

  RECT r = {};
  if (!GetWindowRect(hwnd, &r)) return TRUE;
  const int w = r.right - r.left;
  const int h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return TRUE;

  const int cx = r.left + w / 2;
  const int cy = r.top + h / 2;
  const bool inside = cx >= ctx->left && cx < ctx->left + ctx->width &&
                      cy >= ctx->top && cy < ctx->top + ctx->height;
  if (inside) return TRUE;

  // Pull it onto the captured monitor, keeping its size when it fits.
  const int nw = std::min(w, ctx->width);
  const int nh = std::min(h, ctx->height);
  const int nx = ctx->left + (ctx->width - nw) / 2;
  const int ny = ctx->top + (ctx->height - nh) / 2;
  UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
  if (nw == w && nh == h) flags |= SWP_NOSIZE;
  SetWindowPos(hwnd, nullptr, nx, ny, nw, nh, flags);
  return TRUE;
}

}  // namespace

WindowHerder::~WindowHerder() {
  Stop();
}

void WindowHerder::Start(int left, int top, int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (running_.exchange(true)) return;
  left_ = left;
  top_ = top;
  width_ = width;
  height_ = height;
  thread_ = std::thread(&WindowHerder::Run, this);
  LOG_INFO << "window herder: keeping windows within " << width << 'x' << height
           << " at (" << left << ',' << top << ')';
}

void WindowHerder::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void WindowHerder::Run() {
  const EnumContext ctx{left_, top_, width_, height_};
  while (running_.load()) {
    EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&ctx));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

}  // namespace glsplay
