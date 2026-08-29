#include "capture/dxgi_duplicator.h"

#include <algorithm>

#include "util/log.h"

namespace glsplay {
namespace {

constexpr uint32_t kVendorNvidia = 0x10DE;

std::string WideToUtf8(const wchar_t* text) {
  if (text == nullptr) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) return {};
  std::string out(static_cast<size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
  return out;
}

// QPC ticks to microseconds. Desktop Duplication reports presentation times on
// the performance counter, which is also what we timestamp frames with.
int64_t QpcToMicroseconds(LARGE_INTEGER qpc) {
  static const int64_t frequency = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  if (frequency == 0) return 0;
  return (qpc.QuadPart * 1000000LL) / frequency;
}

}  // namespace

DxgiDuplicator::DxgiDuplicator() = default;

DxgiDuplicator::~DxgiDuplicator() {
  ReleaseFrame();
}

std::vector<AdapterInfo> DxgiDuplicator::EnumerateAdapters() {
  std::vector<AdapterInfo> adapters;

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    LOG_ERROR << "CreateDXGIFactory1 failed: " << HrToString(hr);
    return adapters;
  }

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
      adapter.Reset();
      continue;
    }

    AdapterInfo info;
    info.index = static_cast<int>(i);
    info.description = WideToUtf8(desc.Description);
    info.vendor_id = desc.VendorId;
    info.dedicated_vram = desc.DedicatedVideoMemory;
    info.is_nvidia = desc.VendorId == kVendorNvidia;

    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      ++info.output_count;
      output.Reset();
    }

    adapters.push_back(std::move(info));
    adapter.Reset();
  }

  return adapters;
}

bool DxgiDuplicator::Initialise(int adapter_index, int output_index) {
  adapter_index_ = adapter_index;
  output_index_ = output_index;

  if (!CreateDevice(adapter_index)) return false;
  if (!CreateDuplication(output_index)) return false;

  LOG_INFO << "DXGI duplication ready: " << adapter_description_ << ' '
           << width_ << 'x' << height_;
  return true;
}

bool DxgiDuplicator::CreateDevice(int adapter_index) {
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
  if (FAILED(hr)) {
    LOG_ERROR << "CreateDXGIFactory1 failed: " << HrToString(hr);
    return false;
  }

  if (adapter_index >= 0) {
    hr = factory_->EnumAdapters1(static_cast<UINT>(adapter_index), &adapter_);
    if (FAILED(hr)) {
      LOG_ERROR << "adapter " << adapter_index << " not found: " << HrToString(hr);
      return false;
    }
  } else {
    // Prefer an NVIDIA adapter that actually has an output attached. On a GCP
    // L4 box the Basic Display Adapter also enumerates, and picking it is how
    // you end up with software capture and no NVENC.
    ComPtr<IDXGIAdapter1> candidate;
    ComPtr<IDXGIAdapter1> nvidia_without_output;
    for (UINT i = 0; factory_->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(candidate->GetDesc1(&desc)) && desc.VendorId == kVendorNvidia) {
        ComPtr<IDXGIOutput> output;
        if (candidate->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND) {
          adapter_ = candidate;
          LOG_INFO << "selected NVIDIA adapter " << i << ": " << WideToUtf8(desc.Description);
          break;
        }
        if (!nvidia_without_output) nvidia_without_output = candidate;
      }
      candidate.Reset();
    }
    if (!adapter_ && nvidia_without_output) {
      adapter_ = nvidia_without_output;
      LOG_WARN << "NVIDIA adapter has no attached output - is the IDD virtual "
                  "display installed and enabled?";
    }
    if (!adapter_) {
      hr = factory_->EnumAdapters1(0, &adapter_);
      if (FAILED(hr)) {
        LOG_ERROR << "no DXGI adapters at all: " << HrToString(hr);
        return false;
      }
      LOG_WARN << "no NVIDIA adapter found - falling back to adapter 0. "
                  "NVENC will not be available.";
    }
  }

  DXGI_ADAPTER_DESC1 desc{};
  if (SUCCEEDED(adapter_->GetDesc1(&desc))) {
    adapter_description_ = WideToUtf8(desc.Description);
  }

  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  D3D_FEATURE_LEVEL achieved{};

  // BGRA support is required because the desktop surface is B8G8R8A8, and
  // the cursor compositor blends into it directly.
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
  // Only if the SDK layers are installed; the create is retried without it.
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                         levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                         &device_, &achieved, &context_);
#ifndef NDEBUG
  if (FAILED(hr)) {
    flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
    hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                           levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                           &device_, &achieved, &context_);
  }
