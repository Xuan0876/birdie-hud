// hudgfx.dll — GPU renderer for the FPV overlay HUD.
//
// A click-through, topmost, transparent popup window whose content is a DirectComposition swap chain drawn with
// Direct2D / DirectWrite. Python builds the frame as a flat float command list (see hudgfx.py) and calls
// hg_frame() once per sim frame; drawing happens on the GPU and DWM composites it with per-pixel alpha
// (no layered-window bitmap upload).
//
// Build: native\build.bat (MSVC x64).  Everything runs on the caller's thread; the window's messages are pumped by
// the caller's event loop (Qt does this for every window on its thread).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

#define API extern "C" __declspec(dllexport)

namespace {

wchar_t g_err[512] = L"";
void set_err(const wchar_t* what, HRESULT hr) { swprintf_s(g_err, L"%s (hr=0x%08lX)", what, (unsigned long)hr); }

struct Ctx {
    HWND hwnd = nullptr;
    int x = 0, y = 0, w = 0, h = 0;
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
    std::wstring family = L"Bahnschrift";
    bool blank = false;  // last frame presented was empty
} g;

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, m, wp, lp);
}

HRESULT make_target() {
    g.dc->SetTarget(nullptr);
    g.target.Reset();
    ComPtr<IDXGISurface> surf;
    HRESULT hr = g.swap->GetBuffer(0, IID_PPV_ARGS(&surf));
    if (FAILED(hr)) return hr;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = g.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &g.target);
    if (FAILED(hr)) return hr;
    g.dc->SetTarget(g.target.Get());
    return S_OK;
}

HRESULT create_device() {
    HRESULT hr;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl,
                           ARRAYSIZE(fl), D3D11_SDK_VERSION, &g.d3d, nullptr, nullptr);
    if (FAILED(hr)) { set_err(L"D3D11CreateDevice", hr); return hr; }
    g.d3d.As(&g.dxgi);
    ComPtr<IDXGIDevice1> d1;
    if (SUCCEEDED(g.dxgi.As(&d1))) d1->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> ad;
    g.dxgi->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> fac;
    ad->GetParent(IID_PPV_ARGS(&fac));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = std::max(1, g.w);
    sd.Height = std::max(1, g.h);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = fac->CreateSwapChainForComposition(g.d3d.Get(), &sd, nullptr, &g.swap);
    if (FAILED(hr)) { set_err(L"CreateSwapChainForComposition", hr); return hr; }

    D2D1_FACTORY_OPTIONS fo = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)g.d2f.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { set_err(L"D2D1CreateFactory", hr); return hr; }
    hr = g.d2f->CreateDevice(g.dxgi.Get(), &g.d2dev);
    if (FAILED(hr)) { set_err(L"ID2D1Factory1::CreateDevice", hr); return hr; }
    hr = g.d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.dc);
    if (FAILED(hr)) { set_err(L"CreateDeviceContext", hr); return hr; }
    g.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g.dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    hr = make_target();
    if (FAILED(hr)) { set_err(L"target bitmap", hr); return hr; }
    g.dc->CreateSolidColorBrush(D2D1::ColorF(0, 1, 0, 1), &g.brush);

    // square caps / bevel joins like the Qt version; dashes 6 on 4 off in stroke widths, flat dash caps
    D2D1_STROKE_STYLE_PROPERTIES1 sp = D2D1::StrokeStyleProperties1(
        D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_BEVEL, 10.0f,
        D2D1_DASH_STYLE_SOLID, 0.0f, D2D1_STROKE_TRANSFORM_TYPE_NORMAL);
    g.d2f->CreateStrokeStyle(sp, nullptr, 0, &g.solid);
    sp.startCap = sp.endCap = D2D1_CAP_STYLE_FLAT;
    sp.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    float dashes[] = {6.0f, 4.0f};
    g.d2f->CreateStrokeStyle(sp, dashes, 2, &g.dashed);

    hr = DCompositionCreateDevice(g.dxgi.Get(), IID_PPV_ARGS(&g.dcomp));
    if (FAILED(hr)) { set_err(L"DCompositionCreateDevice", hr); return hr; }
    hr = g.dcomp->CreateTargetForHwnd(g.hwnd, TRUE, &g.dtarget);
    if (FAILED(hr)) { set_err(L"CreateTargetForHwnd", hr); return hr; }
    g.dcomp->CreateVisual(&g.visual);
    g.visual->SetContent(g.swap.Get());
    g.dtarget->SetRoot(g.visual.Get());
    hr = g.dcomp->Commit();
    if (FAILED(hr)) { set_err(L"DComp Commit", hr); return hr; }
    return S_OK;
}

