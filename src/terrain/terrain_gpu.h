// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// The terrain fill on the GPU: the fill mesh (terrain/terrain.h) drawn with Direct3D 11 into an offscreen opacity
// texture (4x MSAA, depth buffer: the nearest terrain wins, so ridges hide what lies behind them), which the
// Direct2D renderer then paints in the HUD colour through the HUD's clips (render op 11, render/d2d.cpp).
#pragma once

#include <memory>

struct ID3D11Device;
struct ID2D1DeviceContext;
struct ID2D1Bitmap1;

namespace birdie::terrain {

class TerrainGpu {
 public:
  TerrainGpu();
  ~TerrainGpu();
  TerrainGpu(const TerrainGpu&) = delete;
  TerrainGpu& operator=(const TerrainGpu&) = delete;

  // Renders fill_mesh() for the op-11 parameters p (19 floats after the op code) into a w x h opacity bitmap
  // (alpha = opacity of the fill, before the op's own opacity). nullptr = nothing to draw or failure.
  ID2D1Bitmap1* render(ID3D11Device* dev, ID2D1DeviceContext* dc, const float* p, int w, int h);
  void release();  // device lost / renderer gone

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace birdie::terrain
