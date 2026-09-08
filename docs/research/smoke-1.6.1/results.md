# Smoke test — Renderforge 1.6.1 on Instance2 (2026-09-08)

Zip `build\release\Renderforge-Full-1.6.1.zip` 171,439,518 B sha256
`201D71B54C91F43C7332C2E18EEC98D38CB6470345173A348D08857D08C781D1` (built from `5549b98`, DLL 1.6.1.0, 25 files).
The zip's `Renderforge\` folder replaced `D:\PP-Instance2\Mods\Renderforge\` whole (DLL hash = zip's).
Rig: D:\PP-Instance2, profile 76561197996210592, 2560x1440 borderless, D3D11, DLSS Quality (1707x960 -> 2560x1440),
RTX 5070 Ti. Instance2 `ModConfig.json` had `PixelPerfectUi:false` -> backed up, key removed for the run (fresh
default), restored after. PPBridge armed, cold start `PhoenixPointWin64.exe -mods` PID 31228, `connect state` gate
answered after 5 s. Screenshots in the session scratchpad `smoke161\` (`-Window` captures = finished frame).

## Results

| | check | result | evidence |
|---|---|---|---|
| a | Cold launch, HomeScreen, log | **PASS** | live assembly `Renderforge, Version=1.6.1.0`; `upscaler available provider=DLSS ... renderer=D3D11`; first sweep `MipBias: bias=0,000 applied to 972 textures (ui=-0,585 on 304 sprite textures, skipped 685)`; `Pixel-perfect UI on: 7 root overlay canvases` (12 after HomeScreen). `a-home.png`. 0 Renderforge exceptions. |
| b | Tactical, DLSS Quality, overlay | **PASS** | `start-mission.json` ALN_PLT_Nest_48x48_A -> `phase:tactical / Playing`. Overlay via `RenderforgeMod.ToggleOverlay` -> `overlay=True at TopCenter`; capture shows `Renderer: D3D11 / Upscaler: DLSS SR (nvngx 310.9.0.0) / Mode: Quality (1707x960 -> 2560x1440) / FPS 137`. Squad bar `1 БРОНЕНОСЕЦ` shows the vehicle class icon, ОЗ 1250/1250, ОД 4/4 pips; TAB/SPC/F/R/X hotkey badges, portrait class badges, ability icons, weapon slot 1 all present, nothing missing (`b-tactical-window.png`, crops `b-crop-squadbar.png`, `b-crop-hotkeys.png`). Level sweep `ui=-0,585 on 440 sprite textures`, `Pixel-perfect UI on: 13 root overlay canvases`. |
| c | Geoscape, markers + HUD | **PASS** | `start-campaign.json` difficultyIndex 1 -> `phase:geoscape / Playing`. `Geoscape markers: drawn after reconstruction by the DlssPresent camera, layer 15, 425 sites / 8064 objects, 1 cameras lost the bit`. Capture `c-geoscape.png`: base / ? / 1% / squad markers on the globe, resource bar icons, squad class icons, aircraft card, tabs, overlay FPS 240 (4,2 ms). |
| d | Pixel-perfect UI off -> on live | **PASS** | `Cfg.PixelPerfectUi=false` + `OnConfigChanged` -> `Pixel-perfect UI off: 14 canvases restored`; `true` -> `Pixel-perfect UI on: 14 root overlay canvases` (also 13/13 in tactical). HUD crops `d-crop-hud-off-top-on-bottom.png`: identical apart from sub-pixel snapping. |
| e | Resolution change re-sweep (review fix `5549b98`) | **PASS** | `Screen.SetResolution(1920,1080)` -> within 0.5 s `MipBias: bias=-0,585 ... (ui=-1,000 on 331 sprite textures)`; back to 2560x1440 -> `ui=-0,585`. |

Player.log: 3 exceptions, all `[MP][tftv] TFTV REPORTED AN EXCEPTION` (TFTVRevenant, mission start — TFTV's own,
same as the 1.6.0 smoke); Renderforge exceptions/THREW: 0. Copy of the log in `smoke161\Player.log`.

## Notes

- The borderless game window minimises whenever the driving shell takes focus; `screenshot -Window` refuses a
  minimised window, so each capture is preceded by `ShowWindow(SW_RESTORE)` + `SetForegroundWindow` (3 s).
- Handles die on scene change (epoch 15 -> 21 after `start-campaign`): re-resolve `RenderforgeMod.Instance` / `Cfg`.
- No PPCLI defects encountered.

## Cleanup

PID 31228 stopped, 0 Instance2 processes; `Mods\PPBridge\ppcli-enabled` deleted; profile 592 `ModConfig.json`
restored from backup (`PixelPerfectUi:false` back). 1.6.1 files left installed.
