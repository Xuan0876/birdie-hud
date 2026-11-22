// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Port of the reference renderer (reference/native/hudgfx.cpp); the drawing must stay pixel-identical.
#include "render/d2d.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "terrain/terrain_gpu.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace birdie::render {
namespace {

struct Line {
  float w;
  bool dash, closed;
  std::vector<D2D1_POINT_2F> p;
};
struct Fill {
  std::vector<D2D1_POINT_2F> p;
};
struct Text {
  float x, y, px, ang, dy;
  int align, start, len;
};
struct Style {
  D2D1_COLOR_F green{0, 1, 0, 1}, black{0, 0, 0, 0.745f}, halo{0, 0, 0, 0.47f};
  float outline = 2.4f;
};

// GWLP_USERDATA = the renderer's input hook (D2DRenderer::set_input) while the window takes the mouse
LRESULT CALLBACK overlay_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  auto* hook = reinterpret_cast<D2DRenderer::InputHook*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (m) {
    case WM_NCHITTEST:
      return hook && *hook ? HTCLIENT : HTTRANSPARENT;
    case WM_MOUSEACTIVATE:  // editing: a click takes the focus from the sim (it would read the mouse too)
      return hook && *hook ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
  }
  if (hook && *hook) {
    LRESULT r = 0;
    if ((*hook)(m, wp, lp, r)) return r;
  }
  return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

struct D2DRenderer::Impl {
  HWND hwnd = nullptr;
  int x = 0, y = 0, w = 1, h = 1;
  bool shown = false;
  ComPtr<ID3D11Device> d3d;
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<ID2D1Factory1> d2f;
  ComPtr<ID2D1Device> d2dev;
  ComPtr<ID2D1DeviceContext> dc;
  ComPtr<ID2D1Bitmap1> target;
  ComPtr<IDCompositionDevice> dcomp;
  ComPtr<IDCompositionTarget> dtarget;
  ComPtr<IDCompositionVisual> visual;
  ComPtr<IDWriteFactory> dw;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<ID2D1StrokeStyle1> solid, dashed;
  std::map<int, ComPtr<IDWriteTextFormat>> fonts;
  // Feathered clips (op 7): one blur effect per mask of a frame. A mask's brush draws from its effect's output, so
  // a shared effect left every mask with the last one's shape (11/22: the terrain border came out as the HUD's).
  std::vector<ComPtr<ID2D1Effect>> blurs;
  struct Mask {
    ComPtr<ID2D1PathGeometry> geo;  // the border itself (hard edge)
    ComPtr<ID2D1ImageBrush> brush;  // blurred, inset by half the feather: the fade
  };
  std::vector<Mask> masks;  // one per feathered clip (op 7) of the frame being drawn, in order
  InputHook input;
  terrain::TerrainGpu terrain;       // op 11: the terrain fill, drawn by Direct3D before the frame
  ID2D1Bitmap1* fill_bmp = nullptr;  // ... its opacity, for this frame
  std::wstring family = L"Bahnschrift";
  bool blank = false;  // last frame presented was empty
  std::wstring err;

  void set_err(const wchar_t* what, HRESULT hr) {
    wchar_t buf[256];
    swprintf_s(buf, L"%s (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
    err = buf;
  }

  HRESULT make_target() {
    dc->SetTarget(nullptr);
    target.Reset();
    ComPtr<IDXGISurface> surf;
    HRESULT hr = swap->GetBuffer(0, IID_PPV_ARGS(&surf));
    if (FAILED(hr)) return hr;
    D2D1_BITMAP_PROPERTIES1 bp =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target);
    if (FAILED(hr)) return hr;
    dc->SetTarget(target.Get());
    return S_OK;
  }

