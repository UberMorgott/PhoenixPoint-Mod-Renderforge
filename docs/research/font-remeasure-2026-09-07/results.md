# Font remeasure 2026-09-07 — CrispFonts and pixelPerfect at 2560x1440

Rig: RTX 5070 Ti, 2560x1440 FullScreenWindow, vsync off, D3D11.
Install: D:\PP-Instance2, profile 76561197996210592.
Build: Renderforge.dll 207872 B from commit f6d382f (CrispFonts restored).
Scene: fresh campaign geoscape, paused, timeScale 0.0001 during capture (frozen animation).
Capture: `connect screenshot` (end-of-frame Camera.main readback).

## Correction coverage (CrispFonts ON, 36 non-console Text objects)

| Outcome | Count |
|---|---|
| corrected | 14 |
| original (not supported) | 11 |
| fallback: best-fit/hinted size changed | 5 |
| fallback: glyph box mismatch | 5 |
| fallback: rich-text color mapping changed | 1 |
| FontRasterCorrection.CachedCount | 100 |

Corrected labels include: resource digits (Purista Light 32), speed display (SourceHanSans-Medium 32),
ZADACHI header (Purista Medium 48), BAZY small label (SourceHanSans-Medium 36).

## CrispFonts ON vs OFF (frozen captures Vf/Cf)

| Region | px>4 | maxDelta | Sobel ratio | Correction | Verdict |
|---|---|---|---|---|---|
| digit-200 (Purista Light 32) | 0 | 2 | 1.00 | corrected | same |
| digit-1000 (Purista Light 32) | 0 | 2 | 1.00 | corrected | same |
| time 00:00 | 0 | 0 | 1.00 | fallback: glyph box mismatch | same |
| ZADACHI (Purista Medium 48) | 0 | 1 | 1.00 | corrected | same |
| nav BAZY (SourceHanSans-Med 36) | 0 | 3 | 1.00 | corrected | same |

**Zero px>4 in any corrected label. Sobel ratio = 1.00 everywhere. Pixel-identical at 1440p.**

## Toggle clean check: V vs V2 (OFF -> ON -> OFF, frozen)

| Region | px>4 | maxDelta | Sobel ratio | Verdict |
|---|---|---|---|---|
| digit-200 | 0 | 2 | 1.00 | same |
| digit-1000 | 0 | 2 | 1.00 | same |
| time 00:00 | 0 | 0 | 1.00 | same |
| ZADACHI | 0 | 1 | 1.00 | same |
| nav BAZY | 0 | 3 | 1.00 | same |

**Toggle is clean. maxDelta <= 3 everywhere (sub-threshold noise).**

## pixelPerfect = true (both GeoscapeUICanvas + CommonUICanvas, CrispFonts OFF)

| Region | px>4 | maxDelta | Sobel ratio | Verdict |
|---|---|---|---|---|
| digit-200 | 499 | 180 | 1.06 | shifted, slightly sharper |
| digit-1000 | 524 | 145 | 1.05 | shifted, slightly sharper |
| time 00:00 | 778 | 124 | 1.07 | shifted, slightly sharper |
| ZADACHI | 744 | 117 | 1.03 | shifted, slightly sharper |
| nav BAZY | 567 | 83 | 1.02 | shifted, slightly sharper |
| PXBase-icon | 286 | 136 | 1.03 | shifted +3% |
| resource-icons | 182 | 115 | 0.98 | shifted, -2% (noise) |
| faction-icons | 196 | 140 | 0.99 | shifted, no change |

pixelPerfect causes visible position shifts (maxDelta up to 180) with small sharpness gains (+2-7%).
This is consistent with the 09-07 doc's rejection: alignment only, animation jitter risk.

## KHARAKTERISTIKI / VYNOSLIVOST

Not reachable from the geoscape main view; these labels live on the soldier detail screen. The roster
state navigation failed (UIStateGeoscapeOptions conflict); skipped per "if reachable" qualifier.

## Conclusion

CrispFonts produces pixel-identical output at 2560x1440. The 2x UV remap corrects 14/36 visible labels,
but the corrected UVs sample the same glyph pixels as vanilla because fontSize x scaleFactor (32 x 0.667
= 21.3) already rasterises glyphs 1:1 to device pixels at this resolution. The 2x atlas contains no
additional detail to sample.

pixelPerfect snaps UI elements to integer pixels (Sobel +2-7%), but large position deltas (80-180)
confirm element shifting that would cause animation jitter. Not recommended.

## Files

- `frozen-comparison.json` — raw per-crop metrics
- `crisp-*-{A,B}-8x.png` — 8x nearest-neighbor zooms, A=OFF B=ON
- `toggle-*-{A,B}-8x.png` — 8x zooms, A=V(OFF) B=V2(OFF-after-toggle)
- `pxperf-*-{A,B}-8x.png` — 8x zooms, A=normal B=pixelPerfect
