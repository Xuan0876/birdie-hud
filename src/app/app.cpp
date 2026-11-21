// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/app.h"

#include <mmsystem.h>
#include <objidl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <windowsx.h>
// GDI+ (tray icon drawing)
#include <gdiplus.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "app/hotkeys.h"
#include "app/i18n.h"
#include "app/panel_proto.h"
#include "app/platform.h"
#include "core/geo.h"
#include "core/pause.h"
#include "core/version.h"
#include "hud/anim.h"
#include "hud/scene.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

namespace birdie::app {
namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_BORDER_END = WM_APP + 2;  // wp = save: ends the border editor outside the overlay's own message
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kTimerPeek = 1, kTimerPeekRelease = 2;

// The border editor's Enter / Esc; the panel's hotkeys (app/hotkeys.h) are kHkAction + their index.
enum Hotkey { kHkEditSave = 8, kHkEditCancel = 9, kHkAction = 100 };
// Held down, these repeat (the steps); the rest fire once per press.
bool hotkey_repeats(const std::string& a) {
  return a == "glide_up" || a == "glide_down" || a == "lead_up" || a == "lead_down" || a == "bright_up" ||
         a == "bright_down" || a == "size_up" || a == "size_down";
}

// ---- tray menu: everything else lives in the panel (S5)
enum : int { kCmdOpenPanel = 100, kCmdToggleHud, kCmdOpenFolder, kCmdQuit };

// The panel server: 127.0.0.1 only; the panel tries the same ports in the same order.
constexpr uint16_t kPanelPort = 47820;
constexpr int kPanelPortTries = 4;

HICON make_bird_icon(Gdiplus::Color colour, int n = 32) {
  using namespace Gdiplus;
  Bitmap bmp(n, n, PixelFormat32bppARGB);
  Graphics g(&bmp);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.Clear(Color(0, 0, 0, 0));
  const REAL k = n / 64.0f;
  auto draw = [&](const Pen& pen) {
    g.DrawEllipse(&pen, (32 - 11) * k, (36 - 11) * k, 22 * k, 22 * k);
    g.DrawLine(&pen, 21 * k, 36 * k, 3 * k, 36 * k);
    g.DrawLine(&pen, 43 * k, 36 * k, 61 * k, 36 * k);
    g.DrawLine(&pen, 32 * k, 25 * k, 32 * k, 11 * k);
  };
  Pen outline(Color(170, 0, 0, 0), 10 * k);
  outline.SetStartCap(LineCapRound);
  outline.SetEndCap(LineCapRound);
  draw(outline);
  Pen fg(colour, 5.5f * k);
  fg.SetStartCap(LineCapRound);
  fg.SetEndCap(LineCapRound);
  draw(fg);
  HICON icon = nullptr;
  bmp.GetHICON(&icon);
  return icon;
}

// ---- replay input (parity fixture)
bool load_replay(const std::filesystem::path& dir, std::vector<SimState>& rows, double& hz,
                 std::optional<Runway>& runway, std::string& err) {
  std::ifstream fj(dir / "fixture.json", std::ios::binary);
  if (!fj) {
    err = "no fixture.json";
    return false;
  }
  nlohmann::json meta;
  try {
    meta = nlohmann::json::parse(fj);
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
  hz = meta.value("input", nlohmann::json::object()).value("hz", 30.0);
  if (meta.contains("runway") && meta["runway"].is_object()) {
    const auto& r = meta["runway"];
    auto num = [](const nlohmann::json& j) { return j.is_string() ? std::stod(j.get<std::string>()) : j.get<double>(); };
    runway = make_runway("TEST", num(r.at("lat")), num(r.at("lon")), num(r.at("alt_m")), num(r.at("landing_hdg_true")),
                         num(r.at("length_m")), num(r.at("width_m")), r.at("name").get<std::string>());
  }
  std::ifstream f(dir / "input.csv", std::ios::binary);
  if (!f) {
    err = "no input.csv";
    return false;
  }
  std::string line;
  std::vector<int> map;  // column -> kVars index (-1 = t / other)
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::vector<std::string> cells;
    std::stringstream ss(line);
    std::string c;
    while (std::getline(ss, c, ',')) cells.push_back(c);
    if (map.empty()) {
      for (const auto& name : cells) {
        int idx = -1;
        for (size_t i = 0; i < kVars.size(); ++i)
          if (name == kVars[i].key) idx = static_cast<int>(i);
        map.push_back(idx);
      }
      continue;
    }
    SimState s;
    for (size_t i = 0; i < cells.size() && i < map.size(); ++i)
      if (map[i] >= 0) s.*kVars[map[i]].field = std::strtod(cells[i].c_str(), nullptr);
    s.ra_raw_ft = s.radio_alt_ft;
    rows.push_back(s);
  }
  if (rows.empty()) {
    err = "input.csv has no rows";
    return false;
  }
  return true;
}

}  // namespace

App::App(Options opt)
    : opt_(std::move(opt)), first_run_(!std::filesystem::exists(settings_path())), set_(load_settings()),
      model_(set_) {
  set_language(resolve_language(set_.language));
}

App::~App() {
  if (hwnd_) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    unregister_hotkeys();
    DestroyWindow(hwnd_);
  }
  sim_.close();
  if (sim_event_) CloseHandle(sim_event_);
  if (icon_on_) DestroyIcon(icon_on_);
  if (icon_off_) DestroyIcon(icon_off_);
  if (win_icon_big_) DestroyIcon(win_icon_big_);
  if (win_icon_small_) DestroyIcon(win_icon_small_);
}

bool App::init_window() {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = &App::wndproc;
  wc.hInstance = inst;
  wc.lpszClassName = L"BirdieMain";
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Birdie", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                          this);
  if (!hwnd_) return false;
  msg_taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
  icon_on_ = make_bird_icon(Gdiplus::Color(255, 70, 255, 110));
  icon_off_ = make_bird_icon(Gdiplus::Color(255, 160, 160, 160));
  win_.set_look(set_.ui_style);  // the windows' background and border follow the panel's look
  apply_look();
  win_icon_big_ = make_bird_icon(Gdiplus::Color(255, 0xf2, 0xb4, 0x8a), 48);  // the panel's apricot
  win_icon_small_ = make_bird_icon(Gdiplus::Color(255, 0xf2, 0xb4, 0x8a), 16);
  tray_add();
  return true;
}

LRESULT CALLBACK App::wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (self) return self->on_message(h, m, wp, lp);
  return DefWindowProcW(h, m, wp, lp);
}

LRESULT App::on_message(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == msg_taskbar_created_ && m != 0) {
    tray_add();
    return 0;
  }
  switch (m) {
    case WM_HOTKEY:
      on_hotkey(static_cast<int>(wp));
      return 0;
    case WM_TRAY:
      switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
          hide_peek();
          show_menu();
          break;
        case NIN_SELECT:  // a click (NOTIFYICON_VERSION_4): the settings card comes up
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
          hide_peek();
          open_panel();
          break;
        case NIN_POPUPOPEN:  // the mouse rests on the icon (NOTIFYICON_VERSION_4, no standard tooltip)
          show_peek();
          break;
        case NIN_POPUPCLOSE:  // left the icon: peek_tick decides (the mouse may be on its way into the strip)
          break;
      }
      return 0;
    case WM_TIMER:
      if (wp == kTimerPeek) peek_tick();
      else if (wp == kTimerPeekRelease) {
        KillTimer(h, kTimerPeekRelease);
        peek_.release();
      }
      return 0;
    case WM_COMMAND:
      on_command(LOWORD(wp));
      return 0;
    case WM_BORDER_END:
      border_edit_end(wp != 0);
      return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------- tray
void App::tray_add() {
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | (peek_ok_ ? 0 : NIF_SHOWTIP);
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = tray_active_ ? icon_on_ : icon_off_;
  wcsncpy_s(nid.szTip, L"Birdie", _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &nid);
  nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
  tray_update();
}

std::wstring App::status_text() const {
  const bool drawing = set_.visible && why_.empty();
  if (drawing) return tr(S::Showing);
  if (!set_.visible) return tr(S::Off);
  return trf(S::HiddenFmt, why_.c_str());
}

std::wstring App::runway_text() const {
  if (!runway_) return tr(S::RunwayNone);
  return trf(S::RunwayFmt, runway_->icao.c_str(), runway_->name.c_str(), runway_->length, runway_->width);
}

