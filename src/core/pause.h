// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The SimConnect system event "Pause_EX1" and when it hides the HUD. The ESC menu pauses the sim while the
// per-frame data keeps coming, so "the data stopped" alone cannot see it; this event can.
#pragma once

namespace birdie {

// Pause_EX1 flags (SimConnect SDK, SubscribeToSystemEvent). 0 = running.
enum PauseFlag : unsigned {
  kPauseFull = 0x1,    // full pause: the pause key / ESC menu (MSFS 2024: to be confirmed in flight)
  kPauseLegacy = 0x2,  // FSX "pause with sound", not used any more
  kPauseActive = 0x4,  // Active Pause: the aircraft holds still, the sim and the cockpit keep running
  kPauseSim = 0x8,     // the user's aircraft is paused, traffic / multiplayer keep running
};

// Active Pause keeps the HUD (the user's choice, 11/17: the view stays live while it is on).
inline constexpr bool kHideOnActivePause = false;

// True if the HUD should hide for these flags. Any pause bit hides it, unknown bits included (a new kind of pause is
// still a pause); Active Pause alone hides it only when `hide_on_active`.
constexpr bool pause_hides_hud(unsigned flags, bool hide_on_active = kHideOnActivePause) {
  if (flags & ~static_cast<unsigned>(kPauseActive)) return true;
  return hide_on_active && (flags & kPauseActive) != 0;
}

}  // namespace birdie
