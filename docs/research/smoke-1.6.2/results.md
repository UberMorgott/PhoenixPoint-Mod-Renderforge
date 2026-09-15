# Smoke test — Renderforge 1.6.2 on Instance2 (2026-09-15)

Zip `build\release\Renderforge-Full-1.6.2.zip` 171,439,754 B sha256
`D4DF28DAA7751E1BEFE7BE5F12EB2EC952E25E59316331234C8F48DDFF763D20` (DLL 1.6.2.0, DLL sha256
`EF788186CF86E7647E5632A1ED6965E998B8EBB557F10A7A805ADBAFE39962F3`).
The zip's `Renderforge\` folder replaced `D:\PP-Instance2\Mods\Renderforge\` whole (DLL hash = zip's).
Rig: D:\PP-Instance2, profile 76561197996210592, 2560x1440 borderless, D3D11, DLSS Quality (1707x960 -> 2560x1440),
RTX 5070 Ti. Instance2 `ModConfig.json` had `PixelPerfectUi:true` (unchanged for this run). PPBridge armed,
cold start `PhoenixPointWin64.exe -mods` PID 34808, `connect state` gate answered after ~25 s.
Screenshots in the session scratchpad `smoke162\` (`-Window` captures = finished frame).

## Results

| | check | result | evidence |
|---|---|---|---|
| a | Cold launch, HomeScreen, log | **PASS** | live assembly `Renderforge, Version=1.6.2.0` (confirmed via reflection `Assembly.Load("Renderforge").FullName`); `upscaler available provider=DLSS version= api=11 unityIface=1 renderer=D3D11 unity=2019.4.31f1`; first sweep `MipBias: bias=0,000 applied to 693 textures (ui=-0,585 on 25 sprite textures, skipped 685) in 5 ms`; `Pixel-perfect UI on: 373 graphics rebuilt`. `a-home.png` overlay: `D3D11 / DLSS SR (nvngx 310.9.0.0) / Quality (1707x960 -> 2560x1440) / FPS 573 (1,7 ms)`. |
| b | Geoscape, FPS A/B PP On vs Off | **PASS** | `start-campaign.json` difficultyIndex 1 -> `phase:geoscape / Playing` in 12.4 s. Overlay captures — On: 378/380 fps (2,6 ms); Off: 377/368/378 fps (2,6-2,7 ms). Equal within noise; the 0.5-0.6 ms/frame delta from 1.6.1's canvas.pixelPerfect is eliminated. Geoscape HUD: resource bar icons, base/squad/1% markers on globe, aircraft card, tabs all present. |
| c | Live toggle Off -> On -> Off | **PASS** | `Pixel-perfect UI off: 87376 graphics rebuilt` / `on: 87379` / `off: 87382` — all N > 0. |
| d | Tactical HUD with PP On | **PASS** | `start-mission.json` ALN_PLT_Nest_48x48_A seed 12345 -> `phase:tactical / Playing` in 18.3 s. `d-tactical-hud.png`: vehicle class icon (1 БРОНЕНОСЕЦ), ОЗ 1250/1250, ОД 4/4 pips, 6 soldier portraits with class badges, TAB/6/SPC/F/R/X hotkey badges, weapon slot, ability icons all present. TFTV error dialog visible (known, not Renderforge). |
| e | Exceptions | **PASS** | 3 exceptions, all `[MP][tftv] TFTV REPORTED AN EXCEPTION` (NullReferenceException x2, InvalidOperationException x1 — same pattern as 1.6.1 smoke); Renderforge exceptions/THREW: 0. |

Player.log: 3 exceptions total, all TFTV-attributed; Renderforge exceptions/THREW: 0. Copy of the log in `smoke162\Player.log`.

## Notes

- The borderless game window minimises whenever the driving shell takes focus; `screenshot -Window` refuses a
  minimised window, so each capture is preceded by `ShowWindow(SW_RESTORE)` + `SetForegroundWindow` (3 s).
- Handles die on scene change: `RenderforgeMod.Instance` re-resolved after `start-campaign` (h:4:17/h:4:18).
- Assembly version line not printed in Player.log by 1.6.2 (unlike 1.6.1); confirmed via runtime reflection
  `System.Reflection.Assembly.Load("Renderforge").FullName` = `Renderforge, Version=1.6.2.0, Culture=neutral, PublicKeyToken=null`.
- TFTV error dialog appeared on tactical entry (same as 1.6.1 smoke); all HUD elements visible behind it.
- No PPCLI defects encountered.

## Cleanup

PID 34808 stopped, 0 Instance2 processes; `Mods\PPBridge\ppcli-enabled` deleted; profile 592 `ModConfig.json`
unchanged (`PixelPerfectUi:true` — same as before the run). 1.6.2 files left installed.
