// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Windows helpers: app folder, log file, settings file, autostart, MSFS window.
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

#include "core/settings.h"

namespace birdie::app {

// %APPDATA%\Birdie (created on first use).
std::filesystem::path app_dir();
std::filesystem::path log_path();
std::filesystem::path settings_path();
// Folder of Birdie.exe.
std::filesystem::path exe_dir();

// Log file (UTF-8, timestamped lines, rotated at 2 MB). Also printed to the debugger.
void log_open();
void log_line(const std::string& utf8);
void logf(const char* fmt, ...);

Settings load_settings();
void save_settings(const Settings& s);

std::string to_utf8(const std::wstring& w);
std::wstring from_utf8(const std::string& s);

bool autostart_enabled();
bool set_autostart(bool on);

// Visible top-level window whose title starts with "Microsoft Flight Simulator", or nullptr.
HWND find_msfs_window();
// Client area in physical screen pixels.
bool client_rect_screen(HWND h, RECT& out);

// The taskbar's rectangle when it is on this monitor (where it shows, also while it hides itself), else false.
bool taskbar_rect(HMONITOR mon, RECT& out);
// The monitor's area the taskbar leaves free. With a self-hiding taskbar the system's work area is the whole monitor
// and the taskbar slides over whatever sits at that edge, so it is taken off here as well. Physical pixels.
RECT usable_area(HMONITOR mon);

// Monotonic seconds.
double now_s();

}  // namespace birdie::app
