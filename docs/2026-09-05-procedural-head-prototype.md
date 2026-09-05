# Numerical head experiment, 2026-09-05

- **Result: rejected as a human-face improvement.** One deterministic micro-normal variant produced a barely visible surface change on Sophia and Yael. It did not reconstruct eyelids, eyes, lips or natural anatomy. No automatic enhancement or production setting was added.
- Current user direction: compact code and numerical profiles; no generated per-face image packs, generator backend or model downloads. Earlier multiview/toolchain notes are historical and superseded. Computer shutdown was explicitly cancelled.
- Useful new evidence: both heads expose 21 facial/skinning bones and readable bindposes/weights, despite `Mesh.isReadable=false`. Both have **zero blendshapes**, so there are no existing face-morph sliders to turn. A later anatomical experiment can investigate those bones; this experiment never modifies them.

## What was measured

| Live head | Mesh vertices | Blendshapes | Bindposes / weights | Current gloss scale |
|---|---:|---:|---:|---:|
| Sophia Brown, fixed unique | 2960 | 0 | 21 / 2960 | 1.0 |
| Yael Gonzales, ordinary | 2958 | 0 | 21 / 2958 | 0.5 |

- Bone names include `Eye_LowEyelid_R/L`, `Eye_UpEyelid_R/L`, `LowerLip_M/R/L`, `UpperLip_M`, `Nose`, `Eyebrow_R/L`, `Frown`, and mapped head/jaw/chest/neck/shoulder/arm bones. Full names, sample frame numbers and counts are in [evidence](procedural-head-prototype/evidence.json).
- Paired live snapshots at `timeScale=1` found no local position, rotation or scale changes in these 21 bones. This does **not** prove that animation never writes them, or establish a safe override phase. Full local paths, transforms and column-major bind matrices remain local at `D:\RenderforgeWork\procedural-head\sofia-a\morphs.json` and `yael-a\morphs.json`; raw bind matrices are not packaged in Git.
- Reading `boneWeights` and `bindposes` succeeded on the installed game. Readability of vertex arrays/rest geometry is a separate question; the earlier baked posed mesh is still not a safe rigged replacement.

## Normal control and single candidate

- Original `_BumpMap` is `RG_BC5_UNorm`, 2048 square for Sophia and 512 for Yael. A linear GPU blit/readback sampled RG with B=0 and A=255. An unchanged RGBA32 copy with regenerated mipmaps was bound through an owned material clone, retaining the original shader, texture transform, sampling settings and every other material property.
- Original versus unchanged-copy face RGB MAE on the source render, in 0–255 units, was Sophia `1.865 / 1.158 / 0.572`, Yael `1.061 / 0.574 / 0.278` for camera-relative yaw `0 / 45 / 90`. A repeated unchanged Yael original itself differed by `0.932`, showing frame/render variance. The control showed no gross channel/gamma corruption. This is not a bit-identical shader-decoder proof; source mipmaps were regenerated. The retained [control contact sheet](procedural-head-prototype/normal-control-comparison.jpg) records the initial comparison; Yael raw original capture paths were later reused for the matched candidate run, so use the retained sheet/metrics for that initial control.
- Each comparison used NR Off, DLAA, the same camera-relative yaw and frozen simulation pose, with 32 warmup frames. These yaw labels are relative to the existing roster camera, not guaranteed anatomical front/profile views. The contact sheets show the captured directions.
- One fixed-seed UV candidate added two continuous sinusoidal slope bands at amplitude `0.025`, with a smooth falloff inside three small positive islands. It retained the source macro-normal and untouched channels. The islands were checked against the original albedo and exported UV wireframes; brows, eyelids, nose, lips, ears, neck and atlas borders receive no direct perturbation.
- Sophia cheek centers `(0.31,0.48)` and `(0.69,0.48)`, radii `(0.055,0.065)`; Yael centers `(0.34,0.49)` and `(0.66,0.49)`, radii `(0.065,0.065)`; forehead center `(0.5,0.76)`, radii `(0.08,0.035)`. UV origin is bottom left. Unknown head layouts fail closed. This conservative island experiment is not a complete skin segmentation system.
- Changed pixels: Sophia `84328`, Yael `5943`, repeatable across angles and motion captures. The [candidate comparison](procedural-head-prototype/micro-comparison.jpg) shows that the face remains essentially the same. Increasing grain would not establish human anatomy, so there was no second strength sweep.

