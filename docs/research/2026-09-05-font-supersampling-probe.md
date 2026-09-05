# Dynamic font supersampling: live diagnostic, not a product fix

At 2560x1440 a temporary, two-label `UnityEngine.UI.Text.pixelsPerUnit` multiplier made the numeric/Cyrillic storage label visibly cleaner. It also changed glyph positions and preferred layout dimensions. The experiment does not justify enabling this globally. The original density was restored and both diagnostic Harmony patches removed; no production font patch or replacement asset was deployed.

## Source and asset grounding

- The requested earlier analysis is `docs/superpowers/2026-09-03-handoff-next-session.md:60–83`. Its first static-atlas hypothesis was superseded by historical observations of dynamic fonts and a screen-space overlay. Its 4096x4096 image atlas is not evidence of a font atlas of that size.
- Installed `UnityEngine.UI.dll` SHA256: `659F3B8C939060464C83A33E21D7084A3FD4F755A09C641C577592A35CB5AABA`. The live runtime is Unity 2019.4.31f1. Source dumps are in `D:\RenderforgeWork\fonts-diagnosis`.
- Actual `Text.cs:276–294` returns Canvas scale for dynamic `pixelsPerUnit`; `GetGenerationSettings:376–398` passes it as `scaleFactor`; `OnPopulateMesh:419–472` divides generated vertices by it and then applies pixel adjustment. Preferred width/height also divide by `pixelsPerUnit` (`:303–317`). This is the complete candidate seam, rather than changing Canvas scale or public font size.
- `TextGenerator.Invalidate` (`TextGenerator.cs:165–168`) clears the generation cache. `TextGenerationSettings.Equals:51–53` includes `scaleFactor`, but changing a diagnostic multiplier does not emit a dirty event. The probe invalidates both text generators and calls `SetAllDirty` once per transition. Existing `Font.textureRebuilt` callbacks are preserved; a shared font can affect labels outside the whitelist when its atlas rebuilds.
- `sharedassets0.assets` already contains vector outlines: Purista Semibold pathId4005 is a 104196-byte OTF/CFF; Purista Bold pathId4004 is 107696 bytes; SourceHanSans-Medium pathId4009 is 16558996 bytes. All 16 inspected Font assets contained outlines. No font bytes were exported, downloaded or redistributed. Buying a higher-resolution copy does not address a fixed bitmap ceiling, since there is no such demonstrated ceiling here.
- `CanvasScaler.dynamicPixelsPerUnit` affects the world-space path, not the observed screen-space ScaleWithScreenSize path. Existing TMP binaries/localization adapters do not make a legacy Text-to-TMP conversion a drop-in change.

## Current live experiment

- Exclusive PPCLI handoff, PID3368, geoscape `Playing`, 2560x1440, the same Sophia equipment screen throughout. No scene, time scale, renderer, NR/FG, font size, Canvas scale, RectTransform or OS display changes.
- Actual components were `UnityEngine.UI.Text`, dynamic fonts, `ScreenSpaceOverlay`, Canvas scale `0.6666667`.
- Exact instance1242382: `GeoscapeUICanvas/SoldierEquipModule/EquipPanel/StoresPanel/StorageBG/StorageLimitText`, text `СКЛАД 56/200`, assigned font `SourceHanSans-Medium`, size32, best-fit disabled.
- Exact instance1090434: `GeoscapeUICanvas/ProgressionScreenModule/Canvas/MenuElements/Statuses/UIPanel_Line/StatBar_Stamina/Bar_Holder (2)/GenericBar/UITextGeneric_Small`, text `ВЫНОСЛИВОСТЬ`, assigned font `Purista Semibold`, size36, best-fit enabled. Assigned Font identity is observed; the native per-glyph fallback face was not independently identified.
- A1 density1 → B density2 → A2 density1, 2.5 seconds of settling each. A postfix multiplied only the two whitelisted instances. A read-only `OnPopulateMesh(VertexHelper)` postfix captured the actual emitted screen-space quads. A 180-second watchdog and script `finally` restore/unpatch prevented a persistent experiment.
- Text strings, public font settings, Canvas scale, screen resolution and world corners stayed exactly equal, including immediately before and after each capture. Both labels remained single-line with line start `[0]`, 12 visible characters, and respectively 11/12 emitted quads. This did not test wrapped paragraphs.