  // with_window: also the swap chain + DirectComposition tree for the overlay window
  HRESULT create_device(bool with_window) {
    HRESULT hr;
    if (!dw) {
      hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
      if (FAILED(hr)) {
        set_err(L"DWriteCreateFactory", hr);
        return hr;
      }
    }
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                              D3D_FEATURE_LEVEL_10_0};
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl,
                           ARRAYSIZE(fl), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) {
      set_err(L"D3D11CreateDevice", hr);
      return hr;
    }
    d3d.As(&dxgi);
    ComPtr<IDXGIDevice1> d1;
    if (SUCCEEDED(dxgi.As(&d1))) d1->SetMaximumFrameLatency(1);

    D2D1_FACTORY_OPTIONS fo = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                           reinterpret_cast<void**>(d2f.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
      set_err(L"D2D1CreateFactory", hr);
      return hr;
    }
    hr = d2f->CreateDevice(dxgi.Get(), &d2dev);
    if (FAILED(hr)) {
      set_err(L"ID2D1Factory1::CreateDevice", hr);
      return hr;
    }
    hr = d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    if (FAILED(hr)) {
      set_err(L"CreateDeviceContext", hr);
      return hr;
    }
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 1, 0, 1), &brush);

    // square caps / bevel joins; dashes 6 on 4 off in stroke widths, flat dash caps
    D2D1_STROKE_STYLE_PROPERTIES1 sp = D2D1::StrokeStyleProperties1(
        D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_BEVEL, 10.0f,
        D2D1_DASH_STYLE_SOLID, 0.0f, D2D1_STROKE_TRANSFORM_TYPE_NORMAL);
    d2f->CreateStrokeStyle(sp, nullptr, 0, &solid);
    sp.startCap = sp.endCap = D2D1_CAP_STYLE_FLAT;
    sp.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    float dashes[] = {6.0f, 4.0f};
    d2f->CreateStrokeStyle(sp, dashes, 2, &dashed);

    if (!with_window) return S_OK;

    ComPtr<IDXGIAdapter> ad;
    dxgi->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> fac;
    ad->GetParent(IID_PPV_ARGS(&fac));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = std::max(1, w);
    sd.Height = std::max(1, h);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = fac->CreateSwapChainForComposition(d3d.Get(), &sd, nullptr, &swap);
    if (FAILED(hr)) {
      set_err(L"CreateSwapChainForComposition", hr);
      return hr;
    }
    hr = make_target();
    if (FAILED(hr)) {
      set_err(L"target bitmap", hr);
      return hr;
    }
    hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp));
    if (FAILED(hr)) {
      set_err(L"DCompositionCreateDevice", hr);
      return hr;
    }
    hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &dtarget);
    if (FAILED(hr)) {
      set_err(L"CreateTargetForHwnd", hr);
      return hr;
    }
    dcomp->CreateVisual(&visual);
    visual->SetContent(swap.Get());
    dtarget->SetRoot(visual.Get());
    hr = dcomp->Commit();
    if (FAILED(hr)) {
      set_err(L"DComp Commit", hr);
      return hr;
    }
    return S_OK;
  }

  void release_device() {
    terrain.release();
    fill_bmp = nullptr;
    fonts.clear();
    masks.clear();
    blurs.clear();
    brush.Reset();
    solid.Reset();
    dashed.Reset();
    if (dc) dc->SetTarget(nullptr);
    target.Reset();
    dc.Reset();
    d2dev.Reset();
    d2f.Reset();
    visual.Reset();
    dtarget.Reset();
    dcomp.Reset();
    swap.Reset();
    dxgi.Reset();
    d3d.Reset();
  }

  IDWriteTextFormat* font(float px) {
    const int key = static_cast<int>(std::lround(px * 4.0f));
    auto it = fonts.find(key);
    if (it != fonts.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dw->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_SEMI_CONDENSED, std::max(1, key / 4) * 1.0f, L"en-us", &f)))
      return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fonts[key] = f;
    return f.Get();
  }

  ComPtr<ID2D1PathGeometry> geometry(const std::vector<D2D1_POINT_2F>& p, bool closed, bool filled) {
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(d2f->CreatePathGeometry(&geo))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure(p[0], filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(p.data() + 1, static_cast<UINT32>(p.size() - 1));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
  }

  void flush(std::vector<Line>& lines, std::vector<Fill>& fills, std::vector<Text>& texts, const Style& st,
             const wchar_t* str, int nstr) {
    ID2D1DeviceContext* c = dc.Get();
    std::vector<ComPtr<ID2D1PathGeometry>> lg(lines.size()), fg(fills.size());
    for (size_t i = 0; i < lines.size(); ++i)
      if (lines[i].p.size() > 2 || lines[i].closed) lg[i] = geometry(lines[i].p, lines[i].closed, false);
    for (size_t i = 0; i < fills.size(); ++i) fg[i] = geometry(fills[i].p, true, true);

    auto stroke = [&](size_t i, float width) {
      const Line& L = lines[i];
      ID2D1StrokeStyle* ss = L.dash ? static_cast<ID2D1StrokeStyle*>(dashed.Get()) : solid.Get();
      // dashes are in stroke widths: keep the outline's dashes the same absolute length as the green ones
      ComPtr<ID2D1StrokeStyle1> custom;
      if (L.dash && width != L.w) {
        D2D1_STROKE_STYLE_PROPERTIES1 sp = D2D1::StrokeStyleProperties1(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_BEVEL, 10.0f,
            D2D1_DASH_STYLE_CUSTOM, 0.0f, D2D1_STROKE_TRANSFORM_TYPE_NORMAL);
        float d[] = {6.0f * L.w / width, 4.0f * L.w / width};
        d2f->CreateStrokeStyle(sp, d, 2, &custom);
        ss = custom.Get();
      }
      if (lg[i]) c->DrawGeometry(lg[i].Get(), brush.Get(), width, ss);
      else c->DrawLine(L.p[0], L.p[1], brush.Get(), width, ss);
    };
    brush->SetColor(st.black);
    for (size_t i = 0; i < lines.size(); ++i) stroke(i, lines[i].w + st.outline);
    for (auto& f : fg)
      if (f) c->DrawGeometry(f.Get(), brush.Get(), st.outline, solid.Get());
    brush->SetColor(st.green);
    for (size_t i = 0; i < lines.size(); ++i) stroke(i, lines[i].w);
    for (auto& f : fg)
      if (f) c->FillGeometry(f.Get(), brush.Get());

    D2D1_MATRIX_3X2_F base;
    c->GetTransform(&base);
    for (auto& t : texts) {
      if (t.start < 0 || t.start + t.len > nstr || t.len <= 0) continue;
      IDWriteTextFormat* f = font(t.px);
      if (!f) continue;
      ComPtr<IDWriteTextLayout> lay;
      if (FAILED(dw->CreateTextLayout(str + t.start, static_cast<UINT32>(t.len), f, 4096.0f, 1024.0f, &lay))) continue;
      DWRITE_TEXT_METRICS tm;
      lay->GetMetrics(&tm);
      DWRITE_LINE_METRICS lm;
      UINT32 nl = 0;
      lay->GetLineMetrics(&lm, 1, &nl);
      const float wdt = tm.widthIncludingTrailingWhitespace;
      const float dx = t.align == 0 ? 0.0f : (t.align == 1 ? -wdt / 2 : -wdt);
      float x0, y0;
      if (t.ang != 0.0f) {
        c->SetTransform(D2D1::Matrix3x2F::Rotation(t.ang) * D2D1::Matrix3x2F::Translation(t.x, t.y) * base);
        x0 = dx;
        y0 = t.dy;
      } else {
        x0 = t.x + dx;
        y0 = t.y + t.dy;
      }
      y0 -= lm.baseline;  // positions are baselines
      brush->SetColor(st.halo);
      const float off[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
      for (auto& o : off) c->DrawTextLayout(D2D1::Point2F(x0 + o[0], y0 + o[1]), lay.Get(), brush.Get());
      brush->SetColor(st.green);
      c->DrawTextLayout(D2D1::Point2F(x0, y0), lay.Get(), brush.Get());
      if (t.ang != 0.0f) c->SetTransform(base);
    }
    lines.clear();
    fills.clear();
    texts.clear();
  }

  // Before BeginDraw: the opacity masks of the feathered clips (op 7). Each is the border polygon inset by half the
  // feather width, Gaussian-blurred (sigma = feather / 4): about 2 % at the border, 98 % one feather inside.
  void prepare(const float* cm, int n, int tw, int th) {
    masks.clear();
    fill_bmp = nullptr;
    int i = 0;
    auto skip_pts = [&](int k) { i += 2 * std::max(0, k); };
    while (i < n) {
      const int op = static_cast<int>(cm[i++]);
      if (op == 10) i += 13;
      else if (op == 1 && i + 4 <= n) { const int k = static_cast<int>(cm[i + 3]); i += 4; skip_pts(k); }
      else if ((op == 2 || op == 4) && i + 1 <= n) { const int k = static_cast<int>(cm[i++]); skip_pts(k); }
      else if (op == 3) i += 8;
      else if (op == 5 || op == 6) continue;
      else if (op == 8 && i + 5 <= n) { const int k = static_cast<int>(cm[i + 4]); i += 5; skip_pts(k); }
      else if (op == 11 && i + 19 <= n) { fill_bmp = terrain.render(d3d.Get(), dc.Get(), cm + i, tw, th); i += 19; }
      else if (op == 7 && i + 2 <= n) {
        const float feather = cm[i];
        const int k = static_cast<int>(cm[i + 1]);
        i += 2;
        std::vector<D2D1_POINT_2F> p;
        for (int j = 0; j < k && i + 1 < n; ++j, i += 2) p.push_back(D2D1::Point2F(cm[i], cm[i + 1]));
        masks.push_back(make_mask(p, feather, tw, th));
      } else break;
    }
  }

  Mask make_mask(const std::vector<D2D1_POINT_2F>& p, float feather, int tw, int th) {
    Mask m;
    if (p.size() < 3) return m;
    m.geo = geometry(p, true, true);
    if (!m.geo || feather < 0.5f) return m;
    HRESULT hr = S_OK;
    const size_t ix = masks.size();  // this mask's own effect (make_mask runs once per op 7, in order)
    if (blurs.size() <= ix) blurs.resize(ix + 1);
    ComPtr<ID2D1Effect>& blur = blurs[ix];
    if (!blur) hr = dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
    if (FAILED(hr)) return m;
    // inset = polygon minus its own outline stroked at the feather width
    ComPtr<ID2D1PathGeometry> wide, inset;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2f->CreatePathGeometry(&wide)) || FAILED(wide->Open(&sink))) return m;
    hr = m.geo->Widen(feather, solid.Get(), nullptr, 0.25f, sink.Get());
    sink->Close();
    sink.Reset();
    if (FAILED(hr) || FAILED(d2f->CreatePathGeometry(&inset)) || FAILED(inset->Open(&sink))) return m;
    hr = m.geo->CombineWithGeometry(wide.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, 0.25f, sink.Get());
    sink->Close();
    if (FAILED(hr)) return m;
    ComPtr<ID2D1CommandList> shape;
    if (FAILED(dc->CreateCommandList(&shape))) return m;
    ComPtr<ID2D1Image> old;
    dc->GetTarget(&old);
    dc->SetTarget(shape.Get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
    dc->FillGeometry(inset.Get(), brush.Get());
    hr = dc->EndDraw();
    dc->SetTarget(old.Get());
    if (FAILED(hr) || FAILED(shape->Close())) return m;
    blur->SetInput(0, shape.Get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, feather / 4.0f);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
    ComPtr<ID2D1Image> out;
    blur->GetOutput(&out);
    // the whole target as the source rectangle: brush space = target pixels either way
    dc->CreateImageBrush(out.Get(),
                         D2D1::ImageBrushProperties(D2D1::RectF(0, 0, static_cast<float>(tw), static_cast<float>(th))),
                         &m.brush);
    return m;
  }

  // Draws a command stream into the device context's current target (between BeginDraw / EndDraw).
  void draw_cmds(const float* cm, int n, const wchar_t* str, int nstr) {
    std::vector<Line> lines;
    std::vector<Fill> fills;
    std::vector<Text> texts;
    Style st;
    int clips = 0;
    size_t mask_i = 0;
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    int i = 0;
    auto pts = [&](int cnt) {
      std::vector<D2D1_POINT_2F> p;
      p.reserve(std::max(0, cnt));
      for (int k = 0; k < cnt && i + 1 < n; ++k, i += 2) p.push_back(D2D1::Point2F(cm[i], cm[i + 1]));
      return p;
    };
    while (i < n) {
      const int op = static_cast<int>(cm[i++]);
      if (op == 10 && i + 13 <= n) {
        st.green = D2D1::ColorF(cm[i], cm[i + 1], cm[i + 2], cm[i + 3]);
        st.black = D2D1::ColorF(cm[i + 4], cm[i + 5], cm[i + 6], cm[i + 7]);
        st.halo = D2D1::ColorF(cm[i + 8], cm[i + 9], cm[i + 10], cm[i + 11]);
        st.outline = cm[i + 12];
        i += 13;
      } else if (op == 1 && i + 4 <= n) {
        Line L{cm[i], cm[i + 1] != 0, cm[i + 2] != 0, {}};
        const int cnt = static_cast<int>(cm[i + 3]);
        i += 4;
        L.p = pts(cnt);
        if (L.p.size() >= 2) lines.push_back(std::move(L));
      } else if (op == 2 && i + 1 <= n) {
        const int cnt = static_cast<int>(cm[i++]);
        Fill F{pts(cnt)};
        if (F.p.size() >= 3) fills.push_back(std::move(F));
      } else if (op == 3 && i + 8 <= n) {
        texts.push_back({cm[i], cm[i + 1], cm[i + 2], cm[i + 4], cm[i + 5], static_cast<int>(cm[i + 3]),
                         static_cast<int>(cm[i + 6]), static_cast<int>(cm[i + 7])});
        i += 8;
      } else if (op == 4 && i + 1 <= n) {
        const int cnt = static_cast<int>(cm[i++]);
        auto p = pts(cnt);
        if (p.size() >= 3) {
          auto geo = geometry(p, true, true);
          if (geo) {
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                          nullptr);
            ++clips;
          }
        }
      } else if (op == 7 && i + 2 <= n) {  // feathered polygon clip (HUD border)
        const int cnt = static_cast<int>(cm[i + 1]);
        i += 2;
        pts(cnt);
        const Mask* mk = mask_i < masks.size() ? &masks[mask_i] : nullptr;
        ++mask_i;
        if (mk && mk->geo) {
          dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), mk->geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                               D2D1::IdentityMatrix(), 1.0f, mk->brush.Get()),
                        nullptr);
          ++clips;
        }
      } else if (op == 8 && i + 5 <= n) {  // fill everything outside a polygon (border editor's veil)
        flush(lines, fills, texts, st, str, nstr);
        const D2D1_COLOR_F col = D2D1::ColorF(cm[i], cm[i + 1], cm[i + 2], cm[i + 3]);
        const int cnt = static_cast<int>(cm[i + 4]);
        i += 5;
        auto p = pts(cnt);
        D2D1_SIZE_F sz = dc->GetSize();
        ComPtr<ID2D1RectangleGeometry> all;
        d2f->CreateRectangleGeometry(D2D1::RectF(-1, -1, sz.width + 1, sz.height + 1), &all);
        brush->SetColor(col);
        if (p.size() >= 3 && all) {
          ComPtr<ID2D1PathGeometry> hole = geometry(p, true, true);
          ID2D1Geometry* parts[2] = {all.Get(), hole.Get()};
          ComPtr<ID2D1GeometryGroup> grp;
          if (hole && SUCCEEDED(d2f->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, parts, 2, &grp)))
            dc->FillGeometry(grp.Get(), brush.Get());
        } else if (all) {
          dc->FillGeometry(all.Get(), brush.Get());
        }
      } else if (op == 11 && i + 19 <= n) {  // terrain fill: the HUD colour through the opacity Direct3D drew
        flush(lines, fills, texts, st, str, nstr);
        if (fill_bmp) {
          D2D1_COLOR_F col = st.green;
          col.a *= std::clamp(cm[i], 0.0f, 1.0f);
          brush->SetColor(col);
          dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // FillOpacityMask needs it
          dc->FillOpacityMask(fill_bmp, brush.Get(), nullptr, nullptr);
          dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        i += 19;
      } else if (op == 5) {
        if (clips > 0) {
          dc->PopLayer();
          --clips;
        }
      } else if (op == 6) {
        flush(lines, fills, texts, st, str, nstr);
      } else {
        break;  // malformed
      }
    }
    flush(lines, fills, texts, st, str, nstr);
    while (clips-- > 0) dc->PopLayer();
  }

  void handle_loss(HRESULT hr) {
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
      set_err(L"device lost, recreating", hr);
      release_device();
      create_device(hwnd != nullptr);
    }
  }
};