void release_device() {
    g.fonts.clear();
    g.brush.Reset(); g.solid.Reset(); g.dashed.Reset();
    if (g.dc) g.dc->SetTarget(nullptr);
    g.target.Reset(); g.dc.Reset(); g.d2dev.Reset(); g.d2f.Reset();
    g.visual.Reset(); g.dtarget.Reset(); g.dcomp.Reset();
    g.swap.Reset(); g.dxgi.Reset(); g.d3d.Reset();
}

IDWriteTextFormat* font(float px) {
    int key = (int)std::lround(px * 4.0f);
    auto it = g.fonts.find(key);
    if (it != g.fonts.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(g.dw->CreateTextFormat(g.family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_SEMI_CONDENSED, std::max(1, key / 4) * 1.0f, L"en-us", &f)))
        return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g.fonts[key] = f;
    return f.Get();
}

// ---------------------------------------------------------------- frame drawing
struct Line { float w; bool dash, closed; std::vector<D2D1_POINT_2F> p; };
struct Fill { std::vector<D2D1_POINT_2F> p; };
struct Text { float x, y, px, ang, dy; int align, start, len; };
struct Style { D2D1_COLOR_F green{0, 1, 0, 1}, black{0, 0, 0, 0.745f}, halo{0, 0, 0, 0.47f}; float outline = 2.4f; };

ComPtr<ID2D1PathGeometry> geometry(const std::vector<D2D1_POINT_2F>& p, bool closed, bool filled) {
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(g.d2f->CreatePathGeometry(&geo))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure(p[0], filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(p.data() + 1, (UINT32)(p.size() - 1));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
}

void flush(std::vector<Line>& lines, std::vector<Fill>& fills, std::vector<Text>& texts, const Style& st,
           const wchar_t* str, int nstr) {
    auto* dc = g.dc.Get();
    std::vector<ComPtr<ID2D1PathGeometry>> lg(lines.size()), fg(fills.size());
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].p.size() > 2 || lines[i].closed) lg[i] = geometry(lines[i].p, lines[i].closed, false);
    for (size_t i = 0; i < fills.size(); ++i) fg[i] = geometry(fills[i].p, true, true);

    auto stroke = [&](size_t i, float width) {
        const Line& L = lines[i];
        ID2D1StrokeStyle* ss = L.dash ? (ID2D1StrokeStyle*)g.dashed.Get() : (ID2D1StrokeStyle*)g.solid.Get();
        // dashes are in stroke widths: keep the outline's dashes the same absolute length as the green ones
        ComPtr<ID2D1StrokeStyle1> custom;
        if (L.dash && width != L.w) {
            D2D1_STROKE_STYLE_PROPERTIES1 sp = D2D1::StrokeStyleProperties1(
                D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_BEVEL, 10.0f,
                D2D1_DASH_STYLE_CUSTOM, 0.0f, D2D1_STROKE_TRANSFORM_TYPE_NORMAL);
            float d[] = {6.0f * L.w / width, 4.0f * L.w / width};
            g.d2f->CreateStrokeStyle(sp, d, 2, &custom);
            ss = custom.Get();
        }
        if (lg[i]) dc->DrawGeometry(lg[i].Get(), g.brush.Get(), width, ss);
        else dc->DrawLine(L.p[0], L.p[1], g.brush.Get(), width, ss);
    };
    g.brush->SetColor(st.black);
    for (size_t i = 0; i < lines.size(); ++i) stroke(i, lines[i].w + st.outline);
    for (auto& f : fg) if (f) dc->DrawGeometry(f.Get(), g.brush.Get(), st.outline, g.solid.Get());
    g.brush->SetColor(st.green);
    for (size_t i = 0; i < lines.size(); ++i) stroke(i, lines[i].w);
    for (auto& f : fg) if (f) dc->FillGeometry(f.Get(), g.brush.Get());

    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    for (auto& t : texts) {
        if (t.start < 0 || t.start + t.len > nstr || t.len <= 0) continue;
        IDWriteTextFormat* f = font(t.px);
        if (!f) continue;
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(g.dw->CreateTextLayout(str + t.start, (UINT32)t.len, f, 4096.0f, 1024.0f, &lay))) continue;
        DWRITE_TEXT_METRICS tm;
        lay->GetMetrics(&tm);
        DWRITE_LINE_METRICS lm;
        UINT32 nl = 0;
        lay->GetLineMetrics(&lm, 1, &nl);
        float wdt = tm.widthIncludingTrailingWhitespace;
        float dx = t.align == 0 ? 0.0f : (t.align == 1 ? -wdt / 2 : -wdt);
        float x0, y0;
        if (t.ang != 0.0f) {
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(t.ang) * D2D1::Matrix3x2F::Translation(t.x, t.y) * base);
            x0 = dx; y0 = t.dy;
        } else {
            x0 = t.x + dx; y0 = t.y + t.dy;
        }
        y0 -= lm.baseline;  // the Python side gives baselines (Qt drawText semantics)
        g.brush->SetColor(st.halo);
        const float off[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (auto& o : off) dc->DrawTextLayout(D2D1::Point2F(x0 + o[0], y0 + o[1]), lay.Get(), g.brush.Get());
        g.brush->SetColor(st.green);
        dc->DrawTextLayout(D2D1::Point2F(x0, y0), lay.Get(), g.brush.Get());
        if (t.ang != 0.0f) dc->SetTransform(base);
    }
    lines.clear(); fills.clear(); texts.clear();
}

}  // namespace

