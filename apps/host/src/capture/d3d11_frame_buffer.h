// A libwebrtc VideoFrameBuffer that carries a D3D11 texture instead of pixels.
//
// This is the hinge of the zero-copy path in PRD section 4.2. libwebrtc moves
// VideoFrames from the capture source to the encoder; by declaring type
// kNative and hiding the texture inside, the frame reaches NvencVideoEncoder
// still living in VRAM. The encoder downcasts it back and registers the
// texture with NVENC directly.

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "api/video/video_frame_buffer.h"
#include "api/scoped_refptr.h"

namespace glsplay {

class D3D11FrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  static webrtc::scoped_refptr<D3D11FrameBuffer> Create(
      Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
      Microsoft::WRL::ComPtr<ID3D11Device> device,
      int width,
      int height);

  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  // Software fallback. Only invoked if something in the pipeline genuinely
  // needs pixels - a software encoder, or libwebrtc's frame dumper. On the
  // normal path this is never called, and if it starts being called the
  // zero-copy route has silently broken, so it logs loudly.
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;

  ID3D11Texture2D* texture() const { return texture_.Get(); }
  ID3D11Device* device() const { return device_.Get(); }

 protected:
  D3D11FrameBuffer(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                   Microsoft::WRL::ComPtr<ID3D11Device> device,
                   int width,
                   int height);
  ~D3D11FrameBuffer() override;

 private:
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  int width_;
  int height_;
};

}  // namespace glsplay