#endif
  if (FAILED(hr)) {
    LOG_ERROR << "D3D11CreateDevice failed: " << HrToString(hr);
    return false;
  }

  // Desktop Duplication is driven from one thread, but libwebrtc may call into
  // the encoder from another, and both touch this device.
  ComPtr<ID3D11Multithread> multithread;
  if (SUCCEEDED(context_.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }

  return true;
}

bool DxgiDuplicator::CreateDuplication(int output_index) {
  ComPtr<IDXGIOutput> output;
  HRESULT hr = adapter_->EnumOutputs(static_cast<UINT>(output_index), &output);
  if (FAILED(hr)) {
    LOG_ERROR << "no output " << output_index << " on adapter " << adapter_description_
              << ": " << HrToString(hr);
    LOG_ERROR << "A headless L4 has no display head. Install the IDD virtual "
                 "display driver - see vm-scripts/install-virtual-display.ps1.";
    return false;
  }

  hr = output.As(&output_);
  if (FAILED(hr)) {
    LOG_ERROR << "IDXGIOutput1 unavailable: " << HrToString(hr);
    return false;
  }

  DXGI_OUTPUT_DESC output_desc{};
  if (SUCCEEDED(output_->GetDesc(&output_desc))) {
    width_ = output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left;
    height_ = output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top;
    desktop_left_ = output_desc.DesktopCoordinates.left;
    desktop_top_ = output_desc.DesktopCoordinates.top;
    if (!output_desc.AttachedToDesktop) {
      LOG_ERROR << "output " << output_index << " is not attached to the desktop";
      return false;
    }
  }

  hr = output_->DuplicateOutput(device_.Get(), &duplication_);
  if (FAILED(hr)) {
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
      LOG_ERROR << "DuplicateOutput unavailable (" << HrToString(hr) << ')';
      LOG_ERROR << "Either another process already holds duplication, or this "
                   "process is running in an RDP session. Desktop Duplication "
                   "captures the console session only - disconnect RDP and use "
                   "tscon to return the session to the console.";
    } else if (hr == E_ACCESSDENIED) {
      LOG_ERROR << "DuplicateOutput access denied. A secure desktop (UAC prompt "
                   "or lock screen) is in the foreground.";
    } else {
      LOG_ERROR << "DuplicateOutput failed: " << HrToString(hr);
    }
    return false;
  }

  DXGI_OUTDUPL_DESC dupl_desc{};
  duplication_->GetDesc(&dupl_desc);
  format_ = dupl_desc.ModeDesc.Format;
  if (dupl_desc.ModeDesc.Width != 0) {
    width_ = static_cast<int>(dupl_desc.ModeDesc.Width);
    height_ = static_cast<int>(dupl_desc.ModeDesc.Height);
  }

  // Desktop Duplication is only fast when the desktop is GPU-composited. If
  // DWM has fallen back to CPU composition every frame arrives via system
  // memory, which caps out around 30fps regardless of the encoder.
  if (dupl_desc.DesktopImageInSystemMemory) {
    LOG_WARN << "desktop image is in system memory - capture will be slow and "
                "the zero-copy path to NVENC is unavailable";
  }

  // Our own frame texture, allocated once per mode. BIND_RENDER_TARGET lets
  // the cursor compositor draw into it; SHADER_RESOURCE lets NVENC read it.
  D3D11_TEXTURE2D_DESC tex{};
  tex.Width = static_cast<UINT>(width_);
  tex.Height = static_cast<UINT>(height_);
  tex.MipLevels = 1;
  tex.ArraySize = 1;
  tex.Format = format_;
  tex.SampleDesc.Count = 1;
  tex.Usage = D3D11_USAGE_DEFAULT;
  tex.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  tex.CPUAccessFlags = 0;
  tex.MiscFlags = 0;

  // A fresh texture has undefined contents, so nothing is safe to resend until
  // a real frame has landed in it.
  frame_texture_.Reset();
  has_content_ = false;
  hr = device_->CreateTexture2D(&tex, nullptr, &frame_texture_);
  if (FAILED(hr)) {
    LOG_ERROR << "CreateTexture2D for the frame buffer failed: " << HrToString(hr);
    return false;
  }

  return true;
}

