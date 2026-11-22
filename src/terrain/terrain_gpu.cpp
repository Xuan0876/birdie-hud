// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "terrain/terrain_gpu.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#include "hud/terrain_draw.h"

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace birdie::terrain {
namespace {

// Reversed depth: z_ndc = near / distance in front, nearer = larger (GREATER test, cleared to 0).
constexpr char kShader[] = R"(
cbuffer C : register(b0) {
  float4 eye;   // xyz from the anchor, w = range of the mesh
  float4 c0;    // camera right (ENU)
  float4 c1;    // camera up
  float4 c2;    // camera forward
  float4 pr;    // ax, bx, ay, by: overlay ndc = a * c / cz + b
};
struct VO { float4 pos : SV_Position; float a : ALPHA; };
VO vs(float4 p : POSITION) {
  VO o;
  float3 q = p.xyz - eye.xyz;
  float cx = dot(c0.xyz, q), cy = dot(c1.xyz, q), cz = dot(c2.xyz, q);
  o.pos = float4(pr.x * cx + pr.y * cz, pr.z * cy + pr.w * cz, 20.0, cz);
  float d = length(q.xy), R = eye.w;
  float fade = clamp(1.05 - d / (1.15 * R), 0.35, 1.0) * saturate((R - d) / (0.1 * R));
  o.a = p.w * fade;
  return o;
}
float4 ps(VO i) : SV_Target { return float4(i.a, i.a, i.a, i.a); }
)";

}  // namespace

struct TerrainGpu::Impl {
  ID3D11Device* dev = nullptr;  // what the resources below belong to
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11Buffer> cb, vb, ib;
  ComPtr<ID3D11RasterizerState> rs;
  ComPtr<ID3D11DepthStencilState> ds;
  ComPtr<ID3D11BlendState> bs;
  std::shared_ptr<const TerrainMesh> mesh;  // what vb holds
  int ib_na = 0, ib_nd = 0;
  UINT n_idx = 0;
  int w = 0, h = 0;
  UINT samples = 1;
  ComPtr<ID3D11Texture2D> color, depth, resolved;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11DepthStencilView> dsv;
  ComPtr<ID2D1Bitmap1> bitmap;
  ID2D1DeviceContext* bitmap_dc = nullptr;
  bool broken = false;  // shader compile failed: do not try every frame

  void release() {
    *this = Impl{};
  }

  bool setup(ID3D11Device* d) {
    if (dev == d && vs) return true;
    release();
    dev = d;
    ComPtr<ID3DBlob> bvs, bps, err;
    if (FAILED(D3DCompile(kShader, sizeof kShader - 1, "terrain", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &bvs, &err)) ||
        FAILED(D3DCompile(kShader, sizeof kShader - 1, "terrain", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &bps, &err))) {
      broken = true;
      return false;
    }
    if (FAILED(d->CreateVertexShader(bvs->GetBufferPointer(), bvs->GetBufferSize(), nullptr, &vs)) ||
        FAILED(d->CreatePixelShader(bps->GetBufferPointer(), bps->GetBufferSize(), nullptr, &ps)))
      return false;
    D3D11_INPUT_ELEMENT_DESC ie{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    if (FAILED(d->CreateInputLayout(&ie, 1, bvs->GetBufferPointer(), bvs->GetBufferSize(), &layout))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 5 * 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(d->CreateBuffer(&bd, nullptr, &cb))) return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    d->CreateRasterizerState(&rd, &rs);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER;
    d->CreateDepthStencilState(&dd, &ds);
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    d->CreateBlendState(&bl, &bs);
    UINT q = 0;
    samples = SUCCEEDED(d->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &q)) && q > 0 ? 4 : 1;
    return rs && ds && bs;
  }

