# Renderforge 1.6.0

Four more image sliders, a one-click reset, a shorter resolution list, and geoscape markers that
stop trailing behind DLSS.

## Highlights

- **Exposure, Brightness, Saturation and Vibrance** -- four new live sliders under Options > Graphics, in the same pass as the LUT filters and the 1.5.0 image controls. Chain order is Exposure -> Black/White point -> Brightness -> Contrast -> Clarity -> Vibrance -> Saturation. Every image tooltip now ends with the setting's default value. Scene only, in tactical missions and on the Geoscape; the HUD is untouched.
- **Reset image settings** -- a new row under Options > Graphics that puts every Renderforge image and quality knob back to its default: Sharpness, LUT filter and strength, the eight image sliders, scene style with its strength and pixel size, Vignette, Shadow resolution, Anisotropic filtering and LOD detail. The tooltip lists them. Renderer, Upscaler, Quality, Frame generation and Colour vision stay as you chose them.
- **One entry per resolution** -- Options > Screen no longer repeats the same screen size once per refresh rate; each size appears once, with its highest rate. Refresh rate was never applied by the game's resolution switch anyway; the frame-rate limiter owns pacing.
- **Geoscape markers after DLSS** -- site markers are drawn by the present camera at output resolution instead of going through the upscaler, so rotating the globe no longer leaves ghost trails behind them.

## Known limits

- The marker overlay is armed only on **DirectX 11**, with **frame generation off** and **Colour vision = None**. Any other combination keeps the vanilla path, trails included.
- The markers now bypass the game's post-processing: their whites reach full 255 instead of the tonemapped 246, the unknown-site disk reads as translucent grey rather than crushed black, and a parked aircraft sits under the soldier row instead of over it. Site clicking, picking and the far-side hiding are unchanged.

## Install

Download **`Renderforge-Full-1.6.0.zip`** from the GitHub release (single archive, all vendor runtimes included) or subscribe on the Steam Workshop. Extract into `<Phoenix Point>\Mods\` so the `Renderforge` folder is at `Mods\Renderforge\`. SHA256 checksum in `SHA256SUMS.txt`.
