# Reference implementation: architecture and algorithms

Everything the C++ port has to reproduce, written down from `reference/python/` (frozen 2026-11-14). File and line
references point into that folder. When this document and the code disagree, **the code wins** — fix the document.

## 1. Data flow

```
SimConnect (per sim frame, ~25–35 Hz)            SimConnect facilities (every 20 s)
   │ 29 doubles: attitude, GPS track/GS,             │ airports within 30 km → runways (once per airport)
   │ VELOCITY WORLD Y, ChasePlane L-vars, ...        ▼
   ▼                                              runways.pick() → lined-up runway end (sticky)
Model.update()  ── velocity low-pass, attitude rates, HUD filters (speed trend, VS lag)
   │
   ▼  on every data message (no separate render timer)
usable() + foreground/stale checks ── hide reason? → blank frame
   │
   ▼
Model.geometry() → camera rotation R, focal f, bird pixel, glide line, runway polygon
hud.Frame + hud.conformal() → horizon / heading scale / pitch ladder / 2.5° marks (world directions)
clip conformal lines against the runway polygon
hud.build() → scene {main Batch, clip quad, altitude drum, flash text}
hud.commands() → flat float list + string pool
   │
   ▼
hudgfx.dll hg_frame(): Direct2D draw into a DirectComposition swap chain → Present
```

Threads in the reference: Qt main thread (SimConnect polling every 4 ms timer, drawing, tray), a hotkey thread
(`RegisterHotKey` + `PeekMessage`), housekeeping every 500 ms (window placement, runway requests, stats).

## 2. Coordinate frames and the camera model (`cp_camera.py`)

- **ENU** world frame at the aircraft: x east, y north, z up, metres. `enu(lat0, lon0, lat, lon)` is a flat-earth
  approximation (R = 6 378 137 m); runway corners additionally drop by `(e² + n²) / 2R` for earth curvature
  (`runways.world_points_cam`).
  **The C++ port deviates on purpose (11/15):** `enu` / `offset_latlon` use the WGS84 local radii (meridional M,
  prime vertical N·cos lat) at lat0. MSFS lives on the ellipsoid; the sphere makes north 0.3 % long and east 0.1 %
  short at 35°, turning bearings by up to 0.13° (≈ 8 m sideways at 4 km on final, ZSNB 31). Measured with
  `tools/runway_calib/forward.py`: with the sphere the picture runway drifted ±1.5 m over ±700 m of runway as
  −sin(2·heading); with WGS84 it stays on the facility centreline within ~0.2 m. The parity tests switch back to the
  sphere (`set_earth_model(EarthModel::Sphere)`) to compare with the reference.
- **Body** frame: x right, y up, z forward. `aircraft_to_enu(hdg_true, pitch_up, bank_right)` returns the matrix whose
  columns are body axes in ENU. SimConnect signs are flipped first: `PLANE PITCH DEGREES` is **negative nose-up**,
  `PLANE BANK DEGREES` is **positive left-wing-down** (`aircraft_rotation`).
- **Camera** frame: x right, y up, z forward. `camera_to_body(yaw_right, pitch_up, roll)` from ChasePlane
  `L:p42_cp_r_yaw / r_pitch / r_roll` (degrees, relative to the aircraft).
- Full rotation: `R = aircraft_rotation(s) @ camera_to_body(cp_yaw, cp_pitch, cp_roll)` (camera → ENU).
- **Projection**: pinhole, principal point = client centre, `x = W/2 + f·r.x/r.z`, `y = H/2 − f·r.y/r.z` with
  `r = Rᵀ·d`. Points with `r.z ≤ 1e-3` are dropped.
- **Focal length**: `f = cp_zoom × F_PER_ZOOM × W / 3440`, `F_PER_ZOOM = 1298 / 0.49725` (zoom 0.497 → f 1298 px,
  horizontal FOV ≈ 106° on 3440×1440). **Only verified on a 21:9 3440×1440 client.** If MSFS defines its FOV
  vertically, other aspect ratios need `H`-based scaling — this is the first item of phase-1 milestone M2.
  **C++ port (11/15, measured):** the reference's linear focal is wrong away from 0.497 (C152 at zoom 0.576: −4.5 %,
  runway outline visibly off). `tools/zoom_calib` (paused sim, only the ChasePlane zoom changed, ORB feature scale
  between captures) gives **vertical FOV linear in zoom**: `vfov = v_ref − 113.319° × (zoom − 0.49725)`, `v_ref` from
  1298 px at 1440 px height; `f = (H/2) / tan(vfov/2)`. 10 zooms 0.12–0.83 fit within 0.25 %; the scaling centre is
  the picture centre. Vertical → `H`-based; 16:9 / 16:10 still to be confirmed by a screenshot.
