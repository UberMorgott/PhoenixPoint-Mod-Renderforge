# Crisp UI — live measurement (2026-09-05)

Closes the open item from `docs/superpowers/2026-09-03-handoff-next-session.md:83-84`
("on-screen px of icons vs their source texels — check first"). Measured on Instance2, tactical
HUD, 2560x1440, `CanvasScaler` ScaleWithScreenSize ref 3840x2160 → `scaleFactor 0.667`.

## Verdict

- UI sprites are **minified**, not magnified, at 1440p (and 1:1 at 4K). A "Crisp fonts"-style
  2x re-raster cannot add information: the source already has 3–16x more texels than pixels.
- Renderforge already applies `mipMapBias = -0.585` to every mipmapped UI texture (`MipBias.cs`
  sweep). Bias 0 / -0.585 / -1.0 and Trilinear+-0.585 produce **pixel-identical** screenshots.
- No "Crisp UI" feature is warranted. Remaining levers (UI-only CAS pass on a canvas RT, offline
  texture pack) would be sharpening/art, not de-blurring — not pursued.

## Texture inventory (active `Image`s)

| Texture | Size | Mips | Filter | Format |
|---|---|---|---|---|
| `sactx-4096x4096-UIAtlas_UI` (7+ pages) | 4096² | 13 | Bilinear | RGBA32 |
| portrait render textures ×4 | 1024x819 | 11 | Trilinear | RGBA32 |
| `SingleClass` render tex | 2250x1080 | 12 | Bilinear | RGB24 |
| `LoadingScreenVignette` | 1920x1080 | 1 | Trilinear | BC7 |
| standalone icons (`UI_Misc_ExclamationMark`, gradients, plates…) | 50–256 px | 1 | Trilinear | RGBA32 |

## Screen px vs sprite texels (rect × scaleFactor vs `sprite.rect`)

| Image | Type | Screen px | Texels | Ratio |
|---|---|---|---|---|
| `UIElement_Frame` / `_Background` | Sliced | 73x73 | 64x64 | 1.15 |
| `Image_Icon` (MedicalBay) | Simple | 40x40 | 256x256 | 0.16 |
| `SingleClass` | Simple | 139x67 | 2250x1080 | 0.06 |
| `frame` (9-slice bar) | Sliced | 320x67 | 64x64 | 5.0 (flat border, by design) |
| `vignette` | Simple | 2560x1440 | 1920x1080 | 1.33 |
| `MainClass` (Assault) | Simple | 41x44 | 200x200 | 0.21 |
| `Icon` (PhoenixPoint) | Simple | 44x48 | 128x128 | 0.34 |

`mipMapBias` setter on non-readable `Texture2D` works (sampler state), no exceptions.
Unity player is **2019.4.31f1** (`UnityPlayer.dll` FileVersion) — the handoff's "2018.4.15f1" was wrong.
