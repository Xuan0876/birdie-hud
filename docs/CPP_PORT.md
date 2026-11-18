# C++ port plan (phase 1, milestone M1)

Goal of M1: a single `Birdie.exe` that does exactly what `reference/python` does on the ChasePlane path, proven by
the parity fixtures before anyone flies it. No new features in M1.

## Decisions already made

- MSFS 2024 only; C++17/20, MSVC 2022 x64, **CMake**; Windows only.
- Renderer: fold `reference/native/hudgfx.cpp` (Direct2D + DirectWrite + DirectComposition) into the app; the
  command-list interface can stay internal or be replaced by direct calls — parity is checked on geometry and pixels.
- Settings/calibration UI: tray menu now; the in-game **MSFS toolbar panel** (community package, HTML/JS, talking to
  the app over a localhost WebSocket) is milestone M3 — look at how existing open-source toolbar panels package and
  register themselves before writing one.
- GPL-3.0-only. No dependency that is incompatible with it.

## Proposed layout

```
CMakeLists.txt
src/
  main.cpp              WinMain, single-instance mutex, message loop, timers
  sim/simconnect.*      connection, data definition (VARS), dispatch, reconnect
  sim/facilities.*      airport list + runway facility requests, parsing (runways.py FacilityClient)
  core/geo.*            ENU, rotations, projection (cp_camera.py)
  core/model.*          velocity filter, rates/lead, FPA/VS (Model, HudFilters)
  core/runway.*         Runway ends, pick(), corners, near clip, Cyrus-Beck (runways.py)
  hud/scene.*           conformal + airframe-fixed symbols → scene (hud.py build)
  hud/flare.*           flare cue
  render/d2d.*          window, swap chain, batches, text (hudgfx.cpp)
  app/settings.*        JSON settings (same keys as the reference) in %APPDATA%\Birdie\ (the reference used
                        %APPDATA%\fpv_overlay; mutex Local\Birdie_single_instance, SimConnect client name "Birdie")
  app/tray.*            tray icon + menu (strings through i18n)
  app/hotkeys.*
  app/i18n/zh-CN.json, en.json
tests/
  parity/               replays tests/fixtures/*/input.csv, compares with golden/
third_party/            header-only libs only if needed (e.g. nlohmann/json, MIT)
```

SimConnect: link against the MSFS 2024 SDK (`SimConnect.lib` + DLL, or the static library) found through an
`MSFS2024_SDK` CMake variable; nothing from the SDK is committed. Check redistribution terms before a release.

## Parity tests

Fixture format: `tests/README.md`. For each fixture:

1. Build the settings from `fixture.json → settings`, the runway from `fixture.json → runway` (inject it, do not
   pick), client size `client_px`, overlay box `box_px`.
2. Feed `input.csv` rows in order to the model with `t` as the clock (the flare blink uses the same clock).
3. At each `checks[i].t`, compare with `golden/NNN.json`:
   - `geometry.bird_px`, `focal_px`, `runway_px` within 0.05 px; `fpa_deg`, `vs_fpm`, `flare.n` exact-ish;
   - optionally the command list (same ops, floats within 1e-3) while the C++ renderer still uses it;
   - render the frame off-screen at `box_px` size and compare with `golden/NNN.png` (RGBA premultiplied) with a
     small per-pixel tolerance; write a diff image on failure.
4. Regenerate goldens only from the reference: `python tests/make_golden.py ...` (command line in
   `fixture.json → provenance` and in `tests/README.md`).

## M1 checklist (Python → C++)

- [ ] geo: `enu`, `aircraft_to_enu`, `aircraft_rotation`, `camera_to_body`, focal from zoom
- [ ] model: velocity low-pass, angle rates, lead, `fpa_deg`, `vs_fpm`, HUD filters
- [ ] runway: ends from record (+ slope sign), `pick`, corners, curvature drop, near clip, Cyrus-Beck
- [ ] facilities client (airport list every 20 s, 30 km radius, one request per airport)
- [ ] hud: frame/homography, conformal (horizon, heading, rungs, minor marks), runway clipping of conformal lines,
      glide line (with/without runway), bird + numbers, flare cue, speed tape (+trend, Mach, GS, wind), altitude
      tape (+drum, baro, RA), VS needle, roll scale + index + sideslip, aircraft reference, flash message
- [ ] render: batches (outline pass → colour pass → text), clip stack, dashes, straight-alpha brushes, fonts
- [ ] window: placement over the MSFS client (physical px), DWM border removal, show/hide rules, foreground check
- [ ] settings (same JSON keys), tray menu (zh/en), hotkeys with failure reporting, RA ground calibration, log file
- [ ] parity tests green on every fixture; live smoke test with ChasePlane

## After M1 (phase 1)

- M2: zoom→focal on 16:9 / 16:10; eye-point datum on Fenix / FBW / default; RA defaults; clear message when
  ChasePlane is missing.
- M3: toolbar panel.
- M4: release (bilingual UI, single exe, GitHub Releases).
