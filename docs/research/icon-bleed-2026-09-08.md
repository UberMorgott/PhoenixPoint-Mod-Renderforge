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

## Round 3 (2026-09-08): vanished vehicle icon, TFTV's real PNGs, vanilla sweep

- **Regression + fix.** With `Canvas.pixelPerfect` on, `Image.OnPopulateMesh` → `Graphic.GetPixelAdjustedRect()` →
  `RectTransformUtility.PixelAdjustRect`, which returns `(0,0,0,0)` for any element whose world matrix is singular. The
  game's `ActorClassIcon` prefab carries `localScale.z = 0` (so does every child: `LeftClass/Mask/Icon`), as do
  `UIPCHotkey` backgrounds, the weapon `WeaponEnabledIcon` and `ActionPointsBarFullPips/*/Full`. Diag sweep of every
  enabled sprite `Image` OFF vs ON: **12 tactical + 6 Geoscape** images drew a zero rect ON (all with `lossyScale.z == 0`),
  spread over nested `SquadManagementModule` / `AbilitiesAndSpottedEnemies` AND the root `TacticalUICanvas` /
  `GeoscapeUICanvas` - excluding a canvas was not an option. Fix = `Graphic_GetPixelAdjustedRect_Patch`
  (`src\PixelPerfectUi.cs`): a postfix that returns `rectTransform.rect` whenever the snapped rect has width or height
  ≤ 0. Verified on the rebuilt DLL: `Graphic.GetPixelAdjustedRect` ≤ 0 on **0 of 442 / 469 / 160** images (tactical /
  Options→Screen / Geoscape); `UI_Vehicle_ClassIcon_Armadillo` rect luminance OFF 88.4 → ON 88.4, all 45 z=0 rects
  within 5/255 of OFF. Mip pin innocent (unchanged).
- **TFTV's icons through `Helper.CreateSpriteFromImageFile`** (`new Texture2D(128,128,RGBA32,true)` + `LoadImage` +
  3-arg `Sprite.Create`, Repeat wrap, 7-10 mips, pin writes −0.585): 36 px list on a root overlay canvas at .00/.33/.66
  screen px, outermost row/col mean luminance minus panel background, worst edge per icon (OFF .00/.33/.66 → ON, ON is
  position-independent):
  `ODI_Skull` 12/32/49 → 8 · `Stat_Accuracy` 9/22/22 → 8 · `Drill_drawfire` 10/16/18 → 9 · `Drill_bullethell`
  21/29/19 → 22 · `TFTVBasicClinicSmallIcon` 7/4/11 → 8 · `KG_Pistol_Ammo` 0/0/0 → 0 · `Drill_override` 50/52/38 → 51
  (the bolt glyph touches the right edge) · `FactionIcons_NewJericho` 126/132/131 → 105 (the icon's own white frame).
  The fractional-position surplus (+37 skull, +13 accuracy, +8 drawfire) is gone ON; what remains is the glyph itself.
- **Vanilla sweep.** 237 vanilla sprite Images 12-96 px across the three screens, "line" = outermost row/col mean >
  interior mean + 40 while the row just inside is not: OFF **0**, ON **2** (one 74 px Geoscape `UI_MainButton_Frame` +
  its background whose frame bottom snapped into the rect - a button, not an icon). Top-5 crops OFF | ON identical
  apart from the ≤ 1 px snap. Verdict: vanilla icons do not bleed OFF, and ON does not change them.
- Diag used: `src\Diag\UiDiag.cs` (deleted after; Dump/Sweep/TftvSetup/Options via PPCLI `call`), scripts
  `cmp_sweep.py` / `cmp_fix.py` / `measure_tftv.py` / `measure_vanilla.py` in the session scratchpad `round3\`.