void App::tray_update() {
  if (!hwnd_) return;
  const bool drawing = set_.visible && why_.empty();
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_TIP | NIF_ICON | (peek_ok_ ? 0 : NIF_SHOWTIP);
  nid.hIcon = drawing ? icon_on_ : icon_off_;
  tray_active_ = drawing;
  const std::wstring tip = trf(S::TooltipFmt, (status_text() + L"\n" + runway_text()).c_str(), set_.glide_deg);
  wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::show_menu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, kCmdOpenPanel, tr(S::OpenPanel));
  std::wstring show = tr(S::ShowOverlay);
  if (auto hk = set_.hotkeys.find("toggle"); hk != set_.hotkeys.end() && !hk->second.empty()) show += L"	" + from_utf8(hk->second);
  AppendMenuW(m, MF_STRING | (set_.visible ? MF_CHECKED : MF_UNCHECKED), kCmdToggleHud, show.c_str());
  if (!panel_.running()) {  // fallback when the panel cannot connect: the settings file itself
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, tr(S::PanelDown));
    AppendMenuW(m, MF_STRING, kCmdOpenFolder, tr(S::OpenSettingsFolder));
  }
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kCmdQuit, tr(S::Quit));
  SetMenuDefaultItem(m, kCmdOpenPanel, FALSE);  // double-click opens it too

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd_);  // so the menu closes when clicking elsewhere
  TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(m);
}

