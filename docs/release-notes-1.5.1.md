# Renderforge 1.5.1

A small patch on top of 1.5.0: one optional UI sharpness toggle.

## Highlights

- **Pixel-perfect UI** -- a new toggle under Options > Screen, **off by default**. It sets `Canvas.pixelPerfect` on the game's root screen-space overlay canvases, which snaps interface elements to the pixel grid. Measured at 2560x1440: UI text edge contrast +2-7% (digits 1.05-1.06, the geoscape clock 1.07). Icons are unchanged -- the same measurement puts them at 0.98-1.03, i.e. inside frame noise. Applies live, no restart; camera and world-space canvases are never touched, and turning it off restores every canvas to its original value.

## Known limits

- Snapping means moving: elements shift onto the pixel grid (by a fraction of a pixel, up to 0.5 px at the 1440p UI scale), so **animated panels may step by whole pixels** while they slide. That is why the toggle is off by default -- turn it on if you prefer the sharper text.
- Crisp fonts stay removed. A restore was tried this release and reverted after re-measuring: the exact glyph remap samples the same pixels as vanilla at 1440p (Sobel 1.00). Numbers: `docs\research\font-remeasure-2026-09-07\results.md`.

## Install

Download **`Renderforge-Full-1.5.1.zip`** from the GitHub release (single archive, all vendor runtimes included) or subscribe on the Steam Workshop. Extract into `<Phoenix Point>\Mods\` so the `Renderforge` folder is at `Mods\Renderforge\`. SHA256 checksum in `SHA256SUMS.txt`.
