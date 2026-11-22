// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// Runway ends, selection and projection helpers (reference: runways.py).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/vec.h"

namespace birdie {

// One landing direction of a runway (an "end").
struct Runway {
  std::string icao;
  double lat = 0, lon = 0, alt_m = 0;  // runway centre
  double hdg = 0;                      // true landing direction, deg [0, 360)
  double length = 0, width = 0;        // m
  std::string name;                    // "07L"
  double slope = 0;                    // deg, + = uphill in this landing direction
  // The facility ALTITUDE is not always the centre on a sloped runway: on some airports it is the primary end
  // (KASE, KTEX), on others the centre (KSUN, KEGE). Re-anchored from the ground under the aircraft whenever it is
  // over the pavement (anchor_from_ground); facility_alt_m keeps the original value for the log.
  double facility_alt_m = 0;
  bool anchored = false;

  struct Corner {
    double lat, lon, alt_m;
  };
  // Pavement corners: near-left, near-right, far-right, far-left (landing direction).
  std::array<Corner, 4> corners() const;
  // A rectangle on the pavement: `along` metres from the threshold (a0 .. a1), `right` metres from the centreline
  // (r0 .. r1, + = right of the landing direction). Corners in the same order as corners().
  std::array<Corner, 4> patch(double a0, double a1, double r0, double r1) const;
};

// Touchdown zone markings as painted (rectangles in runway metres, see Runway::patch), from the runway length:
//   ICAO Annex 14 Vol I 5.2.5 (aiming point: 150 / 250 / 300 / 400 m from the threshold for LDA < 800 / < 1200 /
//   < 2400 / >= 2400 m) and 5.2.6 (touchdown zone pairs every 150 m: 1 / 2 / 3 / 4 / 6 pairs for LDA < 900 / < 1200 /
//   < 1500 / < 2400 / >= 2400 m, none within 50 m of the aiming point);
//   FAA AC 150/5340-1 (aiming point 1,020 ft from the threshold, 150 ft long; touchdown zone bars every 500 ft to
//   3,000 ft in groups of 3, 2, 2, 1, 1). The runway length stands in for the landing distance available.
struct PatchM {
  double a0, a1, r0, r1;
};
struct TouchdownMarks {
  std::vector<PatchM> aim;     // the two aiming point stripes
  std::vector<PatchM> bars;    // touchdown zone bars
  double zone_end_m = 0;       // end of the touchdown zone: 900 m / 3,000 ft, at most a third of the runway
};
TouchdownMarks touchdown_marks(const Runway& rw, bool faa);

Runway make_runway(std::string icao, double lat, double lon, double alt_m, double hdg, double length, double width,
                   std::string name, double slope = 0.0);

// Facility record (SimConnect runway fields, in definition order).
struct RunwayRecord {
  double lat, lon, alt_m;
  float heading, length, width;
  int primary_number, primary_designator, secondary_number, secondary_designator;
  float true_slope;
};

std::string runway_name(int number, int designator);
// Both landing directions, or empty if the record is implausible.
std::vector<Runway> ends_from_record(const std::string& icao, const RunwayRecord& r);

// The end we are lined up for (see runways.py pick): within 30° of track, ahead, within max_dist of the threshold,
// within 3 km of the extended centreline. Keeps `current` while it is still valid, unless another lines up clearly
// better (a parallel runway the aircraft has lined up with). nullptr = none.
const Runway* pick_runway(const std::vector<const Runway*>& ends, double lat, double lon, double trk,
                          const Runway* current, double max_dist = 25000.0);

// If (lat, lon) is over this runway's pavement, sets the centre altitude so the sloped surface passes through
// ground_m there (smoothed after the first reading) and returns true.
bool anchor_from_ground(Runway& rw, double lat, double lon, double ground_m);

// (lat, lon, alt) corners -> camera coordinates (x right, y up, z fwd), with earth-curvature drop.
std::vector<Vec3> world_points_cam(const std::array<Runway::Corner, 4>& pts, double lat0, double lon0,
                                   const Vec3& cam_pos, const Mat3& R);
// Sutherland-Hodgman clip of a 3D polygon against z >= near.
std::vector<Vec3> clip_near(const std::vector<Vec3>& poly, double near_z = 0.5);
// Pieces of a 2D polyline lying outside a convex polygon (Cyrus-Beck per segment).
std::vector<Polyline> clip_segments_outside(const Polyline& polyline, const Polyline& poly);

}  // namespace birdie
