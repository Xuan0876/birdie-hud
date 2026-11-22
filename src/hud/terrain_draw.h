// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Terrain on the HUD: the skyline / ridge lines as HUD strokes, the green fill as render op 11.
// The app points current() at the latest terrain (terrain/terrain.h TerrainWorker) before build_scene.
#pragma once

#include <memory>
#include <vector>

#include "core/vec.h"
#include "terrain/terrain.h"

namespace birdie::hud {

class Batch;

struct TerrainFrame {
  std::shared_ptr<const terrain::TerrainLines> lines;  // null = no terrain
  double fill_opacity = 1.0;  // multiplier of the fill (the panel's terrain strength)
  // The terrain's own border, HUD-plane degrees (core/border.h); empty = the terrain stays inside the HUD's clips.
  std::vector<Vec2> border;
};
// What build_scene draws for terrain (nullptr = none). The app keeps the object alive while drawing.
void set_terrain(const TerrainFrame* t);
const TerrainFrame* terrain_frame();

// World -> overlay px for this frame (the runway's camera: core/runway.h world_points_cam).
struct CamProj {
  double lat0, lon0;  // aircraft
  Vec3 cam_pos;       // eye, metres: east / north from the aircraft, up from MSL
  Mat3 R;             // camera -> ENU
  double f, W, H, ox, oy;
};

// Skyline and ridge lines (conformal): thin, thicker where the terrain is near or above the aircraft.
void draw_terrain_lines(Batch& B, const CamProj& cam, double sc);
// The fill: the parameters of render op 11 for this frame's camera, empty = none. Op 11 = 11, opacity, eye (east,
// north, up: metres from the mesh's anchor), camera axes c0 c1 c2 (ENU), f, W, H, ox, oy, range (m). The renderer
// draws fill_mesh() with them (render/d2d.cpp, terrain/terrain_gpu.h).
std::vector<double> terrain_fill_cmd(const CamProj& cam);
// The mesh of the last terrain_fill_cmd().
std::shared_ptr<const terrain::TerrainMesh> fill_mesh();

}  // namespace birdie::hud
