# Third-party notices

This project is licensed GPL-3.0-only (see `LICENSE`). It uses or refers to the following.

## Derived material (inside this repository)

| What | Where | Source and license | Notes |
|---|---|---|---|
| HUD layout geometry: canvas 1280×1024 at 36.5714 px/°, boresight offset, tape scales, tick and label positions, polygon vertices of the speed index, altitude readout, roll scale / index / sideslip, aircraft reference, wind indicator placement | `reference/python/hud.py` (and the C++ port) | FlyByWire Simulations A32NX HUD, `fbw-a32nx/src/systems/instruments/src/HUD/` and `fbw-common/.../ND/shared/WindIndicator.tsx`, as found on the `hudTest` branch of the JulKem/aircraft fork of flybywiresim/aircraft. **GPL-3.0.** | No source code was copied; coordinates and sizes were transcribed from the reference SVG/TSX. Treated as derived work, hence this project is GPL-3.0. |
| Runway position of LAX 07L/25R used by the LAX test fixture | `tests/fixtures/lax_dusk_07L/fixture.json` | © OpenStreetMap contributors, **ODbL 1.0** | The live program uses the simulator's own facility data. |

## Bundled source code

The in-sim panel package (`panel/birdie-panel`, with its fonts) is built into `Birdie.exe` as resources and written
to the MSFS Community folder when the user installs it; the release zip carries the font licenses in `licenses/`.

| Component | Where | License | Use |
|---|---|---|---|
| nlohmann/json 3.11.3 (single header) | `third_party/nlohmann/json.hpp` | **MIT**, © 2013–2023 Niels Lohmann (`third_party/nlohmann/LICENSE.MIT`) | Reading test fixtures; later the settings file. |
| Noto Serif SC, Noto Sans SC (subsets) | `panel/birdie-panel/html_ui/InGamePanels/Birdie/fonts/birdie-serif.ttf`, `birdie-sans*.ttf` | **SIL OFL 1.1**, © Google LLC (`fonts/OFL-noto.txt`) | Panel text (the sim's UI engine has no system font fallback). Renamed subsets, as the OFL allows for modified versions. |
| Cormorant Garamond Italic (subset) | `.../fonts/birdie-numerals.ttf` | **SIL OFL 1.1**, © The Cormorant Project Authors (`fonts/OFL-cormorant.txt`) | Panel numerals. |
| Microsoft WebView2 SDK 1.0.4258.31: headers and the static loader `WebView2LoaderStatic.lib` (x64) | `third_party/webview2/` | **BSD-3-Clause**, © Microsoft Corporation (`third_party/webview2/LICENSE.txt`; the package's `NOTICE.txt` kept next to it) | Birdie's settings window. The loader is linked into `Birdie.exe`, so the release package must carry this notice. |
| Barlow Semi Condensed Medium (subset) | `.../fonts/birdie-hud.ttf` | **SIL OFL 1.1**, © The Barlow Project Authors (`fonts/OFL-barlow.txt`) | Lettering of the HUD preview in the panel. |

## Runtime dependencies (not redistributed here)

| Component | License | Use |
|---|---|---|
| Microsoft Flight Simulator 2024 SDK — SimConnect | MSFS SDK EULA (`Licenses/MSFS SDK EULA.pdf` in the SDK) | Data source. `Birdie.exe` links the SDK import library and delay-loads `SimConnect.dll` from its own folder (the build copies it there; the Python reference loads it from `SIMCONNECT_DLL`). The release zip ships `SimConnect.dll` next to the exe, as MSFS add-on tools commonly do (GSX/Couatl, Volanta, SayIntentions all ship one). The EULA allows using the SDK to develop MSFS add-ons and forbids distributing the SDK "except for any distributable code", without listing which files that is; Birdie treats the SimConnect client DLL as such. The static library is not used (it requires the dynamic CRT and OpenMP). |
| ChasePlane (Parallel 42) | Commercial, user-installed | Only its public L-vars (`L:p42_cp_*`) are read. |
| Microsoft Edge WebView2 Runtime | Microsoft software license (part of Windows 11, Evergreen) | Renders the settings window. Not bundled; when it is missing the panel opens in the browser. |
| Windows: Direct2D, DirectWrite, Direct3D 11, DirectComposition, DWM, GDI+, WIC | OS components | Renderer, tray icon, test images. |
| Bahnschrift font | Windows system font | Not bundled. |
| Python reference only: PyQt6 (GPL-3.0 / commercial), NumPy (BSD-3), OpenCV (Apache-2.0), Matplotlib (PSF-based), Pillow (HPND) | as stated | Reference implementation and tools; not part of the C++ program. |

## Trademarks

Microsoft Flight Simulator, ChasePlane, FlyByWire, FSLabs, Fenix and Airbus are trademarks of their owners. This
project is not affiliated with or endorsed by any of them.
