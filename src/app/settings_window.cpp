// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/settings_window.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <WebView2.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>

#include <nlohmann/json.hpp>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")

namespace birdie::app {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT WM_HOST = WM_APP + 40;  // wParam: kHostClose / kHostDrag (posted from a WebView2 event handler)
enum : WPARAM { kHostClose = 1, kHostDrag, kHostFit, kHostOpen };
// The panel's look (settings ui_style, birdie.css): the window's own background (seen before the page paints) and
// DWM's 1 px border, toned to the panel. One look for every Birdie window.
struct Look {
  COLORREF bg, border;
  bool dark;
};
Look look_of(const std::string& style) {
  if (style == "light") return {RGB(0xf1, 0xf1, 0xf0), RGB(0xc4, 0xc4, 0xc2), false};
  if (style == "dusk") return {RGB(0x1e, 0x1e, 0x1f), RGB(0x3a, 0x35, 0x36), true};
  return {RGB(0x1c, 0x1c, 0x1e), RGB(0x3a, 0x3a, 0x3d), true};  // tool
}
Look g_look = look_of("tool");
constexpr int kCssStartHeight = 380;

std::string utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::filesystem::path local_data_dir() {  // %LOCALAPPDATA%\Birdie\WebView2: browser cache, not roaming settings
  PWSTR p = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = std::filesystem::path(p) / L"Birdie" / L"WebView2";
  CoTaskMemFree(p);
  std::error_code ec;
  std::filesystem::create_directories(out, ec);
  return out;
}

}  // namespace

struct SettingsWindow::Impl {
  bool is_peek = false;
  RECT anchor{};  // peek: the tray icon
  HWND hwnd = nullptr;
  Host host;
  std::wstring url, origin;
  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller> ctl;
  ComPtr<ICoreWebView2> wv;
  int gen = 0;            // bumped on close: callbacks of an older browser are ignored
  bool want_show = false;  // shown once the page has loaded (no empty flash)
  int css_h = kCssStartHeight;
  int css_w = kCssWidth;

  void log(const std::string& s) const {
    if (host.log) host.log(s);
  }
  double scale() const { return hwnd ? GetDpiForWindow(hwnd) / 96.0 : 1.0; }

  static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->on_message(h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
  }

