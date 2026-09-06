# Renderforge 1.4.1

First public release of the 1.4 line (1.4.0 was an internal build).

## Highlights

- **Quality knobs** -- four new settings the game never exposes, under Options > Graphics. Vignette (Vanilla / Off), Shadow resolution (Vanilla / Very High), Anisotropic filtering (Vanilla / 16x), and LOD detail (0 = Vanilla / 1.0--4.0). Every knob has a Vanilla position that restores the value the game's own graphics preset had set, and the row shows that value -- `Vanilla (High)`, `Vanilla (2.0)` -- so you can see which way the slider moves relative to the preset.
- **Colour vision correction** -- Deuteranopia, Protanopia and Tritanopia daltonization at full fixed strength (Machado et al. 2009 simulation, per-type error redistribution), running after the LUT grade and scene style inside the analytic post pass. Scene only: the HUD and menus are composited after this pass and stay uncorrected. Tactical missions only, same gate as the LUT. Composes with any LUT preset or style. Works on D3D11 and D3D12 with DLSS/FSR/XeSS and with the upscaler Off, and survives frame generation (verified FSR-FG and XeSS-FG). Picker row under Options > Graphics between LUT filter and Scene style; values None / Deuteranopia / Protanopia / Tritanopia. Console/PPCLI setter `RenderforgeMod.SetColorVision(<mode>)`.
- **All eight game languages** -- every Renderforge row, value and tooltip is now localized for English, Russian, Chinese (Simplified), French, German, Italian, Polish and Spanish, following the game's own language setting. Strings live in one embedded table (`strings.csv`); corrections welcome as pull requests.
- **Tooltips state the rules** -- each row's tooltip now says where it works: renderer (DirectX 11 / 12), whether an upscaler must be active, tactical missions only, scene only with the HUD untouched, restart required. LUT filters, colour vision and vignette are tactical-only; frame generation needs DirectX 12 with an upscaler; changing the renderer needs a restart.
- **Post pass without an upscaler** -- when NGX init fails but the D3D device survives, the native shim answers `DLSS_OK_POST_ONLY` and the mod runs a permanent passthrough generation carrying the analytic post pass (LUT grades, scene styles, colour vision correction, NIS sharpening); mip bias stays 0, vanilla SMAA is preserved, and the upscaler row greys out with the real reason. On D3D12 the auto-fallback chain (FSR, then XeSS, then post-only) is preserved. Verified on NVIDIA RTX 5070 Ti under D3D11 and D3D12 with NGX failures both faked (`RENDERFORGE_FAKE_INIT=2/3/4`) and forced (`nvngx_dlss.dll` removed); non-NVIDIA and GTX hardware is expected to work but has not yet been smoke-tested.
- **NIS sharpening with Mode Off** -- the sharpness slider is now enabled whenever the shim is up. Setting Mode Off with Sharpness above 0 (default 40) runs NIS on the vanilla frame while preserving the game's own anti-aliasing; before, Mode Off left the frame untouched.
- **D3D11 pinned-upscaler fix** -- a D3D12-only upscaler (FSR or XeSS) left selected while running D3D11 used to disable the entire post pass (LUT, scene styles, colour vision) until the upscaler was changed. Fixed by falling back to Auto on D3D11; the greyed row says which upscaler runs instead.

## Fixes

- Disabling the mod in the mod manager now restores the game's own shadow, anisotropic, LOD and vignette values; re-enabling re-applies your knobs without a level reload.
- `Vanilla (…)` labels refresh when you apply a different graphics preset with Options still open.

## Project

- License changed to **CC BY-NC 4.0** (same as the author's other Phoenix Point mods).
- GitHub bug-report template (English / Russian) that asks for the overlay screenshot and `Player.log`.

## Install

Download **`Renderforge-Full-1.4.1.zip`** from the GitHub release (single archive, all vendor runtimes included). Extract into `<Phoenix Point>\Mods\` so the `Renderforge` folder is at `Mods\Renderforge\`. SHA256 checksum in `SHA256SUMS.txt`.

## Known limits

- The colour vision correction applies to the scene only; the interface (HUD, menus, tooltips) is drawn after the post pass and is not corrected.
- Quality knob cost measured on an RTX 5070 Ti at 1440p (~240 FPS): costliest knob (Aniso 16x) adds ~8 pp GPU utilization; no knob measurably affects frame time on this GPU.
- Post-only mode has not been smoke-tested on non-NVIDIA (AMD, Intel) or GTX hardware.
- Translations other than English and Russian are machine-made and reviewed, not native-checked.
