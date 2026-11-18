// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// HUD border (docs/HUD_BORDER.md): a polygon the user traces around the windscreen, so the HUD never spills onto the
// glareshield or the window posts. It lives on the HUD plane, the same body-fixed plane as the HUD canvas, in
// "plane degrees": x = (u - CX) / ONE_DEG (+ right), y = (BORE_Y - v) / ONE_DEG (+ up), at HUD size 1. A plane point
// (x, y) is the body direction (x * pi/180, y * pi/180, 1) — so zoom, window size and head turns never move it
// against the cockpit, and it does not depend on the HUD size setting.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/settings.h"
#include "core/vec.h"

namespace birdie {

constexpr double kBorderFeatherDeg = 1.0;  // fades out over the last degree inside the border
constexpr double kBorderMaxDeg = 60.0;     // vertices are clamped to +-60 plane degrees
constexpr int kBorderMaxPoints = 32;
// The HUD canvas (FBW geometry) at size 1, in plane degrees: the border a new trace starts from.
constexpr double kCanvasHalfWDeg = 17.5, kCanvasTopDeg = 9.0, kCanvasBottomDeg = -19.0;

enum class Seat { kLeft, kRight };
// From the eye's lateral offset (geo.h eye_lateral_m, + = right): the captain sits left of the centreline.
Seat seat_of(double eye_lateral_m);
const char* seat_key(Seat s);  // "L" / "R"

// The canvas rectangle TL, TR, BR, BL.
std::vector<Vec2> default_border();
// Left <-> right seat: x -> -x, order reversed (keeps the winding), eye mirrored.
Settings::BorderPoly mirrored(const Settings::BorderPoly& b);

// The border for a seat: its own, else the other seat's mirrored. how = "drawn", "mirrored" or "none".
std::optional<Settings::BorderPoly> border_for(const Settings::BorderSeats& seats, Seat seat, std::string* how = nullptr);

// Clamps the vertices to +-kBorderMaxDeg and drops non-finite ones; false if fewer than 3 or more than the maximum
// remain.
bool sanitize_border(std::vector<Vec2>& pts);

bool inside_polygon(const std::vector<Vec2>& poly, Vec2 p);
double distance_to_edges(const std::vector<Vec2>& poly, Vec2 p);
// Every point fully visible: inside the polygon and at least `margin` from its edges.
bool all_inside(const std::vector<Vec2>& poly, const std::vector<Vec2>& pts, double margin);
// Largest HUD size k in [lo, hi] (to 0.01) at which every point (given at size 1; the airframe-fixed symbols scale
// about the boresight, the plane origin) stays inside with `margin`; nullopt if none fits even at lo.
std::optional<double> fit_scale(const std::vector<Vec2>& poly, const std::vector<Vec2>& pts, double margin, double lo,
                                double hi);

}  // namespace birdie
