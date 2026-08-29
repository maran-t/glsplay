// DXGI Desktop Duplication capture (PRD section 4.2).
//
// Produces D3D11 textures that stay in VRAM for the whole journey to NVENC.
// Nothing here ever touches system memory - the CPU only handles metadata.

#pragma once

#include <d3d11.h>
// ID3D11Multithread lives here, not in d3d11.h.
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace glsplay {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Why a capture attempt failed. The distinction matters: kAccessLost is
// routine and recoverable, kNoDisplay almost always means the virtual display
// driver is missing or the process is stuck in an RDP session.
enum class CaptureStatus {
  kOk,
  kTimeout,       // no new frame within the wait window; not an error
  kAccessLost,    // mode change, UAC prompt, or session switch - reinitialise
  kNoDisplay,     // no attached output on the chosen adapter
  kFailed,
};

struct CapturedFrame {
  // Owned by the duplicator, valid until the next AcquireFrame() call.
  ID3D11Texture2D* texture = nullptr;
  // QPC timestamp of the underlying present, converted to microseconds.
  int64_t timestamp_us = 0;
  // True when the desktop image changed. A cursor-only move produces a frame
  // with this false, which the encoder can skip to save bitrate.
  bool content_changed = false;
  bool cursor_changed = false;
};

struct CursorState {
  bool visible = false;
  int32_t x = 0;
  int32_t y = 0;
  int32_t hotspot_x = 0;
  int32_t hotspot_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // DXGI_OUTDUPL_POINTER_SHAPE_TYPE: monochrome, colour, or masked colour.
  uint32_t shape_type = 0;
  std::vector<uint8_t> shape;
  uint32_t pitch = 0;
};

// Decodes a raw DXGI pointer shape (any of the three types) into straight-alpha
// RGBA8888 pixels, row-major, tightly packed. Monochrome AND/XOR and masked
// inversion are approximated as opaque black/white - the client cannot XOR the
// remote screen, so this matches what RDP does. `out_w`/`out_h` receive the
// real dimensions (monochrome shapes are half the reported height). Returns
// false for an empty or unrecognised shape.
bool DecodeCursorRgba(const CursorState& cursor, std::vector<uint8_t>* out_rgba,
                      uint32_t* out_w, uint32_t* out_h);

struct AdapterInfo {
  int index = 0;
  std::string description;
  uint32_t vendor_id = 0;
  uint64_t dedicated_vram = 0;
  bool is_nvidia = false;
  int output_count = 0;
};

// Not thread-safe. Owned and driven by a single capture thread.
class DxgiDuplicator {
 public:
  DxgiDuplicator();
  ~DxgiDuplicator();

  DxgiDuplicator(const DxgiDuplicator&) = delete;
  DxgiDuplicator& operator=(const DxgiDuplicator&) = delete;

  // Lists adapters so startup can log what it found and pick an NVIDIA one.
  static std::vector<AdapterInfo> EnumerateAdapters();

  // adapter_index of -1 selects the first NVIDIA adapter present.
  bool Initialise(int adapter_index, int output_index);

  // Tears down and rebuilds the duplication interface. Called after
  // kAccessLost, which happens routinely on resolution changes and UAC
  // prompts, and must not be treated as fatal.
  bool Reinitialise();

  // Blocks up to timeout_ms for a new frame.
  CaptureStatus AcquireFrame(uint32_t timeout_ms, CapturedFrame* out);

  // Must be called after every successful AcquireFrame, before the next one.
  void ReleaseFrame();

  const CursorState& cursor() const { return cursor_; }

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }

  int width() const { return width_; }
  int height() const { return height_; }
  // Top-left of the captured output in virtual-desktop coordinates. Needed to
  // map client mouse deltas onto the monitor the viewer is actually watching.
  int desktop_left() const { return desktop_left_; }
  int desktop_top() const { return desktop_top_; }
  const std::string& adapter_description() const { return adapter_description_; }
  DXGI_FORMAT format() const { return format_; }

 private:
  bool CreateDevice(int adapter_index);
  bool CreateDuplication(int output_index);
  // Copies the acquired frame into our own texture so the shared surface can
  // be released immediately.
  bool CopyToStaging(ID3D11Texture2D* source);
  void UpdateCursor(const DXGI_OUTDUPL_FRAME_INFO& info);

  ComPtr<IDXGIFactory1> factory_;
  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGIOutput1> output_;
  ComPtr<IDXGIOutputDuplication> duplication_;

  // Our own copy of the current frame. Desktop Duplication hands out a shared
  // surface that blocks the compositor until released, so we CopyResource into
  // this and release immediately. The copy is GPU-to-GPU inside VRAM, so the
  // zero-CPU-copy guarantee in PRD section 4.2 still holds.
  ComPtr<ID3D11Texture2D> frame_texture_;

  bool frame_acquired_ = false;
  // True once frame_texture_ holds a real captured image. Until then there is
  // nothing meaningful to resend on a timeout, and its contents are undefined.
  bool has_content_ = false;
  int adapter_index_ = -1;
  int output_index_ = 0;
  int width_ = 0;
  int height_ = 0;
  int desktop_left_ = 0;
  int desktop_top_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
  std::string adapter_description_;

  CursorState cursor_;
  // DXGI reports a pointer shape only when it changes, so the last one is
  // retained across frames.
  std::vector<uint8_t> cursor_shape_buffer_;
};

}  // namespace glsplay
