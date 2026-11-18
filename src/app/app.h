// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The resident program: SimConnect, overlay window, tray icon, hotkeys, the panel server (reference: overlay.py run_live).
#pragma once

#include <windows.h>

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "app/ingame.h"
#include "app/panel_server.h"
#include "app/settings_window.h"
#include "core/border.h"
#include "core/model.h"
#include "core/runway.h"
#include "core/settings.h"
#include "core/state.h"
#include "render/d2d.h"
#include "sim/sim_client.h"

namespace birdie::app {

struct Options {
  // Offline self-test: play a parity fixture's input.csv as if it came from SimConnect, over the primary
  // monitor, with the fixture's runway (no simulator needed).
  std::filesystem::path replay;
  double replay_seconds = 0;  // 0 = loop forever
};

class App {
 public:
  explicit App(Options opt);
  ~App();
  int run();

 private:
  static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp);
  LRESULT on_message(HWND h, UINT m, WPARAM wp, LPARAM lp);

  bool init_window();
  void register_hotkeys(bool report);  // report: a balloon when some could not be registered
  void unregister_hotkeys();
  void on_hotkey(int id);
  void housekeeping();
  void connect_sim();
  void update_runway();
  void place();
  void on_state(SimState s);
  void replay_step();
  void redraw();
  bool compute_why();
  void say(const std::wstring& text);
  void calibrate_ra();
  void clear_ra();
  void store_ra(double ground_reading_ft, bool automatic);
  double ra_offset() const;  // for the current aircraft
  double show_above_kt() const { return set_.auto_show ? set_.auto_show_kt : 0.0; }  // hide_reason's ground speed
  bool holding() const;      // the panel's look pages are open: the HUD shows where it would rest (ground, up high)
  void apply_look();         // the renderer's font for the settings' hud_font
  // Openings and closings (hud/anim.h): p = 0 hidden .. 1 shown, moving towards hud_on_.
  double anim_progress(double now) const;
  bool anim_busy(double now) const;
  double anim_step(bool want, double now);  // follows `want`; 1 = shown and settled, 0 = animating or hidden
  void anim_test(bool closing);
  void on_aircraft(const std::string& title, const std::string& model);
  void check_chaseplane();
  void balloon(const std::wstring& text, bool warning);

  // HUD border (core/border.h, docs/HUD_BORDER.md)
  std::string border_key() const;  // settings key: the aircraft TITLE
  Seat seat() const;               // from the eye's lateral offset
  std::optional<std::vector<Vec2>> active_border() const;  // what clips the HUD now (the editor's while editing)
  bool can_edit_border() const;
  void border_edit_begin();
  void border_edit_end(bool save);
  void border_clear();
  void border_mirror();
  void border_fit();
  bool border_input(unsigned msg, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result);
  void border_hover(Vec2 p);
  nlohmann::json border_status() const;
  RECT overlay_box(const RECT& client) const;  // where the overlay window goes (the whole client while editing)
  void relayout();
  void changed();  // settings changed: save, redraw, refresh tray and panel

  // panel (S5): settings and status for the MSFS toolbar panel / the same page in a browser
  void start_panel();
  void on_panel_message(const std::string& text);
  nlohmann::json panel_status() const;
  std::string panel_state() const;  // the "state" message
  void panel_push(bool force);  // send the state if it changed (or always)
  void open_panel(const std::wstring& fragment = L"");  // Birdie's own window (#border: that page); else the browser
  void open_panel_in_browser(const std::wstring& url);
  void set_focus(const std::string& key);           // panel hover: that symbol lights up on the real HUD
  void show_peek();
  void hide_peek();
  void peek_tick();  // while the strip is up: gone shortly after the mouse leaves icon and strip
  std::wstring panel_url() const;

  // the in-sim toolbar panel, carried in the exe (app/ingame.h): optional, installed from the settings window
  void ingame_refresh();                          // find the Community folder(s) and what is in them
  void ingame_update_if_old();                    // at start: an older copy of Birdie's panel is replaced
  void ingame_action(const std::string& name);    // the panel's ingame_* actions
  nlohmann::json ingame_status() const;
  // "Birdie beta" on the HUD while the version is a pre-release ("0.9.0-beta"), empty for a stable one
  static std::wstring watermark();
  std::optional<std::filesystem::path> pick_folder(const wchar_t* title);  // the Windows folder picker

