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

// Monotonic seconds.
double now_s();

}  // namespace birdie::app
