# DLSS bias-current-colour mask — constant-fill experiment (2026-09-07)

Question: does the shipped nvngx_dlss 310.9 (preset K, D3D11 path) honour
`NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask`? If yes, phase 2 renders geoscape
marker coverage into it to stop the WorldSpace-canvas ghosting (`../results.md`).

## Setup

- Rig: RTX 5070 Ti, Instance2 (`D:\PP-Instance2`, profile 592), 2560x1440 FullScreenWindow, vsync off, D3D11.
- Mod: commit `33874f9` (`feat(diag): DLSS bias-current-colour mask constant test`). Runtime knob
  `RenderforgeMod.SetBiasMaskMode(int)` (PPCLI `call`), `Diagnostics.BiasMaskMode`, native
  `Dlss_SetBiasMaskMode` -> `FrameParams::biasMaskMode` -> `Device11::BiasMask()`: R8_UNORM
  render-res texture (1707x960 at Quality), `ClearRenderTargetView` to 0 (mode 1) or 1 (mode 2)
  every evaluate, bound as `ep.pInBiasCurrentColorMask`, subrect base (0,0). Mode 0 = unbound.
  D3D12 / FSR / XeSS: NOT implemented (field ignored).
- Reference "no history" run **R**: mask 0 + `SetForceReset(true)` (`InReset` every frame).
- Scene: fresh campaign geoscape, DLSS Quality (render 1707x960 -> 2560x1440), preset K.
  `focus_pos` on the Phoenix base site (`-1,93 4,754 3,826` — the console parses floats with the
  RU locale comma) with `RotationDamping=5`, 3.5 s settle, two stills 330 ms apart, then
  `RotationDamping=1` and `focus_pos` to the same point yawed 30 deg (`-3,585 4,754 2,348`);
  `DumpOut` (outRT = SDK output before HUD) at +200 / +550 / +900 ms (frames a/b/c).
  Same recipe for mask 0, 1, 2, R.
- WorldSpace markers (base ring, aircraft, "?" sites) ARE in outRT (`hud-vs-outRT-*.png`), so
  they do pass through DLSS — the ghosting is DLSS's.

## Status per mode (from `GetStatus` after each sweep)

| mode | create | eval | feature | lastError |
|---|---|---|---|---|
| 0 (unbound) | 0x1 Success | 0x1 Success | 1 | 0 |
| 1 (all-zero) | 0x1 Success | 0x1 Success | 1 | 0 |
| 2 (all-one) | 0x1 Success | 0x1 Success | 1 | 0 |
| R (ForceReset) | 0x1 Success | 0x1 Success | 1 | 0 |

No parameter rejection: the DLL accepts the R8 mask silently.

## Whole-frame metrics (still frames, `metrics.json`)

| metric | mode 0 | mode 1 | mode 2 | R (no history) |
|---|---|---|---|---|
| still Laplacian var | 0.00442 | 0.00523 | 0.00478 | **0.00797** |
| still Sobel mean | 0.01756 | 0.01833 | 0.01789 | **0.02234** |
| flicker MAD (still1 vs still2) | 0.00399 | 0.00406 | 0.00413 | **0.00782** |
| flicker frac >0.05 | 0.94 % | 1.01 % | 1.03 % | **2.34 %** |
| sweep frame b Laplacian var | 0.00607 | 0.00507 | 0.00506 | **0.00939** |

Mode 2 sits inside the mode-0/1 run-to-run spread (<=18 %); R is 1.7-2x away on every metric.
`still-zoom4x-modes-0-1-2-R.png` (rows 0/1/2/R, 4x nearest): only R shows the aliased single-frame
look on the "?" icon and coastline; mode 2 is indistinguishable from 0/1.

## Marker metrics (base aircraft icon, 200x100 crop, `marker-metrics.json`)

| frame | metric | mode 0 | mode 1 | mode 2 | R |
|---|---|---|---|---|---|
| still | crop Laplacian var | 0.0471 | 0.0496 | 0.0515 | 0.0569 |
| b (+550 ms, moving ~0.6 px/ms) | crop Laplacian var | 0.0365 | 0.0329 | 0.0426 | **0.0638** |
| b | crop Sobel mean (x) | 0.031 | 0.031 | 0.038 | **0.049** |
| b | faint/strong warm-pixel ratio (ghost halo) | 0.67 | 0.88 | 1.01 | 0.70 |

The icon loses ~30 % of its edge energy while moving under history (modes 0/1/2) and none under R
— that IS the ghosting, and mode 2 (all-one = "prefer current frame" everywhere) does not recover
it. A dedicated trail-length metric (faint warm pixels trailing the icon bbox) was too noisy to
report: blob detection split the icon differently per frame.
`base-icon-still-b-modes-0-1-2-R.png`: rows 0/1/2/R, columns still / frame b.

## Verdict

**Not honoured (or no measurable effect) in this configuration**: all-one mask == all-zero mask ==
unbound within noise, while the calibrated no-history reference (R) is clearly separable on every
metric. Phase 2 (marker coverage into the mask) would do nothing with preset K on 310.9 via D3D11.

Unverified / next: (1) CNN presets (C/E/F) may honour the mask where the transformer preset K does
not — needs a preset knob (`SetPresetHints`) and a re-run; (2) a spatially split mask (left 0 /
right 1) would separate "ignored" from "weak global effect" better than whole-frame stats;
(3) D3D12 path untested (mask not implemented there).

## Cleanup

RotationDamping restored to 5.0, mask mode 0, ForceReset off, mode Off (as found; no
ModConfig written to Instance2), process 36004 stopped, `ppcli-enabled` deleted, 0 Instance2
processes.