- **Eye point** (only needed for things at a finite distance, i.e. the runway): camera position =
  `(0, 0, PLANE ALTITUDE·0.3048) + R_ac · (L:p42_cp_a_x, a_y, a_z)` in ENU (ChasePlane mode 0 = cockpit/cabin).
  **The C++ port deviates on purpose (11/15):** the lateral component is `2·L:p42_cp_r_x − L:p42_cp_a_x`, because
  a_x = eyepoint anchor (+ left) + view offset r_x (+ right) — see `core/geo.h eye_lateral_m`. Measured on the FBW
  A320 (pilot and copilot views, `tools/runway_calib/forward.py`): a_x as is put the eye 1.1 m too far right.
  Directions at infinity (bird, horizon, ladder, glide line) ignore it.

## 3. Inputs (`overlay.py` `VARS`, one data definition, `SIMCONNECT_PERIOD_SIM_FRAME`)

`lat, lon` (PLANE LATITUDE/LONGITUDE), `hdg_true`, `pitch_deg`, `bank_deg`, `trk_true` (GPS GROUND TRUE TRACK),
`gs_kt` (GROUND VELOCITY), `vy_fps` (VELOCITY WORLD Y), `vs_fpm` (VERTICAL SPEED, logged only), `alt_ft`
(PLANE ALTITUDE), `agl_ft`, `on_ground`, `cp_mode`, `cp_pitch`, `cp_yaw`, `cp_roll`, `cp_zoom`, `cp_eye_x/y/z`,
`ias_kt`, `mach`, `ind_alt_ft`, `radio_alt_ft` (RADIO HEIGHT), `baro_mb` (KOHLSMAN SETTING MB), `hdg_mag`,
`wind_dir` (true, from), `wind_kt`, `beta_deg` (INCIDENCE BETA). All as FLOAT64.

**Do not use VERTICAL SPEED for the bird**: it lags by 0.5–0.8° in the flare (see VALIDATION.md). Use
`VELOCITY WORLD Y` (or position differences).

## 4. Model (`overlay.py` `Model`)

- Ground velocity in ENU: `(gs·sin trk, gs·cos trk, vy)` (gs in m/s, vy in m/s). First-order low-pass with
  `smooth_s` (default 0.15 s): `v += (1 − e^(−dt/τ))·(v_new − v)`; reset if the gap is > 0.5 s.
- Attitude/camera angle rates (for the lead) for `hdg_true, pitch_deg, bank_deg, cp_pitch, cp_yaw, cp_roll`:
  wrapped difference / dt, smoothed with factor 0.5 per sample.
- **Lead**: `lead_ms` extrapolates those six angles by `rate × lead` before building the rotation. Default 0.
- **Bird**: projection of the smoothed velocity direction (hidden when horizontal speed ≤ 1 m/s).
- `fpa_deg() = atan2(vz, |v_xy|)`; `vs_fpm() = vz / 0.3048 × 60` (same smoothed velocity).
- `HudFilters` (`hud.py`): speed trend = d(IAS)/dt low-passed with τ 1 s, drawn ×10 s; VS needle uses
  `vy·60` low-passed with τ 0.5 s.

## 5. Glide-path reference line

Directions `glide_deg` below the horizon, azimuth = track (heading if slow), from −span to +span where
`span = min(line_span_deg, 10 × hud_scale)`. Dashed (6 on / 4 off, in stroke widths). With a runway outline on
screen: one continuous line, pieces inside the runway polygon removed (Cyrus-Beck against the convex polygon,
`runways.clip_segments_outside`). Without a runway: a fixed gap of ±`line_gap_deg` around the track. Label
`-3.0` at the right end.

## 6. Runways (`runways.py`)

- Facilities API (MSFS 2024 SDK, `SimConnect.h` pack 1): every 20 s `RequestFacilitiesList_EX1(AIRPORT)`; airports
  within 30 km get one `RequestFacilityData` with definition
  `OPEN AIRPORT, OPEN RUNWAY, LATITUDE, LONGITUDE, ALTITUDE (f64), HEADING, LENGTH, WIDTH (f32),
  PRIMARY_NUMBER, PRIMARY_DESIGNATOR, SECONDARY_NUMBER, SECONDARY_DESIGNATOR (i32), TRUE_SLOPE (f32),
  CLOSE RUNWAY, CLOSE AIRPORT`.
  Message layouts: AIRPORT_LIST id 18, 36-byte entries from offset 28 (ident char[9], region char[3], lat/lon/alt
  f64); FACILITY_DATA id 28, data from offset 40; FACILITY_DATA_END id 29.
