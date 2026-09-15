# Verification: Harmony postfix pixel-perfect UI (2026-09-15)

Rig: D:\PP-Instance2, 2560x1440 borderless, D3D11, DLSS Quality (1707x960 -> 2560x1440), RTX 5070 Ti,
vsync off, `Application.targetFrameRate=-1`. Working-tree build with IconBleed diag (removed after).
Campaign started fresh via `plans\start-campaign.json`, 3x `geo-fast-forward.json` (30 s each).

Baseline: `results.md` in same directory (old code: On=336/287 vs Off=399/345 fps, 0.5-0.6 ms delta).

## Test 1 — Log on start

**PASS.** `Pixel-perfect UI on: 5047 graphics rebuilt` (triggered via `cfg.PixelPerfectUi=true` +
`OnConfigChanged` — Instance2 profile loads saved `false`). First MipBias: `bias=-0,585 applied to
972 textures (ui=-0,585 on 304 sprite textures, skipped 685) in 5 ms`. 0 Renderforge exceptions.

## Test 2 — FPS A/B (overlay, 0.5 s avg, `-Window` capture)

**PASS.** On = Off within noise (0-2 fps), the 0.5-0.6 ms/frame delta is eliminated.

| State | Geo idle | Roster | Geo ff |
|---|---|---|---|
| On | 346 (2.9 ms) | 346 (2.9 ms) | 355 (2.8 ms) |
| Off | 346 (2.9 ms) | 344 (2.9 ms) | 348 (2.9 ms) |
| On (repeat) | 344 (2.9 ms) | — | — |

Crops: `crop-fps-on-geo.png`, `crop-fps-off-geo.png`.

## Test 3 — IconBleed `prod` mode

**FAIL.** Mip pin works; Harmony snap does NOT replicate canvas.pixelPerfect's zeroing.

| state | atlas .00/.33/.66 | fullrect .00/.33/.66 | vanilla |
|---|---|---|---|
| Prod On (expected 0/0/0) | **118 / 115 / 0** | **118 / 0 / 0** | 104 flat |
| Prod Off (expected 138/136/0, 89/25/0) | 138 / 136 / 0 | 89 / 25 / 0 | 104 flat |

Prod On values match the `mip0` row from the baseline table — the mip pin (ui=-0.585) pins mip 0,
but the quad edges still land at fractional screen pixels. Verified: switching to `mip0+pp` mode
(explicit `PixelPerfectUi.Apply(true)` call), and forcing Off->On transitions to trigger
`SetVerticesDirty` all produce identical non-zero values. The Harmony postfix on
`Graphic.GetPixelAdjustedRect` calls `RectTransformUtility.PixelAdjustRect` and replaces `__result`,
but the native canvas renderer apparently also performs pixel-alignment when `canvas.pixelPerfect=true`
that the C#-side postfix cannot replicate. This is a regression from the shipped 1.6.1 fix.

Practical impact: vanilla sprites never bled (104 flat control), and TFTV's real mod icons' fractional-
position surplus was already eliminated by the mip pin alone (per icon-bleed-2026-09-08.md); the
remaining edge values (~8-9) were the glyph's own content. The synthetic IconBleed diagnostic at .00
and .33 positions is the affected case.

## Test 4 — Zero-Z prefabs (tactical)

**PASS.** `start-mission.json` -> `phase:tactical`. Vehicle class icon, hotkey badges (6/TAB/SPC/F/R),
AP pips (ОД 4/4), soldier portraits all visible with PP On. No vanished elements. Crop: `crop-squadbar-on.png`.

## Test 5 — Live toggle sanity

**PASS.** Off -> On -> Off in tactical logs rebuilt-count each time: `off: 32946`, `on: 32949`,
`off: 32952`. Throughout the session 18 total Pixel-perfect UI on/off lines logged, all with N > 0.

## Test 6 — Cleanup

**PASS.** PID 24392 stopped, 0 Instance2 processes. Arm marker deleted. `src\IconBleed.cs` removed,
Release rebuilt (242176 bytes, no IconBleed), clean DLL deployed to Instance2. `git status`:
`M docs/DESIGN.md`, `M src/Overlay.cs`, `M src/PixelPerfectUi.cs`, `M src/RenderforgeMod.cs` —
matches the pre-existing working tree.

## Commands used

```powershell
# State transitions
.\ppcli.ps1 connect call '{"op":"set","target":"<cfg>","member":"PixelPerfectUi","value":true|false}'
.\ppcli.ps1 connect call '{"op":"invoke","target":"<mod>","member":"OnConfigChanged","args":[]}'

# Measurement
.\ppcli.ps1 connect screenshot '{"path":"<abs>"}' -Window     # overlay FPS
.\ppcli.ps1 connect screenshot '{"path":"<abs>"}'              # in-game (for IconBleed rects)

# IconBleed
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.IconBleed","assembly":"Renderforge","member":"Setup|Mode|Rects|Teardown","args":[...]}'
python measure.py <png> <rects>

# Scene navigation
.\ppcli.ps1 plan .\plans\start-campaign.json '{"difficultyIndex":1}'
.\ppcli.ps1 plan .\plans\geo-fast-forward.json '{"forMs":30000}'
.\ppcli.ps1 plan .\plans\start-mission.json
.\ppcli.ps1 connect call '{"op":"invoke","target":"@view","member":"ToRosterState","args":[{"$enum":"Soldiers"},{"$enum":"PushOnTop"},null]}'
```

## PPCLI defects

None encountered.