// ---------------------------------------------------------------- API
API const wchar_t* hg_error() { return g_err; }

API int hg_init(int x, int y, int w, int h) {
    if (g.hwnd) return 0;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HudGfxOverlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    g.x = x; g.y = y; g.w = std::max(1, w); g.h = std::max(1, h);
    g.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT |
                                 WS_EX_LAYERED | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"FPV HUD", WS_POPUP, g.x, g.y, g.w, g.h, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) { set_err(L"CreateWindowEx", HRESULT_FROM_WIN32(GetLastError())); return 1; }
    SetLayeredWindowAttributes(g.hwnd, 0, 255, LWA_ALPHA);
    DWORD v = 1;
    DwmSetWindowAttribute(g.hwnd, 2, &v, sizeof(v));        // DWMWA_NCRENDERING_POLICY = disabled
    DwmSetWindowAttribute(g.hwnd, 33, &v, sizeof(v));       // DWMWA_WINDOW_CORNER_PREFERENCE = do not round
    DWORD none = 0xFFFFFFFE;
    DwmSetWindowAttribute(g.hwnd, 34, &none, sizeof(none)); // DWMWA_BORDER_COLOR = none
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)g.dw.GetAddressOf());
    if (FAILED(hr)) { set_err(L"DWriteCreateFactory", hr); return 2; }
    if (FAILED(create_device())) return 3;
    return 0;
}

API int hg_hwnd() { return (int)(intptr_t)g.hwnd; }

API void hg_set_family(const wchar_t* fam) { g.family = fam; g.fonts.clear(); }

API int hg_move(int x, int y, int w, int h) {
    if (!g.hwnd) return 1;
    w = std::max(1, w); h = std::max(1, h);
    bool resize = (w != g.w || h != g.h);
    g.x = x; g.y = y; g.w = w; g.h = h;
    SetWindowPos(g.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    if (resize && g.swap) {
        g.dc->SetTarget(nullptr);
        g.target.Reset();
        HRESULT hr = g.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) { set_err(L"ResizeBuffers", hr); return 2; }
        if (FAILED(make_target())) return 3;
        g.blank = false;
    }
    return 0;
}

API void hg_show(int on) {
    if (g.hwnd) ShowWindow(g.hwnd, on ? SW_SHOWNOACTIVATE : SW_HIDE);
}