  LRESULT on_message(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
      case WM_NCCALCSIZE:  // no Windows frame: the page is the whole window (the caption style is kept for the shadow)
        if (wp) return 0;
        break;
      case WM_ERASEBKGND: {  // before the page paints: the look's background
        RECT r;
        GetClientRect(h, &r);
        HBRUSH br = CreateSolidBrush(g_look.bg);
        FillRect(reinterpret_cast<HDC>(wp), &r, br);
        DeleteObject(br);
        return 1;
      }
      case WM_NCHITTEST:  // nothing to resize; dragging is the page's head row (app-region: drag)
        return HTCLIENT;
      case WM_MOUSEACTIVATE:
        if (is_peek) return MA_NOACTIVATE;
        break;
      case WM_SIZE:
        if (ctl) {
          RECT rc;
          GetClientRect(h, &rc);
          ctl->put_Bounds(rc);
        }
        return 0;
      case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
          case SC_MAXIMIZE: case SC_SIZE: case SC_MINIMIZE: case SC_RESTORE: return 0;
        }
        break;
      case WM_GETMINMAXINFO:
        break;
      case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        place(r->left, r->top);
        return 0;
      }
      case WM_EXITSIZEMOVE: {
        RECT r;
        GetWindowRect(h, &r);
        if (host.moved) host.moved(r.left, r.top);
        return 0;
      }
      case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && ctl) ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        break;
      case WM_CLOSE:  // Alt+F4, the taskbar: hide, Birdie stays
        hide();
        return 0;
      case WM_HOST:
        if (wp == kHostClose) hide();
        else if (wp == kHostOpen) {
          hide();
          if (host.open_full) host.open_full();
        }
        else if (wp == kHostDrag) {
          ReleaseCapture();
          SendMessageW(h, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
        } else if (wp == kHostFit) {
          refit();
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
  }

  void refit() {
    if (is_peek) {
      place_peek();
      return;
    }
    RECT r;
    GetWindowRect(hwnd, &r);
    place(r.left, r.top);
  }

  // Peek: centred on the tray icon, on the side of it away from the taskbar, inside the work area.
  void place_peek() {
    const POINT c = {(anchor.left + anchor.right) / 2, (anchor.top + anchor.bottom) / 2};
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    const double k = dx / 96.0;
    const int w = static_cast<int>(std::lround(css_w * k)), hh = static_cast<int>(std::lround(css_h * k));
    const int gap = static_cast<int>(std::lround(10 * k));
    int x = c.x - w / 2, y;
    if (anchor.top >= wa.bottom - 2) y = wa.bottom - gap - hh;  // taskbar at the bottom (the usual)
    else if (anchor.bottom <= wa.top + 2) y = wa.top + gap;      // at the top
    else y = c.y - hh / 2;                                        // left / right: beside it
    if (anchor.right <= wa.left + 2) x = wa.left + gap;
    else if (anchor.left >= wa.right - 2) x = wa.right - gap - w;
    x = std::clamp<int>(x, wa.left + gap, std::max<int>(wa.left, wa.right - gap - w));
    y = std::clamp<int>(y, wa.top, std::max<int>(wa.top, wa.bottom - hh));
    SetWindowPos(hwnd, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  // Size from the page (css px) at this monitor's scale; top-left kept, kept inside the work area.
  void place(int x, int y) {
    const double k = scale();
    const int w = static_cast<int>(std::lround(css_w * k)), hh = static_cast<int>(std::lround(css_h * k));
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({x + w / 2, y + 10}, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    const int h2 = std::min<int>(hh, wa.bottom - wa.top);
    x = std::clamp<int>(x, wa.left, std::max<int>(wa.left, wa.right - w));
    y = std::clamp<int>(y, wa.top, std::max<int>(wa.top, wa.bottom - h2));
    SetWindowPos(hwnd, nullptr, x, y, w, h2, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void apply_look() {
    if (!hwnd) return;
    const COLORREF border = g_look.border;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    const BOOL dark = g_look.dark;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    ComPtr<ICoreWebView2Controller2> c2;
    if (ctl && SUCCEEDED(ctl.As(&c2)))
      c2->put_DefaultBackgroundColor({255, GetRValue(g_look.bg), GetGValue(g_look.bg), GetBValue(g_look.bg)});
    InvalidateRect(hwnd, nullptr, TRUE);
  }

  bool create_window(HICON big_icon, HICON small_icon) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
      WNDCLASSEXW wc = {sizeof(wc)};
      wc.lpfnWndProc = &Impl::wndproc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.hbrBackground = nullptr;  // WM_ERASEBKGND paints the look's colour
      wc.lpszClassName = L"BirdieSettings";
      registered = RegisterClassExW(&wc) != 0;
    }
    // WS_CAPTION only for DWM's shadow; WM_NCCALCSIZE removes the frame itself. Topmost: it sits over the sim like
    // the toolbar panel does, and above Birdie's own (topmost) HUD layer.
    // The peek: no taskbar button, never activated.
    const DWORD ex = is_peek ? WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE : WS_EX_APPWINDOW | WS_EX_TOPMOST;
    hwnd = CreateWindowExW(ex, L"BirdieSettings", L"Birdie", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                           0, 0, 100, 100, nullptr, nullptr, inst, this);
    if (!hwnd) return false;
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;  // user's rule: no rounded rectangles
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    apply_look();
    const MARGINS shadow = {0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &shadow);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    return true;
  }

  void bring_up() {
    if (is_peek) {
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      if (ctl) ctl->put_IsVisible(TRUE);
      if (wv) wv->PostWebMessageAsString(L"show");  // the page lifts its line in again
      return;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd);
    if (ctl) {
      ctl->put_IsVisible(TRUE);  // made while the window was hidden: WebView2 keeps its own windows hidden until told
      ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
  }

  bool start_browser() {
    const int my = ++gen;
    const std::wstring udf = local_data_dir().wstring();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this, my](HRESULT r, ICoreWebView2Environment* e) -> HRESULT {
          if (my != gen) return S_OK;
          if (FAILED(r) || !e) {
            log("[win] WebView2 environment failed: 0x" + hex(r));
            fail();
            return S_OK;
          }
          env = e;
          return env->CreateCoreWebView2Controller(
              hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this, my](HRESULT r2, ICoreWebView2Controller* c) -> HRESULT {
                if (my != gen) {
                  if (c) c->Close();
                  return S_OK;
                }
                if (FAILED(r2) || !c) {
                  log("[win] WebView2 controller failed: 0x" + hex(r2));
                  fail();
                  return S_OK;
                }
                on_controller(c);
                return S_OK;
              }).Get());
        }).Get());
    if (FAILED(hr)) {
      log("[win] WebView2 could not start: 0x" + hex(hr));
      return false;
    }
    return true;
  }

  void on_controller(ICoreWebView2Controller* c) {
    ctl = c;
    ctl->get_CoreWebView2(&wv);
    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(ctl.As(&c2)))
      c2->put_DefaultBackgroundColor({255, GetRValue(g_look.bg), GetGValue(g_look.bg), GetBValue(g_look.bg)});
    ComPtr<ICoreWebView2Settings> s;
    if (SUCCEEDED(wv->get_Settings(&s))) {
#ifdef NDEBUG
      s->put_AreDevToolsEnabled(FALSE);
#endif
      s->put_AreDefaultContextMenusEnabled(FALSE);
      s->put_IsZoomControlEnabled(FALSE);
      s->put_IsStatusBarEnabled(FALSE);
      s->put_AreDefaultScriptDialogsEnabled(FALSE);
      ComPtr<ICoreWebView2Settings3> s3;
      if (SUCCEEDED(s.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
      ComPtr<ICoreWebView2Settings9> s9;  // CSS app-region: drag on the page's head row moves the window
      if (SUCCEEDED(s.As(&s9))) s9->put_IsNonClientRegionSupportEnabled(TRUE);
      else log("[win] WebView2 without app-region support: dragging goes through the page");
    }
    const int my = gen;
    EventRegistrationToken tok;
    // only Birdie's own page: anything else (a link, a redirect) is refused
    wv->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
      LPWSTR uri = nullptr;
      a->get_Uri(&uri);
      const std::wstring u = uri ? uri : L"";
      CoTaskMemFree(uri);
      if (u.compare(0, origin.size(), origin) != 0) {
        a->put_Cancel(TRUE);
        log("[win] blocked navigation to " + utf8(u.c_str()));
      }
      return S_OK;
    }).Get(), &tok);
    wv->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
      a->put_Handled(TRUE);
      return S_OK;
    }).Get(), &tok);
    wv->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([this, my](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
      if (my != gen) return S_OK;
      BOOL ok = FALSE;
      a->get_IsSuccess(&ok);
      if (!ok) log("[win] the panel page did not load");
      if (want_show) bring_up();
      return S_OK;
    }).Get(), &tok);
    wv->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
      LPWSTR src = nullptr;
      a->get_Source(&src);
      const bool ours = src && std::wstring(src).compare(0, origin.size(), origin) == 0;
      CoTaskMemFree(src);
      LPWSTR msg = nullptr;
      if (!ours || FAILED(a->TryGetWebMessageAsString(&msg)) || !msg) return S_OK;
      on_page(utf8(msg));
      CoTaskMemFree(msg);
      return S_OK;
    }).Get(), &tok);
    RECT rc;
    GetClientRect(hwnd, &rc);
    ctl->put_Bounds(rc);
    wv->Navigate(url.c_str());
  }

  void on_page(const std::string& text) {
    nlohmann::json m = nlohmann::json::parse(text, nullptr, false);
    if (!m.is_object()) return;
    const std::string what = m.value("host", "");
    if (what == "close") {
      PostMessageW(hwnd, WM_HOST, kHostClose, 0);
    } else if (what == "open") {
      PostMessageW(hwnd, WM_HOST, kHostOpen, 0);
    } else if (what == "drag") {
      PostMessageW(hwnd, WM_HOST, kHostDrag, 0);
    } else if (what == "size" && m.contains("h") && m["h"].is_number()) {
      const int h = std::clamp(static_cast<int>(std::lround(m["h"].get<double>())), 60, 1200);
      if (h != css_h) {
        css_h = h;
        PostMessageW(hwnd, WM_HOST, kHostFit, 0);
      }
    }
  }

  // Runtime present, window made, old browser gone, url / origin set.
  bool prepare(const std::wstring& u, HICON big_icon, HICON small_icon) {
    LPWSTR ver = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver)) || !ver) {
      log("[win] WebView2 runtime not installed");
      return false;
    }
    log("[win] WebView2 runtime " + utf8(ver) + (is_peek ? " (peek)" : ""));
    CoTaskMemFree(ver);
    if (!hwnd && !create_window(big_icon, small_icon)) {
      log("[win] settings window could not be created");
      return false;
    }
    if (ctl || env) teardown();
    url = u;
    const size_t slash = u.find(L'/', u.find(L"//") + 2);
    origin = u.substr(0, slash == std::wstring::npos ? u.size() : slash + 1);
    return true;
  }

  void fail() {
    teardown();
    if (host.failed) host.failed();
  }

  // A peek keeps its browser (the next hover is instant); the full window lets it go.
  void hide() {
    if (!is_peek) {
      teardown();
      return;
    }
    want_show = false;
    if (hwnd) ShowWindow(hwnd, SW_HIDE);
    if (ctl) ctl->put_IsVisible(FALSE);  // throttled while hidden (no animations running for nobody)
  }

  void teardown() {
    ++gen;
    want_show = false;
    if (hwnd) ShowWindow(hwnd, SW_HIDE);
    if (ctl) ctl->Close();
    wv.Reset();
    ctl.Reset();
    env.Reset();
  }

  static std::string hex(HRESULT h) {
    char b[16];
    snprintf(b, sizeof(b), "%08lX", static_cast<unsigned long>(h));
    return b;
  }
};

