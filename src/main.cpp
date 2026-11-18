// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
//
//   Birdie.exe                              resident: tray icon, HUD over MSFS when the ChasePlane cockpit view is on
//   Birdie.exe --replay <fixture_dir> [--seconds N]
//                                           offline self-test: plays a parity fixture over the primary monitor
//   Birdie.exe --export-panel <folder>      writes the in-sim panel to <folder>\birdie-panel and exits (0 = done)
//   Birdie.exe --remove-panel <folder>      removes <folder>\birdie-panel if it is Birdie's and exits
#include <windows.h>
#include <objbase.h>
// GDI+ needs these from objidl before its own headers
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <string>

#include "app/app.h"
#include "app/i18n.h"
#include "app/ingame.h"
#include "app/platform.h"
#include "core/version.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // all placement in physical pixels

  birdie::app::Options opt;
  std::wstring panel_cmd, panel_dir;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--replay" && i + 1 < argc) opt.replay = argv[++i];
    else if (a == L"--seconds" && i + 1 < argc) opt.replay_seconds = _wtof(argv[++i]);
    else if ((a == L"--export-panel" || a == L"--remove-panel") && i + 1 < argc) panel_cmd = a, panel_dir = argv[++i];
  }
  if (argv) LocalFree(argv);

  birdie::app::log_open();
  if (!panel_cmd.empty()) {  // no window, no tray: for scripts and for people who manage add-ons by hand
    std::string err;
    const bool ok = panel_cmd == L"--export-panel" ? birdie::app::ingame::install(panel_dir, &err)
                                                   : birdie::app::ingame::remove(panel_dir, &err);
    birdie::app::logf("[ingame] %s %s: %s", birdie::app::to_utf8(panel_cmd).c_str(),
                      birdie::app::to_utf8(panel_dir).c_str(), ok ? "done" : err.c_str());
    return ok ? 0 : 1;
  }
  birdie::app::logf("[app] Birdie %s%s", birdie::version(), opt.replay.empty() ? "" : " (replay)");

  HANDLE mutex = nullptr;
  if (opt.replay.empty()) {
    mutex = CreateMutexW(nullptr, FALSE, L"Local\\Birdie_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      birdie::app::log_line("[app] already running, exit");
      birdie::app::set_language(birdie::app::resolve_language(birdie::app::load_settings().language));
      MessageBoxW(nullptr, birdie::app::tr(birdie::app::S::AlreadyRunning), L"Birdie", MB_ICONINFORMATION | MB_OK);
      return 0;
    }
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Gdiplus::GdiplusStartupInput gsi;
  ULONG_PTR gdip = 0;
  Gdiplus::GdiplusStartup(&gdip, &gsi, nullptr);

  int rc;
  {
    birdie::app::App app(opt);
    rc = app.run();
  }

  Gdiplus::GdiplusShutdown(gdip);
  CoUninitialize();
  if (mutex) CloseHandle(mutex);
  return rc;
}
