#include "capture/cursor_compositor.h"

#include <d3dcompiler.h>

#include <vector>

#include "util/log.h"

namespace glsplay {
namespace {

// A textured quad positioned in normalised device coordinates by the caller.
// Kept in one string so there is no build-time shader compilation step and no
// .cso files to ship alongside the exe.
constexpr char kShaderSource[] = R"(
struct VsIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VsOut VsMain(VsIn input) {
  VsOut output;
  output.pos = float4(input.pos, 0.0f, 1.0f);
  output.uv = input.uv;
  return output;
}

Texture2D    shapeTexture : register(t0);
SamplerState shapeSampler : register(s0);

float4 PsMain(VsOut input) : SV_TARGET {
  return shapeTexture.Sample(shapeSampler, input.uv);
}
)";

struct Vertex {
  float x, y;
  float u, v;
};

bool Compile(const char* entry, const char* target, ID3DBlob** out) {
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr,
                                nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
  if (FAILED(hr)) {
    LOG_ERROR << "shader " << entry << " failed to compile: " << HrToString(hr);
    if (errors) {
      LOG_ERROR << static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
  }
  return true;
}

}  // namespace

CursorCompositor::CursorCompositor() = default;
CursorCompositor::~CursorCompositor() = default;

bool CursorCompositor::Initialise(ID3D11Device* device) {
  if (device == nullptr) return false;
  device_ = device;
  device_->GetImmediateContext(&context_);

  Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
  if (!Compile("VsMain", "vs_5_0", &vs_blob)) return false;
  if (!Compile("PsMain", "ps_5_0", &ps_blob)) return false;

  HRESULT hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                           vs_blob->GetBufferSize(), nullptr,
                                           &vertex_shader_);
  if (FAILED(hr)) { LOG_ERROR << "CreateVertexShader: " << HrToString(hr); return false; }

  hr = device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                  nullptr, &pixel_shader_);
  if (FAILED(hr)) { LOG_ERROR << "CreatePixelShader: " << HrToString(hr); return false; }

  const D3D11_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  hr = device_->CreateInputLayout(layout, ARRAYSIZE(layout), vs_blob->GetBufferPointer(),
                                  vs_blob->GetBufferSize(), &input_layout_);
  if (FAILED(hr)) { LOG_ERROR << "CreateInputLayout: " << HrToString(hr); return false; }

  // Six vertices, rewritten per draw with the cursor rectangle.
  D3D11_BUFFER_DESC vb{};
  vb.Usage = D3D11_USAGE_DYNAMIC;
  vb.ByteWidth = sizeof(Vertex) * 6;
  vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device_->CreateBuffer(&vb, nullptr, &vertex_buffer_);
  if (FAILED(hr)) { LOG_ERROR << "CreateBuffer: " << HrToString(hr); return false; }

  // Straight alpha over the desktop.
  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0].BlendEnable = TRUE;
  blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  hr = device_->CreateBlendState(&blend, &blend_state_);
  if (FAILED(hr)) { LOG_ERROR << "CreateBlendState: " << HrToString(hr); return false; }

  // Point sampling: the cursor is pixel art and bilinear filtering makes it
  // look blurry against a sharp desktop.
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  hr = device_->CreateSamplerState(&sampler, &sampler_);
  if (FAILED(hr)) { LOG_ERROR << "CreateSamplerState: " << HrToString(hr); return false; }

  ready_ = true;
  LOG_INFO << "cursor compositor ready";
  return true;
}