namespace {
std::vector<float> to_floats(const hud::CommandList& cl) {
  std::vector<float> f(cl.cmds.size());
  for (size_t i = 0; i < f.size(); ++i) f[i] = static_cast<float>(cl.cmds[i]);
  return f;
}
}  // namespace

D2DRenderer::D2DRenderer() : d_(std::make_unique<Impl>()) {}

D2DRenderer::~D2DRenderer() {
  d_->release_device();
  if (d_->hwnd) DestroyWindow(d_->hwnd);
}

bool D2DRenderer::init_offscreen() {
  if (d_->dc) return true;
  return SUCCEEDED(d_->create_device(false));
}

bool D2DRenderer::init_window(int x, int y, int w, int h) {
  if (d_->hwnd) return true;
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = overlay_wndproc;
  wc.hInstance = inst;
  wc.lpszClassName = L"BirdieHudOverlay";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  d_->x = x, d_->y = y, d_->w = std::max(1, w), d_->h = std::max(1, h);
  d_->hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT |
                                 WS_EX_LAYERED | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Birdie HUD", WS_POPUP, d_->x, d_->y, d_->w, d_->h, nullptr, nullptr,
                             inst, nullptr);
  if (!d_->hwnd) {
    d_->set_err(L"CreateWindowEx", HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  SetLayeredWindowAttributes(d_->hwnd, 0, 255, LWA_ALPHA);
  // Windows 11 draws a 1 px accent border and rounded corners on frameless windows: turn both off
  DWORD v = 1;
  DwmSetWindowAttribute(d_->hwnd, 2, &v, sizeof(v));   // DWMWA_NCRENDERING_POLICY = disabled
  DwmSetWindowAttribute(d_->hwnd, 33, &v, sizeof(v));  // DWMWA_WINDOW_CORNER_PREFERENCE = do not round
  DWORD none = 0xFFFFFFFE;
  DwmSetWindowAttribute(d_->hwnd, 34, &none, sizeof(none));  // DWMWA_BORDER_COLOR = none
  return SUCCEEDED(d_->create_device(true));
}

bool D2DRenderer::move(int x, int y, int w, int h) {
  if (!d_->hwnd) return false;
  w = std::max(1, w);
  h = std::max(1, h);
  const bool resize = (w != d_->w || h != d_->h);
  d_->x = x, d_->y = y, d_->w = w, d_->h = h;
  SetWindowPos(d_->hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
  if (resize && d_->swap) {
    d_->dc->SetTarget(nullptr);
    d_->target.Reset();
    HRESULT hr = d_->swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      d_->set_err(L"ResizeBuffers", hr);
      return false;
    }
    hr = d_->make_target();
    if (FAILED(hr)) {
      d_->set_err(L"target bitmap", hr);
      return false;
    }
    d_->blank = false;
  }
  return true;
}

void D2DRenderer::show(bool on) {
  if (!d_->hwnd || on == d_->shown) return;
  d_->shown = on;
  ShowWindow(d_->hwnd, on ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool D2DRenderer::visible() const { return d_->shown; }

void D2DRenderer::set_input(InputHook hook) {
  if (!d_->hwnd) return;
  d_->input = std::move(hook);
  const bool on = static_cast<bool>(d_->input);
  SetWindowLongPtrW(d_->hwnd, GWLP_USERDATA, on ? reinterpret_cast<LONG_PTR>(&d_->input) : 0);
  LONG_PTR ex = GetWindowLongPtrW(d_->hwnd, GWL_EXSTYLE);
  const LONG_PTR passive = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
  ex = on ? (ex & ~passive) : (ex | passive);
  SetWindowLongPtrW(d_->hwnd, GWL_EXSTYLE, ex);
  SetWindowPos(d_->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
HWND D2DRenderer::hwnd() const { return d_->hwnd; }

bool D2DRenderer::frame(const hud::CommandList& cl) {
  if (!d_->dc || !d_->swap) return false;
  if (cl.cmds.empty() && d_->blank) return true;  // nothing to draw and the screen is already clear
  const auto f = to_floats(cl);
  d_->prepare(f.data(), static_cast<int>(f.size()), d_->w, d_->h);
  d_->dc->BeginDraw();
  d_->draw_cmds(f.data(), static_cast<int>(f.size()), cl.text.c_str(), static_cast<int>(cl.text.size()));
  HRESULT hr = d_->dc->EndDraw();
  if (SUCCEEDED(hr)) hr = d_->swap->Present(0, 0);
  d_->blank = cl.cmds.empty();
  if (FAILED(hr)) {
    d_->set_err(L"EndDraw/Present", hr);
    d_->handle_loss(hr);  // recreates the device (next frame draws again)
    return false;
  }
  return true;
}

bool D2DRenderer::render_to_memory(const hud::CommandList& cl, int w, int h, std::vector<std::uint8_t>& out) {
  if (!d_->dc) return false;
  D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1Bitmap1> tgt, cpu;
  HRESULT hr = d_->dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &tp, &tgt);
  if (FAILED(hr)) {
    d_->set_err(L"CreateBitmap target", hr);
    return false;
  }
  D2D1_BITMAP_PROPERTIES1 cp =
      D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  hr = d_->dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &cp, &cpu);
  if (FAILED(hr)) {
    d_->set_err(L"CreateBitmap cpu", hr);
    return false;
  }
  const auto f = to_floats(cl);
  d_->prepare(f.data(), static_cast<int>(f.size()), w, h);
  ComPtr<ID2D1Image> old;
  d_->dc->GetTarget(&old);
  d_->dc->SetTarget(tgt.Get());
  d_->dc->BeginDraw();
  d_->draw_cmds(f.data(), static_cast<int>(f.size()), cl.text.c_str(), static_cast<int>(cl.text.size()));
  hr = d_->dc->EndDraw();
  d_->dc->SetTarget(old.Get());
  if (FAILED(hr)) {
    d_->set_err(L"EndDraw (memory)", hr);
    return false;
  }
  cpu->CopyFromBitmap(nullptr, tgt.Get(), nullptr);
  D2D1_MAPPED_RECT m;
  hr = cpu->Map(D2D1_MAP_OPTIONS_READ, &m);
  if (FAILED(hr)) {
    d_->set_err(L"Map", hr);
    return false;
  }
  out.resize(static_cast<size_t>(w) * h * 4);
  for (int yy = 0; yy < h; ++yy)
    std::memcpy(out.data() + static_cast<size_t>(yy) * w * 4, m.bits + static_cast<size_t>(yy) * m.pitch,
                static_cast<size_t>(w) * 4);
  cpu->Unmap();
  return true;
}

void D2DRenderer::set_font_family(const std::wstring& family) {
  d_->family = family;
  d_->fonts.clear();
}

const std::wstring& D2DRenderer::error() const { return d_->err; }

}  // namespace birdie::render
