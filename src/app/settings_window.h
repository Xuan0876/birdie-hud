// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Birdie's own settings window (tray: Open panel). A borderless, square-cornered window that shows the panel page
// (the same birdie.html as the MSFS toolbar panel, served by PanelServer on 127.0.0.1) in WebView2, so the page
// keeps talking to Birdie over the same WebSocket. The page has no title bar of its own drawn by Windows: its head
// row is the drag area (CSS app-region: drag) and it draws its own close mark.
//
// Page -> window (window.chrome.webview.postMessage, JSON):  {"host":"size","h":<css px>}  fit the height to the page
//                                                            {"host":"close"}               hide
//                                                            {"host":"drag"}                move (fallback for app-region)
//
//                                                            {"host":"open"}                (peek) open the full window
//
// Closing only hides it (Birdie keeps running); the WebView2 is released then, so nothing of it stays in memory while
// flying. Needs the WebView2 runtime (part of Windows 11); without it open() returns false and the caller falls back
// to the desktop browser.
//
// The same class also makes the "peek": a small strip (the bird, the voice, the switch) that floats up next to the tray
// icon while the mouse rests on it. It never takes the focus, has no taskbar button, and keeps its browser while hidden
// (release() frees it) so the next hover shows at once.
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace birdie::app {

class SettingsWindow {
 public:
  struct Host {
    std::function<void(const std::string&)> log;
    std::function<void(int x, int y)> moved;  // window dragged to a new top-left (physical px)
    std::function<void()> failed;            // the browser did not start after all (asynchronous): fall back
    std::function<void()> open_full;         // (peek) the strip was clicked
  };

  explicit SettingsWindow(bool peek = false);
  ~SettingsWindow();
  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;

  // Brings the window up showing `url` (created on first use, otherwise just brought to the front). (x, y) = last
  // top-left in physical px, or INT_MIN for "top right of `sim_window`'s monitor". Returns false when the WebView2 runtime
  // is missing or cannot start.
  bool open(const std::wstring& url, int x, int y, HWND sim_window, HICON big_icon, HICON small_icon, const Host& host);
  // Peek: show `url` next to the tray icon `anchor` (screen rect, physical px), without taking the focus.
  bool peek(const std::wstring& url, const RECT& anchor, const Host& host);
  void close();    // hide (and release the browser; a peek only hides)
  // The panel's look (settings ui_style): window background and border. Shared by every Birdie window; applied to
  // this one now and to any window created later.
  void set_look(const std::string& style);
  void release();  // free the browser of a hidden peek
  bool shown() const;
  bool is_foreground() const;  // the window (or the browser inside it) has the focus
  HWND hwnd() const;

  static constexpr int kCssWidth = 352;      // the v2 "pocket" panel width
  static constexpr int kCssPeekWidth = 236;  // the tray peek card

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace birdie::app