SettingsWindow::SettingsWindow(bool peek) : d_(std::make_unique<Impl>()) {
  d_->is_peek = peek;
  if (peek) {
    d_->css_h = 200;
    d_->css_w = kCssPeekWidth;
  }
}

SettingsWindow::~SettingsWindow() {
  d_->teardown();
  if (d_->hwnd) DestroyWindow(d_->hwnd);
}

bool SettingsWindow::open(const std::wstring& url, int x, int y, HWND sim_window, HICON big_icon, HICON small_icon, const Host& host) {
  Impl& d = *d_;
  d.host = host;
  if (d.ctl && d.url == url) {  // already there: to the front
    d.bring_up();
    return true;
  }
  if (!d.prepare(url, big_icon, small_icon)) return false;
  if (x == INT_MIN) {  // first time: top right of the monitor the sim is on, like the toolbar panel
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(sim_window ? MonitorFromWindow(sim_window, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(d.hwnd, nullptr, mi.rcWork.right - 10, mi.rcWork.top + 10, 10, 10, SWP_NOZORDER | SWP_NOACTIVATE);
    const double k = d.scale();
    x = mi.rcWork.right - static_cast<int>(std::lround((kCssWidth + 48) * k));
    y = mi.rcWork.top + static_cast<int>(std::lround(120 * k));
  } else {
    SetWindowPos(d.hwnd, nullptr, x, y, 10, 10, SWP_NOZORDER | SWP_NOACTIVATE);  // the right monitor's DPI first
  }
  d.place(x, y);
  d.want_show = true;
  if (!d.start_browser()) {
    d.teardown();
    return false;
  }
  return true;
}

bool SettingsWindow::peek(const std::wstring& url, const RECT& anchor, const Host& host) {
  Impl& d = *d_;
  d.host = host;
  d.anchor = anchor;
  if (d.ctl && d.url == url) {  // kept from the last hover: at once
    d.place_peek();
    d.want_show = true;
    d.bring_up();
    return true;
  }
  if (d.env && d.url == url) {  // still starting from the last hover
    d.want_show = true;
    return true;
  }
  if (!d.prepare(url, nullptr, nullptr)) return false;
  d.place_peek();
  d.want_show = true;
  if (!d.start_browser()) {
    d.teardown();
    return false;
  }
  return true;
}

void SettingsWindow::close() { d_->hide(); }
void SettingsWindow::set_look(const std::string& style) {
  g_look = look_of(style);
  d_->apply_look();
}
void SettingsWindow::release() {
  if (!shown()) d_->teardown();
}
bool SettingsWindow::shown() const { return d_->hwnd && IsWindowVisible(d_->hwnd); }
bool SettingsWindow::is_foreground() const { return d_->hwnd && GetForegroundWindow() == d_->hwnd; }
HWND SettingsWindow::hwnd() const { return d_->hwnd; }

}  // namespace birdie::app