## Lifecycle, motion and size

- Six unchanged-map captures, six candidate captures and two short motion captures all report `authoredMaterialsRestored=true` and `temporaryResourcesReleased=true`. Source materials, albedo, gloss, geometry and rig were not edited. Every candidate exists only during the explicitly invoked probe.
- Four distinct game framebuffers per head were recorded at `timeScale=1`, with NR restored to Auto: [Sophia motion](procedural-head-prototype/sofia-motion.gif), [Yael motion](procedural-head-prototype/yael-motion.gif). Four frames demonstrate a live moving render, not comprehensive temporal artifact/FPS validation.
- Selecting Sophia while Yael's candidate was active cancelled with `Visual generation changed; capture cancelled.` and released temporary resources. The old renderer had been destroyed, so its restoration field is absent rather than falsely marked true. A separate hair-switch test was not repeated in this bounded experiment; the existing guarded material lifecycle is reused, but ordinary hair compatibility is not newly certified here.
- Live handoff: Sophia selected, helmet visible; NR Auto `0.99 / 0.89 / 0.82 / 0.26`, DLAA, FG X2, ShowOverlay false, `timeScale=1`; `GameObject.Find("Renderforge mask diagnostic")` returned null. No runtime DLL deployment/restart, game save, push, backend download or shutdown occurred.
- Isolated baseline DLL `33792` bytes; diagnostic DLL `41984` bytes: **8192 bytes extra code/metadata**, including inventory and control methods. The production mod package grows by **zero bytes** because the rejected probe remains under `docs/` and is not compiled into `src/`. No texture image/model weights are distributed as runtime dependencies; JPEG/GIF files here are review evidence only.
- Runtime textures cost memory regardless of package size: one RGBA32 mip chain is about 21.33 MiB at 2048 or 1.33 MiB at 512, plus temporary readback/CPU arrays. The microstructure loop/upload took about 325 ms on Sophia and 21 ms on Yael in the first front captures (not a complete end-to-end GPU timing). This synchronous diagnostic must not become an automatic main-thread hook. There is no per-frame texture regeneration and no measured FPS claim.

## Reproduction and verification

- [Probe source](procedural-head-prototype/ProceduralHeadProbe.cs) is an isolated partial extension of the existing two diagnostic source files. It requires no new library. Original shader behavior is observed live; no guessed custom ShaderLab compiler or undocumented NR input was introduced.
- Build: `dotnet build docs\procedural-head-prototype\Probe.csproj -c Release /p:PPRoot="D:\Steam\steamapps\common\Phoenix Point" /p:BaseIntermediateOutputPath="D:\RenderforgeWork\procedural-head\checked-obj\"`. Result: zero warnings/errors. Output is deliberately on D:.
- Use PPCLI `connect call`, invoke `System.Reflection.Assembly.LoadFrom` with the resulting absolute DLL, then `Renderforge.CharacterMaskDiagnostic, RenderforgeProceduralProbe7`. `InventoryMorphs(cameraId,prefab,absoluteDirectory)` is read-only apart from evidence files. `CaptureNormalControl` and `CaptureMicroNormal` add `yaw`; `CaptureMicroMotion` uses the first three arguments. Re-resolve the live camera and selected identity; do not reuse historical PPCLI handles.
- The isolated packaging check returned `PASS: 14 captures restored and released; 4 distinct motion frames/head; head-change cancellation released.` The evidence JSON includes the underlying capture records. Installed Unity/game DLLs, existing source and official [Unity normal-map examples](https://docs.unity3d.com/2018.1/Documentation/Manual/SL-VertexFragmentShaderExamples.html) grounded the APIs; Context7 was consulted before adding API calls.
- Remove this experiment by reverting its documentation commit; it has no production hook, generated-asset cache or settings entry to disable.