| Metric | Storage / SourceHanSans | Stamina / Purista |
|---|---:|---:|
| pixelsPerUnit A → B | 0.6666667 → 1.33333337 | 0.6666667 → 1.33333337 |
| fontSize × pixelsPerUnit A → B | 21.333334 → 42.6666679 | 24 → 48 |
| Native generated font-size result A → B | 21 → 42 | 24 → 48 |
| Preferred width change B−A, layout units | −4.5 | +2.25 |
| Preferred height change B−A, layout units | 0 | −2.25 |
| Maximum corresponding screen vertex change | 3.5 px | 2 px |
| A2 preferred dimensions/vertices vs A1 | exact return | exact return |

Storage text in B appears noticeably sharper and thinner; Purista changes more subtly. This is a qualitative image observation, not a blinded user preference score. The shifted advances/edges fail a strict layout-preservation acceptance condition. Text/click RectTransform bounds stayed fixed, but matching those rectangles alone would hide the glyph movement.

Both assigned fonts exposed unchanged 256x512 Alpha8, one-mip, bilinear atlases. No selected-font `textureRebuilt` events occurred. `Profiler.GetRuntimeMemorySizeLong(texture)` returned zero: treat this as unavailable runtime memory telemetry, not zero allocation or measured VRAM. The raw Alpha8 base image represents 131072 bytes per atlas, excluding backend copies, allocation overhead and fallback atlases. Doubling raster dimensions can require roughly four times the glyph bitmap area before packing; this run demonstrated neither an atlas allocation increase nor general memory bounds. Removing the override does not force Unity's shared glyph cache to shrink.

Unity frame-interval samples A1/B/A2: 172/171/170; median 15.3843/15.3810/15.3366 ms; p95 16.0897/16.1500/16.1065 ms. These are `Time.unscaledDeltaTime` observations with diagnostic overhead, not GPU times or a performance benchmark. No error/exception/assert was captured. Storage mesh rebuilds occurred approximately every frame already in A1 and continued in B/A2; Purista required one rebuild per transition. No recurring atlas rebuild regression was observed during this short run.

## Evidence and reproduction

- Isolated project/source: `D:\RenderforgeWork\fonts-probe\FontsProbe.csproj`, `FontsProbe.cs`. Assembly `RenderforgeFontsProbe1.dll` was loaded from D: in memory, never copied to Mods. The assembly cannot be unloaded independently from this Mono application domain; after cleanup it has no active hooks or GameObject.
- Build: `dotnet build D:\RenderforgeWork\fonts-probe\FontsProbe.csproj -c Release -v:q --ignore-failed-sources` → 0 warnings, 0 errors. TEMP/TMP were on D:. No new dependencies or fonts.
- Runner: `D:\RenderforgeWork\fonts-probe\run-probe.ps1 -Mode Census -RunName census1`; then, only with fresh observed IDs and exclusive live ownership, `-Mode Compare -TextIds @(1242382,1090434) -RunName run1`. IDs are session-specific, not reusable constants. Script parsing passed.
- Evidence directory: `D:\RenderforgeWork\fonts-probe\run1`. `A1/B2x/A2-before.json` and `-after.json` contain original settings, line starts, generated size, mesh positions/UVs, atlas metadata, errors and frame intervals. `comparison.json` records deltas.
- `A1.png`, `B2x.png`, `A2.png` are native PPCLI end-of-frame PNGs at 2560x1440. `compare-and-crop.ps1` copies the same integer glyph-union rectangle from each image without resampling; `*-label-1242382.png` and `*-label-1090434.png` are the readable native crops. Animated scene/background pixels differ between A1 and A2, so a whole-crop pixel hash is not a restoration proof. Restored text-generation metrics and actual screen vertices match exactly.
- `restore.json`: `active:false`, `density:1`, `getterPatchOwned:false`, `meshPatchOwned:false`. A subsequent PPCLI `GameObject.Find("Renderforge temporary font probe")` returned null and `Time.timeScale` returned1.0. Restored census retained the original density. The game remained `geoscape/Playing`. No restart or shutdown.

## Next decision

Keep supersampling out of the default renderer. First obtain a human comparison of the original/native B crops. A further bounded candidate must address the measured hinting/advance changes and test wrapped text, best-fit limits, rich text and fallback before claiming layout safety. Merely retaining the original preferredWidth/Height getter would conceal mesh drift rather than fix it. Pixel alignment remains a competing explanation for some labels. 4K and a global TMP/SDF/font-replacement migration were deliberately not attempted; they are not proven by this result.
