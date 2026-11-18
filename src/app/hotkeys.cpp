// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/hotkeys.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace birdie::app {
namespace {

struct KeyName {
  const char* name;
  unsigned vk;
};
const KeyName kKeys[] = {
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME},    {"End", VK_END},     {"PgUp", VK_PRIOR},
    {"PgDn", VK_NEXT},     {"Up", VK_UP},         {"Down", VK_DOWN},    {"Left", VK_LEFT},   {"Right", VK_RIGHT},
    {"Space", VK_SPACE},   {"Num+", VK_ADD},      {"Num-", VK_SUBTRACT}, {"Num*", VK_MULTIPLY}, {"Num/", VK_DIVIDE},
};

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::optional<std::pair<unsigned, std::string>> key_of(const std::string& t) {
  const std::string l = lower(t);
  for (const auto& k : kKeys)
    if (l == lower(k.name)) return std::make_pair(k.vk, std::string(k.name));
  if (l == "pageup") return std::make_pair(static_cast<unsigned>(VK_PRIOR), std::string("PgUp"));
  if (l == "pagedown") return std::make_pair(static_cast<unsigned>(VK_NEXT), std::string("PgDn"));
  if (l.size() == 1 && std::isalnum(static_cast<unsigned char>(l[0])))
    return std::make_pair(static_cast<unsigned>(std::toupper(static_cast<unsigned char>(l[0]))),
                          std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(l[0])))));
  if (l.size() >= 2 && l[0] == 'f' && std::all_of(l.begin() + 1, l.end(), ::isdigit)) {
    const int n = std::stoi(l.substr(1));
    if (n >= 1 && n <= 24) return std::make_pair(static_cast<unsigned>(VK_F1 + n - 1), "F" + std::to_string(n));
  }
  if (l.size() == 4 && l.rfind("num", 0) == 0 && std::isdigit(static_cast<unsigned char>(l[3])))
    return std::make_pair(static_cast<unsigned>(VK_NUMPAD0 + (l[3] - '0')), "Num" + l.substr(3));
  return std::nullopt;
}

struct Parsed {
  Combo c;
  std::string text;
};
std::optional<Parsed> parse(const std::string& s) {
  if (s.empty()) return std::nullopt;
  Parsed p;
  std::string mods, key;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find('+', i);
    // "Num+" ends in a plus: a trailing "+" belongs to the key
    if (j != std::string::npos && j + 1 == s.size()) j = std::string::npos;
    std::string t = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    while (!t.empty() && t.back() == ' ') t.pop_back();
    const std::string l = lower(t);
    if (j == std::string::npos) {
      key = t;
      break;
    }
    if (l == "ctrl" || l == "control") p.c.mod |= MOD_CONTROL;
    else if (l == "alt") p.c.mod |= MOD_ALT;
    else if (l == "shift") p.c.mod |= MOD_SHIFT;
    else if (l == "win") p.c.mod |= MOD_WIN;
    else return std::nullopt;
    i = j + 1;
  }
  const auto k = key_of(key);
  if (!k) return std::nullopt;
  p.c.vk = k->first;
  const bool fkey = p.c.vk >= VK_F1 && p.c.vk <= VK_F24;
  if (!p.c.mod && !fkey) return std::nullopt;  // a bare letter would eat the user's typing everywhere
  if (p.c.mod & MOD_CONTROL) mods += "Ctrl+";
  if (p.c.mod & MOD_ALT) mods += "Alt+";
  if (p.c.mod & MOD_SHIFT) mods += "Shift+";
  if (p.c.mod & MOD_WIN) mods += "Win+";
  p.text = mods + k->second;
  return p;
}

}  // namespace

const std::vector<HotkeyAction>& hotkey_actions() {
  static const std::vector<HotkeyAction> a = {
      {"toggle"},   {"xwind"},     {"glide_up"},    {"glide_down"}, {"lead_up"},   {"lead_down"}, {"bright_up"},
      {"bright_down"}, {"size_up"}, {"size_down"}, {"preset_next"}, {"anim_test"}, {"panel"},     {"quit"},
  };
  return a;
}

std::optional<Combo> parse_combo(const std::string& s) {
  const auto p = parse(s);
  if (!p) return std::nullopt;
  return p->c;
}

std::string canonical_combo(const std::string& s) {
  const auto p = parse(s);
  return p ? p->text : std::string();
}

}  // namespace birdie::app
