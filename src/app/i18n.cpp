// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "app/i18n.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace birdie::app {
namespace {

Lang g_lang = Lang::kEn;

struct Pair {
  S id;
  const wchar_t* zh;
  const wchar_t* en;
};

// Formats: %ls = text, %.1f / %d = numbers (swprintf).
const Pair kStrings[] = {
    {S::ShowOverlay, L"显示 HUD", L"Show HUD"},  // the menu adds a tab and the hotkey set in the panel
    {S::OpenPanel, L"打开面板", L"Open panel"},
    {S::PanelDown, L"面板连不上（端口被占用？见日志）", L"The panel cannot connect (port in use? see the log)"},
    {S::OpenSettingsFolder, L"打开设置文件夹", L"Open the settings folder"},
    {S::RunwayFmt, L"跑道：%hs %hs（%.0f×%.0f m）", L"Runway: %hs %hs (%.0f×%.0f m)"},
    {S::RunwayNone, L"跑道：未选中", L"Runway: none"},
    {S::Quit, L"退出", L"Quit"},
    {S::Showing, L"正在显示", L"showing"},
    {S::Off, L"已关闭", L"turned off"},
    {S::HiddenFmt, L"隐藏：%ls", L"hidden: %ls"},
    {S::WhyStarting, L"启动中", L"starting"},
    {S::WhySimNotRunning, L"模拟器没开", L"simulator not running"},
    {S::WhyWaitData, L"等数据", L"waiting for data"},
    {S::WhyNoChasePlane, L"没检测到 ChasePlane", L"ChasePlane not detected"},
    {S::WhyNoCamera, L"读不到 MSFS 镜头", L"No MSFS camera data"},
    {S::FlashCamOriginFmt, L"MSFS 镜头原点已标定（%+.2f m）", L"MSFS camera origin measured (%+.2f m)"},
    {S::FlashCamOriginNeed, L"MSFS 镜头原点还没标定，跑道框会偏：停稳或暂停几秒即可",
     L"MSFS camera origin not measured yet, the runway will be off: stand still or pause a few seconds"},
    {S::FlashCamOriginRedo, L"停稳几秒（或暂停），重新标定 MSFS 镜头原点", L"Stand still a few seconds (or pause) to measure the MSFS camera origin again"},
    {S::WhyExternalView, L"外部视角", L"external view"},
    {S::WhyNotForward, L"没在看前方", L"not looking ahead"},
    {S::WhyGroundSlow, L"地面低速", L"slow on the ground"},
    {S::WhyNotForeground, L"MSFS 不在前台", L"MSFS not in front"},
    {S::WhyPaused, L"模拟器暂停了", L"simulator paused"},
    {S::WhyDataStopped, L"数据停了（暂停？）", L"no data (paused?)"},
    {S::WhyNoWindow, L"没找到 MSFS 窗口", L"MSFS window not found"},
    {S::WhyHigh, L"高空自动隐藏", L"hidden up high"},
    {S::TerrainStreamStartFmt, L"地形：开始下载附近 %d 块（约 %ls）", L"Terrain: downloading %d tiles nearby (about %ls)"},
    {S::TerrainStreamDoneFmt, L"地形：附近 %d 块下载好了", L"Terrain: %d tiles nearby ready"},
    {S::TerrainDownloadingFmt, L"地形下载中 %d/%d", L"terrain downloading %d/%d"},
    {S::TerrainNoNetwork, L"地形：连不上下载服务器，过一会儿再试", L"Terrain: cannot reach the download server, trying again shortly"},
    {S::TerrainDiskFull, L"地形：磁盘快满了，停止下载", L"Terrain: the disk is nearly full, downloads stopped"},
    {S::TerrainMovedFmt, L"地形数据搬好了（%d 个文件）", L"Terrain data moved (%d files)"},
    {S::TerrainMoveFailed, L"地形数据没搬成，仍在原来的位置", L"Terrain data not moved: it stays where it was"},
    {S::PickTerrainFolder, L"选择地形数据存放的文件夹", L"Choose a folder for the terrain data"},
    {S::TerrainPackDoneFmt, L"地形包：装好 %d 块", L"Terrain packages: %d tiles installed"},
    {S::TerrainImportFmt, L"地形：导入 %d 块", L"Terrain: %d tiles imported"},
    {S::FlashTerrainBorderSaved, L"地形边框存好了", L"Terrain border saved"},
    {S::FlashTerrainBorderCleared, L"地形边框已清除，改回跟着 HUD 边框", L"Terrain border cleared: it follows the HUD border again"},
    {S::TerrainBorderHint1, L"地形边框：把小点拖到地形要铺到的地方（可以超出合像镜）",
     L"Terrain border: drag the points to where the terrain may reach (past the combiner is fine)"},
    {S::FlashOn, L"HUD 开", L"HUD on"},
    {S::FlashOff, L"HUD 关", L"HUD off"},
    {S::FlashXwindOn, L"侧风模式：仪表带收窄", L"Crosswind mode: narrow tapes"},
    {S::FlashXwindOff, L"侧风模式关", L"Crosswind mode off"},
    {S::FlashGlideFmt, L"下滑参考线 −%.1f°", L"Glide reference −%.1f°"},
    {S::FlashLeadFmt, L"提前量 %d ms", L"Lead %d ms"},
    {S::FlashRaNeedGround, L"RA 校准：要在地面上停稳一会儿再点", L"RA calibration: stand still on the ground for a moment first"},
    {S::FlashRaDoneFmt, L"RA 已校准：地面读数 %.1f ft 记为 0", L"RA calibrated: ground reading %.1f ft = 0"},
    {S::FlashRaCleared, L"RA 校准已清除", L"RA calibration cleared"},
    {S::RaNoteFmt, L"%ls 地面读数 %.1f ft", L"%ls ground reading %.1f ft"},
    {S::RaAutoNoteFmt, L"%ls 停在地面时自动校准，读数 %.1f ft", L"%ls calibrated automatically on the ground, %.1f ft"},
    {S::BorderHint1, L"沿着窗框，把小点拖过去", L"Drag the points onto the window frame"},
    {S::BorderHint2, L"点边线加一点 · 右键删点 · Enter 存好 · Esc 不改",
     L"Click an edge to add a point · right-click removes one · Enter saves · Esc cancels"},
    {S::FlashBorderSaved, L"HUD 边框存好了", L"HUD border saved"},
    {S::FlashBorderCancelled, L"边框没改", L"Border unchanged"},
    {S::FlashBorderBadEye, L"眼点读数不对，先不能描边框：在面板“镜头来源”里重新标定镜头原点",
     L"The eye position reads wrong, no tracing for now: measure the camera origin again (panel: Camera source)"},
    {S::FlashBorderNeedCockpit, L"进驾驶舱视角后才能描边框", L"Trace the border from the cockpit view"},
    {S::FlashBorderCleared, L"HUD 边框已清除", L"HUD border cleared"},
    {S::FlashBorderFitFmt, L"HUD 大小 %d%%，正好放进边框", L"HUD size %d%%: fits inside the border"},
    {S::FlashBorderMirrored, L"另一座改成按这边镜像", L"The other seat now mirrors this one"},
    {S::FlashOpacityFmt, L"HUD 亮度 %d%%", L"HUD brightness %d%%"},
    {S::FlashSizeFmt, L"HUD 大小 %d%%", L"HUD size %d%%"},
    {S::FlashPresetFmt, L"HUD 风格：%ls", L"HUD style: %ls"},
    {S::PresetAirbus, L"空客", L"Airbus"},
    {S::PresetBoeing, L"波音", L"Boeing"},
    {S::PresetFighter, L"战斗机", L"Fighter"},
    {S::PresetMinimal, L"极简", L"Minimal"},
    {S::RaClearedNote, L"已清除（不再自动校准，可以手动校准）", L"cleared (no automatic calibration; calibrate by hand)"},
    {S::ChasePlaneMissing,
     L"没检测到 ChasePlane。Birdie 默认用 ChasePlane 的驾驶舱镜头把 HUD 画对位置，打开 ChasePlane 后 HUD 会自己出现。"
     L"没有 ChasePlane 的话，可以在面板 细调 → 镜头来源 改用 MSFS 原生镜头（Beta）。",
     L"ChasePlane was not detected. By default Birdie places the HUD with the ChasePlane cockpit camera; start "
     L"ChasePlane and the HUD appears by itself. Without ChasePlane, try the MSFS camera (beta): panel, Fine tuning, "
     L"Camera source."},
    {S::SimConnectMissing,
     L"找不到 SimConnect.dll。\n\n它应该和 Birdie.exe 在同一个文件夹里（发布包里自带）。请把整个压缩包解压出来再运行，不要直接在压缩包里双击。",
     L"SimConnect.dll was not found.\n\nIt belongs in the same folder as Birdie.exe (it ships in the release zip). "
     L"Extract the whole zip first; do not run Birdie from inside the zip."},
    {S::AlreadyRunning, L"Birdie 已经在运行（看任务栏右下角的托盘图标）。", L"Birdie is already running (see the tray icon)."},
    {S::HotkeyFailedFmt, L"热键 %ls 注册失败（被其他程序占用？）", L"Hotkey %ls could not be registered (used by another program?)"},
    {S::FirstRunHello,
     L"Birdie 在托盘里：单击小鸟打开设置，右键是菜单。",
     L"Birdie lives in the tray: click the bird for the settings, right-click for the menu."},
    {S::IngameUpdated,
     L"游戏内面板已换成这一版。模拟器开着的话，重启后生效。",
     L"The in-sim panel now matches this version. If the sim is running, it takes effect after a restart."},
    {S::PickCommunity, L"选择 MSFS 2024 的 Community 文件夹", L"Choose the MSFS 2024 Community folder"},
    {S::PickExport, L"把游戏内面板导出到哪个文件夹", L"Export the in-sim panel to which folder"},
    {S::Watermark, L"Birdie 测试版", L"Birdie beta"},
    {S::TooltipFmt, L"Birdie HUD\n%ls\n下滑角 %.1f°", L"Birdie HUD\n%ls\nGlide %.1f°"},
};

const Pair* find(S id) {
  for (const auto& p : kStrings)
    if (p.id == id) return &p;
  return nullptr;
}

}  // namespace