- Each runway becomes two landing directions; `TRUE_SLOPE` is + uphill along the primary (lower-numbered) direction
  and negated for the other (checked on LOWI 08/26). Sanity filter: 100 < length < 7000 m, 5 < width < 150 m.
- **Selection** (`pick`): landing direction within 30° of track, not past the far end, threshold within 25 km,
  within 3 km of the extended centreline; score `|cross| + 60·Δangle`; the current one is kept while still valid.
- **Projection**: four pavement corners → camera frame (with eye point and curvature) → Sutherland-Hodgman clip at
  z ≥ 0.5 m → project. **C++ port (11/14, user request):** corner altitude follows the runway slope,
  `alt = centre_alt ± length/2 · tan(slope)` (far end higher when uphill in the landing direction); the Python
  reference put all four corners at the centre altitude (identical when the slope is 0, as in the LAX fixture).
  **Facility ALTITUDE is not always the centre** (11/15): on KASE/KTEX it is the primary end, on KSUN/KEGE the
  centre (checked against the sim ground and FAA runway-end elevations). The C++ port re-anchors the centre altitude
  from the ground under the aircraft (PLANE ALTITUDE − PLANE ALT ABOVE GROUND) whenever it is over the pavement. Drawn as a thin closed outline + runway number below the near edge.
- Nothing conformal (horizon, ladder, 2.5° marks, glide line) is drawn across the runway polygon.

## 7. HUD geometry (`hud.py`)

Canvas 1280×1024, `1° = 36.5714 px` (35°×28° field), the **boresight is at (640, 329.143)** — 5° above the canvas
centre. All airframe-fixed symbols are defined in canvas units and scaled by `hud_scale` (default in use: 0.75).

- **HUD plane → screen**: a canvas point `(u, v)` is the body direction `((u−640)·k, (329.143−v)·k, F_CANVAS)` with
  `F_CANVAS = 36.5714·180/π`; rotate body→camera and project. The four canvas corners give a homography
  (`QTransform.quadToQuad`); every canvas-space polyline is mapped through it. The quad is also the clip region of
  the whole HUD. Glyphs are positioned through the homography but not warped.
- **Conformal** (world directions): horizon ±30° around true heading; heading ticks every 5° (long every 10°, label
  = heading/10, rotated with the horizon) on the horizon, magnetic labels placed at true azimuth (magvar =
  hdg_true − hdg_mag); pitch rungs every `ladder_step_deg` (default 6°, off allowed) up to `ladder_max_deg` (≤ 30°),
  from 3.33° to 5.27° either side of the heading, solid above / 4 dashes below the horizon, end ticks towards the
  horizon, labels outside; 2.5° short marks (±0.33°) where no rung is.
- **Airframe-fixed** (canvas): speed tape (line x 140.882, 10 kt = 42.5 px, labels every 20 kt, fixed index
  polygon, 10 s trend arrow if |trend| > 2 kt), Mach ≥ 0.5 below, `GS nnn` and wind (`DDD / SS` magnetic + arrow
  showing where the wind blows, nose up, hidden ≤ 2 kt) above; altitude tape (line x 1113.112, 100 ft = 32 px,
  labels every 500 ft in hundreds, readout box with a rolling 20-ft drum clipped to its window), baro below,
  `RA nnnn` below that (0–2500 ft, airborne); VS needle right of the altitude tape (non-linear: ±1000 fpm =
  136.1 px, then 50.5 px per 1000 to 2000, 50.5 px per 4000 to 6000), two-digit readout when |VS| ≥ 200; roll arc
  radius 190 around the boresight with ticks at 10/20/30/45/67°, fixed top triangle, roll index + sideslip
  trapezoid (β × 6, clamped ±30 px) turning with bank; aircraft reference symbol (wings + small square).
- **Bird**: circle radius `13·sc + 2` px (sc = screen px per canvas px), wings 2r, fin 1.1r; FPA number right of the
  right wing (one decimal, real minus sign `−`), VS (fpm, rounded to 10) left of the left wing.
- **Flare cue** (`overlay.flare_cue`): active below 50 ft RA, airborne. Target FPA relative to the runway:
  approach FPA (−glide_deg) above 40 ft, linear to −0.5° at 10 ft, −0.5° below. Relative FPA = FPA − runway slope.
  Within ±`flare_tol_deg` (0.3°) → blinking `+` above the bird; shallower than target → `++`; steeper → nothing.
  Blink 4 Hz, 60 % on.
