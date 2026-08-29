// Draws the mouse pointer into the captured frame.
//
// Desktop Duplication never composites the cursor - it hands back the desktop
// image plus a separate pointer shape and position (PRD section 4.1 does not
// mention this, but without it the remote desktop looks cursorless and is
// unusable for anything but a fullscreen game).
//
// The blend runs on the GPU with a small runtime-compiled shader, so the frame
// never leaves VRAM and the zero-copy path to NVENC is preserved.

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

#include "capture/dxgi_duplicator.h"

namespace glsplay {

class CursorCompositor {
 public:
  CursorCompositor();
  ~CursorCompositor();

  CursorCompositor(const CursorCompositor&) = delete;
  CursorCompositor& operator=(const CursorCompositor&) = delete;

  bool Initialise(ID3D11Device* device);

  // Blends the cursor into target in place. Returns false if the cursor is
  // hidden, has no shape yet, or the compositor is not initialised - in every
  // one of those cases the frame is still perfectly usable, just cursorless.
  bool Draw(ID3D11Texture2D* target, const CursorState& cursor);

 private:
  // Converts the DXGI pointer shape into a BGRA texture. Monochrome and masked
  // shapes need expanding; colour shapes are already BGRA.
  bool UpdateShapeTexture(const CursorState& cursor);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> shape_texture_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shape_view_;
  uint32_t shape_width_ = 0;
  uint32_t shape_height_ = 0;
  // The shape only arrives when it changes, so the converted texture is cached
  // and rebuilt on this counter changing rather than every frame.
  size_t cached_shape_bytes_ = 0;

  bool ready_ = false;
};

}  // namespace glsplay
