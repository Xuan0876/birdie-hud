// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The in-sim toolbar panel (panel/birdie-panel), carried inside Birdie.exe: the build puts every file of the package
// into the exe's resources (CMakeLists: panel_pack.rc), so one exe serves the settings page on 127.0.0.1 and can
// install, update or remove the package in the MSFS 2024 Community folder. Optional: Birdie works without it.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace birdie::app::ingame {

struct PackFile {
  std::string path;        // relative to the package root, '/' separated ("html_ui/InGamePanels/Birdie/birdie.js")
  std::string_view data;   // the resource bytes (live as long as the exe)
};

// Every file of the package, in the order of the build's index. Empty if the exe has no pack.
const std::vector<PackFile>& pack();
// One file of the settings page (html_ui/InGamePanels/Birdie/<rel>), for the 127.0.0.1 server.
std::optional<std::string_view> page_file(const std::string& rel);

constexpr const wchar_t* kFolderName = L"birdie-panel";

// Community folders of MSFS 2024 (Microsoft Store, Steam), from UserCfg.opt's InstalledPackagesPath. `chosen` (the
// user's own pick, may be empty) wins when it is a folder.
std::vector<std::filesystem::path> find_community(const std::filesystem::path& chosen);
// A folder the user picked: its Community subfolder when it has one (they picked the packages folder), else itself.
std::filesystem::path community_from_pick(const std::filesystem::path& picked);

enum class State {
  kNone,     // no birdie-panel folder there
  kCurrent,  // every file the same as this exe's
  kOld,      // Birdie's panel, other files (another version)
  kForeign,  // a folder called birdie-panel that is not Birdie's: never touched
};
const char* state_key(State s);  // none current old foreign
State state_in(const std::filesystem::path& community);

// Writes the package to <folder>/birdie-panel (replacing Birdie's own older copy); also used to export it anywhere.
bool install(const std::filesystem::path& folder, std::string* err);
// Removes <community>/birdie-panel if it is Birdie's (a link is unlinked, its target left alone).
bool remove(const std::filesystem::path& community, std::string* err);

}  // namespace birdie::app::ingame
