# Code-only scene stylization

- Default: `SceneStyle=Off`, `SceneStyleStrength=100`, `PixelSize=6`.
- Graphics options: Scene style (Off / Cartoon / Pixel art), Style strength (0–100), Pixel block size (2–16 output pixels).
- `Renderforge.RenderforgeMod.SetSceneStyle("Cartoon", 100, 6)` is the PPCLI control; `Off` restores the original post path.
- Cartoon uses five luminance bands, a small texture-smoothing cross and dark contrast outlines. These are color edges, not geometry/depth/normal edges. It is a cartoon post filter, not a replacement cel material.
- Pixel art samples the center of each output-sized block and quantizes each display RGB channel to eight levels. Its screen grid is stable, but moving objects cross the grid; it is not an authored pixel-art animation system.
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
- Native DLL: **237,568 bytes**, versus **235,008 bytes** at the preceding LUT commit (**+2,560 bytes**). Managed DLL: **146,432 bytes**. No model/texture payload is added to the mod; the 1.18 MB comparison PNG lives only in docs.
- Ready artifacts remain under `D:\Renderforge-work\scene-style\`. Managed SHA256: `80A7B307C428349AB05A0FE88CA62111A5C37CFD683CC87299E44FB940168D43`; native SHA256: `981AB304EF60A51D7413A60B36D7386BC296D028E401F4DFC1CE994D6EF76664`.
- The running game was not controlled, restarted or deployed to. Live temporal stability, FG interaction, UI layout and performance remain unverified.

## Runtime verification required

1. Deploy matching managed and native artifacts together after the game is closed; record hashes. Do not replace the loaded DLLs.
2. Start the game with style Off. Record `GetStatus`, NR status, actual FG foreground/present/generated counters, resolution and all style/LUT/upscaler settings.
3. On the same static tactical camera, capture Off / Cartoon 100 / PixelArt 100 with LUT Off. Capture output texture and UI framebuffer separately. Confirm HUD text, menus and cursor remain legible/unfiltered where drawn as overlays.
4. With FG On and a live foreground game, sweep strength 100 times and block size through 2–16. Expect no temporal feature/child-window recreation; record original and final handles and generated-frame counter progress.
5. Move/rotate the camera in both modes, including fine fences, transparent foliage, dark interiors and bright skies. Check edge shimmer, aliasing, frame time and FG artifacts. No motion/quality claim is made by the isolated shader probe.
6. Test upscaler Off → Cartoon → PixelArt → Off, and each supported upscaler/backend. With upscaling Off, the first/last effect changes may create/release the passthrough pipeline; intermediate strength changes must not.
7. Restore the exact original settings and capture Off again. Record any limitations before declaring the feature visually accepted.
