# Renderforge 1.4.0

## Highlights

- **Quality knobs** -- four new settings the game never exposes, under Options > Graphics. Vignette (Vanilla / Off), Shadow resolution (Vanilla / Very High), Anisotropic filtering (Vanilla / 16x), and LOD detail (0 = Vanilla / 1.0--4.0). Every knob has a Vanilla position that restores the value the game's own graphics preset had set.
- **Colour vision correction** -- Deuteranopia, Protanopia and Tritanopia daltonization at full fixed strength (Machado et al. 2009 simulation, per-type error redistribution), running after the LUT grade and scene style inside the analytic post pass. Scene only: the HUD and menus are composited after this pass and stay uncorrected. Tactical missions only, same gate as the LUT. Composes with any LUT preset or style. Works on D3D11 and D3D12 with DLSS/FSR/XeSS and with the upscaler Off, and survives frame generation (verified FSR-FG and XeSS-FG). Picker row under Options > Graphics between LUT filter and Scene style; values None / Deuteranopia / Protanopia / Tritanopia. Console/PPCLI setter `RenderforgeMod.SetColorVision(<mode>)`.
- **D3D11 pinned-upscaler fix** -- a D3D12-only upscaler (FSR or XeSS) left selected while running D3D11 used to disable the entire post pass (LUT, scene styles, colour vision) until the upscaler was changed. Fixed by falling back to Auto on D3D11.

## Install

Download **`Renderforge-Full-1.4.0.zip`** from the GitHub release (single archive, all vendor runtimes included). Extract into `<Phoenix Point>\Mods\` so the `Renderforge` folder is at `Mods\Renderforge\`. SHA256 checksum in `SHA256SUMS.txt`.

## Known limits

- The colour vision correction applies to the scene only; the interface (HUD, menus, tooltips) is drawn after the post pass and is not corrected.
- Quality knob frame-time and VRAM cost of Very High shadows and LOD detail 4.0 has not been measured.