bool DxgiDuplicator::Reinitialise() {
  LOG_INFO << "reinitialising DXGI duplication";
  ReleaseFrame();
  duplication_.Reset();
  output_.Reset();
  frame_texture_.Reset();

  // Access loss is usually a mode change, so the device is normally still
  // good. Rebuild only the duplication chain; if that fails, rebuild fully.
  if (device_ && adapter_ && CreateDuplication(output_index_)) return true;

  context_.Reset();
  device_.Reset();
  adapter_.Reset();
  factory_.Reset();
  return Initialise(adapter_index_, output_index_);
}

CaptureStatus DxgiDuplicator::AcquireFrame(uint32_t timeout_ms, CapturedFrame* out) {
  if (!duplication_) return CaptureStatus::kFailed;
  // A caller that forgot to release would deadlock on the next acquire.
  if (frame_acquired_) ReleaseFrame();

  DXGI_OUTDUPL_FRAME_INFO info{};
  ComPtr<IDXGIResource> resource;

  HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &info, &resource);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
    // Nothing on screen changed - the common case on an idle desktop. Hand back
    // the previous frame so the caller can resend it. Returning no texture here
    // means an idle desktop produces no frames at all, the browser's jitter
    // buffer drains, and the stream simply never starts.
    if (has_content_) {
      out->texture = frame_texture_.Get();
      out->timestamp_us = 0;
      out->content_changed = false;
      out->cursor_changed = false;
    }
    return CaptureStatus::kTimeout;
  }
  if (hr == DXGI_ERROR_ACCESS_LOST) {
    LOG_WARN << "duplication access lost";
    return CaptureStatus::kAccessLost;
  }
  if (hr == DXGI_ERROR_INVALID_CALL) {
    // DXGI thinks a frame is still checked out while frame_acquired_ says it is
    // not. The two have diverged, and nothing recovers on its own: because our
    // flag is false ReleaseFrame() returns early, so every later acquire hits
    // this same error forever. Release unconditionally to resync, then rebuild
    // the duplication chain in case that was not enough.
    if (duplication_) duplication_->ReleaseFrame();
    frame_acquired_ = false;
    LOG_WARN << "AcquireNextFrame reports an unreleased frame - rebuilding duplication";
    return CaptureStatus::kAccessLost;
  }
  if (FAILED(hr)) {
    LOG_ERROR << "AcquireNextFrame failed: " << HrToString(hr);
    return CaptureStatus::kFailed;
  }

  frame_acquired_ = true;

  ComPtr<ID3D11Texture2D> acquired;
  hr = resource.As(&acquired);
  if (FAILED(hr)) {
    LOG_ERROR << "acquired resource is not a texture: " << HrToString(hr);
    ReleaseFrame();
    return CaptureStatus::kFailed;
  }

  UpdateCursor(info);

  // LastPresentTime is zero when only the cursor moved.
  const bool content_changed = info.LastPresentTime.QuadPart != 0;
  if (content_changed && !CopyToStaging(acquired.Get())) {
    ReleaseFrame();
    return CaptureStatus::kFailed;
  }

  // Release straight away. Holding the acquired surface blocks the desktop
  // compositor, and on a busy scene that shows up as input lag on the host.
  ReleaseFrame();

  out->texture = frame_texture_.Get();
  out->timestamp_us = QpcToMicroseconds(info.LastPresentTime);
  out->content_changed = content_changed;
  out->cursor_changed = info.LastMouseUpdateTime.QuadPart != 0;
  has_content_ = true;
  return CaptureStatus::kOk;
}

bool DxgiDuplicator::CopyToStaging(ID3D11Texture2D* source) {
  if (!frame_texture_ || source == nullptr) return false;

  D3D11_TEXTURE2D_DESC src_desc{};
  source->GetDesc(&src_desc);

  // A mode change can arrive before DXGI reports access loss.
  if (static_cast<int>(src_desc.Width) != width_ ||
      static_cast<int>(src_desc.Height) != height_) {
    LOG_WARN << "captured frame is " << src_desc.Width << 'x' << src_desc.Height
             << " but duplication reported " << width_ << 'x' << height_;
    return false;
  }

  context_->CopyResource(frame_texture_.Get(), source);
  return true;
}