- Text: Bahnschrift (Qt tries Bahnschrift SemiCondensed first), green with a 1 px four-way black halo (alpha 120);
  canvas text sizes × 1.2 (`TEXT_GAIN`).

## 8. Renderer protocol (`hud.commands` → `hudgfx.cpp`)

One float array per frame plus one UTF-16 string pool. Opcodes:

| op | payload | meaning |
|---|---|---|
| 10 | green rgba, outline rgba, halo rgba (straight alpha), outline width | style (opacity already multiplied in) |
| 1 | width, dashed, closed, n, x0 y0 … | polyline |
| 2 | n, x0 y0 … | filled polygon |
| 3 | x, y, px, align (0 start/1 mid/2 end), angle°, dy, str offset, str length | text |
| 4 | n, x0 y0 … | push polygon clip |
| 5 | — | pop clip |
| 6 | — | flush batch: all black outlines (width + 2.4), then all green strokes/fills, then text |

Colours: green `#00FF00`, outline black alpha 0.745, text halo alpha 0.47. Square caps, bevel joins; dashes
6 on / 4 off in stroke widths, outline dashes rescaled to the same absolute length. **Direct2D brush colours take
straight (non-premultiplied) alpha** — passing premultiplied values darkens everything twice.

## 9. Window and visibility

- The overlay box is `box_w × box_h` (0.6 × 0.7) of the MSFS client area, centred, in physical pixels
  (per-monitor DPI aware). MSFS window found by title prefix "Microsoft Flight Simulator".
- Window: topmost popup, `WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`,
  `WM_NCHITTEST → HTTRANSPARENT`, DirectComposition swap chain with premultiplied alpha. Windows 11 border/shadow
  removed with DWM attributes (NC rendering off, `DWMWCP_DONOTROUND`, border colour none).
- Hidden (blank frame) when: no SimConnect / no data / data older than 0.5 s; `cp_zoom ≤ 0.05` (no ChasePlane);
  `cp_mode ≥ 0.5` (external view); |cp_yaw| > 45° or |cp_pitch| > 40°; on ground below 40 kt; MSFS not the
  foreground window.
- Single instance via the named mutex `Local\fpv_overlay_single_instance`.

## 10. Settings (`DEFAULTS` in `overlay.py`; values flown by the author in brackets)

`glide_deg` 3.0 · `lead_ms` 0 · `smooth_s` 0.15 · `box_w` 0.6 · `box_h` 0.7 · `visible` · `line_span_deg` 18 ·
`line_gap_deg` 6 · `show_runway` · `show_bird` · `show_glide` · `show_horizon` · `opacity` 1.0 [0.4] ·
`ladder_step_deg` 6 [5] · `ladder_max_deg` 30 [18] · `show_speed` · `show_alt` · `show_vs` · `show_roll` ·
`show_hdg` · `show_gs` · `show_att` · `hud_scale` 1.0 [0.75] · `show_pitch_minor` · `show_fpa_num` ·
`show_vs_num` · `show_flare` · `flare_tol_deg` 0.3 · `ra_offset_ft` 0 (ground calibration: median of ~2 s of raw
RA while on ground below 30 kt; RA used everywhere = raw − offset) · `ra_offset_note`.
FD-related keys (`show_fd`, `fd_ra_cutoff`, `fpd_lag_ms`, `fpd_tau_ms`) belong to phase 3.

## 11. Performance notes (measured 2026-11-06, 3440×1440, sim 25–35 Hz)

- Qt/CPU drawing: 17–24 ms per frame (strokePath with AA + projective transforms, text with halos).
- GPU (hudgfx.dll): whole frame 5–7 ms, of which **2.7–3.8 ms is Python geometry**; data-to-photon on our side
  median 6 ms, p95 14–22 ms. In C++ the geometry should be well under 1 ms.
- Batching matters: draw all outlines first then all colour strokes, cache fonts and text layouts.

## 12. Pitfalls already paid for

- Signs: SimConnect pitch is negative nose-up, bank positive left-wing-down.
- VERTICAL SPEED lags in the flare; VELOCITY WORLD Y does not.
- The recording is 0.34–0.38 s behind the log; the live display has its own, unmeasured, latency → `lead_ms`.
- Direct2D wants straight alpha in brush colours.
- MSFS's RADIO HEIGHT is several feet above zero on the ground (gear not counted) → per-aircraft offset.
- In Qt offline rendering, the QGuiApplication must stay referenced or the process exits silently (127).
- `RegisterHotKey`: Ctrl+Alt+arrows and Ctrl+Alt+F were taken on the author's PC; report failures instead of
  assuming success.
