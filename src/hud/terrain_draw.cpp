// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "hud/terrain_draw.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "core/geo.h"
#include "hud/scene.h"

namespace birdie::hud {
namespace {
const TerrainFrame* g_cur = nullptr;
std::mutex g_mu;  // the renderer reads the mesh on the same thread today; cheap insurance
std::shared_ptr<const terrain::TerrainMesh> g_fill;
}  // namespace

void set_terrain(const TerrainFrame* t) { g_cur = t; }
const TerrainFrame* terrain_frame() { return g_cur; }

std::shared_ptr<const terrain::TerrainMesh> fill_mesh() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_fill;
}

std::vector<double> terrain_fill_cmd(const CamProj& cam) {
  const TerrainFrame* g = g_cur;
  auto m = g && g->lines ? g->lines->mesh : nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_fill = m;
  }
  if (!m || m->na < 2 || m->nd < 2) return {};
  const Vec2 a = enu(m->lat0, m->lon0, cam.lat0, cam.lon0);  // aircraft from the anchor
  std::vector<double> c{11.0, g->fill_opacity, a.x + cam.cam_pos.x, a.y + cam.cam_pos.y, cam.cam_pos.z};
  for (int i = 0; i < 3; ++i) c.insert(c.end(), {cam.R.c[i].x, cam.R.c[i].y, cam.R.c[i].z});
  c.insert(c.end(), {cam.f, cam.W, cam.H, cam.ox, cam.oy, m->range_m});
  return c;
}

void draw_terrain_lines(Batch& B, const CamProj& cam, double sc) {
  const TerrainFrame* g = g_cur;
  if (!g || !g->lines) return;
  const double w_norm = std::max(1.0, 1.7 * sc), w_warn = std::max(1.8, 3.2 * sc);
  constexpr double kNear = 50.0;  // m in front of the eye
  for (const auto& line : g->lines->lines) {
    Polyline seg;
    bool warn = false;
    auto flush = [&] {
      if (seg.size() >= 2) B.line(seg, warn ? w_warn : w_norm);
      seg.clear();
    };
    for (const auto& q : line) {
      const Vec2 en = enu(cam.lat0, cam.lon0, q.lat, q.lon);
      const double up = q.alt - (en.x * en.x + en.y * en.y) / (2 * kEarthRadius);
      const Vec3 c = cam.R.tmul(Vec3{en.x, en.y, up} - cam.cam_pos);
      if (c.z < kNear) {
        flush();
        continue;
      }
      const Vec2 px{cam.W / 2 + cam.f * c.x / c.z - cam.ox, cam.H / 2 - cam.f * c.y / c.z - cam.oy};
      if (!seg.empty() && q.warn != warn) {  // keep the joint in both pieces
        const Vec2 last = seg.back();
        flush();
        seg.push_back(last);
      }
      warn = q.warn;
      seg.push_back(px);
    }
    flush();
  }
}

}  // namespace birdie::hud
