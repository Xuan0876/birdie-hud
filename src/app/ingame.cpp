// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/ingame.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

#include "app/platform.h"

namespace fs = std::filesystem;

namespace birdie::app::ingame {
namespace {

// CMakeLists.txt writes panel_pack.rc: the index (one path per line) and every file, in that order.
constexpr int kIndexId = 2999;
constexpr int kFirstFileId = 3000;
constexpr const char* kPageDir = "html_ui/InGamePanels/Birdie/";
constexpr const char* kManifestTitle = "\"Birdie HUD panel\"";  // panel/birdie-panel/manifest.json

std::optional<std::string_view> resource(int id) {
  HMODULE self = GetModuleHandleW(nullptr);
  HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
  if (!r) return std::nullopt;
  HGLOBAL g = LoadResource(self, r);
  const void* p = g ? LockResource(g) : nullptr;
  if (!p) return std::nullopt;
  return std::string_view(static_cast<const char*>(p), SizeofResource(self, r));
}

fs::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
  CoTaskMemFree(p);
  return out;
}

bool is_link(const fs::path& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::optional<std::string> read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

fs::path native(const fs::path& root, const std::string& rel) {
  return root / fs::path(std::u8string(rel.begin(), rel.end())).make_preferred();
}

std::string describe(const std::error_code& ec) { return ec.message() + " (" + std::to_string(ec.value()) + ")"; }

// Every pack file under dir (the folders are made as needed).
bool write_all(const fs::path& dir, std::string* err) {
  for (const auto& f : pack()) {
    const fs::path p = native(dir, f.path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (o) o.write(f.data.data(), static_cast<std::streamsize>(f.data.size()));
    if (!o) {
      if (err) *err = "cannot write " + to_utf8(p.wstring());
      return false;
    }
  }
  return true;
}

}  // namespace

const std::vector<PackFile>& pack() {
  static const std::vector<PackFile> files = [] {
    std::vector<PackFile> out;
    const auto index = resource(kIndexId);
    if (!index) return out;
    std::istringstream lines{std::string(*index)};
    std::string line;
    for (int id = kFirstFileId; std::getline(lines, line); ++id) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      const auto data = resource(id);
      if (!data) break;  // index and resources out of step: stop rather than mislabel files
      out.push_back({line, *data});
    }
    return out;
  }();
  return files;
}

std::optional<std::string_view> page_file(const std::string& rel) {
  const std::string want = kPageDir + rel;
  for (const auto& f : pack())
    if (f.path == want) return f.data;
  return std::nullopt;
}

std::vector<fs::path> find_community(const fs::path& chosen) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!chosen.empty() && fs::is_directory(chosen, ec)) {
    out.push_back(chosen);
    return out;
  }
  const fs::path local = known_folder(FOLDERID_LocalAppData), roaming = known_folder(FOLDERID_RoamingAppData);
  std::vector<fs::path> cfgs;
  if (!local.empty()) cfgs.push_back(local / L"Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/UserCfg.opt");  // Store
  if (!roaming.empty()) cfgs.push_back(roaming / L"Microsoft Flight Simulator 2024/UserCfg.opt");                    // Steam
  static const std::regex re(R"re(InstalledPackagesPath\s+"([^"]+)")re");
  for (const auto& cfg : cfgs) {
    const auto text = read_file(cfg);
    std::smatch m;
    if (!text || !std::regex_search(*text, m, re)) continue;
    const fs::path c = fs::path(from_utf8(m[1].str())) / L"Community";
    if (!fs::is_directory(c, ec)) continue;
    const bool dup = std::any_of(out.begin(), out.end(), [&](const fs::path& o) { return fs::equivalent(o, c, ec); });
    if (!dup) out.push_back(c);
  }
  return out;
}

fs::path community_from_pick(const fs::path& picked) {
  std::error_code ec;
  if (_wcsicmp(picked.filename().c_str(), L"Community") != 0 && fs::is_directory(picked / L"Community", ec))
    return picked / L"Community";
  return picked;
}

const char* state_key(State s) {
  switch (s) {
    case State::kCurrent: return "current";
    case State::kOld: return "old";
    case State::kForeign: return "foreign";
    default: return "none";
  }
}

State state_in(const fs::path& community) {
  const fs::path dir = community / kFolderName;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return State::kNone;
  const auto manifest = read_file(dir / L"manifest.json");
  if (!manifest || manifest->find(kManifestTitle) == std::string::npos) return State::kForeign;
  for (const auto& f : pack()) {
    const fs::path p = native(dir, f.path);
    if (fs::file_size(p, ec) != f.data.size() || ec) return State::kOld;
    const auto have = read_file(p);
    if (!have || *have != f.data) return State::kOld;
  }
  return State::kCurrent;
}

bool install(const fs::path& folder, std::string* err) {
  if (pack().empty()) {
    if (err) *err = "this Birdie.exe carries no panel";
    return false;
  }
  const fs::path target = folder / kFolderName;
  std::error_code ec;
  if (fs::exists(target, ec) && state_in(folder) == State::kForeign) {
    if (err) *err = "a folder called birdie-panel is there and it is not Birdie's";
    return false;
  }
  if (is_link(target)) return write_all(target, err);  // the user's own link (add-on manager): fill what it points to

  // Write a fresh copy next to it, then swap: the sim never sees half a package.
  const fs::path fresh = folder / L"birdie-panel.new", old = folder / L"birdie-panel.old";
  fs::remove_all(fresh, ec);
  if (!write_all(fresh, err)) {
    fs::remove_all(fresh, ec);
    return false;
  }
  if (fs::exists(target, ec)) {
    fs::remove_all(old, ec);
    fs::rename(target, old, ec);
    if (ec) {  // in use (the sim is running): overwrite file by file instead
      log_line("[ingame] swap refused (" + describe(ec) + "), writing in place");
      std::error_code ec2;
      fs::remove_all(fresh, ec2);
      return write_all(target, err);
    }
  }
  fs::rename(fresh, target, ec);
  if (ec) {
    std::error_code ec2;
    if (fs::exists(old, ec2)) fs::rename(old, target, ec2);  // put the old one back
    fs::remove_all(fresh, ec2);
    if (err) *err = "cannot move the new copy in place: " + describe(ec);
    return false;
  }
  fs::remove_all(old, ec);  // a leftover .old does no harm (MSFS skips folders without a manifest it can read)
  return true;
}

bool remove(const fs::path& community, std::string* err) {
  const fs::path target = community / kFolderName;
  std::error_code ec;
  if (!fs::exists(target, ec) && !is_link(target)) return true;
  if (!is_link(target) && state_in(community) == State::kForeign) {
    if (err) *err = "the birdie-panel folder there is not Birdie's; left alone";
    return false;
  }
  if (is_link(target)) {  // unlink only; what it points to stays
    if (!RemoveDirectoryW(target.c_str()) && !DeleteFileW(target.c_str())) {
      if (err) *err = "cannot remove the link (error " + std::to_string(GetLastError()) + ")";
      return false;
    }
    return true;
  }
  fs::remove_all(target, ec);
  if (ec) {
    if (err) *err = "cannot remove it: " + describe(ec) + " (is the sim running?)";
    return false;
  }
  return true;
}

}  // namespace birdie::app::ingame
