# Renderforge 1.5.0

## Highlights

- **LUT filters and colour vision on the Geoscape** -- the nine LUT grades, the LUT strength slider and the Deuteranopia / Protanopia / Tritanopia correction now apply on the Geoscape as well as in tactical missions (the post pass runs on the one scene camera in both). Scene only as before: the HUD and menus are drawn after this pass and stay unchanged. A grade or correction chosen on the Geoscape carries into the next mission and back. Tooltips no longer say "Tactical missions only"; the vignette knob's tooltip drops the same claim (it always applied on the Geoscape).
- **Levels, Contrast and Clarity** -- four new live sliders under Options > Graphics, after Colour vision: Black point (0-40), White point (215-255), Contrast (50-150) and Clarity (0-100), all off by default. They run in the same analytic post pass as the LUT filters (after the grade, before the colour-vision compensation), in tactical missions and on the Geoscape; Clarity is a luma unsharp mask against a 13-tap Poisson blur. Scene only: the HUD stays unchanged. Console: `RenderforgeMod.SetGrade(black, white, contrast, clarity)`.
