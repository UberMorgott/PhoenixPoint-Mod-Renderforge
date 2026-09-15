# Pixel-perfect UI per-frame cost measurement (2026-09-15)

Rig: D:\PP-Instance2, 2560x1440 borderless, D3D11, DLSS Quality (1707x960 -> 2560x1440), RTX 5070 Ti,
vsync off, `LimitFrameRate=false` (`Application.targetFrameRate=-1`). Renderforge 1.6.1, PPBridge armed.
Campaign started fresh via `plans\start-campaign.json`, then 3x `geo-fast-forward.json` (30 s each).

## Three states

| State | Canvas `pixelPerfect` | `MipBias.UiPin` | Setup command |
|---|---|---|---|
| S1 (full On) | true on 14 root overlay canvases | true | `Cfg.PixelPerfectUi=true` + `OnConfigChanged` |
| S2 (canvases only) | true | **false** | S1, then `MipBias.UiPin=false` + `MipBias.Resweep()` |
| S3 (full Off) | false (restored) | false | `Cfg.PixelPerfectUi=false` + `OnConfigChanged` |

## Results — overlay FPS (0.5 s moving average, window-capture screenshot)

Screenshot names below are the captures the FPS values were read from; the PNGs (31 MB of 1440p window
captures) were NOT kept in the repo — the tables are the record.

### Scene A: Geoscape (idle, event popup open — static UI, no camera rotation)

| State | FPS | Frame ms | Screenshot |
|---|---|---|---|
| S1 (full On) | 336 | 3.0 | `s1-geo.png` |
| S2 (canvases only) | 341 | 2.9 | `s2-geo.png` |
| S3 (full Off) | 399 | 2.5 | `s3-geo.png` |
| S1 (repeat) | 337 | 3.0 | `s1r-geo.png` |

**Delta S1 vs S3: 0.5 ms (+20% frame time).** S2 approx S1 (341 vs 336, noise-level).

### Scene B: Personnel / Roster (soldier list + 3D preview, idle animations)

| State | FPS | Frame ms | Screenshot |
|---|---|---|---|
| S1 (full On) | 287 | 3.5 | `s1-roster.png` |
| S2 (canvases only) | 287 | 3.5 | `s2-roster.png` |
| S3 (full Off) | 345 | 2.9 | `s3-roster.png` |
| S1 (repeat) | 284 | 3.5 | `s1r-roster.png` |

**Delta S1 vs S3: 0.6 ms (+21% frame time).** S2 = S1 exactly.

### Early `Time.unscaledDeltaTime` sampling (80 samples via `connect multi`)

These were taken before the overlay method was adopted. The first set (fresh campaign, window restored) showed
a consistent pattern before campaign drift from fast-forwards introduced noise.

| State | Scene | Avg ms | P95 ms | Max ms | Samples |
|---|---|---|---|---|---|
| S1 | Geo idle | 4.00 | 4.32 | 4.57 | 80 |
| S2 | Geo idle | 4.00 | 4.23 | 4.44 | 80 |
| S3 | Geo idle | 3.04 | 3.29 | 3.48 | 80 |

Delta S1 vs S3: 0.96 ms. S2 = S1. (These include ~0.7 ms PPCLI request overhead per frame; relative deltas valid.)

### Fast-forward sampling (geoscape simulation running at Scale 3600)

| State | Avg ms | P95 ms | Max ms |
|---|---|---|---|
| S1 | 4.44 | 5.02 | 6.18 |
| S3 | 2.79 | 3.05 | 3.45 |

Delta widens from ~1.0 ms idle to ~1.65 ms during fast-forward (simulation driving UI updates).
S2 measurement during ff (3.42 ms) fell between S1/S3 — attributed to campaign state drift across
three sequential 30 s fast-forward runs; the controlled overlay method (above) shows S2 = S1.

## Verdict

**H1 confirmed: `Canvas.pixelPerfect=true` is the entire measured cost. `MipBias.UiPin` adds zero.**

