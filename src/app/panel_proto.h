// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// What the panel may read and change (S5). Messages are JSON text frames:
//
//   Birdie -> panel  {"type":"state", "settings":{...}, "status":{...}}   on connect and whenever something changes
//                    {"type":"notice", "text":"..."}                      the HUD's short flash messages
//   panel -> Birdie  {"type":"set", "key":"opacity", "value":0.7}
//                    {"type":"action", "name":"ra_calibrate" | "ra_clear" | "open_log" | "quit" | "hello"}
//                    {"type":"action", "name":"autostart", "value":true}
//                    {"type":"action", "name":"border_edit" | "border_clear" | "border_mirror" | "border_fit"}
//                        HUD border (docs/HUD_BORDER.md); its state is status.border
//
// The panel can change everything the tray menu used to (S5 acceptance); values are checked and clamped here.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/settings.h"

namespace birdie::app {

// The settings the panel shows, under their settings.json names.
nlohmann::json panel_settings(const Settings& s);

// Applies one "set". Returns false (and leaves s alone) for an unknown key or a value of the wrong type.
// Numbers are clamped to the ranges the HUD can draw.
bool apply_panel_set(Settings& s, const std::string& key, const nlohmann::json& value);

}  // namespace birdie::app