  bool targets(ID2D1DeviceContext* dc, int tw, int th) {
    if (tw == w && th == h && color && bitmap && bitmap_dc == dc) return true;
    color.Reset(), depth.Reset(), resolved.Reset(), rtv.Reset(), dsv.Reset(), bitmap.Reset();
    w = tw, h = th, bitmap_dc = dc;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = tw;
    td.Height = th;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = samples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &color))) return false;
    if (FAILED(dev->CreateRenderTargetView(color.Get(), nullptr, &rtv))) return false;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &depth))) return false;
    if (FAILED(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv))) return false;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &resolved))) return false;
    ComPtr<IDXGISurface> surf;
    if (FAILED(resolved.As(&surf))) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    return SUCCEEDED(dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &bitmap));
  }

  bool upload(const std::shared_ptr<const TerrainMesh>& m) {
    if (m == mesh && vb) return true;
    vb.Reset();
    mesh = m;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(m->v.size() * sizeof(float));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{m->v.data(), 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &sd, &vb))) return false;
    if (ib && ib_na == m->na && ib_nd == m->nd) return true;
    ib.Reset();
    std::vector<UINT> idx;
    idx.reserve(static_cast<size_t>(m->na - 1) * (m->nd - 1) * 6);
    for (int i = 0; i + 1 < m->na; ++i)
      for (int k = 0; k + 1 < m->nd; ++k) {
        const UINT a = i * m->nd + k, b = a + 1, c = a + m->nd, d = c + 1;
        idx.insert(idx.end(), {a, c, b, b, c, d});
      }
    bd.ByteWidth = static_cast<UINT>(idx.size() * sizeof(UINT));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = idx.data();
    if (FAILED(dev->CreateBuffer(&bd, &sd, &ib))) return false;
    ib_na = m->na, ib_nd = m->nd, n_idx = static_cast<UINT>(idx.size());
    return true;
  }
};

TerrainGpu::TerrainGpu() : d_(std::make_unique<Impl>()) {}
TerrainGpu::~TerrainGpu() = default;
void TerrainGpu::release() { d_->release(); }

ID2D1Bitmap1* TerrainGpu::render(ID3D11Device* dev, ID2D1DeviceContext* dc, const float* p, int w, int h) {
  Impl& d = *d_;
  const auto m = hud::fill_mesh();
  if (!m || m->na < 2 || m->nd < 2 || w < 1 || h < 1 || !dev || !dc) return nullptr;
  if (d.broken && d.dev == dev) return nullptr;
  if (!d.setup(dev) || !d.targets(dc, w, h) || !d.upload(m)) return nullptr;
  ComPtr<ID3D11DeviceContext> ctx;
  dev->GetImmediateContext(&ctx);
  // p: opacity, eye xyz, c0 c1 c2, f, W, H, ox, oy, range
  const float f = p[13], W = p[14], H = p[15], ox = p[16], oy = p[17];
  const float cb[20] = {p[1], p[2], p[3], p[18],
                        p[4], p[5], p[6], 0,
                        p[7], p[8], p[9], 0,
                        p[10], p[11], p[12], 0,
                        2 * f / w, (W / 2 - ox) / w * 2 - 1, 2 * f / h, 1 - (H / 2 - oy) / h * 2};
  D3D11_MAPPED_SUBRESOURCE ms;
  if (FAILED(ctx->Map(d.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return nullptr;
  std::memcpy(ms.pData, cb, sizeof cb);
  ctx->Unmap(d.cb.Get(), 0);

  const float clear[4] = {0, 0, 0, 0};
  ctx->ClearRenderTargetView(d.rtv.Get(), clear);
  ctx->ClearDepthStencilView(d.dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
  ID3D11RenderTargetView* rt = d.rtv.Get();
  ctx->OMSetRenderTargets(1, &rt, d.dsv.Get());
  ctx->OMSetDepthStencilState(d.ds.Get(), 0);
  ctx->OMSetBlendState(d.bs.Get(), nullptr, 0xffffffff);
  D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(d.rs.Get());
  ctx->IASetInputLayout(d.layout.Get());
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const UINT stride = 16, off = 0;
  ID3D11Buffer* vb = d.vb.Get();
  ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
  ctx->IASetIndexBuffer(d.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
  ID3D11Buffer* c = d.cb.Get();
  ctx->VSSetShader(d.vs.Get(), nullptr, 0);
  ctx->VSSetConstantBuffers(0, 1, &c);
  ctx->PSSetShader(d.ps.Get(), nullptr, 0);
  ctx->DrawIndexed(d.n_idx, 0, 0);
  if (d.samples > 1) ctx->ResolveSubresource(d.resolved.Get(), 0, d.color.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
  else ctx->CopyResource(d.resolved.Get(), d.color.Get());
  ctx->ClearState();  // Direct2D sets up what it needs itself
  return d.bitmap.Get();
}

}  // namespace birdie::terrain
