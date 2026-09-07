# UI sharpness measurements — fonts, icons, HUD isolation (2026-09-07)

Why this exists: two "crisp UI" features were built and measured at 2560x1440 (RTX 5070 Ti, borderless, vsync off)
and both were deleted before release. Nothing about the interface can be sharpened in software at ≤1440p; the numbers
below are the proof so nobody repeats the work. Screenshots referenced by name lived in the session scratchpad and
are not archived.

## Verdict

- Interface is authored for 4K and is already at its asset ceiling at 1440p: fonts rasterise 1:1, icons are minified
  from larger source art with the detail already present in mip 1 of the atlas.
- Crisp fonts (1.4.x) → removed in 1.5.0. Crisp icons (built for 1.5.0) → deleted unreleased (commit `8a1a58d`).
- The only real improvement would be higher-resolution source art (new sprites), not filtering or bias.

## Canvas facts

- `GeoscapeUICanvas` / `TacticalUICanvas` / `CommonUICanvas`: `ScreenSpaceOverlay`, `CanvasScaler` =
  `ScaleWithScreenSize`, reference 3840x2160 → `scaleFactor` 0.6667 at 1440p.
- All 43 canvases are Overlay (0 `ScreenSpaceCamera`, 0 `WorldSpace`).
- Scene camera `cullingMask` includes layer 5 (UI), but no UI is rendered into `colorRT` / `outRT`: the
  DumpOut / DumpColorIn PNGs show the scene only.

## HUD isolation from the post pass — PROVEN

- LUT `B&W Cinema`, strength 100: final frame = grey scene + fully coloured HUD (portraits, icons, TFTV dialog).
- Opaque HUD crops are pixel-identical between grade on and off.
- An earlier "HUD is graded" reading was semi-transparent panels over a changed scene: max delta 19 on an "opaque"
  dialog vs ~64 expected for a real Contrast-50 grade.

## Crisp fonts (removed 1.5.0)

- Sobel sharpness ON/OFF = 1.000 on resource-bar digits, nav tabs, time display.
- Coverage: 13/34 labels corrected. Fallbacks: glyph box mismatch 3 (+62 in the console), best-fit size changed 5,
  rich text 1, atlas rebuilt 1.
- Dynamic font atlas 512² in both states; glyphs rasterised at `fontSize × 0.667` = 1:1 to screen pixels, so there
  is nothing to gain.
- 1.4.1 drift root cause: the 2x atlas rect was not exactly 2× the 1x rect (±2 texel tolerance), which stretched the
  UV window. An exact remap fixed it (centroid shift 0.027 px) but the sharpness gain stayed 0 → feature deleted.

## Icons (crisp icons, deleted unreleased)

- Every UI sprite is DOWNscaled, ratios 1.7–10:

| Sprite | Source | On screen (1440p) |
|---|---|---|
| `Requirement_Icon` | 128² | 27x67 |
| `Manticore_smaller` | 256² | 140² |
| `PXBase_Geoscape_Icon` | 512² | 51x61 |
| faction icon | 128² | 44x48 |
| `UnitsOnBoard` | 100² | 60² |

- Main atlas `sactx-4096x4096-Uncompressed-UIAtlas_UI`: RGBA32, 13 mips, `Bilinear`. Standalone textures: no mips.
- Trilinear + `anisoLevel = 8` on every mipmapped HUD texture — ON/OFF diff = frame noise: max delta 2 on HUD crops,
  0 px > 4; full frame 626,994 px > 0 vs 606,181 between two ON captures (ON/ON2).
- `mipMapBias` −0.5 / −1.0 (trilinear and bilinear): px > 4 on the ability bar 387–423, squad portraits 2237–3167
  (frames/badges only), weapon panel 92–105; Sobel +0–2 %; no perceptible sharpening, no shimmer.
- Reason: `QualitySettings.anisotropicFiltering = ForceEnable` already, and mip 1 of a 4096 atlas already carries the
  detail; mip LOD 0.585 blends mips that differ by 2–4/channel.

## Levers rejected (with a Codex second opinion)

- `Canvas.pixelPerfect`: alignment only, animation jitter.
- Box / Lanczos UI shader: atlas bleed, breaks batching and masks.
- `mipMapBias`: measured nothing (above).
