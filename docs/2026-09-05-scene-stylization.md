# Code-only scene stylization

- Default: `SceneStyle=Off`, `SceneStyleStrength=100`, `PixelSize=4`.
- Graphics options: Scene style (Off / Cartoon / Pixel art), Style strength (0–100), Pixel block size (2–16 output pixels).
- `Renderforge.RenderforgeMod.SetSceneStyle("PixelArt", 100, 4)` is the PPCLI control; `Off` restores the original post path.
- Cartoon uses five luminance bands, a small texture-smoothing cross and dark contrast outlines. These are color edges, not geometry/depth/normal edges. It is a cartoon post filter, not a replacement cel material.
- Pixel art samples the center of each output-sized block and quantizes each display RGB channel to 32 levels. The default is four actual output pixels at both 1280×720 and 2560×1440, with no automatic resolution multiplier. Blocks remain adjustable from 2 to 16. No extra outlines are applied. Its screen grid is stable, but moving objects cross the grid; it is not an authored pixel-art animation system.
- Both share the existing post-reconstruction compute pass with color grading. Pixel art replaces the sharpening contribution inside the styled portion; the strength blend still uses the normal sharpened input. LUT grading follows the style.
- The existing scene output is processed before normal overlay UI. World-space UI or UI already baked into the camera image receives the filter. Verify the real game's UI with the runtime plan below.
- No generated faces, texture files, model weights, new DLL dependencies or downloads. Geometry, silhouette, animation and face identity stay unchanged. True low-poly geometry and depth/normal cel outlines are outside this change.
- Strength and block-size changes are frame-slot constants. Switching between Cartoon and PixelArt uses the same shader. The original NIS/RCAS path remains when both style and LUT are off. With upscaling Off, enabling the first scene effect starts the existing full-resolution passthrough pipeline; disabling its last effect releases that pipeline.
- The existing FP16 output path uses a gamma-2.2 approximation while stylizing, then converts back to linear. LUT behavior is unchanged. This is not a calibrated HDR display transform.

## Verified in isolation

