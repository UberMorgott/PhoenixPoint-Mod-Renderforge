# Smoke test — Renderforge 1.6.3 on Instance2 (2026-09-24)

Zip `build\release\Renderforge-Full-1.6.3.zip` 171,439,853 B sha256
`23957E44A4D7FC010689210FC5570F81A5F8EBD9C933961EA54AFB21E5104DAF` (DLL 1.6.3.0, DLL sha256
`FF9A2A4592A1814BE0CE959717A8AFD34F3236A2B7DBD95559A0430B3CD0DFB5`; built from `7439e20`).
The zip's `Renderforge\` folder replaced `D:\PP-Instance2\Mods\Renderforge\` whole (DLL hash = zip's).
Rig: D:\PP-Instance2, profile 76561197996210592, 2560x1440 borderless, D3D11, DLSS Quality (1707x960 -> 2560x1440),
RTX 5070 Ti. Cold start via `ppcli.ps1 run state` (PID 22800, log `ppcli-DPPInstance2-15944.log`).

1.6.3 = one fix over 1.6.2 (`2386807`, marker overlay vs `CameraManager.PauseObjectRendering`), verified in-game
BEFORE the bump on the same rig; the release zip only adds the version bump.

## Results

| | check | result | evidence |
|---|---|---|---|
| a | Release zip layout | **PASS** | Full zip extracts to exactly one folder `Renderforge` (25 files); `Renderforge.dll` FileVersion `1.6.3.0`; size 171,439,853 B vs 1.6.2's 171,439,754 B. |
| b | Cold launch, HomeScreen, log | **PASS** | `Discovered mod com.morgott.Renderforge`; `upscaler available provider=DLSS version= api=11 unityIface=1 renderer=D3D11 unity=2019.4.31f1`; `DLSS generation: mode=Quality ... render=1707x960 out=2560x1440`; `MipBias: bias=0,000 applied to 693 textures (ui=-0,585 on 25 sprite textures ...)`; `Pixel-perfect UI on: 374 graphics rebuilt`; `state` -> `phase:menu / HomeScreen / Playing`. |
| c | Exceptions | **PASS** | 0 `Exception` lines in the run log (Renderforge or otherwise). |
| d | Geoscape cutscene fix (`2386807`, pre-bump build, same source) | **PASS** | Repro `play_cutscene DA_Intro_Cutscene` on a live geoscape. Before: overlay flapped every other frame (630 release + 631 re-activate cycles in a 55 s cutscene, each `FindObjectsOfType` over 425 sites + Sync add/destroy + command-buffer move), 27-34 fps vs 133-169 with the overlay off. After: 0 flaps, 5 marker log lines total, 134-159 fps in the cutscene, frames advance, cutscene ends normally, overlay re-arms after exit (`0xFFFFFFFF -> 0xFFFF7FFF`, 195 fps geoscape). The reported freeze itself did not reproduce on this rig before or after; the flap mechanism is gone. |

## Notes

- Codex review `cx -Review -Commit 2386807 -Deep`: no confirmed regressions; static only (changed branches, callers and
  the game's pause-rendering implementation checked).
- Screenshots of (d) in the session scratchpad `repro\` (before) and `verify\` (after).
- No PPCLI defects encountered.

## Cleanup

`run` stopped PID 22800 itself and restored `Options.jopt` byte-exact; `Mods\PPBridge\ppcli-enabled` deleted;
0 Instance2 processes. 1.6.3 files left installed.
