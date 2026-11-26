// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/platform.h"

#include <shellapi.h>
#include <shlobj.h>

#include <share.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

namespace birdie::app {
namespace {

std::mutex g_log_mu;
FILE* g_log = nullptr;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunName[] = L"Birdie";

std::wstring exe_path() {
  wchar_t p[MAX_PATH];
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  return p;
}

std::wstring autostart_command() { return L"\"" + exe_path() + L"\""; }

}  // namespace

fs::path app_dir() {
  static fs::path dir = [] {
    PWSTR p = nullptr;
    fs::path d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) d = fs::path(p) / L"Birdie";
    CoTaskMemFree(p);
    if (d.empty()) d = fs::path(exe_path()).parent_path();
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
  }();
  return dir;
}

fs::path log_path() { return app_dir() / L"birdie.log"; }
fs::path settings_path() { return app_dir() / L"settings.json"; }
fs::path exe_dir() { return fs::path(exe_path()).parent_path(); }

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring from_utf8(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

void log_open() {
  std::lock_guard<std::mutex> lk(g_log_mu);
  if (g_log) return;
  const fs::path p = log_path();
  std::error_code ec;
  if (fs::exists(p, ec) && fs::file_size(p, ec) > 2'000'000) fs::rename(p, fs::path(p).concat(L".old"), ec);
  g_log = _wfsopen(p.c_str(), L"ab", _SH_DENYNO);  // others may read it (or a second instance report itself)
  if (g_log) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "\n===== %04d-%02d-%02d %02d:%02d:%02d start =====\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond);
    std::fflush(g_log);
  }
}

void log_line(const std::string& s) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  char ts[32];
  std::snprintf(ts, sizeof ts, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
  OutputDebugStringA((ts + s + "\n").c_str());
  std::lock_guard<std::mutex> lk(g_log_mu);
  if (!g_log) return;
  std::fputs(ts, g_log);
  std::fputs(s.c_str(), g_log);
  std::fputc('\n', g_log);
  std::fflush(g_log);
}

void logf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  log_line(buf);
}

Settings load_settings() {
  std::ifstream f(settings_path(), std::ios::binary);
  if (!f) return Settings{};
  try {
    return settings_from_json(nlohmann::json::parse(f));
  } catch (const std::exception& e) {
    logf("[set] settings.json unreadable, using defaults: %s", e.what());
    return Settings{};
  }
}

void save_settings(const Settings& s) {
  const fs::path p = settings_path();
  const fs::path tmp = fs::path(p).concat(L".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << settings_to_json(s).dump(1);
  }
  MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool autostart_enabled() {
  wchar_t buf[1024];
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
    return false;
  return autostart_command() == buf;
}

bool set_autostart(bool on) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
  LSTATUS rc;
  if (on) {
    const std::wstring cmd = autostart_command();
    rc = RegSetValueExW(k, kRunName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    rc = RegDeleteValueW(k, kRunName);
    if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
  }
  RegCloseKey(k);
  return rc == ERROR_SUCCESS;
}

HWND find_msfs_window() {
  struct Ctx {
    HWND hit = nullptr;
  } ctx;
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        wchar_t buf[256];
        GetWindowTextW(h, buf, 256);
        if (wcsncmp(buf, L"Microsoft Flight Simulator", 26) == 0 && IsWindowVisible(h)) {
          reinterpret_cast<Ctx*>(lp)->hit = h;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.hit;
}

bool client_rect_screen(HWND h, RECT& out) {
  RECT r;
  if (!GetClientRect(h, &r)) return false;
  POINT pt{0, 0};
  ClientToScreen(h, &pt);
  out = {pt.x, pt.y, pt.x + r.right, pt.y + r.bottom};
  return true;
}

double now_s() {
  static const double freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart / freq;
}

bool taskbar_rect(HMONITOR mon, RECT& out) {
  APPBARDATA ab = {sizeof(ab)};
  if (!SHAppBarMessage(ABM_GETTASKBARPOS, &ab)) return false;
  if (MonitorFromRect(&ab.rc, MONITOR_DEFAULTTONULL) != mon) return false;
  out = ab.rc;
  return true;
}

RECT usable_area(HMONITOR mon) {
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  RECT r = mi.rcWork, tb;
  if (!taskbar_rect(mon, tb)) return r;
  const RECT& m = mi.rcMonitor;
  const int w = tb.right - tb.left, h = tb.bottom - tb.top;
  if (w >= h) {  // along the top or bottom
    if (tb.bottom >= m.bottom - 1) r.bottom = std::min(r.bottom, tb.top);
    else if (tb.top <= m.top + 1) r.top = std::max(r.top, tb.bottom);
  } else {
    if (tb.left <= m.left + 1) r.left = std::max(r.left, tb.right);
    else if (tb.right >= m.right - 1) r.right = std::min(r.right, tb.left);
  }
  return r;
}

}  // namespace birdie::app
