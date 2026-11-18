# Parity fixtures

Recorded in real flights and replayed through the Python reference to produce the expected ("golden") output. The
C++ port must reproduce them (see `docs/CPP_PORT.md`).

## `fixtures/<name>/`

| File | Content |
|---|---|
| `fixture.json` | client size, overlay box, frozen settings, injected runway, check list, provenance, tolerances |
| `input.csv` | the state stream as the app would receive it (30 Hz; `t` in s from the first row; columns = the reference `VARS` keys + `ra_raw_ft`) |
| `golden/NNN.json` | at check NNN: input state, geometry (bird px, focal px, runway polygon px, FPA, VS, flare), renderer command list + string pool |
| `golden/NNN.png` | the frame rendered by `hudgfx.dll` off-screen, box-sized, RGBA premultiplied |
| `frames/NNN.jpg` | the recording frame at that moment (full client); made when regenerating, not in the repository |
| `preview/NNN.jpg` | frame + HUD composited, box crop — for humans; not in the repository |

Derived inputs (not in the recording, read live from SimConnect): `vy_fps` from altitude differences,
`radio_alt_ft = agl_ft`, `baro_mb = 1013`, `mach = ias/661.5`, `beta_deg = 0`. The flare cue's blink phase uses the
replay clock `t`.

## Current fixtures

- `lax_dusk_07L` — FSLabs A321, ChasePlane cockpit (zoom 0.497), 3440×1440, LAX 07L at dusk, video 285–392 s,
  9 checks from 1500 ft to 15 ft. Runway from OpenStreetMap.

## Regenerating (needs the original recording, not in this repo)

```bat
reference\native\build.bat
python tests\make_golden.py <session_dir> --name lax_dusk_07L --start 285 --end 392 --lag -0.38 ^
  --runway "33.9376997,-118.4011429,26.6,82.97,3388,45,07L" --checks 290,320,350,370,380,384,386,388,390
```

A session directory is what the author's recording logger writes: `track.csv` (60 Hz SimConnect log), `meta.json`,
`obs.jsonl` (OBS websocket log, used to map log time to video time). Only regenerate goldens from the reference
implementation, never from the port.