  // tray
  void tray_add();
  void tray_update();
  void show_menu();  // Open panel / Show HUD / Quit
  void on_command(int id);
  std::wstring status_text() const;
  std::wstring runway_text() const;

  Options opt_;
  bool first_run_ = false;  // no settings file yet: say hello from the tray
  Settings set_;
  Model model_;
  render::D2DRenderer gfx_;
  bool gfx_ok_ = false;
  sim::SimClient sim_;
  HANDLE sim_event_ = nullptr;
  bool sim_dll_ = false;
  double t_connect_ = -1e9;
  double t_airports_ = -1e9;
  const Runway* runway_ = nullptr;
  std::optional<Runway> replay_runway_;
  std::vector<SimState> replay_rows_;
  double replay_hz_ = 30.0;
  size_t replay_i_ = 0;
  double replay_t0_ = 0, replay_next_ = 0;

  std::optional<SimState> state_;
  double t_state_ = 0;
  double high_since_ = -1;  // since when the radio altitude is above auto_hide_ft (-1 = not above)
  double hold_until_ = 0;   // the panel's "hold" (look pages open), renewed by the panel
  bool hud_on_ = false;     // where the animation is heading: shown or hidden
  double anim_t0_ = 0, anim_p0_ = 0;  // when it set off, and from which progress
  double anim_hide_until_ = 0;        // anim_test("out"): pretend hidden until then, then open again
  unsigned pause_flags_ = 0;  // last Pause_EX1 flags (core/pause.h), 0 = running
  // Whose window is in front. Birdie's own windows (settings window, tray card) count as the sim, so the HUD stays
  // while the user changes settings there (the only way when the in-sim panel is not installed).
  enum class Front { Sim, Birdie, Other };
  Front front();
  Front last_front_ = Front::Other;
  HWND hwnd_ = nullptr;      // hidden window: tray, hotkeys, menu
  HWND hwnd_sim_ = nullptr;  // MSFS main window
  std::optional<RECT> client_;
  std::optional<RECT> box_;  // overlay window, physical px
  std::wstring flash_;
  double flash_until_ = 0;
  std::wstring why_;
  const char* why_key_ = "starting";  // the same reason for the panel: starting, nosim, wait, nocp, ext, look, ...
  std::deque<double> ra_ground_;  // raw RA while standing still on the ground
  std::string ac_title_;          // SimConnect TITLE of the user aircraft
  double t_no_cp_ = -1;           // since when the ChasePlane L-vars read nothing (-1 = they do)
  bool cp_warned_ = false;
  bool running_ = true;
  UINT msg_taskbar_created_ = 0;
  HICON icon_on_ = nullptr, icon_off_ = nullptr;
  bool tray_active_ = false;
  std::vector<std::string> hotkey_failed_;  // the actions whose combo is taken (status.hotkey_failed)
  double hk_capture_until_ = 0;             // the panel is recording a combo: no hotkeys until then
  PanelServer panel_;
  PanelServer::Events panel_events_;
  std::string panel_sent_;  // last state message, to send only changes
  std::vector<std::filesystem::path> community_;  // MSFS Community folders found (usually one)
  ingame::State ingame_ = ingame::State::kNone;   // the first one that has something, else kNone
  std::string ingame_note_;   // last outcome for the panel: installed, updated, removed, exported, failed
  std::string ingame_err_;    // the reason, when failed
  double t_ingame_ = -1e9;    // last refresh
  SettingsWindow win_;
  SettingsWindow peek_{true};  // hover over the tray icon: the bird, the voice, the switch
  bool peek_ok_ = true;        // false once WebView2 failed: the plain tooltip comes back
  double peek_inside_ = 0;     // last time the mouse was on the icon or the strip
  HICON win_icon_big_ = nullptr, win_icon_small_ = nullptr;
  struct BorderEdit {
    bool on = false;
    std::vector<Vec2> pts;  // HUD-plane degrees
    int hover = -1, drag = -1, add_edge = -1;
    OptVec2 add_at;  // screen px of a point to insert after vertex add_edge
    double t0 = 0;
    bool reopen_window = false;  // the settings window was open: back to its border page afterwards
  } edit_;
  std::string focus_key_;  // show_* key the panel points at, "" = none
  double focus_until_ = 0;

  // stats for the periodic log line
  int n_frames_ = 0, n_msgs_ = 0;
  double build_ms_ = 0, paint_ms_ = 0, t_stat_ = 0, last_print_ = 0;
  int gfx_err_reported_ = 0;
};

}  // namespace birdie::app
