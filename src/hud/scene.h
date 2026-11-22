// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Airbus-style HUD scene (reference: hud.py; layout geometry from the FlyByWire A32NX HUD, GPL-3.0, see
// THIRD_PARTY_NOTICES.md). Builds the frame as batches of screen-space polylines / fills / texts and flattens them
// into the renderer's command list (render/d2d.h).
//
// Two kinds of symbols:
//   airframe-fixed (tapes, roll scale, readouts): canvas coordinates (1280 x 1024, 36.5714 px/deg) mapped onto the
//     screen through the HUD-plane homography (the HUD plane is fixed to the aircraft body);
//   conformal (horizon, heading scale, pitch ladder, glide line, bird, runway): world directions through the camera.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/model.h"
#include "core/runway.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/vec.h"

namespace birdie::hud {

// Projective map canvas -> screen.
struct Homography {
  double m[3][3]{};
  Vec2 pt(double x, double y) const;
  Vec2 pt(Vec2 p) const { return pt(p.x, p.y); }
};
// Maps the canvas rectangle (0,0)-(w,h) corners in order TL, TR, BR, BL onto `dst`; nullopt if degenerate.
std::optional<Homography> rect_to_quad(double w, double h, const std::array<Vec2, 4>& dst);

enum Align { kStart = 0, kMid = 1, kEnd = 2 };

struct TextItem {
  double x, y, px;
  std::wstring s;
  Align align;
  double ang, dy;
};

// A layout transform for canvas points (before the homography): scale s about (ax, ay), then move by (dx, dy).
struct Xf {
  double dx = 0, dy = 0, s = 1, ax = 0, ay = 0;
  Vec2 apply(double x, double y) const {
    if (s == 1.0) return {x + dx, y + dy};  // bit-exact when only moved (the parity tests)
    return {ax + (x - ax) * s + dx, ay + (y - ay) * s + dy};
  }
};

// Collects geometry in screen coordinates; rendered as: every outline, then every green stroke / fill, then text.
class Batch {
 public:
  Batch() = default;
  Batch(std::optional<Homography> H, double sc) : H_(H), sc_(sc) {}

  void line(const std::vector<OptVec2>& pts, double w, bool canvas = false, bool close = false, bool dash = false);
  void line(const Polyline& pts, double w, bool canvas = false, bool close = false, bool dash = false);
  void fill(const Polyline& pts, bool canvas = false);
  void text(double x, double y, const std::wstring& s, double px, Align align = kMid, bool canvas = false,
            double ang = 0.0, double dy = 0.0);
  void emit(std::vector<double>& cmds, std::wstring& strs) const;
  // Every point the batch draws (line and fill vertices, rough text boxes), for the HUD border's fit check.
  void points(std::vector<Vec2>& out) const;

  const std::optional<Homography>& homography() const { return H_; }
  // Layout transform for the canvas points that follow (each group of airframe-fixed symbols has its own); canvas
  // text grows with its scale.
  void set_xform(const Xf& xf) { xf_ = xf; }
  // Global finish: every line width and every text size multiplied (settings line_w, text_size).
  void set_finish(double line_w, double text_k) { lw_ = line_w, tk_ = text_k; }
  // A canvas point as it will be drawn (layout transform, then homography).
  Vec2 map(double x, double y) const;

 private:
  struct Key {
    double w;
    bool dash;
  };
  struct LineItem {
    Polyline pts;
    bool closed;
  };
  std::optional<Homography> H_;
  double sc_ = 1.0;
  Xf xf_;
  double lw_ = 1.0, tk_ = 1.0;
  std::vector<std::pair<Key, std::vector<LineItem>>> lines_;  // insertion-ordered like the reference's dict
  std::vector<Polyline> fills_;
  std::vector<TextItem> texts_;
};

struct Scene {
  bool has_main = false;
  Batch main;
  std::optional<Polyline> clip;  // HUD field of view
  std::optional<std::pair<Polyline, double>> border;  // the user's HUD border (screen px) + feather width (px)
  std::optional<std::pair<Polyline, Batch>> drum;  // altitude readout window + rolling digits
  std::optional<Batch> flash;  // short status message, top left
  // the finish (settings hud_color, hud_outline, hud_glow)
  double rgb[3] = {0.0, 1.0, 0.0};
  double outline = 1.0, glow = 0.0;
  OptVec2 bore;  // the boresight on the screen (the openings grow from it)
  std::vector<double> terrain_fill;  // render op 11 (terrain fill under the HUD symbols), empty = none
  std::optional<Batch> terrain_lines;  // skyline / ridges
  // The terrain's own border (screen px) + feather; none = the terrain is drawn inside the HUD's clips instead.
  std::optional<std::pair<Polyline, double>> terrain_border;
};

struct CommandList {
  std::vector<double> cmds;
  std::wstring text;
};

// The full HUD for one frame. (W, H) = MSFS client size, (ox, oy) = overlay window offset inside the client.
// `now` drives the flare cue blink (s). `flash` = status message or empty. `border` = the HUD border in HUD-plane
// degrees (core/border.h), nullptr = none: everything is then exactly as without the feature. `watermark` = a line
// above the altitude tape that no setting turns off (test builds: "Birdie beta"), empty = none.
Scene build_scene(const Model& model, const SimState& s, double W, double H, double ox, double oy,
                  const Settings& set, const Runway* runway, const std::wstring& flash, double now,
                  const std::vector<Vec2>* border = nullptr, const std::wstring& watermark = {});

// ---- HUD border (core/border.h): HUD-plane degrees <-> overlay px for this frame's camera
// The border polygon on the screen, cut at the camera's near plane; nullopt if none of it is in front.
std::optional<Polyline> border_to_screen(const Model& model, const SimState& s, double W, double H, double ox,
                                         double oy, const std::vector<Vec2>& pts);
// One screen point per vertex (nullopt = behind the camera): the editor's handles.
std::vector<OptVec2> border_vertices(const Model& model, const SimState& s, double W, double H, double ox, double oy,
                                     const std::vector<Vec2>& pts);
// Overlay px -> HUD-plane degrees (nullopt = the ray points backwards from the HUD plane).
OptVec2 screen_to_border(const Model& model, const SimState& s, double W, double H, double ox, double oy, Vec2 px);
// Where the airframe-fixed symbols (tapes, bank scale, readouts) are, in HUD-plane degrees at HUD size 1 (they
// scale about the plane origin): only those the settings draw.
std::vector<Vec2> fixed_symbol_points(const Model& model, const SimState& s, const Settings& set);

// The border editor on top of the (faint) HUD: veil outside the polygon, the outline, handles, a two-line hint.
struct EditView {
  Polyline poly;                  // outline, screen px (border_to_screen)
  std::vector<OptVec2> handles;   // vertices (border_vertices)
  int hover = -1, drag = -1;      // vertex under the mouse / being dragged
  OptVec2 add_at;                 // "click here to add a point" on an edge
  double alpha = 1.0;             // fade in
  double W = 0, H = 0;            // overlay size, px
  std::wstring line1, line2;
};
void append_edit(CommandList& out, const EditView& e);
// DirectWrite family for the settings' hud_font.
const wchar_t* font_family(const std::string& hud_font);
// Only the status message (HUD hidden).
Scene flash_scene(const std::wstring& flash);
// Renderer input for a scene.
CommandList commands(const Scene& scene, double opacity);
// Adds a scene drawn over what `out` already holds (with its own colours / opacity).
void append_commands(CommandList& out, const Scene& scene, double opacity);

}  // namespace birdie::hud
