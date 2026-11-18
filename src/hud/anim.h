// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The HUD's openings and closings (settings anim_in / anim_out). One function of the progress p: 0 = not shown,
// 1 = fully shown. Opening runs p from 0 to 1, closing from 1 to 0 through the closing's own kind, so a closing is
// its kind played backwards (the CRT switching off, the scan wiping up, the parts flying out...).
//   fade      quiet: the whole HUD fades in / out
//   iris      elegant: a circle opens from the boresight, its rim lit
//   boot      cool: a CRT warming up: a line at the boresight, the picture rolling open with a flicker, a self-test
//   scan      cool: a bright scan line sweeps down and leaves the HUD behind it
//   assemble  showy: tapes slide in from the sides, the bank scale drops in, the bird lands from large
#pragma once

#include <string>

#include "core/settings.h"
#include "hud/scene.h"

namespace birdie::hud {

enum class AnimKind { None, Fade, Iris, Boot, Scan, Assemble };

AnimKind anim_kind(const std::string& name);
// Seconds for the whole opening (opening = true) or closing, times the settings' anim_time.
double anim_duration(AnimKind k, bool opening, double time_scale);
// The settings for one frame (only "assemble" moves things; the others draw the HUD as set).
Settings anim_settings(const Settings& set, AnimKind k, double p);
// The renderer commands for `scene` at progress p. `t` = wall clock (s), for the boot's flicker.
CommandList animated_commands(const Scene& scene, double opacity, AnimKind k, double p, double t);

}  // namespace birdie::hud