namespace {
// Draws a command stream into the device context's current target (between BeginDraw / EndDraw).
void draw_cmds(const float* c, int n, const wchar_t* str, int nstr) {
    std::vector<Line> lines; std::vector<Fill> fills; std::vector<Text> texts;
    Style st;
    int clips = 0;
    g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    g.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    int i = 0;
    auto pts = [&](int cnt) {
        std::vector<D2D1_POINT_2F> p;
        p.reserve(cnt);
        for (int k = 0; k < cnt && i + 1 < n; ++k, i += 2) p.push_back(D2D1::Point2F(c[i], c[i + 1]));
        return p;
    };
    while (i < n) {
        int op = (int)c[i++];
        if (op == 10 && i + 13 <= n) {  // style: green rgba, black rgba, halo rgba (straight alpha), outline
            st.green = D2D1::ColorF(c[i], c[i + 1], c[i + 2], c[i + 3]);
            st.black = D2D1::ColorF(c[i + 4], c[i + 5], c[i + 6], c[i + 7]);
            st.halo = D2D1::ColorF(c[i + 8], c[i + 9], c[i + 10], c[i + 11]);
            st.outline = c[i + 12];
            i += 13;
        } else if (op == 1 && i + 4 <= n) {  // line: width, dash, closed, count, xy...
            Line L{c[i], c[i + 1] != 0, c[i + 2] != 0, {}};
            int cnt = (int)c[i + 3];
            i += 4;
            L.p = pts(cnt);
            if (L.p.size() >= 2) lines.push_back(std::move(L));
        } else if (op == 2 && i + 1 <= n) {  // fill: count, xy...
            int cnt = (int)c[i++];
            Fill F{pts(cnt)};
            if (F.p.size() >= 3) fills.push_back(std::move(F));
        } else if (op == 3 && i + 8 <= n) {  // text: x, y, px, align, angle, dy, start, len
            texts.push_back({c[i], c[i + 1], c[i + 2], c[i + 4], c[i + 5], (int)c[i + 3], (int)c[i + 6], (int)c[i + 7]});
            i += 8;
        } else if (op == 4 && i + 1 <= n) {  // push polygon clip
            int cnt = (int)c[i++];
            auto p = pts(cnt);
            if (p.size() >= 3) {
                auto geo = geometry(p, true, true);
                if (geo) {
                    g.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                                    nullptr);
                    ++clips;
                }
            }
        } else if (op == 5) {  // pop clip
            if (clips > 0) { g.dc->PopLayer(); --clips; }
        } else if (op == 6) {  // flush the batch collected so far (outline pass, colour pass, text)
            flush(lines, fills, texts, st, str, nstr);
        } else {
            break;  // malformed
        }
    }
    flush(lines, fills, texts, st, str, nstr);
    while (clips-- > 0) g.dc->PopLayer();
}

HRESULT handle_loss(HRESULT hr) {
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        set_err(L"device lost, recreating", hr);
        release_device();
        create_device();
    }
    return hr;
}
}  // namespace

// cmds: float stream, see hud.commands(). Returns 0 ok, else error (device is recreated on loss).
API int hg_frame(const float* c, int n, const wchar_t* str, int nstr) {
    if (!g.dc) return 1;
    if (n == 0 && g.blank) return 0;  // nothing to draw and the screen is already clear
    g.dc->BeginDraw();
    draw_cmds(c, n, str, nstr);
    HRESULT hr = g.dc->EndDraw();
    if (SUCCEEDED(hr)) hr = g.swap->Present(0, 0);
    g.blank = (n == 0);
    if (FAILED(hr)) {
        set_err(L"EndDraw/Present", hr);
        handle_loss(hr);  // recreates the device (next frame draws again)
        return 3;
    }
    return 0;
}

// Offline check: draw into a w x h memory bitmap and copy premultiplied BGRA rows into out (w*h*4 bytes).
API int hg_render_to_memory(const float* c, int n, const wchar_t* str, int nstr, int w, int h, unsigned char* out) {
    if (!g.dc) return 1;
    D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> tgt, cpu;
    HRESULT hr = g.dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &tp, &tgt);
    if (FAILED(hr)) { set_err(L"CreateBitmap target", hr); return 2; }
    D2D1_BITMAP_PROPERTIES1 cp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = g.dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &cp, &cpu);
    if (FAILED(hr)) { set_err(L"CreateBitmap cpu", hr); return 3; }
    ComPtr<ID2D1Image> old;
    g.dc->GetTarget(&old);
    g.dc->SetTarget(tgt.Get());
    g.dc->BeginDraw();
    draw_cmds(c, n, str, nstr);
    hr = g.dc->EndDraw();
    g.dc->SetTarget(old.Get());
    if (FAILED(hr)) { set_err(L"EndDraw (memory)", hr); return 4; }
    cpu->CopyFromBitmap(nullptr, tgt.Get(), nullptr);
    D2D1_MAPPED_RECT m;
    hr = cpu->Map(D2D1_MAP_OPTIONS_READ, &m);
    if (FAILED(hr)) { set_err(L"Map", hr); return 5; }
    for (int y = 0; y < h; ++y) memcpy(out + (size_t)y * w * 4, m.bits + (size_t)y * m.pitch, (size_t)w * 4);
    cpu->Unmap();
    return 0;
}

API void hg_shutdown() {
    release_device();
    if (g.hwnd) DestroyWindow(g.hwnd);
    g.hwnd = nullptr;
}