- `dotnet build Renderforge.csproj -c Release --no-restore /p:PPRoot="D:\Steam\steamapps\common\Phoenix Point" /p:OutputPath="D:\Renderforge-work\scene-style\managed\" /p:IntermediateOutputPath="D:\Renderforge-work\scene-style\obj\"`: **0 warnings, 0 errors**.
- `cmake -S native -B D:\Renderforge-work\scene-style\native -G "Visual Studio 17 2022" -A x64`, then `cmake --build D:\Renderforge-work\scene-style\native --config Release --target RenderforgeNative scene_style_probe lut_probe`: **all targets built**, including D3D11, D3D12, FSR and XeSS integration sources.
- `D:\Renderforge-work\scene-style\native\Release\scene_style_probe.exe`: **PASS** production HLSL on D3D11 WARP; both styles in gamma/linear domains; exact Off/zero pixels, preserved alpha, finite output, strength interpolation, 1×1 and 37×23 boundaries, all 15 pixel block sizes/palette constraints, unchanged original Off constant packing.
- The probe caught full-strength `lerp` cancellation producing tiny differences inside a pixel block. Full strength now returns the styled value directly; block uniformity passes exactly even with sharpening enabled.
- `D:\Renderforge-work\scene-style\native\Release\lut_probe.exe`: **PASS** all nine production LUTs: 4,913-color cube, 1,025-step ramps, alpha, blend endpoints, B&W channel equality, floating-point overbrights.
- [Same-input contact sheet](scene-stylization/contact-sheet.png) runs the actual production shader against a saved screenshot using WARP. It is not a new in-game capture. [Manifest](scene-stylization/contact-sheet.json) records input/probe hashes, crop and limitations; [script](scene-stylization/make-preview.py) reproduces it. Screenshot-baked markers are filtered in this fixture; UI isolation requires the runtime check below.
- Native DLL: **237,568 bytes**, versus **235,008 bytes** at the preceding LUT commit (**+2,560 bytes**). Managed DLL: **146,944 bytes**. No model/texture payload is added to the mod; all comparison PNGs live only in docs.
- Ready artifacts remain under `D:\Renderforge-work\scene-style\`. Managed SHA256: `1EADEC0BF455B7CBC1D6FBD14D5484F0996929148F5AC1259D68C4A11328E06D`; native SHA256: `AA8C823A80A11B7178D1F7D7E03911E316CE670B0DBCF863FDC90F6086A920EE`.
- The running game was not controlled, restarted or deployed to. Live temporal stability, FG interaction, UI layout and performance remain unverified.
- Integration audit also fixed the managed frame submission's old LUT upper bound (`4` → `LutPreset.VintageSepia`), so all nine registered LUTs reach native unchanged. The final managed hash above includes that fix; the Release rebuild passed with 0 warnings/errors.

## Historical two-pixel readability refinement

- The first six-pixel/eight-level example was too coarse for ordinary play. The new two-pixel/32-level default favors the small soldier silhouette, cover rims and ground detail; three and four pixels remain optional stronger looks. Explicitly saved block-size preferences are not overwritten.
- [720p normal-framing comparison](scene-stylization/pixel-readability-720p.png) shows the verified 1280×720 fixture pixel-for-pixel: original, default two pixels, optional three and four pixels. The small soldier and cover edges remain more recognizable at two than at three/four in this fixture. This is visual inspection of one static image, not a general gameplay-readability guarantee.
- [1440p display-scale comparison](scene-stylization/pixel-readability-1440p.png) runs the same shader at 2560×1440 on a bilinearly enlarged 720p source, then fits panels to 50%. It verifies block scale and does not pretend to contain native 1440p game detail. At 1440p the fixed two-pixel default is gentler relative to the same camera framing; no hidden scaling inflates it to six or more pixels.
- [Reproduction script](scene-stylization/pixel-readability.py) and [manifest](scene-stylization/pixel-readability.json) record the exact source/probe hashes and simulation limits. All fixture UI is baked into the screenshot; it cannot prove live HUD exclusion.
- Rebuilt native/managed Release and reran both probes: **PASS**. Pixel tests cover all 15 block sizes, exact block uniformity, 32-level palette membership and default quantization error ≤`1/62` per display channel relative to the selected source sample (not an error bound for spatial downsampling).
- Cartoon's full 1280×720 floating-point output is byte-identical before/after this PixelArt change: SHA256 `A1BD02EE069184D75F3F0254776F998CA7B45E37C5C9D1CC9DC1A1397E445383`. All nine LUT checks and exact Off/zero checks still pass. Live movement, UI and FG checks remain pending.

## Four-pixel default after user feedback

- The user found the two-pixel effect too small in the actual game. The shipped configuration and English/Russian hints now default to `PixelSize=4`; the manual range remains2–16 and the shader still uses32 levels per display RGB channel. The earlier two-pixel fixture comparisons above are historical evidence, not proof that the user prefers that size.
- Existing saved configurations are not automatically migrated. At the next coordinated deployment, explicitly change this user's saved `PixelSize=2` to `4` while preserving the selected style, strength and all other preferences. Do not enable a style as part of that adjustment. This source change did not access the game or modify its configuration.

## Runtime verification required

1. Deploy matching managed and native artifacts together after the game is closed; record hashes. Do not replace the loaded DLLs.
2. Start the game with style Off. Record `GetStatus`, NR status, actual FG foreground/present/generated counters, resolution and all style/LUT/upscaler settings.
3. On the same static tactical camera, capture Off / Cartoon 100 / PixelArt 100 with LUT Off. Capture output texture and UI framebuffer separately. Confirm HUD text, menus and cursor remain legible/unfiltered where drawn as overlays.
4. With FG On and a live foreground game, sweep strength 100 times and block size through 2–16. Expect no temporal feature/child-window recreation; record original and final handles and generated-frame counter progress.
5. Move/rotate the camera in both modes, including fine fences, transparent foliage, dark interiors and bright skies. Check edge shimmer, aliasing, frame time and FG artifacts. No motion/quality claim is made by the isolated shader probe.
6. Test upscaler Off → Cartoon → PixelArt → Off, and each supported upscaler/backend. With upscaling Off, the first/last effect changes may create/release the passthrough pipeline; intermediate strength changes must not.
7. Restore the exact original settings and capture Off again. Record any limitations before declaring the feature visually accepted.
