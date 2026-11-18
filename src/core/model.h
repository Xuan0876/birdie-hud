// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Turns raw samples into screen geometry (reference: overlay.py Model, hud.py HudFilters).
#pragma once

#include <optional>
#include <vector>

#include "core/runway.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/vec.h"

namespace birdie {

// Speed trend (kt per 10 s) and lagged vertical speed for the tapes.
struct HudFilters {
  std::optional<double> t;
  double ias = 0, rate = 0, vs = 0;
  void update(const SimState& s, double t);
  double trend10() const { return rate * 10.0; }
};

struct Geometry {
  OptVec2 bird;                    // client px
  std::vector<Polyline> glide;     // glide reference line pieces, client px
  double f = 0;                    // focal length, px
  std::optional<Polyline> runway;  // runway outline, client px
  std::vector<Polyline> tdz_marks;  // touchdown zone stripes (closed), client px
  std::vector<Polyline> tdz_end;    // where the touchdown zone ends: a short mark across each edge, client px
  Mat3 R;                          // camera -> ENU
  SimState p;                      // state with the lead applied
};

struct FlareCue {
  int n = 0;        // 1 = on the profile, 2 = above it (shallower), 0 = below it (still too steep)
  bool on = false;  // blink phase
  double rel = 0, target = 0, ra = 0;
};

class Model {
 public:
  explicit Model(const Settings& settings) : set_(&settings) {}
  void update(const SimState& s, double t);
  std::optional<double> fpa_deg() const;
  std::optional<double> vs_fpm() const;
  Geometry geometry(const SimState& s, double W, double H, const Runway* runway) const;
  const HudFilters& filters() const { return filt_; }
  const std::optional<Vec3>& velocity() const { return v_; }
  void reset();

 private:
  const Settings* set_;
  std::optional<Vec3> v_;  // smoothed ENU velocity, m/s
  std::optional<double> prev_t_;
  SimState prev_{};
  double rates_[6] = {};  // hdg_true, pitch_deg, bank_deg, cp_pitch, cp_yaw, cp_roll (deg/s)
  HudFilters filt_;
};

// Why the HUD should stay hidden for this sample (nullptr = show). Stable ids, translated by the app.
const char* hide_reason(const SimState& s, double show_above_kt = 40.0);

// Raw RADIO HEIGHT samples taken while standing on the ground -> the ground reading to subtract, if they are many
// (>= min_n), steady (spread <= 0.5 ft) and plausible (-5..40 ft). Median.
std::optional<double> steady_ground_ra(const std::vector<double>& samples, size_t min_n = 60);

// The flare cue's target path angle: approach_fpa down to hi_ft, then straight to end_deg at lo_ft (the defaults are
// the reference's 40 ft / 10 ft / -0.5 deg).
double flare_target(double ra, double approach_fpa, double hi_ft = 40.0, double lo_ft = 10.0, double end_deg = -0.5);
// Flare helper below 50 ft RA (airborne), or nothing. `now` drives the blink (s).
std::optional<FlareCue> flare_cue(const Model& m, const SimState& s, const Runway* runway, const Settings& set,
                                  double now);

}  // namespace birdie
