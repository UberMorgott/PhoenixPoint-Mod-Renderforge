# Renderforge 1.5.0

The first user-feedback round after 1.4.1.

## Highlights

- **LUT filters and colour vision on the Geoscape** -- the nine LUT grades, the LUT strength slider and the Deuteranopia / Protanopia / Tritanopia correction now apply on the Geoscape as well as in tactical missions (the post pass runs on the one scene camera in both). Scene only as before: the HUD and menus are drawn after this pass and stay unchanged -- re-verified this release by dumping the post-pass output (no HUD in it) and comparing HUD pixels with a B&W grade on (unchanged). A grade or correction chosen on the Geoscape carries into the next mission and back. Tooltips no longer say "Tactical missions only"; the vignette knob's tooltip drops the same claim (it always applied on the Geoscape).
- **Levels, Contrast and Clarity** -- four new live sliders under Options > Graphics, after Colour vision, all off by default: Black point (0-40, input black: pixels darker than this become black and the rest stretch), White point (215-255, input white), Contrast (50-150, pivots at mid-grey) and Clarity (0-100, luma unsharp mask against a 13-tap Poisson blur, ~10 px radius at 1080p). They run in the same analytic post pass as the LUT filters (after the grade, before the colour-vision compensation), in tactical missions and on the Geoscape; scene only. Cost on an RTX 5070 Ti at 1440p: +0.04 ms with Clarity 100, +0.13 ms with all four at their extremes. Console: `RenderforgeMod.SetGrade(black, white, contrast, clarity)`, -1 keeps a value.
- **Crisp fonts removed.** Measured at 1440p it had no effect on sharpness (glyphs are already rasterised 1:1 for the screen) and covered 13 of 34 labels, while causing the per-glyph drift some of you saw in 1.4.1. The setting is ignored; the drift is gone with the feature. UI icons were measured too: the interface is authored for 4K and already at its quality ceiling at 1440p (trilinear, anisotropy and mip bias change nothing) — no icon feature shipped.

## Fixes

- Settings sliders no longer write the config file on every frame while dragging; saves are coalesced to the end of the frame.
- Quality-knob `Vanilla (...)` labels refresh after applying a different graphics preset with Options still open.

## Project

- Every commit now passes an automated gate: .NET build + whitespace format, 26 unit tests (CSV parser, string table, `strings.csv` contract), the native shim build with its WARP/D3D probes, secret scan and spell check.

## Install

Download **`Renderforge-Full-1.5.0.zip`** from the GitHub release (single archive, all vendor runtimes included) or subscribe on the Steam Workshop. Extract into `<Phoenix Point>\Mods\` so the `Renderforge` folder is at `Mods\Renderforge\`. SHA256 checksum in `SHA256SUMS.txt`.

## Known limits

- Contrast above 100 pivots at mid-grey, so dark tactical scenes get darker; combine with a lower Black point or use Clarity for local contrast instead.
- The colour vision correction and the grade sliders apply to the scene only; the interface is drawn after the post pass and is not corrected.
- Post-only mode (post pass without an upscaler) is still verified on NVIDIA hardware only.
- Translations other than English and Russian are machine-made and reviewed, not native-checked.
