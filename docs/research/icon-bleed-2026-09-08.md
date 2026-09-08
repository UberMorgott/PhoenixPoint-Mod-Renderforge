# 1 px edge bleed on runtime-created UI icons (2026-09-08)

Why this exists: icons that third-party mods build at runtime (`new Texture2D` + `Sprite.Create`) showed a 1 px bright
line along one or two edges under Renderforge, but only in some DLSS modes. Root cause, the two mechanisms, the fix
(Pixel-perfect UI now pins UI textures to mip 0, default On since 1.6.1) and the numbers, so nobody re-measures it.
Rig: Instance2, 2560x1440 borderless, RTX 5070 Ti; root game canvases reference 3840x2160 → `canvas.scaleFactor` 0.6667.

## Verdict

- Two mechanisms, both needed:
  1. **Mip selection.** The UI is minified 0.667x, so bilinear sampling (`MIN_MAG_LINEAR_MIP_POINT`) picks **mip 1**.
     Mip 1 of an unpadded atlas already averages the neighbour cell into the edge texel; the kernel reaches 0.125 texel
     past the sprite rect → line intensity **138/255** (magenta over grey). `mipMapBias = log2(0.667) = −0.585` pins
     mip 0.
  2. **Fractional quad edge.** Even at mip 0 a quad whose screen edge lands at .33/.66 px lets the bilinear kernel reach
     the neighbour texel (**118**). `Canvas.pixelPerfect` snaps the quad to whole pixels → 0.
- Only the combination zeroes it at every fractional position: **0/0/0** for the atlas sub-rect AND the Repeat-wrapped
  full-rect texture.
- Vanilla atlases (`sactx-4096x4096-Uncompressed-UIAtlas_UI`, 13 mips, Clamp, `SpriteAtlas` padding) never bled
  (control 104 flat in every mode); the pin merely sharpens them (they were sampled from mip 1 too).
- **DLSS-Quality coincidence:** `src\MipBias.cs` writes `log2(renderW/outW)` to every mipmapped texture while a
  reduced-res generation is live — at Quality on 1440p that is exactly −0.585 = log2(0.667), so the bug was invisible
  there and appeared at DLAA / Off / passthrough where the bias is 0.

## Measurements (2560x1440, DLSS Off, magenta line intensity 0..255, worst edge)

Columns = the three fractional screen positions of the 64² icon (.00 / .33 / .66 px). `atlas` = 128² four-cell
atlas without gutter, icon in cell (0,0), neighbours solid magenta; `fullrect` = 64² texture, Repeat wrap, magenta
bottom row + left column (bleeds onto the top/right edge); `vanilla` = `XboxOne_LT_uinomipmaps` from the main atlas,
edge luminance instead (no magenta available).

| mode | atlas .00/.33/.66 | fullrect .00/.33/.66 | vanilla |
|---|---|---|---|
| base (bias 0, Repeat, bilinear, mips) | 138 / 136 / 0 | 89 / 25 / 0 | 104 flat |
| clamp | 138 / 61 / 0 | 0 / 0 / 0 | 104 |
| mip0 (bias −0.585) | 118 / 115 / 0 | 118 / 0 / 0 | 104 |
| pp (`Canvas.pixelPerfect`) | 61 / 61 / 61 | 25 / 25 / 25 | 104 |
| **mip0 + pp** | **0 / 0 / 0** | **0 / 0 / 0** | 104 |
| clamp + mip0 + pp | 0 / 0 / 0 | 0 / 0 / 0 | 104 |
| nomip | 118 / 115 / 0 | 118 / 0 / 0 | 104 |
| point | 0 / 0 / 0 | 0 / 0 / 0 | 104 |

Production verification of the shipped fix (same rig, DLSS Off so the DLSS bias is 0, `IconBleed` mode `prod` =
the diag leaves the production pin alone):

| state | atlas | fullrect | vanilla | log / bias |
|---|---|---|---|---|
| Pixel-perfect UI On (default) | 0 / 0 / 0 | 0 / 0 / 0 | 104 | `MipBias: bias=0.000 applied to 1036 textures (ui=-0.585 on 307 sprite textures, skipped 777)`; a sprite created AFTER the sweep through the 3-argument `Sprite.Create` overload: `bias=-0.585` (Harmony postfix) |
| Pixel-perfect UI Off (`cfg.PixelPerfectUi=false` + `OnConfigChanged`) | 138 / 136 / 0 | 89 / 25 / 0 | 104 | `ui=0.000`, `Pixel-perfect UI off: 15 canvases restored`; late sprite `bias=0.000` |

Point filtering also zeroes it but is not a fix (every icon becomes blocky); Clamp alone fixes only the full-rect case
and is the mod author's job, not ours.

## The fix (1.6.1)

- `src\MipBias.cs`: `UiPin` (set by `PixelPerfectUi.Apply`), `CurrentUiBias`, `Resweep()`. In `Sweep`, textures behind
  any `Sprite` get `min(dlssBias, log2(uiScale))`, `uiScale` = smallest `scaleFactor` over root ScreenSpaceOverlay
  canvases (the 4K-reference game canvases win over a mod's 1080p-reference canvas at 1.33; clamped ≤ 0). `Reapply`
  sweeps at level start when the pin is on even at bias 0.
- `src\Patches.cs` `Sprite_Create_Patch`: postfix on the 8-argument `Sprite.Create` (every shorter overload chains into
  it, UnityEngine.CoreModule 2019.4) → `texture.mipMapBias = min(current, CurrentUiBias)` when the pin is on.
- `src\PixelPerfectUi.cs`: `Apply(on)` toggles the pin + resweeps, then the canvases as before.
- `src\DlssConfig.cs`: `PixelPerfectUi` default `true`; descriptions (EN attribute, RU table, `strings.csv`, `VideoPanel`
  tooltip) say it also keeps UI textures at full resolution. `ModConfig.LoadFromRawConfig` writes every field present in
  the profile's `ModConfig.json`, so a user's saved `false` stays `false` (verified live: Instance2's saved `false`
  loaded as `false` against the new default).

## Reproduction recipe

- Diag class: `icon-bleed-2026-09-08\IconBleed.cs.txt` → copy to `src\Diag\IconBleed.cs` (deleted from the tree after
  the fix; public static, not wired to any config). Own root overlay canvas cloned from the game's CanvasScaler, three
  sprite kinds x three fractional positions over a dark background, magenta neighbours.
- Deploy to Instance2, arm PPCLI, launch with `-mods`, wait for `connect state`; DLSS Off so the DLSS bias is 0:
  `call {"op":"set","target":"<cfg handle>","member":"Mode","value":"Off"}` + `OnConfigChanged` on the mod instance.
- `call {"op":"invoke","type":"Renderforge.IconBleed","assembly":"Renderforge","member":"Setup","args":[]}`, then
  `Mode("prod"|"base"|"clamp"|"mip0"|"pp"|"mip0+pp"|"clamp+mip0+pp"|"nomip"|"point")`, `Rects()`, `LateSprite()`
  (bias of a sprite created through the short overload), `Teardown()`.
- Measurement: `icon-bleed-2026-09-08\run.ps1 -Tag <tag> -Modes prod,base,...` (edit `$dir` to your output folder;
  needs python + Pillow) drives Mode → `connect screenshot` → `Rects` → `measure.py <png> "<rects>"`: per edge the max
  over one line outside + two inside of `max(0, min(R,B) − G)`; the table prints the worst edge per position.
