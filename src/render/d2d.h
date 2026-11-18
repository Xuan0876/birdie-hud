// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// GPU renderer: a click-through, topmost, transparent popup window whose content is a DirectComposition swap chain
// drawn with Direct2D / DirectWrite (DWM composites it with per-pixel alpha). The same drawing code renders into
// memory for the parity tests.
//
// Input = hud::CommandList (flat float list + string pool):
//   10 style  green rgba, outline rgba, halo rgba (straight alpha), outline width
//    1 line   width, dashed, closed, n, x0 y0 ...
//    2 fill   n, x0 y0 ...
//    3 text   x, y (baseline), px, align (0 start / 1 mid / 2 end), angle deg, dy, string start, length
//    4 push polygon clip   n, x0 y0 ...
//    5 pop clip
//    6 flush batch (outline pass, colour pass, text)
//    7 push feathered polygon clip   feather px, n, x0 y0 ...   (HUD border: hard edge + fade over the feather)
//    8 fill outside a polygon        r g b a, n, x0 y0 ...      (n < 3: the whole target; border editor's veil)
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hud/scene.h"

struct HWND__;
typedef HWND__* HWND;

namespace birdie::render {

class D2DRenderer {
 public:
  // Window message hook while the overlay takes the mouse (HUD border editor): return true when handled (result in
  // the last argument). An empty hook = click-through again.
  using InputHook = std::function<bool(unsigned msg, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result)>;

  D2DRenderer();
  ~D2DRenderer();
  D2DRenderer(const D2DRenderer&) = delete;
  D2DRenderer& operator=(const D2DRenderer&) = delete;

  // Device only (offline rendering). Returns false and sets error() on failure.
  bool init_offscreen();
  // Device + overlay window at (x, y, w, h) physical screen pixels, hidden until show(true).
  bool init_window(int x, int y, int w, int h);
  bool move(int x, int y, int w, int h);
  void show(bool on);
  bool visible() const;
  HWND hwnd() const;
  void set_input(InputHook hook);

  // Draw one frame into the window (empty list = clear). Recreates the device after a loss.
  bool frame(const hud::CommandList& cl);
  // Draw into a w x h bitmap; out = premultiplied BGRA rows (w * h * 4 bytes).
  bool render_to_memory(const hud::CommandList& cl, int w, int h, std::vector<std::uint8_t>& out);

  void set_font_family(const std::wstring& family);
  const std::wstring& error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace birdie::render
