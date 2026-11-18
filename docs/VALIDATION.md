# Validation

## What has been checked

### Bird projection against the picture (2026-11-06, `reference/python/tools/foe_check.py`)

The bird is where the aircraft goes; in the video that is the focus of expansion (FOE). With camera rotation
removed (attitude + ChasePlane head angles from the log), ground features stream radially from it. Two full
approaches, FSLabs A321, ChasePlane cockpit view, zoom 0.497, 3440×1440:

| Approach | log→video lag | 500–1000 ft horiz / vert | 200–500 ft | 50–200 ft |
|---|---|---|---|---|
| Morning (video 337–437 s) | −0.34 s | −0.04° / +0.11° (p10–p90 ±0.5 / −0.06…+0.31) | +0.07 / +0.05 | −0.02 / +0.11 |
| Dusk LAX 07L (video 289–386 s) | −0.38 s | +0.06 / +0.04 | −0.03 / +0.09 | 0.00 / +0.02 |

1-s rolling medians, degrees (1° ≈ 23 px at 3440 wide). Conclusions:
1. Below ~900 ft the image FOE and the data bird agree within 0.1° (≈ 2 px), jitter ±0.2°.
2. Above 1000 ft the horizontal FOE is noisy — an image-side limit (little parallax), vertical stays stable.
3. In the flare (below 40 ft) VERTICAL SPEED lags: the FOE sits 0.5–0.8° above a VS-based bird; position
   differences agree with the FOE → the live app uses VELOCITY WORLD Y.
4. GPS GROUND TRUE TRACK + GROUND VELOCITY agree with position differences within 0.1°.

### Live flights (2026-11-06/07)

- SimConnect at 21–35 Hz (= sim frame rate), drawing 0.5–7 ms.
- Facility data parsed correctly (ZSWZ 03/21 3200×45 m, elevation 2 m; auto-selected 03).
- `VELOCITY WORLD Y` sign matches VERTICAL SPEED.
- HUD screenshot check: VS −800 fpm at GS 217 kt → FPA −2.1°, bird at −2° on the ladder.
- HUD size chosen by the author: 75 % (`hud_scale` 0.75).
- RA ground reading on the FSLabs A321: 14.5 ft.

### Offline snapshots

- Runway outline from OpenStreetMap LAX 07L at 100 ft: side edges on the edge lights; the near threshold is a bit
  behind the sim's pavement (OSM vs MSFS scenery). Live, the sim's own facility data is used.
- `tests/fixtures/lax_dusk_07L`: 9 frames from 1500 ft to 15 ft (see `fixture.json` → `checks`).

## Not yet verified (phase 1 must close these)

- [x] Zoom → focal length over the zoom range (11/15, `tools/zoom_calib`): vertical FOV is linear in zoom, see
      REFERENCE.md "Focal length". The old linear focal was −4.5 % off at zoom 0.576 (C152 runway outline).
- [ ] Zoom → focal length on **16:9 and 16:10** clients (only 21:9 checked). The measurement says MSFS keeps the
      vertical FOV (now `H`-based); one screenshot per aspect ratio confirms it.
- [ ] ChasePlane eye point vs PLANE ALTITUDE datum on **Fenix A320, FBW A32NX, default aircraft** (runway outline
      should sit on the pavement edges at 100 ft).
- [ ] RA ground offsets for the four first-class aircraft.
- [ ] Flare cue timing and tolerance in real landings; `TRUE_SLOPE` direction on a sloped runway.
- [ ] Display latency of the live overlay (lead value that looks steadiest).
- [ ] Facility field types on more airports (sanity filter logs "跑道记录不合理" if wrong).
