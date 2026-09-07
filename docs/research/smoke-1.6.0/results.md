# Smoke test — Renderforge 1.6.0 on Instance3 (2026-09-07)

Zip `build\release\Renderforge-Full-1.6.0.zip` 171,437,740 B sha256
`8D3EE346065032D172050C0BF8664A2870A7C9C442B0D39B9DA5BF95D826025C`. DLL 1.6.0.0, 25 files.
Rig: D:\PP-Instance3, profile 76561197996210593, 2560x1440 FullScreenWindow vsync off, D3D11,
DLSS Quality (1707x960 -> 2560x1440). PPBridge armed, cold start, `connect state` gate passed.

## Results

| | check | result | evidence |
|---|---|---|---|
| a | Player.log | **PASS** | `provider=DLSS` at init; 0 Renderforge exceptions; 1 "crisp" line = migration notice ("crisp fonts was removed in 1.5.0; the setting is ignored"). Resolution list line NOT emitted: video options panel never opened (transpiler installed but not triggered). |
| b | Tactical DLSS Quality SetImage/ResetImage | **PASS** | SetImage(20,0,0,0): -Window scene crop mean bright 24.3 vs default 21.6 (+2.7), sat 0 vs 1.3; 15529 px differ >4 / 40000 crop; HUD crop 0 px differ. ResetImage after Sharpness=80/Contrast=150/LodBias=3.0: all fields at defaults (Sharpness=40, Contrast=100, LodBias=0, Exposure=0, Brightness=0, Saturation=100, Vibrance=0, LevelsBlack=0, LevelsWhite=255, Clarity=0). Log: `Renderforge image settings reset to defaults: Sharpness, Lut, LutStrength, Exposure, LevelsBlack, LevelsWhite, Brightness, Contrast, Clarity, Vibrance, Saturation, SceneStyle, SceneStyleStrength, PixelSize, Vignette, ShadowResolution, Anisotropic, LodBias (Renderer, Upscaler, Mode, FrameGen, ColorVision kept)`. |
| c | Pixel-perfect UI | **PASS** | on: 12 root overlay canvases; off: 12 canvases restored. |
| d | Geoscape marker overlay | **PASS** | Armed: `layer 15, 425 sites / 7686 objects, 1 cameras lost the bit`. Rotation sweep (RotationDamping=1.0, focus_pos 5,0,0): 12 DumpScreen frames 30842-30970 (~100 ms apart), base marker tracks x=1794 to x=2342 (frames 0-3), off-screen frames 4-11. **Doubled: 0. Missing: 0.** B&W LUT (BlackAndWhiteCinema strength=100): globe sat 0.34, HUD bottom sat 4.13. ColorVision=Deuteranopia: `released - colour vision Deuteranopia (markers would bypass the daltonisation pass)`, `restored 425 sites / 13860 objects / 1 cameras`; None: `drawn after reconstruction ... layer 15, 425 sites / 5796 objects`. Mode Off: `restored 425 sites / 5796 objects / 1 cameras`, `not armed - passthrough generation`. |
| e | Screen.width/height | **PASS** | 2560x1440. |

## Notes

- TFTV error dialog appeared on geoscape entry (TFTV's own exception, not Renderforge). Did not affect
  DumpScreen capture or marker overlay tests. Window was minimized during the DumpScreen sweep; DumpScreen
  captures from the GPU backbuffer and is unaffected.
- DumpScreen raw RGB24 files are horizontally flipped when converted to PNG via System.Drawing (known
  backbuffer storage order).
- 4 NVIDIA runtime logs created in the mod folder during the run (nvngx_dlss_310_9_0.log, nvngx_dlssg_310_9_0.log,
  nvngx.log, nvsdk_ngx.log) — expected, total file count 29.
- No PPCLI defects encountered.

## Cleanup

Instance3 process stopped (PID 46628), `Mods\PPBridge\ppcli-enabled` deleted, 0 Instance3 processes.
1.6.0 files left installed (25 zip files + 4 runtime logs).

## § candidate 3 — 2026-09-07

Zip sha256 `96402B2886A070818C3C4B06E19BB0A457E8014F629CC9CA55F0C45BE9DC94A7`, size 171437854.
DLL 1.6.0.0, 25 files. Rig: D:\PP-Instance3, profile 76561197996210593, 2560x1440
FullScreenWindow, D3D11. PPBridge armed, cold start PID 28288, `connect state` gate passed.

### Results

| | check | result | evidence |
|---|---|---|---|
| a | Tactical DLSS Quality | **PASS** | `gen=Live mode=Quality eval=0x1(NVSDK_NGX_Result_Success)` render=1707x960 out=2560x1440. Screenshot 2560x1440 (HUD) + scene 1707x960 (pre-upscale). ResetImage: `image settings reset to defaults: Sharpness, Lut, …` logged. |
| b | Geoscape marker overlay | **PASS** | Armed: `layer 15, 425 sites / 8274 objects, 1 cameras lost the bit`. Rotation sweep (RotationDamping=1.0, focus_pos 5,0,0): 12 DumpScreen frames 29436-29447 (~67 ms apart), base marker tracks x=1387 to x=2364 (frames 0-9), off-screen frames 10-11. **Doubled: 0. Missing: 0.** SetMarkerOverlay(false): `restored 425 sites / 12894 objects / 1 cameras`. SetMarkerOverlay(true): `drawn after reconstruction … layer 15, 425 sites`. Mode Off: `restored 425 sites / 4410 objects / 1 cameras`, `not armed - passthrough generation`. |
| c | Player.log | **PASS** | 0 Renderforge exceptions. |

### Cleanup

Instance3 process stopped (PID 28288), `Mods\PPBridge\ppcli-enabled` deleted, 0 Instance3 processes.
1.6.0 files left installed. No PPCLI defects encountered.