void set_language(Lang lang) { g_lang = lang; }
Lang language() { return g_lang; }

Lang resolve_language(const std::string& setting) {
  if (setting == "zh-CN" || setting == "zh") return Lang::kZh;
  if (setting == "en") return Lang::kEn;
  return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Lang::kZh : Lang::kEn;
}

const wchar_t* tr(S id) {
  const Pair* p = find(id);
  if (!p) return L"?";
  return g_lang == Lang::kZh ? p->zh : p->en;
}

std::wstring trf(S id, ...) {
  wchar_t buf[1024];
  va_list ap;
  va_start(ap, id);
  std::vswprintf(buf, 1024, tr(id), ap);
  va_end(ap);
  return buf;
}

const wchar_t* tr_why(const char* id) {
  if (!id) return L"";
  if (!std::strcmp(id, "no_chaseplane")) return tr(S::WhyNoChasePlane);
  if (!std::strcmp(id, "no_camera")) return tr(S::WhyNoCamera);
  if (!std::strcmp(id, "external_view")) return tr(S::WhyExternalView);
  if (!std::strcmp(id, "not_forward")) return tr(S::WhyNotForward);
  if (!std::strcmp(id, "ground_slow")) return tr(S::WhyGroundSlow);
  return L"?";
}

}  // namespace birdie::app
