# PixelPerfectUi toggle test 2026-09-07

Rig: RTX 5070 Ti, 2560x1440 FullScreenWindow, vsync off, D3D11.
Install: D:\PP-Instance2, profile 76561197996210592.
Build: Renderforge.dll 197632 B (HEAD 7f975f9, 1.5.0 unpublished).
Scene: fresh campaign geoscape -> tactical (ALN_PLT_Nest_48x48_A seed 12345).
Capture: `connect screenshot` (end-of-frame Camera.main readback), timeScale 0.0001 during geoscape captures.

## Feature path

`DlssConfig.PixelPerfectUi` (bool, default false) -> `RenderforgeMod.OnConfigChanged` ->
`PixelPerfectUi.Apply(bool)`: sets `Canvas.pixelPerfect` on every root ScreenSpaceOverlay canvas,
records originals; `Reapply()` from `RenderforgeMod.OnLevelStart` for scene transitions.

## Check 1: OFF (default)

- GeoscapeUICanvas: pixelPerfect=false, renderMode=ScreenSpaceOverlay, isRootCanvas=true
- CommonUICanvas: pixelPerfect=false, renderMode=ScreenSpaceOverlay, isRootCanvas=true
- Screenshot V captured

## Check 2: ON via config

- `cfg.PixelPerfectUi = true` + `OnConfigChanged()`
- Log: `Pixel-perfect UI on: 13 root overlay canvases` (initial), then `14` on re-apply
- GeoscapeUICanvas: pixelPerfect=true
- CommonUICanvas: pixelPerfect=true
- Screenshot P captured (same frozen geoscape scene)

### Metrics: V (OFF) vs P (ON)

| Region | px>4 | maxDelta | Sobel ratio | Crop | Verdict |
|---|---|---|---|---|---|
| digit-200 (Purista Light 32) | 496 | 175 | 1.1231 | 76x36 | shifted, +12% sharper |
| digit-1000 (Purista Light 32) | 484 | 137 | 1.0656 | 86x36 | shifted, +7% sharper |
| time 00:00 | 928 | 124 | 1.0508 | 86x41 | shifted, +5% sharper |
| ZADACHI (Purista Medium 48) | 547 | 114 | 1.0367 | 96x41 | shifted, +4% sharper |
| nav BAZY (SourceHanSans-Med 36) | 544 | 83 | 1.0485 | 86x36 | shifted, +5% sharper |

px>4 in the hundreds (484-928) and maxDelta 83-175 confirm visible position shifts.
Sobel ratio 1.04-1.12 confirms slight sharpening from pixel-grid snapping.
Consistent with previous manual pixelPerfect measurement (px>4 499-778, Sobel 1.02-1.07).

## Check 3: Scene transition (tactical)

- `start-mission.json` from live geoscape (cameFrom:geoscape)
- Log: `Pixel-perfect UI on: 12 root overlay canvases` (Reapply on OnLevelStart)
- TacticalUICanvas: pixelPerfect=true, renderMode=ScreenSpaceOverlay, isRootCanvas=true

## Check 4: OFF (restore)

- `cfg.PixelPerfectUi = false` + `OnConfigChanged()`
- Log: `Pixel-perfect UI off: 12 canvases restored`
- TacticalUICanvas: pixelPerfect=false (restored)

## Check 5: Exceptions

0 exceptions mentioning Renderforge or PixelPerfect in Player.log.

## Check 6: Cleanup

- Game quit cleanly (Application.Quit)
- ppcli-enabled deleted, confirmed gone
- No Instance2 process

## Files

- `toggle-*-{A,B}-8x.png` — 8x nearest-neighbor zooms, A=OFF B=ON
- `toggle-metrics.json` — raw per-crop metrics
