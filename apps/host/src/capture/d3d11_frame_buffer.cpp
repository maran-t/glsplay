#include "capture/d3d11_frame_buffer.h"

#include <libyuv/convert.h>

#include <vector>

#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "util/log.h"

namespace glsplay {

webrtc::scoped_refptr<D3D11FrameBuffer> D3D11FrameBuffer::Create(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    int width,
    int height) {
  return webrtc::make_ref_counted<D3D11FrameBuffer>(std::move(texture), std::move(device),
                                                    width, height);
}

D3D11FrameBuffer::D3D11FrameBuffer(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                                   Microsoft::WRL::ComPtr<ID3D11Device> device,
                                   int width,
                                   int height)
    : texture_(std::move(texture)),
      device_(std::move(device)),
      width_(width),
      height_(height) {}

D3D11FrameBuffer::~D3D11FrameBuffer() = default;

webrtc::scoped_refptr<webrtc::I420BufferInterface> D3D11FrameBuffer::ToI420() {
  // Reaching here means the frame is being pulled back to system memory, which
  // costs a full GPU readback stall plus a colour conversion - roughly an
  // order of magnitude more than the whole encode budget in PRD section 5.
  LOG_WARN << "D3D11FrameBuffer::ToI420 called - the zero-copy path is not "
              "being used. Check that NvencVideoEncoderFactory is installed on "
              "the PeerConnectionFactory.";

  auto i420 = webrtc::I420Buffer::Create(width_, height_);
  if (!device_ || !texture_) return i420;

  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  device_->GetImmediateContext(&context);
  if (!context) return i420;

  D3D11_TEXTURE2D_DESC desc{};
  texture_->GetDesc(&desc);

  // A GPU texture cannot be mapped directly; it has to land in a staging
  // texture with CPU read access first.
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging);
  if (FAILED(hr)) {
    LOG_ERROR << "staging texture creation failed: " << HrToString(hr);
    return i420;
  }

  context->CopyResource(staging.Get(), texture_.Get());

  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    LOG_ERROR << "staging Map failed: " << HrToString(hr);
    return i420;
  }

  // The desktop surface is BGRA; libyuv names this ARGB in little-endian.
  libyuv::ARGBToI420(static_cast<const uint8_t*>(mapped.pData),
                     static_cast<int>(mapped.RowPitch),
                     i420->MutableDataY(), i420->StrideY(),
                     i420->MutableDataU(), i420->StrideU(),
                     i420->MutableDataV(), i420->StrideV(),
                     width_, height_);

  context->Unmap(staging.Get(), 0);
  return i420;
}

}  // namespace glsplay
