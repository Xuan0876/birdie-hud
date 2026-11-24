// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
#include "core/model.h"

#include <algorithm>
#include <cmath>

#include "core/camera.h"
#include "core/geo.h"

namespace birdie {
namespace {

constexpr double SimState::*kAngleKeys[6] = {&SimState::hdg_true, &SimState::pitch_deg, &SimState::bank_deg,
                                             &SimState::cam_pitch, &SimState::cam_yaw,   &SimState::cam_roll};

Vec3 dir_vec(double az, double elev) {  // radians
  return {std::sin(az) * std::cos(elev), std::cos(az) * std::cos(elev), std::sin(elev)};
}

}  // namespace

void HudFilters::update(const SimState& s, double tnow) {
  const double ias_now = s.ias_kt;
  const double vs_now = s.vy_fps * 60.0;
  if (!t || !(0 < tnow - *t && tnow - *t < 0.5)) {
    t = tnow;
    ias = ias_now;
    rate = 0.0;
    vs = vs_now;
    return;
  }
  const double dt = tnow - *t;
  const double a = 1 - std::exp(-dt / 1.0);
  rate += a * ((ias_now - ias) / dt - rate);
  vs += (1 - std::exp(-dt / 0.5)) * (vs_now - vs);
  t = tnow;
  ias = ias_now;
}

void Model::reset() {
  v_.reset();
  prev_t_.reset();
  for (double& r : rates_) r = 0;
  filt_ = HudFilters{};
}

void Model::set_predict(double seconds) { predict_s_ = std::clamp(seconds, 0.0, kMaxPredictS); }

void Model::update(const SimState& s, double t) {
  filt_.update(s, t);
  const double trk = rad(s.trk_true);
  const double gs = s.gs_kt * kKt;
  const Vec3 v{gs * std::sin(trk), gs * std::cos(trk), s.vy_fps * kFt};
  if (!v_ || !prev_t_ || t - *prev_t_ > 0.5) {
    v_ = v;
  } else {
    const double a = 1.0 - std::exp(-(t - *prev_t_) / std::max(set_->smooth_s, 1e-3));
    v_ = *v_ + a * (v - *v_);
  }
  if (prev_t_ && 0 < t - *prev_t_ && t - *prev_t_ < 0.5) {
    const double dt = t - *prev_t_;
    for (int k = 0; k < 6; ++k) {
      const double d = wrap180(s.*kAngleKeys[k] - prev_.*kAngleKeys[k]);
      rates_[k] += 0.5 * (d / dt - rates_[k]);
    }
  }
  prev_t_ = t;
  prev_ = s;
}

std::optional<double> Model::fpa_deg() const {
  if (!v_ || norm_xy(*v_) <= 1.0) return std::nullopt;
  return deg(std::atan2(v_->z, std::hypot(v_->x, v_->y)));
}

std::optional<double> Model::vs_fpm() const {
  if (!v_) return std::nullopt;
  return v_->z / kFt * 60.0;
}

Geometry Model::geometry(const SimState& s, double W, double H, const Runway* runway) const {
  Geometry g;
  const double lead = set_->lead_ms / 1000.0;
  const double ahead = lead + predict_s_;
  SimState p = s;
  if (ahead != 0) {
    for (int k = 0; k < 6; ++k) p.*kAngleKeys[k] = s.*kAngleKeys[k] + rates_[k] * ahead;
  }
  if (predict_s_ > 0 && s.gs_kt > 1) {  // the position moves on too (the track is noise when nearly standing still)
    const double d = s.gs_kt * kKt * predict_s_, trk = rad(s.trk_true);
    const Vec2 ll = offset_latlon(s.lat, s.lon, d * std::sin(trk), d * std::cos(trk));
    p.lat = ll.x, p.lon = ll.y;
    p.alt_ft = s.alt_ft + s.vy_fps * predict_s_;
  }
  const Mat3 R_ac = aircraft_rotation(p);
  const Mat3 R = R_ac * camera_to_body(p.cam_yaw, p.cam_pitch, p.cam_roll);
  const double f = focal_from_vfov(p.cam_vfov, H);
  g.R = R;
  g.p = p;
  g.f = f;

  auto pix = [&](Vec3 d) -> OptVec2 {
    const Vec3 r = R.tmul(d);
    if (r.z <= 1e-3) return std::nullopt;
    return Vec2{W / 2 + f * r.x / r.z, H / 2 - f * r.y / r.z};
  };

  const Vec3 v = v_ ? *v_ : Vec3{};
  const bool moving = norm_xy(v) > 1.0;
  if (moving) g.bird = pix(v);
  const double gl = rad(set_->glide_deg);
  const double trk = moving ? std::atan2(v.x, v.y) : rad(p.hdg_true);

  auto glide = [&](double a0, double a1, int n) {
    Polyline seg;
    for (double daz : linspace(a0, a1, n)) {
      const double az = trk + rad(daz);
      if (auto q = pix(dir_vec(az, -gl))) seg.push_back(*q);
    }
    return seg;
  };

  const double span = std::min(set_->line_span_deg, 10.0 * set_->hud_scale);  // inside the HUD, clear of the tapes
  if (runway) {
    // the runway is a real place: camera position = aircraft datum + the camera's eye offset (body frame)
    const Vec3 eye{p.cam_eye_x, p.cam_eye_y, p.cam_eye_z};
    const Vec3 cam_pos = Vec3{0.0, 0.0, p.alt_ft * kFt} + R_ac * eye;
    const auto pts = world_points_cam(runway->corners(), p.lat, p.lon, cam_pos, R);
    const auto poly3 = clip_near(pts);
    if (poly3.size() >= 3) {
      Polyline poly;
      for (const Vec3& q : poly3) poly.push_back({W / 2 + f * q.x / q.z, H / 2 - f * q.y / q.z});
      g.runway = std::move(poly);
      // touchdown zone (Birdie's own): the painted marks as they are, and a tick across each edge where it ends
      if (set_->show_tdz) {
        const auto tm = touchdown_marks(*runway, set_->tdz_rule == "faa");
        auto put = [&](const PatchM& q, std::vector<Polyline>& out) {
          const auto c = clip_near(world_points_cam(runway->patch(q.a0, q.a1, q.r0, q.r1), p.lat, p.lon, cam_pos, R));
          if (c.size() < 3) return;
          Polyline pl;
          for (const Vec3& v3 : c) pl.push_back({W / 2 + f * v3.x / v3.z, H / 2 - f * v3.y / v3.z});
          out.push_back(std::move(pl));
        };
        const std::string& st = set_->tdz_style;
        if (st != "end")
          for (const auto& q : tm.aim) put(q, g.tdz_marks);
        if (st == "full")
          for (const auto& q : tm.bars) put(q, g.tdz_marks);
        if (tm.zone_end_m > 0) {
          const double hw = 0.5 * runway->width, a = tm.zone_end_m;
          put({a, a + 4, -hw - 10, -hw + 6}, g.tdz_end);
          put({a, a + 4, hw - 6, hw + 10}, g.tdz_end);
        }
      }
    }
  }
  if (g.runway && !g.runway->empty()) {
    // continuous line, cut only where it runs across the runway outline
    const Polyline full = glide(-span, span, 73);
    if (full.size() > 1) g.glide = clip_segments_outside(full, *g.runway);
  } else {
    // no runway known: fixed gap either side of the track
    const double gap = std::min(set_->line_gap_deg, span - 1);
    for (Polyline seg : {glide(-span, -gap, 13), glide(gap, span, 13)})
      if (seg.size() > 1) g.glide.push_back(std::move(seg));
  }
  return g;
}

// show_above_kt: on the ground the HUD waits until the ground speed passes it (0 = always shown on the ground)
const char* hide_reason(const SimState& s, double show_above_kt) {
  if (!s.cam_ok) return s.cam_source == static_cast<int>(CameraSource::kMsfs) ? "no_camera" : "no_chaseplane";
  if (!s.cam_cockpit) return "external_view";
  if (std::abs(s.cam_yaw) > 45 || std::abs(s.cam_pitch) > 40) return "not_forward";
  if (s.gs_kt < show_above_kt && s.on_ground >= 0.5) return "ground_slow";
  return nullptr;
}

std::optional<double> steady_ground_ra(const std::vector<double>& samples, size_t min_n) {
  if (samples.size() < min_n || samples.empty()) return std::nullopt;
  std::vector<double> v(samples);
  std::sort(v.begin(), v.end());
  if (!(v.back() - v.front() <= 0.5)) return std::nullopt;
  const size_t n = v.size();
  const double med = n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
  if (!(med >= -5 && med <= 40)) return std::nullopt;
  return med;
}

double flare_target(double ra, double approach_fpa, double hi_ft, double lo_ft, double end_deg) {
  if (ra >= hi_ft) return approach_fpa;
  if (ra <= lo_ft || hi_ft <= lo_ft) return end_deg;
  return end_deg + (approach_fpa - end_deg) * (ra - lo_ft) / (hi_ft - lo_ft);
}

std::optional<FlareCue> flare_cue(const Model& m, const SimState& s, const Runway* runway, const Settings& set,
                                  double now) {
  if (!set.show_flare || s.on_ground != 0) return std::nullopt;
  const double ra = s.radio_alt_ft;
  const auto fpa = m.fpa_deg();
  if (!fpa || !(0.0 < ra && ra <= set.flare_ra_ft)) return std::nullopt;
  const double slope = runway ? runway->slope : 0.0;  // + = uphill in the landing direction
  FlareCue c;
  c.rel = *fpa - slope;
  c.target = flare_target(ra, -std::abs(set.glide_deg), set.flare_hi_ft, set.flare_lo_ft, set.flare_end_deg);
  const double tol = set.flare_tol_deg;
  c.n = std::abs(c.rel - c.target) <= tol ? 1 : (c.rel > c.target ? 2 : 0);
  c.on = pymod(now * 4.0, 1.0) < 0.6;
  c.ra = ra;
  return c;
}

}  // namespace birdie