bool CursorCompositor::UpdateShapeTexture(const CursorState& cursor) {
  if (cursor.shape.empty() || cursor.width == 0 || cursor.height == 0) return false;

  // Rebuild only when the shape actually changed. DXGI reports a new shape
  // rarely; rebuilding every frame would allocate a texture 60 times a second.
  if (shape_view_ && cached_shape_bytes_ == cursor.shape.size() &&
      shape_width_ == cursor.width) {
    return true;
  }

  uint32_t width = cursor.width;
  uint32_t height = cursor.height;
  std::vector<uint32_t> pixels;

  switch (cursor.shape_type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
      // Monochrome cursors pack an AND mask above an XOR mask in one bitmap,
      // so the real height is half what DXGI reports.
      height = cursor.height / 2;
      pixels.assign(static_cast<size_t>(width) * height, 0);
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const uint32_t byte = y * cursor.pitch + (x / 8);
          const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
          if (byte >= cursor.shape.size()) continue;
          const bool and_bit = (cursor.shape[byte] & bit) != 0;
          const size_t xor_index = static_cast<size_t>(height + y) * cursor.pitch + (x / 8);
          const bool xor_bit =
              xor_index < cursor.shape.size() && (cursor.shape[xor_index] & bit) != 0;

          // AND=1, XOR=0 is transparent; AND=1, XOR=1 inverts the background,
          // which we approximate as white - inversion needs a second pass and
          // is vanishingly rare on modern cursors.
          uint32_t argb = 0x00000000;
          if (!and_bit) argb = xor_bit ? 0xFFFFFFFF : 0xFF000000;
          else if (xor_bit) argb = 0xFFFFFFFF;
          pixels[static_cast<size_t>(y) * width + x] = argb;
        }
      }
      break;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
      pixels.assign(static_cast<size_t>(width) * height, 0);
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = cursor.shape.data() + static_cast<size_t>(y) * cursor.pitch;
        for (uint32_t x = 0; x < width; ++x) {
          const size_t offset = static_cast<size_t>(x) * 4;
          if (static_cast<size_t>(y) * cursor.pitch + offset + 3 >= cursor.shape.size()) {
            continue;
          }
          uint32_t argb = *reinterpret_cast<const uint32_t*>(row + offset);
          // Masked colour uses the alpha byte as a 1-bit mask rather than a
          // real alpha value.
          if (cursor.shape_type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
            argb = (argb & 0x00FFFFFF) | ((argb & 0xFF000000) ? 0x00000000 : 0xFF000000);
          }
          pixels[static_cast<size_t>(y) * width + x] = argb;
        }
      }
      break;
    }
    default:
      return false;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA data{};
  data.pSysMem = pixels.data();
  data.SysMemPitch = width * 4;

  shape_texture_.Reset();
  shape_view_.Reset();

  HRESULT hr = device_->CreateTexture2D(&desc, &data, &shape_texture_);
  if (FAILED(hr)) {
    LOG_DEBUG << "cursor texture creation failed: " << HrToString(hr);
    return false;
  }
  hr = device_->CreateShaderResourceView(shape_texture_.Get(), nullptr, &shape_view_);
  if (FAILED(hr)) {
    LOG_DEBUG << "cursor SRV creation failed: " << HrToString(hr);
    return false;
  }

  shape_width_ = width;
  shape_height_ = height;
  cached_shape_bytes_ = cursor.shape.size();
  return true;
}

bool CursorCompositor::Draw(ID3D11Texture2D* target, const CursorState& cursor) {
  if (!ready_ || target == nullptr) return false;
  if (!cursor.visible) return false;
  if (!UpdateShapeTexture(cursor)) return false;

  D3D11_TEXTURE2D_DESC target_desc{};
  target->GetDesc(&target_desc);
  const float target_w = static_cast<float>(target_desc.Width);
  const float target_h = static_cast<float>(target_desc.Height);

  // Position is the top-left of the shape; the hotspot is already accounted
  // for by Windows in the reported position.
  const float left = static_cast<float>(cursor.x);
  const float top = static_cast<float>(cursor.y);
  const float right = left + static_cast<float>(shape_width_);
  const float bottom = top + static_cast<float>(shape_height_);

  // Pixels to normalised device coordinates: x to [-1,1], y flipped.
  const float x0 = (left / target_w) * 2.0f - 1.0f;
  const float x1 = (right / target_w) * 2.0f - 1.0f;
  const float y0 = 1.0f - (top / target_h) * 2.0f;
  const float y1 = 1.0f - (bottom / target_h) * 2.0f;

  const Vertex vertices[6] = {
      {x0, y0, 0.0f, 0.0f}, {x1, y0, 1.0f, 0.0f}, {x0, y1, 0.0f, 1.0f},
      {x1, y0, 1.0f, 0.0f}, {x1, y1, 1.0f, 1.0f}, {x0, y1, 0.0f, 1.0f},
  };

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return false;
  }
  memcpy(mapped.pData, vertices, sizeof(vertices));
  context_->Unmap(vertex_buffer_.Get(), 0);

  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  if (FAILED(device_->CreateRenderTargetView(target, nullptr, &rtv))) return false;

  D3D11_VIEWPORT viewport{};
  viewport.Width = target_w;
  viewport.Height = target_h;
  viewport.MaxDepth = 1.0f;

  const UINT stride = sizeof(Vertex);
  const UINT offset = 0;
  ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
  ID3D11Buffer* buffers[] = {vertex_buffer_.Get()};
  ID3D11ShaderResourceView* views[] = {shape_view_.Get()};
  ID3D11SamplerState* samplers[] = {sampler_.Get()};
  const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  context_->OMSetRenderTargets(1, rtvs, nullptr);
  context_->RSSetViewports(1, &viewport);
  context_->IASetInputLayout(input_layout_.Get());
  context_->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
  context_->PSSetShaderResources(0, 1, views);
  context_->PSSetSamplers(0, 1, samplers);
  context_->OMSetBlendState(blend_state_.Get(), blend_factor, 0xFFFFFFFF);
  context_->Draw(6, 0);

  // Unbind so the texture can be used as a shader input by NVENC next.
  ID3D11RenderTargetView* null_rtv[] = {nullptr};
  context_->OMSetRenderTargets(1, null_rtv, nullptr);
  return true;
}

}  // namespace glsplay