void App::on_command(int id) {
  switch (id) {
    case kCmdOpenPanel:
      open_panel();
      return;
    case kCmdToggleHud:
      set_.visible = !set_.visible;
      say(set_.visible ? tr(S::FlashOn) : tr(S::FlashOff));
      changed();
      return;
    case kCmdOpenFolder:
      ShellExecuteW(nullptr, L"open", app_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      return;
    case kCmdQuit:
      running_ = false;
      return;
  }
}

void App::changed() {
  save_settings(set_);
  redraw();
  tray_update();
  panel_push(true);
}

// ---------------------------------------------------------------- panel
void App::start_panel() {
  std::string err;
  if (!panel_.start([](const std::string& rel) { return ingame::page_file(rel); }, kPanelPort, kPanelPortTries, &err)) {
    logf("[panel] server not started: %s", err.c_str());
    return;
  }
  size_t bytes = 0;
  for (const auto& f : ingame::pack()) bytes += f.data.size();
  logf("[panel] listening on 127.0.0.1:%u, page from the exe (%zu files, %zu KB)%s", panel_.port(),
       ingame::pack().size(), bytes / 1024, ingame::page_file("birdie.html") ? "" : " (no page: the window will not open)");
}

// ---------------------------------------------------------------- in-sim panel (app/ingame.h)
void App::ingame_refresh() {
  community_ = ingame::find_community(from_utf8(set_.community_dir));
  ingame_ = ingame::State::kNone;
  for (const auto& c : community_) {
    const auto s = ingame::state_in(c);
    if (s != ingame::State::kNone) {
      ingame_ = s;
      break;
    }
  }
  t_ingame_ = now_s();
}

void App::ingame_update_if_old() {
  ingame_refresh();
  if (ingame_ != ingame::State::kOld) return;
  bool ok = true;
  for (const auto& c : community_) {
    if (ingame::state_in(c) != ingame::State::kOld) continue;
    std::string err;
    if (ingame::install(c, &err)) {
      logf("[ingame] updated %s", to_utf8((c / ingame::kFolderName).wstring()).c_str());
    } else {
      ok = false;
      ingame_err_ = err;
      logf("[ingame] update failed in %s: %s", to_utf8(c.wstring()).c_str(), err.c_str());
    }
  }
  ingame_note_ = ok ? "updated" : "failed";
  ingame_refresh();
  if (ok) balloon(tr(S::IngameUpdated), false);
}

void App::ingame_action(const std::string& name) {
  std::string err;
  ingame_err_.clear();
  if (name == "ingame_install") {
    if (community_.empty()) ingame_refresh();
    bool ok = !community_.empty();
    for (const auto& c : community_) {
      std::string e;
      if (ingame::install(c, &e)) logf("[ingame] installed to %s", to_utf8(c.wstring()).c_str());
      else ok = false, err = e, logf("[ingame] install to %s failed: %s", to_utf8(c.wstring()).c_str(), e.c_str());
    }
    ingame_note_ = ok ? "installed" : "failed";
  } else if (name == "ingame_remove") {
    bool ok = true;
    for (const auto& c : community_) {
      std::string e;
      if (ingame::remove(c, &e)) logf("[ingame] removed from %s", to_utf8(c.wstring()).c_str());
      else ok = false, err = e, logf("[ingame] remove from %s failed: %s", to_utf8(c.wstring()).c_str(), e.c_str());
    }
    ingame_note_ = ok ? "removed" : "failed";
  } else if (name == "ingame_open") {
    if (!community_.empty()) {
      const auto dir = community_.front() / ingame::kFolderName;
      std::error_code ec;
      const auto& show = std::filesystem::exists(dir, ec) ? dir : community_.front();
      ShellExecuteW(nullptr, L"open", show.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return;
  } else if (name == "ingame_pick") {
    const auto p = pick_folder(tr(S::PickCommunity));
    if (!p) return;
    const auto c = ingame::community_from_pick(*p);
    set_.community_dir = to_utf8(c.wstring());
    logf("[ingame] Community folder chosen: %s", set_.community_dir.c_str());
    save_settings(set_);
    ingame_note_.clear();
  } else if (name == "ingame_auto") {  // forget the pick: back to what UserCfg.opt says
    set_.community_dir.clear();
    save_settings(set_);
    ingame_note_.clear();
  } else if (name == "ingame_export") {
    const auto p = pick_folder(tr(S::PickExport));
    if (!p) return;
    if (ingame::install(*p, &err)) {
      logf("[ingame] exported to %s", to_utf8(p->wstring()).c_str());
      ingame_note_ = "exported";
      ShellExecuteW(nullptr, L"open", p->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
      ingame_note_ = "failed";
      logf("[ingame] export failed: %s", err.c_str());
    }
  } else {
    return;
  }
  ingame_err_ = err;
  ingame_refresh();
  panel_push(true);
}

std::wstring App::watermark() {
  return std::strchr(version(), '-') ? std::wstring(tr(S::Watermark)) : std::wstring();
}

nlohmann::json App::ingame_status() const {
  nlohmann::json j;
  j["state"] = community_.empty() ? "nofolder" : ingame::state_key(ingame_);
  j["dir"] = community_.empty() ? "" : to_utf8(community_.front().wstring());
  j["chosen"] = !set_.community_dir.empty();
  j["has_pack"] = !ingame::pack().empty();
  j["note"] = ingame_note_;
  j["err"] = ingame_err_;
  j["sim_running"] = hwnd_sim_ != nullptr || sim_.connected();
  return j;
}

std::optional<std::filesystem::path> App::pick_folder(const wchar_t* title) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(title);
  if (!community_.empty()) {  // start where the panel would go
    IShellItem* at = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(community_.front().c_str(), nullptr, IID_PPV_ARGS(&at)))) {
      dlg->SetFolder(at);
      at->Release();
    }
  }
  std::optional<std::filesystem::path> out;
  if (SUCCEEDED(dlg->Show(win_.shown() ? win_.hwnd() : hwnd_))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = std::filesystem::path(p);
      CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

std::wstring App::panel_url() const {
  wchar_t buf[64];
  swprintf_s(buf, L"http://127.0.0.1:%u/", panel_.port());
  return buf;
}

// ---- the peek: hovering the tray icon floats up the bird, the voice and the switch; a click opens the window
void App::show_peek() {
  if (!peek_ok_ || !panel_.running() || win_.shown()) return;
  NOTIFYICONIDENTIFIER id = {sizeof(id)};
  id.hWnd = hwnd_;
  id.uID = kTrayId;
  RECT icon;
  if (FAILED(Shell_NotifyIconGetRect(&id, &icon))) return;
  SettingsWindow::Host host;
  host.log = [](const std::string& s) { log_line(s); };
  host.open_full = [this] {
    hide_peek();
    open_panel();
  };
  host.failed = [this] {  // no WebView2 after all: back to the plain tooltip
    peek_ok_ = false;
    tray_add();
  };
  if (!peek_.peek(panel_url() + L"?peek", icon, host)) {
    host.failed();
    return;
  }
  peek_inside_ = now_s();
  KillTimer(hwnd_, kTimerPeekRelease);
  SetTimer(hwnd_, kTimerPeek, 80, nullptr);
}

void App::hide_peek() {
  KillTimer(hwnd_, kTimerPeek);
  peek_.close();
  SetTimer(hwnd_, kTimerPeekRelease, 60000, nullptr);  // a minute without hovering: let the browser go
}

void App::peek_tick() {
  POINT pt;
  GetCursorPos(&pt);
  NOTIFYICONIDENTIFIER id = {sizeof(id)};
  id.hWnd = hwnd_;
  id.uID = kTrayId;
  RECT icon{}, strip{};
  const bool on_icon = SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon)) && PtInRect(&icon, pt);
  bool on_strip = false;
  if (peek_.hwnd() && GetWindowRect(peek_.hwnd(), &strip)) {
    // the gap between icon and strip counts as inside, so the mouse can travel up to it
    RECT bridge = strip;
    UnionRect(&bridge, &strip, &icon);
    on_strip = PtInRect(&strip, pt) || (PtInRect(&bridge, pt) && pt.x >= icon.left - 8 && pt.x <= icon.right + 8);
  }
  const double now = now_s();
  if (on_icon || on_strip) peek_inside_ = now;
  else if (now - peek_inside_ > 0.35) hide_peek();
}

void App::open_panel(const std::wstring& fragment) {
  if (!panel_.running()) {
    ShellExecuteW(nullptr, L"open", app_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  const std::wstring url = panel_url() + fragment;
  SettingsWindow::Host host;
  host.log = [](const std::string& s) { log_line(s); };
  host.moved = [this](int x, int y) {
    set_.panel_win_x = x;
    set_.panel_win_y = y;
    save_settings(set_);
  };
  host.failed = [this, url] { open_panel_in_browser(url); };
  const bool placed = set_.panel_win_x != Settings::kNoPos && set_.panel_win_y != Settings::kNoPos;
  if (!win_.open(url, placed ? set_.panel_win_x : INT_MIN, placed ? set_.panel_win_y : INT_MIN, hwnd_sim_,
                 win_icon_big_, win_icon_small_, host))
    open_panel_in_browser(url);
}

void App::open_panel_in_browser(const std::wstring& url) {
  log_line("[panel] opening the panel in the browser instead");
  ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

namespace {
// The symbol switches, as the panel names them.
const struct {
  const char* key;
  bool Settings::*field;
} kSymbols[] = {
    {"show_bird", &Settings::show_bird},     {"show_fpa_num", &Settings::show_fpa_num},
    {"show_vs_num", &Settings::show_vs_num}, {"show_bird_ra", &Settings::show_bird_ra},
    {"show_flare", &Settings::show_flare},   {"show_glide", &Settings::show_glide},
    {"show_horizon", &Settings::show_horizon}, {"show_hdg", &Settings::show_hdg},
    {"show_pitch_minor", &Settings::show_pitch_minor}, {"show_runway", &Settings::show_runway},
    {"show_att", &Settings::show_att},       {"show_speed", &Settings::show_speed},
    {"show_alt", &Settings::show_alt},       {"show_vs", &Settings::show_vs},
    {"show_roll", &Settings::show_roll},     {"show_gs", &Settings::show_gs},
};

// The same settings with only one symbol drawn (and what it hangs on: the numbers beside the bird need the bird).
Settings only_symbol(const Settings& s, const std::string& key) {
  Settings f = s;
  for (const auto& sym : kSymbols) f.*sym.field = key == sym.key;
  f.ladder_step_deg = 0;
  if (key == "show_pitch_minor") f.ladder_step_deg = s.ladder_step_deg;  // the panel's "pitch marks" = the ladder too
  if (key == "show_fpa_num" || key == "show_vs_num" || key == "show_bird_ra") f.show_bird = true;
  return f;
}
}  // namespace

void App::set_focus(const std::string& key) {
  bool known = false;
  for (const auto& sym : kSymbols) known = known || key == sym.key;
  const std::string k = known ? key : "";
  focus_until_ = now_s() + 30;  // a panel that went away without saying so does not leave the HUD dimmed
  if (k == focus_key_) return;
  focus_key_ = k;
  redraw();
}

nlohmann::json App::panel_status() const {
  nlohmann::json st;
  st["showing"] = set_.visible && why_.empty();
  st["why"] = !set_.visible ? "off" : why_.empty() ? "show" : why_key_;
  st["why_text"] = to_utf8(why_);
  st["replay"] = !opt_.replay.empty();
  st["sim"] = !opt_.replay.empty() || sim_.connected();
  if (runway_)
    st["runway"] = {{"icao", runway_->icao}, {"name", runway_->name}, {"length", std::round(runway_->length)},
                    {"width", std::round(runway_->width)}};
  else
    st["runway"] = nullptr;
  st["aircraft"] = ac_title_;
  st["hotkey_failed"] = hotkey_failed_;
  const auto ra = set_.ra_by_aircraft.find(ac_title_);
  if (!ac_title_.empty() && ra != set_.ra_by_aircraft.end())
    st["ra"] = {{"ft", ra->second.ft}, {"note", ra->second.note}};
  else
    st["ra"] = nullptr;
  st["on_ground"] = state_ && state_->on_ground != 0 && state_->gs_kt < 1;
  st["lang"] = language() == Lang::kZh ? "zh" : "en";
  st["autostart"] = autostart_enabled();
  st["version"] = version();
  st["border"] = border_status();
  st["ingame"] = ingame_status();
  return st;
}

std::string App::panel_state() const {
  return nlohmann::json{{"type", "state"}, {"settings", panel_settings(set_)}, {"status", panel_status()}}.dump();
}

void App::panel_push(bool force) {
  if (!panel_.running() || panel_.ws_clients() == 0) return;
  const std::string text = panel_state();
  if (!force && text == panel_sent_) return;
  panel_sent_ = text;
  panel_.broadcast(text);
}

void App::on_panel_message(const std::string& text) {
  nlohmann::json m;
  try {
    m = nlohmann::json::parse(text);
  } catch (const std::exception&) {
    log_line("[panel] message is not JSON");
    return;
  }
  if (!m.is_object()) return;
  const std::string type = m.value("type", "");
  if (type == "set") {
    const std::string key = m.value("key", "");
    const auto v = m.contains("value") ? m["value"] : nlohmann::json();
    if (!apply_panel_set(set_, key, v)) {
      logf("[panel] ignored set %s = %s", key.c_str(), v.dump().c_str());
      panel_push(true);
      return;
    }
    if (key.rfind("hotkey.", 0) == 0) logf("[panel] %s = %s", key.c_str(), set_.hotkeys[key.substr(7)].c_str());
    else logf("[panel] %s = %s", key.c_str(), panel_settings(set_)[key].dump().c_str());
    if (key == "language") set_language(resolve_language(set_.language));
    if (key == "ui_style") win_.set_look(set_.ui_style), peek_.set_look(set_.ui_style);
    if (key == "hud_font") apply_look();
    if (key == "visible") say(set_.visible ? tr(S::FlashOn) : tr(S::FlashOff));
    if (key.rfind("hotkey.", 0) == 0 && hk_capture_until_ == 0) register_hotkeys(false);
    changed();
  } else if (type == "focus") {
    set_focus(m.contains("key") && m["key"].is_string() ? m["key"].get<std::string>() : "");
  } else if (type == "action") {
    const std::string name = m.value("name", "");
    if (name == "ra_calibrate") {
      calibrate_ra();
      changed();
    } else if (name == "ra_clear") {
      clear_ra();
      changed();
    } else if (name == "preset" && m.contains("value") && m["value"].is_string()) {
      if (apply_preset(set_, m["value"].get<std::string>())) {
        logf("[panel] HUD preset %s", set_.hud_preset.c_str());
        apply_look();
        changed();
      }
    } else if (name == "reset_layout") {
      reset_layout(set_);
      set_.hud_preset = "custom";
      changed();
    } else if (name == "hold") {  // the panel's look pages are open: the HUD shows even where it would rest
      hold_until_ = m.value("value", false) ? now_s() + 15.0 : 0.0;
      redraw();
    } else if (name == "hotkey_capture") {  // the panel is listening for a combo: let every key through to it
      const bool on = m.value("value", false);
      hk_capture_until_ = on ? now_s() + 20.0 : 0.0;  // back by itself if the panel goes away mid-capture
      if (on) unregister_hotkeys();
      else register_hotkeys(false);
      panel_push(true);
    } else if (name == "hotkeys_reset") {
      set_.hotkeys = default_hotkeys();
      if (hk_capture_until_ == 0) register_hotkeys(false);
      changed();
    } else if (name == "anim_test") {  // replay the opening (value "in") or the closing ("out") once
      anim_test(m.value("value", std::string("in")) == "out");
    } else if (name == "open_log") {
      ShellExecuteW(nullptr, L"open", log_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (name == "open_folder") {
      ShellExecuteW(nullptr, L"open", app_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (name == "autostart" && m.contains("value") && m["value"].is_boolean()) {
      const bool on = m["value"].get<bool>();
      if (!set_autostart(on)) log_line("[panel] autostart change failed");
      else logf("[panel] autostart %s", on ? "on" : "off");
      panel_push(true);
    } else if (name == "quit") {
      log_line("[panel] quit");
      running_ = false;
    } else if (name == "hello") {
      panel_push(true);
    } else if (name == "border_edit") {
      border_edit_begin();
    } else if (name == "border_clear") {
      border_clear();
    } else if (name == "border_mirror") {
      border_mirror();
    } else if (name == "border_fit") {
      border_fit();
    } else if (name.rfind("ingame_", 0) == 0) {
      ingame_action(name);
    }
  }
}

// ---------------------------------------------------------------- hotkeys
void App::unregister_hotkeys() {
  for (size_t i = 0; i < hotkey_actions().size(); ++i) UnregisterHotKey(hwnd_, kHkAction + static_cast<int>(i));
}

void App::register_hotkeys(bool report) {
  unregister_hotkeys();
  hotkey_failed_.clear();
  std::wstring list;
  const auto& actions = hotkey_actions();
  for (size_t i = 0; i < actions.size(); ++i) {
    const std::string name = actions[i].name;
    const auto it = set_.hotkeys.find(name);
    if (it == set_.hotkeys.end() || it->second.empty()) continue;
    const auto c = parse_combo(it->second);
    if (!c || !RegisterHotKey(hwnd_, kHkAction + static_cast<int>(i), c->mod | (hotkey_repeats(name) ? 0 : MOD_NOREPEAT),
                              c->vk)) {
      logf("[hotkey] %s (%s) could not be registered (error %lu, in use?)", it->second.c_str(), name.c_str(),
           c ? GetLastError() : 0ul);
      hotkey_failed_.push_back(name);
      list += (list.empty() ? L"" : L", ") + from_utf8(it->second);
    }
  }
  if (report && !list.empty()) balloon(trf(S::HotkeyFailedFmt, list.c_str()), true);
}

void App::balloon(const std::wstring& text, bool warning) {
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
  wcsncpy_s(nid.szInfoTitle, L"Birdie", _TRUNCATE);
  wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::on_hotkey(int id) {
  if (id == kHkEditSave || id == kHkEditCancel) {
    if (edit_.on) border_edit_end(id == kHkEditSave);
    return;
  }
  const auto& actions = hotkey_actions();
  if (id < kHkAction || id >= kHkAction + static_cast<int>(actions.size())) return;
  const std::string a = actions[id - kHkAction].name;
  const auto step = [](double v, double d, double lo, double hi) {
    return std::round(std::clamp(v + d, lo, hi) * 100) / 100;
  };
  if (a == "quit") {
    running_ = false;
    return;
  } else if (a == "toggle") {
    set_.visible = !set_.visible;
    say(set_.visible ? tr(S::FlashOn) : tr(S::FlashOff));
  } else if (a == "xwind") {
    set_.xwind_mode = !set_.xwind_mode;
    say(set_.xwind_mode ? tr(S::FlashXwindOn) : tr(S::FlashXwindOff));
  } else if (a == "glide_up" || a == "glide_down") {
    set_.glide_deg = std::round(std::clamp(set_.glide_deg + (a == "glide_up" ? 0.1 : -0.1), 0.5, 10.0) * 10) / 10;
    say(trf(S::FlashGlideFmt, set_.glide_deg));
  } else if (a == "lead_up" || a == "lead_down") {
    set_.lead_ms = std::min(300, std::max(-100, set_.lead_ms + (a == "lead_up" ? 10 : -10)));
    say(trf(S::FlashLeadFmt, set_.lead_ms));
  } else if (a == "bright_up" || a == "bright_down") {
    set_.opacity = step(set_.opacity, a == "bright_up" ? 0.05 : -0.05, 0.15, 1.0);
    say(trf(S::FlashOpacityFmt, static_cast<int>(std::lround(set_.opacity * 100))));
  } else if (a == "size_up" || a == "size_down") {
    set_.hud_scale = step(set_.hud_scale, a == "size_up" ? 0.05 : -0.05, 0.5, 1.5);
    say(trf(S::FlashSizeFmt, static_cast<int>(std::lround(set_.hud_scale * 100))));
  } else if (a == "preset_next") {  // airbus -> boeing -> fighter -> minimal -> airbus (custom starts over)
    static const char* const order[] = {"airbus", "boeing", "fighter", "minimal"};
    static const S names[] = {S::PresetAirbus, S::PresetBoeing, S::PresetFighter, S::PresetMinimal};
    size_t k = 0;
    for (size_t i = 0; i < 4; ++i)
      if (set_.hud_preset == order[i]) k = (i + 1) % 4;
    apply_preset(set_, order[k]);
    apply_look();
    say(trf(S::FlashPresetFmt, tr(names[k])));
  } else if (a == "anim_test") {
    anim_test(false);
    return;
  } else if (a == "panel") {
    open_panel();
    return;
  } else {
    return;
  }
  changed();
}

// ---------------------------------------------------------------- simulator
void App::connect_sim() {
  if (!opt_.replay.empty() || !sim_dll_) return;
  const double now = now_s();
  // MSFS can stop sending without a QUIT while the sim runs on (a user's log 11/21: the data stopped while taxiing and
  // never came back, 40 min). This connection has had data, then nothing for a while: drop it and open a new one. Only
  // once per stall: the new connection arms it again with its first sample (the main menu sends none, so no loop).
  // Not while Pause_EX1 says paused (any kind): per-frame data may stop in a pause, and that is not a stall; and the
  // clock starts again when the pause ends, so the first frames after it are waited for.
  const double quiet = now - std::max(t_state_, t_pause_);
  if (sim_.connected() && state_ && pause_flags_ == 0 && quiet > kStallS) {
    logf("[sim] no data for %.0f s: reconnecting", quiet);
    if (edit_.on) border_edit_end(false);
    sim_.close();
    state_.reset();
    runway_ = nullptr;
    t_reconnect_ = now;
    t_connect_ = -1e9;
  }
  if (t_reconnect_ >= 0) {
    if (state_) {
      logf("[sim] data back %.1f s after reconnecting", now - t_reconnect_);
      t_reconnect_ = -1;
    } else if (now - t_reconnect_ > 30) {
      log_line("[sim] still no data 30 s after reconnecting (normal in the main menu; in a flight MSFS's SimConnect "
               "has stopped: restart the sim)");
      t_reconnect_ = -1;
    }
  }
  if (sim_.connected() || now - t_connect_ < 2.0) return;
  t_connect_ = now;
  std::wstring err;
  if (sim_.open(sim_event_, &err)) {
    t_airports_ = -1e9;
    pause_flags_ = 0;
    runway_ = nullptr;
    log_line("[sim] SimConnect connected");
  }
}

void App::update_runway() {
  if (!opt_.replay.empty()) {
    runway_ = replay_runway_ ? &*replay_runway_ : nullptr;
    return;
  }
  if (!sim_.connected() || !state_) return;  // selected even with the outline off: the glide line still clears it
  const double now = now_s();
  if (now - t_airports_ > 20) {  // airports in the reality bubble; runways fetched once per airport
    t_airports_ = now;
    sim_.request_airports();
  }
  const auto nearby_icaos = sim_.nearby(state_->lat, state_->lon);
  for (const auto& icao : nearby_icaos) sim_.request_runways(icao);
  // the ground under the aircraft fixes the height of a runway we are over (takeoff roll, crossing the threshold)
  sim_.anchor_runways(nearby_icaos, state_->lat, state_->lon, (state_->alt_ft - state_->agl_ft) * 0.3048,
                      [](const std::string& m) { log_line(m); });
  const Runway* rw = pick_runway(sim_.all_ends(nearby_icaos), state_->lat, state_->lon, state_->trk_true, runway_);
  if (rw != runway_) {
    if (rw) logf("[rwy] selected %s %s hdg %.1f %.0fx%.0f m slope %+.2f", rw->icao.c_str(), rw->name.c_str(), rw->hdg,
                 rw->length, rw->width, rw->slope);
    else log_line("[rwy] no runway lined up");
    runway_ = rw;
  }
}

void App::on_state(SimState s) {
  // the sim's radio altimeter reads several ft on the ground (gear not accounted for): subtract the offset measured
  // on the ground (tray: RA calibration)
  const double raw = s.radio_alt_ft;
  s.ra_raw_ft = raw;
  s.radio_alt_ft = raw - ra_offset();
  if (s.on_ground != 0 && s.gs_kt < 1) {  // standing still on the ground
    ra_ground_.push_back(raw);
    if (ra_ground_.size() > 60) ra_ground_.pop_front();
    // first time on the ground in this aircraft: calibrate by itself (the tray item can redo / clear it)
    if (!ac_title_.empty() && !set_.ra_by_aircraft.count(ac_title_)) {
      if (auto med = steady_ground_ra(std::vector<double>(ra_ground_.begin(), ra_ground_.end()))) {
        store_ra(*med, true);
        s.radio_alt_ft = raw - ra_offset();
      }
    }
  } else {
    ra_ground_.clear();
  }
  t_state_ = now_s();
  state_ = s;
  model_.update(s, t_state_);
  n_msgs_ += std::max(1, sim_.take_samples());  // data/s above Hz = the paint could not keep up (stale samples skipped)
  ++n_frames_;
  redraw();
}

void App::replay_step() {
  if (replay_rows_.empty()) return;
  const double now = now_s();
  while (now >= replay_next_) {
    SimState s = replay_rows_[replay_i_];
    replay_i_ = (replay_i_ + 1) % replay_rows_.size();
    if (replay_i_ == 0) model_.reset();  // loop: start the filters afresh
    replay_next_ += 1.0 / replay_hz_;
    t_state_ = now;
    state_ = s;
    model_.update(s, now);
    ++n_msgs_;
  }
  ++n_frames_;
  redraw();
}

double App::ra_offset() const {
  auto it = set_.ra_by_aircraft.find(ac_title_);
  return it == set_.ra_by_aircraft.end() ? 0.0 : it->second.ft;
}

void App::on_aircraft(const std::string& title, const std::string& model) {
  if (title == ac_title_) return;
  if (edit_.on) border_edit_end(false);
  ac_title_ = title;
  ra_ground_.clear();
  auto it = set_.ra_by_aircraft.find(title);
  if (it != set_.ra_by_aircraft.end())
    logf("[ac] %s (%s): RA ground offset %.1f ft", title.c_str(), model.c_str(), it->second.ft);
  else
    logf("[ac] %s (%s): RA not calibrated yet (automatic when standing on the ground)", title.c_str(), model.c_str());
  tray_update();
}

void App::store_ra(double med, bool automatic) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t when[32];
  swprintf_s(when, L"%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
  set_.ra_by_aircraft[ac_title_] = {std::round(med * 10) / 10,
                                    to_utf8(trf(automatic ? S::RaAutoNoteFmt : S::RaNoteFmt, when, med))};
  logf("[ra] %s: %s, ground reading %.1f ft", ac_title_.c_str(), automatic ? "calibrated automatically" : "calibrated",
       med);
  if (automatic) {
    save_settings(set_);
    tray_update();
  } else {
    say(trf(S::FlashRaDoneFmt, med));  // on_command saves
  }
}

void App::calibrate_ra() {
  std::optional<double> med;
  if (state_ && state_->on_ground != 0 && !ac_title_.empty())
    med = steady_ground_ra(std::vector<double>(ra_ground_.begin(), ra_ground_.end()), 10);
  if (!med) {
    say(tr(S::FlashRaNeedGround));
    return;
  }
  store_ra(*med, false);
}

void App::clear_ra() {
  if (ac_title_.empty()) return;
  // an explicit 0 (not "no entry"), so the automatic calibration does not come straight back
  set_.ra_by_aircraft[ac_title_] = {0.0, to_utf8(tr(S::RaClearedNote))};
  logf("[ra] %s: cleared", ac_title_.c_str());
  say(tr(S::FlashRaCleared));
}

void App::say(const std::wstring& text) {
  flash_ = text;
  flash_until_ = now_s() + 2.5;
  logf("[set] %s", to_utf8(text).c_str());
  if (panel_.running()) panel_.broadcast(nlohmann::json{{"type", "notice"}, {"text", to_utf8(text)}}.dump());
}

// ---------------------------------------------------------------- window placement and drawing
void App::place() {
  if (!box_) return;
  const RECT& b = *box_;
  const int x = b.left, y = b.top, w = b.right - b.left, h = b.bottom - b.top;
  if (!gfx_ok_) {
    gfx_ok_ = gfx_.init_window(x, y, w, h);
    if (!gfx_ok_) {
      logf("[gfx] renderer could not start: %s", to_utf8(gfx_.error()).c_str());
      return;
    }
    log_line("[gfx] Direct2D + DirectComposition overlay ready");
  }
  gfx_.move(x, y, w, h);
  gfx_.show(true);
}

void App::housekeeping() {
  connect_sim();
  if (hk_capture_until_ > 0 && now_s() > hk_capture_until_) {
    hk_capture_until_ = 0;
    register_hotkeys(false);
  }
  std::optional<RECT> cr;
  if (!opt_.replay.empty()) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    cr = mi.rcMonitor;
    hwnd_sim_ = nullptr;
  } else {
    hwnd_sim_ = find_msfs_window();
    RECT r;
    if (hwnd_sim_ && client_rect_screen(hwnd_sim_, r)) cr = r;
  }
  update_runway();
  if (cr && cr->right - cr->left > 100 &&
      (!client_ || std::memcmp(&*client_, &*cr, sizeof(RECT)) != 0)) {
    client_ = cr;
    box_ = overlay_box(*cr);
    place();
    logf("[win] MSFS client %ldx%ld @(%ld,%ld), overlay %ldx%ld", cr->right - cr->left, cr->bottom - cr->top, cr->left,
         cr->top, box_->right - box_->left, box_->bottom - box_->top);
  }
  redraw();  // re-evaluate the hide reasons even when no data arrives (pause, focus change)
  tray_update();
  check_chaseplane();
  if (panel_.running()) {
    // someone may have copied or deleted the in-sim panel by hand: look again now and then while a page is open
    if (panel_.ws_clients() > 0 && now_s() - t_ingame_ > 20) ingame_refresh();
    panel_.poll(panel_events_);
    panel_push(false);
  }

  const double now = now_s();
  if (now - last_print_ > 5 && state_) {
    const SimState& s = *state_;
    const double dt = std::max(now - t_stat_, 1e-3);
    logf("[live] %4.0f Hz  data %4.1f/s  build %4.1f ms  paint %4.1f ms  gs %5.1f kt  agl %6.0f  cp y/p %+.1f/%+.1f "
         "z %.3f  runway %s  %s",
         n_frames_ / dt, n_msgs_ / dt, build_ms_, paint_ms_, s.gs_kt, s.agl_ft, s.cp_yaw, s.cp_pitch, s.cp_zoom,
         runway_ ? (runway_->icao + " " + runway_->name).c_str() : "-",
         why_.empty() ? "showing" : ("hidden: " + to_utf8(why_)).c_str());
    n_frames_ = n_msgs_ = 0;
    t_stat_ = last_print_ = now;
  }
}

namespace {
// ChasePlane for MSFS 2024 runs a helper process next to the sim ("CP MSFS Bridge.exe").
bool chaseplane_process_running() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return true;  // cannot tell: do not warn
  PROCESSENTRY32W pe = {sizeof(pe)};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
    found = _wcsnicmp(pe.szExeFile, L"CP MSFS Bridge", 14) == 0;
  CloseHandle(snap);
  return found;
}
}  // namespace

void App::check_chaseplane() {
  // Tell the user once per run when ChasePlane is missing: its L-vars read nothing for 15 s while MSFS is in front
  // and sending data, and its helper process is not running (the process check keeps menus/loading quiet).
  if (!opt_.replay.empty() || cp_warned_) return;
  const double now = now_s();
  const bool live = state_ && now - t_state_ < 1.0 && hwnd_sim_ && GetForegroundWindow() == hwnd_sim_;
  if (!live || state_->cp_zoom > 0.05) {
    t_no_cp_ = -1;
    return;
  }
  if (t_no_cp_ < 0) t_no_cp_ = now;
  if (now - t_no_cp_ < 15 || chaseplane_process_running()) return;
  cp_warned_ = true;
  log_line("[cp] ChasePlane not detected (no L-vars for 15 s, no CP MSFS Bridge process)");
  balloon(tr(S::ChasePlaneMissing), true);
}

namespace {
// The taskbar and the tray overflow: a click on Birdie's tray icon makes them the foreground for a moment.
bool is_taskbar(HWND h) {
  wchar_t cls[64] = {};
  if (!h || !GetClassNameW(h, cls, 64)) return false;
  for (const wchar_t* c : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                           L"TopLevelWindowForOverflowXamlIsland"})
    if (!wcscmp(cls, c)) return true;
  return false;
}
}  // namespace

App::Front App::front() {
  const HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (fg) GetWindowThreadProcessId(fg, &pid);
  Front f = Front::Other;
  if (fg && fg == hwnd_sim_) f = Front::Sim;
  else if (win_.is_foreground() || (fg && pid == GetCurrentProcessId())) f = Front::Birdie;
  // passing through the taskbar on the way from the sim to Birdie's tray icon: no blink
  else if (is_taskbar(fg) && last_front_ != Front::Other) f = Front::Birdie;
  last_front_ = f;
  return f;
}

bool App::compute_why() {
  const bool replay = !opt_.replay.empty();
  const Front fr = replay ? Front::Sim : front();
  // MSFS in the background sends its data in bursts (seen 11/17: 18–29 msgs/s with gaps over 0.5 s while the settings
  // window was in front), so "the data stopped" waits longer then; a real pause still hides at once through Pause_EX1
  const bool stale = now_s() - t_state_ > (fr == Front::Birdie ? 2.0 : 0.5);
  if (!state_) {
    const bool nosim = !replay && !sim_.connected();
    why_ = nosim ? tr(S::WhySimNotRunning) : tr(S::WhyWaitData);
    why_key_ = nosim ? "nosim" : "wait";
  } else if (stale) {
    // the reasons below would judge the last sample: an old one tells a wrong story (the 11/21 log said "slow on the
    // ground" for 40 min of no data, after take-off too). The checks further down still go first, then "data stopped".
    why_.clear();
  } else if (const char* r = hide_reason(*state_, holding() ? 0.0 : show_above_kt())) {
    why_ = tr_why(r);
    why_key_ = !std::strcmp(r, "no_chaseplane")   ? "nocp"
               : !std::strcmp(r, "external_view") ? "ext"
               : !std::strcmp(r, "not_forward")   ? "look"
               : !std::strcmp(r, "ground_slow")   ? "taxi"
                                                  : "wait";
  } else {
    why_.clear();
  }
  // up high for a while (auto_hide): hidden until the radio altitude drops below the set height again
  if (stale) {
    // keep high_since_ as it was: the data may come back
  } else if (state_ && set_.auto_hide && state_->on_ground < 0.5 && state_->radio_alt_ft > set_.auto_hide_ft) {
    if (high_since_ < 0) high_since_ = now_s();
    if (why_.empty() && !holding() && now_s() - high_since_ >= set_.auto_hide_s)
      why_ = tr(S::WhyHigh), why_key_ = "high";
  } else {
    high_since_ = -1;
  }
  if (why_.empty() && !replay && !hwnd_sim_) why_ = tr(S::WhyNoWindow), why_key_ = "nosim";
  // Birdie's own windows in front count as the sim: the HUD stays, so changes show while dragging (the tray panel is
  // the only one when the in-sim panel is not installed)
  if (why_.empty() && fr == Front::Other) why_ = tr(S::WhyNotForeground), why_key_ = "away";
  // the ESC menu pauses the sim with the data still coming: the Pause_EX1 event says so; "the data stopped" stays
  // as the fallback (a pause the event does not report, a stalled sim)
  if (why_.empty() && !replay && pause_hides_hud(pause_flags_)) why_ = tr(S::WhyPaused), why_key_ = "pause";
  if (why_.empty() && stale) why_ = tr(S::WhyDataStopped), why_key_ = "pause";
  return set_.visible && why_.empty() && client_ && box_;
}

// ---------------------------------------------------------------- openings and closings (hud/anim.h)
double App::anim_progress(double now) const {
  const auto kind = hud::anim_kind(hud_on_ ? set_.anim_in : set_.anim_out);
  const double d = hud::anim_duration(kind, hud_on_, set_.anim_time);
  if (d <= 0) return hud_on_ ? 1.0 : 0.0;
  const double moved = (now - anim_t0_) / d;
  return hud_on_ ? std::min(1.0, anim_p0_ + moved) : std::max(0.0, anim_p0_ - moved);
}

bool App::anim_busy(double now) const {
  const double p = anim_progress(now);
  return (p > 0.0 && p < 1.0) || now < anim_hide_until_ + 0.05;
}

double App::anim_step(bool want, double now) {
  if (want != hud_on_) {  // turn round from wherever it is
    anim_p0_ = anim_progress(now);
    anim_t0_ = now;
    hud_on_ = want;
  }
  return anim_progress(now) >= 1.0 && hud_on_ ? 1.0 : 0.0;
}

void App::anim_test(bool closing) {
  const double now = now_s();
  if (closing) {  // close, then open again
    anim_hide_until_ = now + hud::anim_duration(hud::anim_kind(set_.anim_out), false, set_.anim_time) + 0.35;
  } else {  // open again from nothing
    anim_hide_until_ = 0;
    hud_on_ = true;
    anim_p0_ = 0;
    anim_t0_ = now;
  }
  redraw();
}

bool App::holding() const { return now_s() < hold_until_ && panel_.ws_clients() > 0; }

void App::apply_look() {
  if (gfx_ok_) gfx_.set_font_family(hud::font_family(set_.hud_font));
}

void App::redraw() {
  if (!gfx_ok_) {
    compute_why();
    return;
  }
  const double t0 = now_s();
  if (!flash_.empty() && t0 > flash_until_) flash_.clear();
  hud::CommandList cl;
  if (edit_.on && !can_edit_border()) {  // left the cockpit view, lost the sim...: nothing is saved
    PostMessageW(hwnd_, WM_BORDER_END, 0, 0);  // (redraw may run inside the overlay's input hook)
    return;
  }
  const auto border = active_border();
  if (edit_.on) {  // the editor: the HUD faint behind (clipped as it would be), the border and its handles on top
    compute_why();
    const double W = client_->right - client_->left, H = client_->bottom - client_->top;
    const double ox = box_->left - client_->left, oy = box_->top - client_->top;
    cl = hud::commands(hud::build_scene(model_, *state_, W, H, ox, oy, set_, runway_, L"", t0, &edit_.pts, watermark()),
                       set_.opacity * 0.28);
    hud::EditView e;
    e.poly = hud::border_to_screen(model_, *state_, W, H, ox, oy, edit_.pts).value_or(Polyline{});
    e.handles = hud::border_vertices(model_, *state_, W, H, ox, oy, edit_.pts);
    e.hover = edit_.hover;
    e.drag = edit_.drag;
    e.add_at = edit_.add_at;
    e.alpha = std::min(1.0, (t0 - edit_.t0) / 0.35);  // fades in
    e.W = box_->right - box_->left;
    e.H = box_->bottom - box_->top;
    e.line1 = tr(S::BorderHint1);
    e.line2 = tr(S::BorderHint2);
    hud::append_edit(cl, e);
  } else if (const bool want = compute_why() && t0 >= anim_hide_until_; anim_step(want, t0) < 1.0) {
    // opening or closing (hud/anim.h); a closing is only drawn while the HUD's place is still known: in the
    // cockpit view, with data (an outside view or a lost sim ends it at once)
    const double p = anim_progress(t0);
    const bool can = state_ && client_ && box_ && gfx_ok_ && std::strcmp(why_key_, "ext") && std::strcmp(why_key_, "nocp") &&
                     std::strcmp(why_key_, "nosim") && std::strcmp(why_key_, "wait");
    if (p > 0 && can) {
      const auto kind = hud::anim_kind(hud_on_ ? set_.anim_in : set_.anim_out);
      const Settings fs = hud::anim_settings(set_, kind, p);
      const double ox = box_->left - client_->left, oy = box_->top - client_->top;
      const auto scene = hud::build_scene(model_, *state_, client_->right - client_->left,
                                          client_->bottom - client_->top, ox, oy, fs, runway_, flash_, t0,
                                          border ? &*border : nullptr, watermark());
      cl = hud::animated_commands(scene, set_.opacity, kind, p, t0);
    } else if (!can && !hud_on_) {
      anim_p0_ = 0, anim_t0_ = t0;  // nothing to close on: done
      if (!flash_.empty()) cl = hud::commands(hud::flash_scene(flash_), set_.opacity);
    } else if (!flash_.empty()) {
      cl = hud::commands(hud::flash_scene(flash_), set_.opacity);
    }
  } else if (want) {
    const double ox = box_->left - client_->left, oy = box_->top - client_->top;
    const auto scene = hud::build_scene(model_, *state_, client_->right - client_->left,
                                        client_->bottom - client_->top, ox, oy, set_, runway_, flash_, t0,
                                        border ? &*border : nullptr, watermark());
    if (!focus_key_.empty() && t0 < focus_until_ && panel_.ws_clients() > 0) {
      // the panel points at one symbol: everything else steps back, that one stays (half strength if it is off)
      cl = hud::commands(scene, set_.opacity * 0.2);
      bool on = false;
      for (const auto& sym : kSymbols)
        if (focus_key_ == sym.key) on = set_.*sym.field;
      hud::append_commands(cl,
                           hud::build_scene(model_, *state_, client_->right - client_->left,
                                            client_->bottom - client_->top, ox, oy, only_symbol(set_, focus_key_),
                                            runway_, L"", t0, border ? &*border : nullptr),
                           set_.opacity * (on ? 1.0 : 0.5));
    } else {
      cl = hud::commands(scene, set_.opacity);
    }
  } else if (!flash_.empty()) {
    cl = hud::commands(hud::flash_scene(flash_), set_.opacity);
  }
  const double t1 = now_s();
  if (!gfx_.frame(cl)) {
    if (gfx_err_reported_++ < 5) logf("[gfx] draw error: %s", to_utf8(gfx_.error()).c_str());
  }
  const double t2 = now_s();
  build_ms_ = 0.9 * build_ms_ + 0.1 * (t1 - t0) * 1000;
  paint_ms_ = 0.9 * paint_ms_ + 0.1 * (t2 - t0) * 1000;
}

// ---------------------------------------------------------------- HUD border (docs/HUD_BORDER.md)
namespace {
// The fit check counts a symbol as cut once it reaches into the feather (fully clear 1° inside the border).
constexpr double kFitMarginDeg = 0.8;
constexpr double kFitMinScale = 0.6;  // the panel's size slider starts at 60 %
constexpr double kEyeMovedM = 0.10;   // the eye moved this far since tracing: the panel says so
}  // namespace

std::string App::border_key() const { return ac_title_.empty() ? std::string("(unknown aircraft)") : ac_title_; }

Seat App::seat() const { return state_ ? seat_of(eye_lateral_m(state_->cp_eye_x, state_->cp_off_x)) : Seat::kLeft; }

std::optional<std::vector<Vec2>> App::active_border() const {
  if (edit_.on) return edit_.pts;
  const auto it = set_.border_by_aircraft.find(border_key());
  if (it == set_.border_by_aircraft.end()) return std::nullopt;
  auto b = border_for(it->second, seat());
  if (!b) return std::nullopt;
  return std::move(b->pts);
}

bool App::can_edit_border() const {
  if (!state_ || !client_ || !box_ || !gfx_ok_) return false;
  if (opt_.replay.empty() && (!sim_.connected() || !hwnd_sim_)) return false;
  // parked at the gate is the usual place to trace; looking aside is fine too (the border is fixed to the cockpit)
  const char* r = hide_reason(*state_, show_above_kt());
  return !r || !std::strcmp(r, "ground_slow") || !std::strcmp(r, "not_forward");
}

RECT App::overlay_box(const RECT& cr) const {
  if (edit_.on) return cr;  // the editor takes the whole client: the window frame can be anywhere
  const int x = cr.left, y = cr.top, w = cr.right - cr.left, h = cr.bottom - cr.top;
  const int bw = static_cast<int>(w * set_.box_w), bh = static_cast<int>(h * set_.box_h);
  return RECT{x + (w - bw) / 2, y + (h - bh) / 2, x + (w - bw) / 2 + bw, y + (h - bh) / 2 + bh};
}

void App::relayout() {
  if (!client_) return;
  box_ = overlay_box(*client_);
  place();
}

void App::border_edit_begin() {
  if (edit_.on) return;
  if (!can_edit_border()) {
    say(tr(S::FlashBorderNeedCockpit));
    return;
  }
  std::vector<Vec2> start = default_border();  // the HUD canvas: dragging its corners is the simple way
  if (const auto it = set_.border_by_aircraft.find(border_key()); it != set_.border_by_aircraft.end())
    if (auto b = border_for(it->second, seat())) start = b->pts;
  edit_ = BorderEdit{};
  edit_.on = true;
  edit_.pts = std::move(start);
  edit_.t0 = now_s();
  edit_.reopen_window = win_.shown();
  if (edit_.reopen_window) win_.close();  // it floats above everything and would cover the frame
  hide_peek();
  focus_key_.clear();
  relayout();
  gfx_.set_input([this](unsigned m, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& r) {
    return border_input(m, wp, lp, r);
  });
  // Enter / Esc reach Birdie wherever the focus is (the toolbar panel leaves it in the sim)
  if (!RegisterHotKey(hwnd_, kHkEditSave, MOD_NOREPEAT, VK_RETURN)) log_line("[border] Enter not available as a key");
  if (!RegisterHotKey(hwnd_, kHkEditCancel, MOD_NOREPEAT, VK_ESCAPE)) log_line("[border] Esc not available as a key");
  SetForegroundWindow(gfx_.hwnd());  // works when Birdie's window had the focus; otherwise the first click takes it
  logf("[border] editing %s, %s seat, %zu points", border_key().c_str(), seat_key(seat()), edit_.pts.size());
  redraw();
  panel_push(true);
}

void App::border_edit_end(bool save) {
  if (!edit_.on) return;
  UnregisterHotKey(hwnd_, kHkEditSave);
  UnregisterHotKey(hwnd_, kHkEditCancel);
  if (GetCapture() == gfx_.hwnd()) ReleaseCapture();
  gfx_.set_input({});
  const bool had_focus = GetForegroundWindow() == gfx_.hwnd();
  std::vector<Vec2> pts = std::move(edit_.pts);
  const bool reopen = edit_.reopen_window;
  edit_ = BorderEdit{};
  if (save && state_ && sanitize_border(pts)) {
    Settings::BorderPoly b;
    b.pts = std::move(pts);
    b.eye = {eye_lateral_m(state_->cp_eye_x, state_->cp_off_x), state_->cp_eye_y, state_->cp_eye_z};
    auto& seats = set_.border_by_aircraft[border_key()];
    (seat() == Seat::kLeft ? seats.left : seats.right) = b;
    logf("[border] %s %s seat: %zu points saved", border_key().c_str(), seat_key(seat()), b.pts.size());
    say(tr(S::FlashBorderSaved));
  } else {
    log_line(save ? "[border] not saved (no data / bad polygon)" : "[border] editing cancelled");
    say(tr(S::FlashBorderCancelled));
  }
  relayout();
  if (had_focus && hwnd_sim_) SetForegroundWindow(hwnd_sim_);
  if (reopen) open_panel(L"#border");
  changed();
}

void App::border_clear() {
  if (set_.border_by_aircraft.erase(border_key())) {
    logf("[border] %s: cleared", border_key().c_str());
    say(tr(S::FlashBorderCleared));
  }
  changed();
}

void App::border_mirror() {
  const auto it = set_.border_by_aircraft.find(border_key());
  if (it == set_.border_by_aircraft.end()) return;
  const bool left = seat() == Seat::kLeft;
  const auto& own = left ? it->second.left : it->second.right;
  if (!own) return;
  (left ? it->second.right : it->second.left) = mirrored(*own);
  logf("[border] %s: other seat mirrors the %s seat", border_key().c_str(), seat_key(seat()));
  say(tr(S::FlashBorderMirrored));
  changed();
}

void App::border_fit() {
  const auto b = active_border();
  if (!b || !state_) return;
  const auto pts = hud::fixed_symbol_points(model_, *state_, set_);
  const auto k = fit_scale(*b, pts, kFitMarginDeg, kFitMinScale, std::max(set_.hud_scale, kFitMinScale));
  if (!k) return;
  set_.hud_scale = std::floor(*k * 20 + 1e-9) / 20;  // the size slider's 5 % steps, rounded down: still fits
  logf("[border] fit: hud_scale = %.2f", set_.hud_scale);
  say(trf(S::FlashBorderFitFmt, static_cast<int>(std::lround(set_.hud_scale * 100))));
  changed();
}

void App::border_hover(Vec2 p) {
  edit_.hover = edit_.add_edge = -1;
  edit_.add_at.reset();
  const double W = client_->right - client_->left, H = client_->bottom - client_->top;
  const double ox = box_->left - client_->left, oy = box_->top - client_->top;
  const auto hs = hud::border_vertices(model_, *state_, W, H, ox, oy, edit_.pts);
  double best = 11.0;
  for (size_t i = 0; i < hs.size(); ++i) {
    if (!hs[i]) continue;
    const double d = std::hypot(hs[i]->x - p.x, hs[i]->y - p.y);
    if (d < best) best = d, edit_.hover = static_cast<int>(i);
  }
  if (edit_.hover >= 0 || edit_.pts.size() >= static_cast<size_t>(kBorderMaxPoints)) return;
  best = 8.0;  // near an edge: offer a new point there
  for (size_t i = 0; i < hs.size(); ++i) {
    const auto& a = hs[i];
    const auto& b = hs[(i + 1) % hs.size()];
    if (!a || !b) continue;
    const Vec2 d = *b - *a;
    const double l2 = d.x * d.x + d.y * d.y;
    if (l2 < 1) continue;
    const double t = ((p.x - a->x) * d.x + (p.y - a->y) * d.y) / l2;
    if (t < 0.06 || t > 0.94) continue;
    const Vec2 q = *a + t * d;
    const double dist = std::hypot(q.x - p.x, q.y - p.y);
    if (dist < best) best = dist, edit_.add_at = q, edit_.add_edge = static_cast<int>(i);
  }
}

bool App::border_input(unsigned m, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result) {
  if (!edit_.on || !state_ || !client_ || !box_) return false;
  switch (m) {
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, edit_.drag >= 0 || edit_.hover >= 0 ? IDC_SIZEALL
                                     : edit_.add_at                       ? IDC_HAND
                                                                          : IDC_ARROW));
      result = TRUE;
      return true;
    case WM_CAPTURECHANGED:
      edit_.drag = -1;
      return true;
    case WM_KEYDOWN:  // the overlay has the focus (the keys below are hotkeys too; this is the fallback)
      if (wp == VK_RETURN || wp == VK_ESCAPE) PostMessageW(hwnd_, WM_BORDER_END, wp == VK_RETURN, 0);
      return true;
    default:
      break;
  }
  if (m != WM_MOUSEMOVE && m != WM_LBUTTONDOWN && m != WM_LBUTTONUP && m != WM_RBUTTONDOWN && m != WM_RBUTTONUP)
    return false;
  const Vec2 p{static_cast<double>(GET_X_LPARAM(lp)), static_cast<double>(GET_Y_LPARAM(lp))};
  const double W = client_->right - client_->left, H = client_->bottom - client_->top;
  const double ox = box_->left - client_->left, oy = box_->top - client_->top;
  auto plane = [&](Vec2 q) -> OptVec2 {
    auto r = hud::screen_to_border(model_, *state_, W, H, ox, oy, q);
    if (r) r = Vec2{std::clamp(r->x, -kBorderMaxDeg, kBorderMaxDeg), std::clamp(r->y, -kBorderMaxDeg, kBorderMaxDeg)};
    return r;
  };
  switch (m) {
    case WM_MOUSEMOVE:
      if (edit_.drag >= 0 && edit_.drag < static_cast<int>(edit_.pts.size())) {
        if (auto q = plane(p)) edit_.pts[edit_.drag] = *q;
      } else {
        border_hover(p);
      }
      break;
    case WM_LBUTTONDOWN:
      border_hover(p);
      if (edit_.hover >= 0) {
        edit_.drag = edit_.hover;
      } else if (edit_.add_at && edit_.add_edge >= 0) {
        if (auto q = plane(*edit_.add_at)) {
          edit_.pts.insert(edit_.pts.begin() + edit_.add_edge + 1, *q);
          edit_.drag = edit_.hover = edit_.add_edge + 1;
        }
      }
      edit_.add_at.reset();
      if (edit_.drag >= 0) SetCapture(gfx_.hwnd());
      break;
    case WM_LBUTTONUP:
      if (edit_.drag >= 0) {
        edit_.drag = -1;
        ReleaseCapture();
      }
      border_hover(p);
      break;
    case WM_RBUTTONUP:
      border_hover(p);
      if (edit_.hover >= 0 && edit_.pts.size() > 3) {
        edit_.pts.erase(edit_.pts.begin() + edit_.hover);
        border_hover(p);
      }
      break;
    default:
      break;
  }
  redraw();
  return true;
}

nlohmann::json App::border_status() const {
  nlohmann::json b;
  b["editing"] = edit_.on;
  b["can_edit"] = edit_.on || can_edit_border();
  b["seat"] = seat_key(seat());
  std::string how = "none";
  std::optional<Settings::BorderPoly> poly;
  bool other_own = false;
  if (const auto it = set_.border_by_aircraft.find(border_key()); it != set_.border_by_aircraft.end()) {
    poly = border_for(it->second, seat(), &how);
    other_own = (seat() == Seat::kLeft ? it->second.right : it->second.left).has_value();
  }
  b["state"] = how;
  b["other_own"] = how == "drawn" && other_own;  // "mirror to the other seat" replaces that seat's own trace
  b["eye_moved"] = false;
  b["cuts_fixed"] = false;
  b["fit_scale"] = nullptr;
  if (poly && state_ && !edit_.on) {
    const Vec3 eye{eye_lateral_m(state_->cp_eye_x, state_->cp_off_x), state_->cp_eye_y, state_->cp_eye_z};
    const Vec3 d = eye - poly->eye;
    b["eye_moved"] = state_->cp_zoom > 0.05 && std::sqrt(dot(d, d)) > kEyeMovedM;
    const auto pts = hud::fixed_symbol_points(model_, *state_, set_);
    std::vector<Vec2> now;
    now.reserve(pts.size());
    for (const auto& q : pts) now.push_back(set_.hud_scale * q);
    if (!all_inside(poly->pts, now, kFitMarginDeg)) {
      b["cuts_fixed"] = true;
      if (const auto k = fit_scale(poly->pts, pts, kFitMarginDeg, kFitMinScale, std::max(set_.hud_scale, kFitMinScale)))
        b["fit_scale"] = std::floor(*k * 20 + 1e-9) / 20;
    }
  }
  return b;
}

// ---------------------------------------------------------------- main loop
int App::run() {
  if (!opt_.replay.empty()) {
    std::string err;
    if (!load_replay(opt_.replay, replay_rows_, replay_hz_, replay_runway_, err)) {
      logf("[replay] cannot load %s: %s", opt_.replay.string().c_str(), err.c_str());
      return 2;
    }
    logf("[replay] %zu rows at %.0f Hz from %s", replay_rows_.size(), replay_hz_, opt_.replay.string().c_str());
    timeBeginPeriod(1);
  } else {
    std::wstring where;
    sim_dll_ = sim::load_simconnect_dll(&where);
    if (!sim_dll_) {
      log_line("[sim] SimConnect.dll not found");
      MessageBoxW(nullptr, tr(S::SimConnectMissing), L"Birdie", MB_ICONERROR | MB_OK);
      return 3;
    }
    logf("[sim] SimConnect.dll: %s", to_utf8(where).c_str());
  }
  if (!init_window()) {
    log_line("[app] main window could not be created");
    return 1;
  }
  register_hotkeys(true);
  sim_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  panel_events_ = {[this] { return panel_state(); }, [this](const std::string& t) { on_panel_message(t); },
                   [](const std::string& s) { log_line(s); }};
  start_panel();
  if (opt_.replay.empty()) ingame_update_if_old();  // a new Birdie brings its own panel along
  if (first_run_ && opt_.replay.empty()) {
    balloon(tr(S::FirstRunHello), false);
    open_panel(L"?welcome");  // requirements, the hotkey, and the in-sim panel offered once
  }
  why_ = tr(S::WhyStarting);

  const double t_start = now_s();
  t_stat_ = t_start;
  replay_t0_ = replay_next_ = t_start;
  double next_house = t_start;
  const sim::SimClient::Events ev{[this](const SimState& s) { on_state(s); },
                                  [this] {
                                    log_line("[sim] simulator closed");
                                    if (edit_.on) border_edit_end(false);
                                    state_.reset();
                                    runway_ = nullptr;
                                    pause_flags_ = 0;
                                    redraw();
                                  },
                                  [](const std::string& s) { log_line(s); },
                                  [this](const std::string& t, const std::string& m) { on_aircraft(t, m); },
                                  [this](unsigned flags) {
                                    pause_flags_ = flags;
                                    t_pause_ = now_s();
                                    redraw();
                                  }};

  while (running_) {
    const double now = now_s();
    if (now >= next_house) {
      housekeeping();
      next_house = now + 0.5;
    }
    double next = next_house;
    if (!replay_rows_.empty()) next = std::min(next, replay_next_);
    if (!flash_.empty()) next = std::min(next, flash_until_ + 0.01);
    if (edit_.on && now - edit_.t0 < 0.4) next = std::min(next, now + 0.016);
    if (anim_busy(now)) next = std::min(next, now + 0.016);  // openings / closings run even when the sim is quiet
    const DWORD timeout = static_cast<DWORD>(std::clamp((next - now_s()) * 1000.0, 0.0, 500.0));
    const HANDLE waits[2] = {sim_event_, panel_.event()};
    const DWORD r =
        MsgWaitForMultipleObjectsEx(panel_.running() ? 2 : 1, waits, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (r == WAIT_OBJECT_0 + 1) panel_.poll(panel_events_);
    if (r == WAIT_OBJECT_0 && sim_.connected()) {
      if (!sim_.dispatch(ev) && !sim_.connected()) {
        state_.reset();
        runway_ = nullptr;
      }
    }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) running_ = false;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!replay_rows_.empty() && now_s() >= replay_next_) replay_step();
    if (!flash_.empty() && now_s() > flash_until_) redraw();
    if (edit_.on && now_s() - edit_.t0 < 0.4) redraw();  // the editor's fade-in, even when the sim sends nothing
    else if (anim_busy(now_s())) redraw();
    if (opt_.replay_seconds > 0 && now_s() - t_start > opt_.replay_seconds) running_ = false;
  }
  if (!replay_rows_.empty()) timeEndPeriod(1);
  log_line("[app] exit");
  return 0;
}

}  // namespace birdie::app
