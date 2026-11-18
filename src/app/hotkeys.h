// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Global hotkeys, set in the panel (settings "hotkeys": action -> "Ctrl+Alt+Insert", "" = none).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace birdie::app {

struct HotkeyAction {
  const char* name;  // settings key in "hotkeys"
};
// In menu order: toggle, xwind, glide_up, glide_down, lead_up, lead_down, bright_up, bright_down, size_up,
// size_down, preset_next, anim_test, panel, quit. (The defaults are in core/settings.cpp default_hotkeys().)
const std::vector<HotkeyAction>& hotkey_actions();

struct Combo {
  unsigned mod = 0;  // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
  unsigned vk = 0;
};
// "Ctrl+Alt+Shift+Insert" -> modifiers + virtual key. Key names: A-Z, 0-9, F1-F24, Insert, Delete, Home, End, PgUp,
// PgDn, Up, Down, Left, Right, Space, Num0-Num9, Num+, Num-, Num*, Num/. Needs a modifier unless the key is F1-F24.
// nullopt = empty or not understood.
std::optional<Combo> parse_combo(const std::string& s);
// The same combo written the canonical way ("ctrl+alt+insert" -> "Ctrl+Alt+Insert"); "" when not understood.
std::string canonical_combo(const std::string& s);

}  // namespace birdie::app
