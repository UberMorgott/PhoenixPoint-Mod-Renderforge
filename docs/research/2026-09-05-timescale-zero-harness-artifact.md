# `Time.timeScale = 0` in the test harness — artifact, not a Renderforge defect (2026-09-05)

Reported during colour-vision acceptance (D3D11): game "froze" (`Responding=False`) twice when a
Renderforge generation was released while `Time.timeScale == 0`. Investigated on Instance3.

## Verdict

- **Unreachable in play.** The decompile has exactly two `Time.timeScale` writes, both in the
  third-party capture plugin (`RenderHeads.Media.AVProMovieCapture\CaptureBase.cs:906,925`).
  Vanilla pause is game-owned: `Timing.Paused` (`GeoscapeView.cs:1146,1171,1265`,
  `GeoLevelController.cs:493,568,611`), tactical speed per actor (`TacTimeScaleRegulator.cs:47,51`),
  escape menu only toggles a panel (`UIModulePauseScreen.cs`). Live readback in tactical: `timeScale == 1.0`.
- **No blocking wait in Renderforge.** `src\` has zero coroutines / `WaitForEndOfFrame` / scaled
  time; retirement is `Update → Step → TryRetire` polling `Dlss_ReleaseStatus()` (an
  `InterlockedCompareExchange`, `RenderforgeNative.cpp:318`), the "two frames" is an unscaled
  `Update` counter (`DlssDriver.cs:218`).
- **Did not reproduce** in 5 attempts (DLSS→Off at 0, passthrough release at 0, 150 forced
  `ObstructFader`s + release at 0 and at 1).
- **What the hang logs show is vanilla:** at `deltaTime == 0` `ObstructFader.cs:763` never converges,
  so it warns and issues `Graphics.DrawMesh` every frame forever; `DebrisChunks.cs:109-111` likewise.
  Frame counter kept advancing in the "frozen" logs (16 frames over the last 4000 lines). The
  process becomes unresponsive from the log flood (291 MB Player.log), not from a deadlock.
  `timeScale = 0.0001` does NOT help (1.85 MB/s vs 1.78 MB/s).

## Rule for test protocols

Pause like the game does: `call set` `Timing.Paused = true` on the level (the lever
`PPCLI\plans\geo-fast-forward.json` already restores), never `Time.timeScale = 0`. The capture
protocol in `docs\superpowers\plans\2026-09-05-colour-vision.md` Task 6b should be read with this
substitution. Also truncate/rotate Player.log after any run that did use `timeScale = 0`.

Two PPCLI defects hit on the way are logged in `PPCLI\ISSUES.md` (handle-bearing JSON from a
PowerShell variable refused; `call {"op":"new"}` ignores `sig`).
