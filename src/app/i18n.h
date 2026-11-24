// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// User-facing strings, Simplified Chinese and English.
#pragma once

#include <string>

namespace birdie::app {

enum class Lang { kZh, kEn };

enum class S {
  // tray menu (everything else is in the panel)
  ShowOverlay, OpenPanel, PanelDown, OpenSettingsFolder, Quit, RunwayFmt, RunwayNone,
  // status
  Showing, Off, HiddenFmt,
  // hide reasons
  WhyStarting, WhySimNotRunning, WhyWaitData, WhyNoChasePlane, WhyNoCamera, FlashCamOriginFmt, FlashCamOriginRedo, WhyExternalView, WhyNotForward, WhyGroundSlow,
  WhyNotForeground, WhyPaused, WhyDataStopped, WhyNoWindow, WhyHigh,
  // flash messages
  FlashOn, FlashOff, FlashXwindOn, FlashXwindOff, FlashGlideFmt, FlashLeadFmt, FlashRaNeedGround, FlashRaDoneFmt, FlashRaCleared,
  RaNoteFmt, RaAutoNoteFmt, RaClearedNote,
  // HUD border editor
  BorderHint1, BorderHint2, FlashBorderSaved, FlashBorderCancelled, FlashBorderNeedCockpit, FlashBorderCleared,
  FlashBorderFitFmt, FlashBorderMirrored,
  // hotkeys
  FlashOpacityFmt, FlashSizeFmt, FlashPresetFmt, PresetAirbus, PresetBoeing, PresetFighter, PresetMinimal,
  // errors / notices
  SimConnectMissing, AlreadyRunning, HotkeyFailedFmt, TooltipFmt, ChasePlaneMissing, FirstRunHello,
  // in-sim panel (app/ingame.h)
  IngameUpdated, PickCommunity, PickExport,
  // terrain
  TerrainStreamStartFmt, TerrainStreamDoneFmt, TerrainDownloadingFmt, TerrainNoNetwork, TerrainDiskFull,
  TerrainMovedFmt, TerrainMoveFailed, PickTerrainFolder, TerrainPackDoneFmt, TerrainImportFmt,
  FlashTerrainBorderSaved, FlashTerrainBorderCleared, TerrainBorderHint1,
  // test builds: on the HUD, above the altitude tape
  Watermark,
  kCount
};

void set_language(Lang lang);
Lang language();
// Language from the setting ("zh-CN", "en", "" = Windows UI language).
Lang resolve_language(const std::string& setting);
const wchar_t* tr(S id);
std::wstring trf(S id, ...);  // swprintf-style
// Hide reason id from core/model.h hide_reason() -> text.
const wchar_t* tr_why(const char* id);

}  // namespace birdie::app