bool DecodeCursorRgba(const CursorState& cursor, std::vector<uint8_t>* out_rgba,
                      uint32_t* out_w, uint32_t* out_h) {
  if (cursor.shape.empty() || cursor.width == 0 || cursor.height == 0 ||
      cursor.pitch == 0) {
    return false;
  }

  uint32_t width = cursor.width;
  uint32_t height = cursor.height;
  std::vector<uint8_t>& rgba = *out_rgba;

  auto put = [&](size_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rgba[i * 4 + 0] = r;
    rgba[i * 4 + 1] = g;
    rgba[i * 4 + 2] = b;
    rgba[i * 4 + 3] = a;
  };

  switch (cursor.shape_type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
      height = cursor.height / 2;  // AND mask stacked above XOR mask
      rgba.assign(static_cast<size_t>(width) * height * 4, 0);
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const size_t and_byte = static_cast<size_t>(y) * cursor.pitch + (x / 8);
          const size_t xor_byte =
              static_cast<size_t>(height + y) * cursor.pitch + (x / 8);
          const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
          const bool and_bit =
              and_byte < cursor.shape.size() && (cursor.shape[and_byte] & bit);
          const bool xor_bit =
              xor_byte < cursor.shape.size() && (cursor.shape[xor_byte] & bit);
          const size_t i = static_cast<size_t>(y) * width + x;
          if (!and_bit) {
            put(i, xor_bit ? 255 : 0, xor_bit ? 255 : 0, xor_bit ? 255 : 0, 255);
          } else if (xor_bit) {
            put(i, 255, 255, 255, 255);  // invert -> approximate as white
          }  // else fully transparent (already zeroed)
        }
      }
      break;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
      rgba.assign(static_cast<size_t>(width) * height * 4, 0);
      const bool masked =
          cursor.shape_type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = cursor.shape.data() + static_cast<size_t>(y) * cursor.pitch;
        for (uint32_t x = 0; x < width; ++x) {
          const size_t off = static_cast<size_t>(x) * 4;
          if (static_cast<size_t>(y) * cursor.pitch + off + 3 >= cursor.shape.size()) {
            continue;
          }
          // DXGI hands back B8G8R8A8.
          const uint8_t b = row[off + 0];
          const uint8_t g = row[off + 1];
          const uint8_t r = row[off + 2];
          uint8_t a = row[off + 3];
          if (masked) a = a ? 0 : 255;  // alpha byte is a 1-bit mask here
          put(static_cast<size_t>(y) * width + x, r, g, b, a);
        }
      }
      break;
    }
    default:
      return false;
  }

  *out_w = width;
  *out_h = height;
  return true;
}

void DxgiDuplicator::UpdateCursor(const DXGI_OUTDUPL_FRAME_INFO& info) {
  if (info.LastMouseUpdateTime.QuadPart != 0) {
    cursor_.visible = info.PointerPosition.Visible != FALSE;
    cursor_.x = info.PointerPosition.Position.x;
    cursor_.y = info.PointerPosition.Position.y;
  }

  // A shape is only delivered when it actually changes, so the previous one is
  // kept and reused for every frame in between.
  if (info.PointerShapeBufferSize == 0) return;

  cursor_shape_buffer_.resize(info.PointerShapeBufferSize);
  DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info{};
  UINT required = 0;

  const HRESULT hr = duplication_->GetFramePointerShape(
      static_cast<UINT>(cursor_shape_buffer_.size()), cursor_shape_buffer_.data(),
      &required, &shape_info);
  if (FAILED(hr)) {
    LOG_DEBUG << "GetFramePointerShape failed: " << HrToString(hr);
    return;
  }

  cursor_.width = shape_info.Width;
  cursor_.height = shape_info.Height;
  cursor_.pitch = shape_info.Pitch;
  cursor_.hotspot_x = static_cast<int32_t>(shape_info.HotSpot.x);
  cursor_.hotspot_y = static_cast<int32_t>(shape_info.HotSpot.y);
  cursor_.shape_type = shape_info.Type;
  cursor_.shape.assign(cursor_shape_buffer_.begin(),
                       cursor_shape_buffer_.begin() + required);
}

void DxgiDuplicator::ReleaseFrame() {
  if (!frame_acquired_ || !duplication_) return;
  const HRESULT hr = duplication_->ReleaseFrame();
  if (FAILED(hr) && hr != DXGI_ERROR_INVALID_CALL) {
    LOG_DEBUG << "ReleaseFrame failed: " << HrToString(hr);
  }
  frame_acquired_ = false;
}

}  // namespace glsplay