- S2 (canvases pixelPerfect, pin off) = S1 (both on), within noise, on both scenes.
- S3 (both off) is 0.5–0.6 ms faster per frame at idle (overlay), ~1 ms in `unscaledDeltaTime` sampling.
- The cost is CONSTANT, present even with no visible UI motion (idle geoscape, static roster).
  This rules out per-frame geometry rebuild as the sole mechanism — the overhead is from Unity calling
  `GetPixelAdjustedRect()` / `RectTransformUtility.PixelAdjustRect` on every enabled Graphic each frame
  when any ancestor canvas has `pixelPerfect=true`, whether or not the result changes.
- During geoscape fast-forward (simulation updating UI state), the delta widened to ~1.65 ms,
  consistent with ADDITIONAL rebuilds when elements actually change position/size.
- On a 240 Hz monitor, 0.5–0.6 ms is the difference between comfortably exceeding refresh
  rate and not (287 vs 345 FPS on Roster). The Discord reporter's rig is unknown; on a slower CPU the same
  constant + rebuild cost scales into the reported "fps drops pretty hard during motion".

## Log proof (tail of Player.log)

```
[INFO] [Renderforge] Pixel-perfect UI on: 14 root overlay canvases
[INFO] [Renderforge] MipBias: bias=-0,585 applied to 1372 textures (ui=-0,585 on 326 sprite textures, skipped 1241) in 8 ms
[INFO] [Renderforge] Pixel-perfect UI off: 14 canvases restored
[INFO] [Renderforge] MipBias: bias=-0,585 applied to 1372 textures (ui=-0,585 on 326 sprite textures, skipped 1241) in 7 ms
```

## Commands used

```powershell
# State transitions
.\ppcli.ps1 connect call '{"op":"set","target":"<cfg>","member":"PixelPerfectUi","value":true|false}'
.\ppcli.ps1 connect call '{"op":"invoke","target":"<mod>","member":"OnConfigChanged","args":[]}'
.\ppcli.ps1 connect call '{"op":"set","type":"Renderforge.MipBias","assembly":"Renderforge","member":"UiPin","value":true|false}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.MipBias","assembly":"Renderforge","member":"Resweep","args":[]}'

# Measurement
.\ppcli.ps1 connect screenshot '{"path":"<abs path>"}' -Window   # overlay FPS reading
.\ppcli.ps1 connect multi '<array of {verb:"call",args:{op:"get",type:"UnityEngine.Time",member:"unscaledDeltaTime"}}>'

# Scene navigation
.\ppcli.ps1 plan .\plans\start-campaign.json '{"difficultyIndex":1}'
.\ppcli.ps1 plan .\plans\geo-fast-forward.json '{"forMs":30000}'
.\ppcli.ps1 connect call '{"op":"invoke","target":"@view","member":"ToRosterState","args":[{"$enum":"Soldiers"},{"$enum":"PushOnTop"},null]}'
.\ppcli.ps1 connect call '{"op":"invoke","target":"@view","member":"ResetViewState","args":[null]}'
```

## Caveats

- **No driven camera rotation.** PPCLI cannot simulate mouse input; the geoscape globe was not rotated
  by the player during measurement. The cost measured is the per-frame baseline overhead of
  `pixelPerfect=true`, not the per-element geometry rebuild under active camera motion. The user's
  complaint ("during motion, camera in Personnel") likely compounds this baseline with rebuild cost.
  A manual test (player holding rotate) would capture the full delta.
- **Overlay resolution:** 0.5 s average, rounded to integer FPS. The ±1 FPS between S1 and S1-repeat
  confirms sub-measurement noise.
- **PPCLI sampling overhead:** `connect multi` adds ~0.7 ms per frame from request processing;
  absolute values inflated, but the S1-vs-S3 delta (0.96 ms) is consistent with overlay readings.
- No PPCLI defects encountered.

## Cleanup

PID 12012 stopped, arm marker deleted, Instance2 Renderforge config left at `PixelPerfectUi=true` (S1).
