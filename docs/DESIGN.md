# Renderforge for Phoenix Point — design (2026-09-01)

Renderforge (working name `DLSS` until the rename on 2026-09-02). Mod id `com.morgott.Renderforge`,
folder `Mods\Renderforge`, DLLs `Renderforge.dll` + `RenderforgeNative.dll`. Adds NVIDIA DLSS Super Resolution + DLAA to
Phoenix Point (Unity 2019.4.31f1, built-in pipeline, D3D11, PostProcessing v2) as a plain
Workshop mod. Zero game-file writes. Subscribe → enable → play; DLSS on by default (Auto),
switchable in the game's own Graphics options panel without restart.

## Facts this rests on (verified)

- Engine: Unity 2019.4.31f1, D3D11, BiRP, PPv2 (`Unity.Postprocessing.Runtime.dll`). No SRP,
  no `UnityEngine.NVIDIAModule`. AA today = SMAA only, driven by the preset bool
  `OptionsManager.GraphicsQualityPreset.SubpixelMorphologicalAntialiasing` (`OptionsManager.cs:64`),
  applied in `LightingManager.ApplyPostProcessOptions` (`LightingManager.cs:163-185`) on every
  `PostProcessLayer`.
- Scene camera: one, `CameraManager.Camera` (`Base.Cameras.CameraManager`, Cinemachine-driven),
  carries the `PostProcessLayer`. Same camera for tactical + geoscape. HUD = `ScreenSpaceOverlay`
  canvas → rendered by Unity after all cameras, straight to the backbuffer at native res.
  Character-preview cameras (`UICameraArm`) render to their own RTs and are untouched.
- Motion vectors: BiRP `DepthTextureMode.MotionVectors` exists and already runs for Amplify
  Occlusion (`AmplifyOcclusionBase.cs:548`). No render-scale / `targetTexture` on the main camera today.
- Graphics options panel: `UIModuleGraphicsOptionsPanel` (`PhoenixPoint.Common.View.ViewModules`,
  `:15`) — `ArrowPickerController` pickers + `UnityEngine.UI.Toggle`s.
- SDK: `refs\DLSS-sdk` (github.com/NVIDIA/DLSS), `nvngx_dlss.dll` 310.7.0.0, D3D11 SR/DLAA
  supported, Frame Gen D3D12-only. Contract: `docs\research\dlss-ngx-d3d11-contract.md`
  (PathListInfo, ProjectID+ENGINE_TYPE_UNITY, output needs UAV, MV = current→previous pixels,
  DepthInverted for Unity reversed-Z, preset K = transformer).
- Mod skeleton: `ModMain` (`PhoenixPoint.Modding`), `ModConfig` public fields → in-game mod
  settings UI + `ModConfig.json`; `meta.json` = `{ID, AssemblyName, Version, Author, Name,
  Description, Dependencies}`; mods deploy to `<install>\Mods\<Folder>\`.

## Scope

IN: DLSS SR (Quality/Balanced/Performance/Ultra Performance), DLAA, Auto mode, native options
picker, runtime toggle, auto-disable SMAA, NVIDIA-only graceful no-op.
OUT (documented, not built): Frame Generation (D3D12-only; `-force-d3d12` on Unity 2019.4 is an
experiment for later), DLSS 5 neural rendering (unreleased; same NGX contract, add when official),
Ray Reconstruction. (Mip bias was pulled into v1 — see "Texture mip bias" below.)

## Architecture — two DLLs, one mod folder

```
Mods\Renderforge\
  Renderforge.dll          managed mod (Harmony + MonoBehaviour driver)      ← src\
  RenderforgeNative.dll    C++ shim: NGX D3D11 init/create/evaluate          ← native\
  nvngx_dlss.dll    NVIDIA runtime, verbatim from SDK (rel)
  meta.json, LICENSE-NVIDIA.txt, README.md
```

### RenderforgeNative.dll (C++, ~400 LOC, VS2022 Build Tools + CMake)

Flat C exports, all state in one static struct; no classes, no threads of its own.

| Export | Runs on | Does |
|---|---|---|
| `Dlss_Init(ID3D11Resource* anyTex, const wchar_t* dllDir, const wchar_t* logDir)` | main | `GetDevice()` off the resource → `NVSDK_NGX_D3D11_Init_with_ProjectID(guid, ENGINE_TYPE_UNITY, "2019.4", logDir, device, &common)` with `PathListInfo={dllDir}`; `GetCapabilityParameters`; reads `SuperSampling.Available`, `NeedsUpdatedDriver`, min driver. Returns status code. |
| `Dlss_Create(w,h,outW,outH,quality,flags)` | render thread (via event) | `NGX_D3D11_CREATE_DLSS_EXT` on immediate context; flags = `MVLowRes|DepthInverted|IsHDR?` + preset hints (K for DLAA/Q/B, M for P). Stores handle. |
| `Dlss_SetFrame(color, depth, mv, out, jx, jy, mvScaleX, mvScaleY, reset, dtMs, w, h)` | main | Copies pointers/values into the static frame block. |
| `Dlss_GetRenderEventFunc()` | — | `UnityRenderingEvent` callback: `eventId 1 = create`, `2 = evaluate`, `3 = release`. |
| `Dlss_Release()`, `Dlss_Shutdown()` | render thread / main | `ReleaseFeature`, `Shutdown1(device)`. |
| `Dlss_GetOptimal(outW,outH,quality,&w,&h)` | main | `NGX_DLSS_GET_OPTIMAL_SETTINGS` → render res for the mode. |
| `Dlss_Status()` | main | last NGX result + availability, for logs/UI. |

Immediate context comes from `device->GetImmediateContext()` inside the render event (Unity's
D3D11 render thread owns it; `IssuePluginEventAndData` runs there). The plugin is loaded with
`LoadLibraryW(<modFolder>\RenderforgeNative.dll)` by the managed side before the first P/Invoke, so
`[DllImport("RenderforgeNative")]` resolves without touching the game's `Plugins` folder.

Offline check: `native\probe\dlss_probe.exe` — creates a bare D3D11 device, runs Init → Create
(1920×1080→3840×2160, Quality) → Release, prints NGX status. Fails loudly if SDK/link/driver
is wrong before we ever touch the game.

### Renderforge.dll (C#, ~700 LOC)

- `RenderforgeMod : ModMain` — `OnModEnabled`: probe NVIDIA (native init on a 1×1 `Texture2D`), if
  unavailable log + stay dormant (`Available=false`). Otherwise Harmony patch + attach driver when
  `CameraManager.Camera` exists (level start hook `OnLevelStart`, plus lazy attach in a Harmony
  postfix on `CameraManager` camera assignment). `OnConfigChanged` → `DlssDriver.Apply(config)`.
- `DlssConfig : ModConfig` — public fields (this is also the mod-manager settings UI):
  `RenderforgeMode Mode = Auto` (`Off, Auto, DLAA, Quality, Balanced, Performance, UltraPerformance`),
  `bool ShowInGraphicsOptions = true`. Nothing else in v1.
- `DlssDriver : MonoBehaviour` on the scene camera (revised after the Codex review, 2026-09-01).
  Gate: `SystemInfo.graphicsDeviceType == Direct3D11`, else dormant.
  1. Jitter is applied in a Harmony **postfix on `PostProcessLayer.OnPreCull`** — PPv2's own
     `OnPreCull` calls `ResetProjectionMatrix()` + re-assigns `nonJitteredProjectionMatrix` every
     frame, and MonoBehaviour message order is not guaranteed, so a sibling `OnPreCull` can lose.
     Postfix body: `nonJittered = projectionMatrix` (already reset by PPv2); Halton(2,3) offset
     `(jx,jy)` in render pixels, phase count = 8·(outW/renderW)²; `proj[0,2] += 2·jx/renderW`,
     `proj[1,2] += 2·jy/renderH` (PPv2 `TemporalAntialiasing` sign convention);
     `useJitteredProjectionMatrixForTransparentRendering = false`;
     `depthTextureMode |= Depth | MotionVectors`; `targetTexture = colorRT` (render res).
  2. `CommandBuffer` at `CameraEvent.BeforeImageEffects` (depth/MV transients are alive there):
     `CopyTexture(BuiltinRenderTextureType.MotionVectors → mvRT RGHalf)` (same size/format,
     no resample, no Y flip); depth: material-less `Blit(BuiltinRenderTextureType.Depth →
     depthRT RFloat)` point-sampled, no conversion (`DepthInverted` flag from
     `SystemInfo.usesReversedZBuffer`). If the material-less depth blit proves lossy, v1.1 embeds
     a tiny copy shader as D3D11 bytecode — no AssetBundle in the package.
  3. `CommandBuffer` at `CameraEvent.AfterEverything`: `IssuePluginEventAndData(evalAndDataFn, 2,
     frameSlotPtr)` → NGX writes `outRT` (native res, `enableRandomWrite=true` → UAV).
  4. `OnPostRender` (scene camera): `ResetProjectionMatrix()` so picking/UI raycasts see the
     clean matrix; `GL.InvalidateState()` so Unity re-applies its D3D11 state after NGX.
  5. **Present camera** — a second `Camera` on our own GameObject, `depth = scene.depth + 1`,
     `targetTexture = null`, `cullingMask = 0`, `clearFlags = Nothing`, no `PostProcessLayer`,
     one `CommandBuffer` at `AfterEverything`: `Blit(outRT → BuiltinRenderTextureType.CameraTarget)`.
     This is the only thing that writes the backbuffer; `ScreenSpaceOverlay` HUD draws after it at
     native res. (`Graphics.Blit(x, null)` from the scene camera would write back into `colorRT`,
     because `null` = the camera's current target — that was the spec's original bug.)
- Threading: per-frame values go through a **ring of 4 frame slots** owned by the native DLL
  (`Dlss_GetFrameSlot()` returns the next slot pointer; managed fills it via `Dlss_SetFrame(slot,
  ...)` and passes the same pointer as the event `data`). The render thread never reads a slot the
  main thread is writing. RT release on resize is deferred two frames after the release event (3)
  was issued, so no in-flight event touches a dead resource. Event callbacks are the
  `UnityRenderingEventAndData(int, void*)` ABI.
- NGX v1 flag matrix: `MVLowRes=1`, `MVJittered=0` (MVs come from the non-jittered matrices),
  `DepthInverted=SystemInfo.usesReversedZBuffer`, `IsHDR=0` (PPv2 output is display-referred
  LDR: tonemapped + dithered; `colorRT` = `ARGB32`... unless PPv2 is found to blit HDR, then
  `ARGBHalf` still with `IsHDR=0`), `AutoExposure=0`. `InMVScale = (-renderW, -renderH)`
  (Unity MV = NDC previous→current; DLSS wants current→previous in pixels). Both signs are still
  confirmed live by the ghosting direction on a moving unit.
  **Live signs (2026-09-01, DLAA 1280×720, Instance2):** projection gets `proj[0,2] += 2jx/w`,
  `proj[1,2] += 2jy/h` (PPv2 convention) and NGX gets `InJitterOffset = (-jx, -jy)` — Unity's view
  space is right-handed so that projection offset moves the image by −j pixels; `(+jx,+jy)` doubles
  thin edges in an 8× crop, `(-jx,-jy)` resolves them (same as HDRP's DLSSPass). `InMVScale =
  (-renderW, -renderH)` kept by derivation (PPv2 TAA fetches history at `uv - mv`) — NOT yet
  confirmed by a moving-unit ghost test (the camera-pan harness snapped instead of animating).
  Also `useJitteredProjectionMatrixForTransparentRendering = true` (not `false` as above): with
  `false` the tactical path lines stay hard-aliased under DLAA.
  Consequence accepted for v1: PPv2 effects (bloom/DoF/SSR/AO, dither) run at render res before
  the upscale. v2 (only if visibly worse than SMAA): Harmony PPv2 so DLSS runs before the uber pass.
- Reset: `InReset=1` on the first frame after create, on `OnLevelStart`, on a Cinemachine cut
  (`CinemachineBrain.ActiveBlend == null` and the active virtual camera changed), and when the
  camera position jumps > 50 m or FOV changes between frames.
- Auto mode table (output height): ≤1200 → DLAA; ≤1600 → Quality; else → Performance.
- SMAA: postfix `LightingManager.ApplyPostProcessOptions` → when DLSS active force
  `antialiasingMode = None` (SMAA on top of DLSS = blur). Restored on Off.
- Resolution / mode changes: a small generation state machine — `Idle → Creating → Live →
  Releasing → Idle`. Trigger = `Screen.width/height` or `Mode` differs from the live generation;
  `Releasing` issues event 3, then after two frames frees RTs, allocates new ones and issues
  event 1. No polling-driven recreate while a generation is in flight.
- Options panel: postfix `UIModuleGraphicsOptionsPanel.Init/Show` → clone the TextureQuality
  `ArrowPickerController` GameObject, label "DLSS", items = mode names, value ↔
  `DlssConfig.Mode`, on change `DlssDriver.Apply` + `Config.Save`. Hidden when
  `Available=false` or `ShowInGraphicsOptions=false`. Native widget, no custom UI.
- `OnModDisabled`: driver off, RTs released, camera `targetTexture=null`, feature released,
  Harmony unpatch. Game must look byte-identical to no-mod.
- **Picking / HUD under a reduced render res** (`src\Picking.cs`, phase 3b, verified 2026-09-02):
  `targetTexture = colorRT` makes `Camera.pixelWidth/Height` the RENDER res while
  `Input.mousePosition`, `Screen.*` and the overlay HUD stay at screen res.
  - Unity dynamic resolution (the zero-patch fix) is DEAD on this engine: DRS is DX12-only on
    Windows in 2019.4 — live `ScalableBufferManager.ResizeBuffers(0.67,0.67)` left
    `widthScaleFactor = 1.0` on Instance2 (D3D11), although PPv2 does carry `useDynamicScale`.
  - So Harmony seams, gated on `Seams.Scaled(cam)` (= driver live, this camera, render ≠ out):
    prefix scales the INPUT of `Camera.ScreenPointToRay(Vector3,Eye)`,
    `ScreenToWorldPoint(Vector3,Eye)`, `ScreenToViewportPoint(Vector3)` by `render/screen`;
    postfix scales the OUTPUT of `WorldToScreenPoint(Vector3,Eye)`, `ViewportToScreenPoint(Vector3)`
    back up. Only the innermost IL wrappers (the ones calling `*_Injected`) are patched; the 1-arg
    overloads route through them. `pixelWidth/Height` are icalls (no IL) so their READERS are
    transpiled (`get_pixelWidth` → `Seams.PixelWidth(cam)` = `Screen` size while live):
    `CameraBehavior.CenterScreenPos`, `PlanarScrollCamera.GetEdgeScrollOffset`,
    `FirstPersonCamera.UpdateInput/HandleInput/GetMouseOffset`, `UIStateFreeCam.GetTargetPos/
    GetCameraPosByCameraTarget/GetDefaultTarget`, `FreeCursorController.SetFreeCursorActive`.
    `CanvasScalerController` left alone: it only picks `matchWidthOrHeight` from `camera.aspect`
    (unchanged by the scale); the HUD canvas itself is ScreenSpaceOverlay → Screen-sized.
  - Evidence (Performance 640×360→1280×720, Instance2): `WorldToScreenPoint(actor)` =
    `(997.9, 238.7)` identical at `pixelWidth` 640 and 1280; `plans\aim-and-run.json` with
    `requireActor:true` (the game's own `SelectAtCursor`) resolved the actor in both modes;
    `CameraManager.CenterScreenPos` = `(640,360)` while live; health bars sit on units.
- **Hotkeys** (`DlssConfig`): fixed `Ctrl+Alt` chord (either side, `Input.GetKey`) + configurable
  letter: `ToggleHotkey=U` (Off ↔ last non-Off mode; the remembered mode is in-memory, after a
  restart in Off the toggle restores Auto), `OverlayHotkey=O`. Polled with `Input.GetKeyDown` in
  `DlssDriver.Update`. No F-keys/Insert/End (user's keyboard has none; F4/F5/F9/F10 are game keys).
  Game chords are the `InputMapDef` ASSET `PhoenixInput` (153 actions, 191 chords), not code
  (`InputChord.Keys` is a plain key set, no modifier flags — a game key still fires while Ctrl+Alt
  is held). Read live 2026-09-02 via `JsonConvert.SerializeObject` per action: the game has NO
  Ctrl+Alt chords; the only modifier chords are `left alt` (Show Item Labels), `ctrl+wheel`
  (OverwatchSpread), `shift+f`. `D` is bound (`Camera Right`), so the requested Ctrl+Alt+D was
  replaced by Ctrl+Alt+U. Free letters: `b h j k l o p u` (`b` left to ContentTool's fit bench).
  A hotkey press is only a `wantMode` change; the driver's Idle→Creating→Live→Releasing machine
  serialises create/release with the render thread, so a press mid-generation cannot wedge it.
  `RenderforgeMod.SaveConfig()` = `ModManager.GetInstance().SaveModConfig()` (`ModManager.cs:120`), used
  by the hotkeys and the graphics-panel picker. PPCLI substitutes for a keypress:
  `call Renderforge.RenderforgeMod.Toggle / ToggleOverlay / SetOverlay("TopLeft")`.
- **Sharpness** (2026-09-02). NOT NGX's: SDK 310 marks `NVSDK_NGX_DLSS_Feature_Flags_DoSharpening`
  `[[deprecated("Sharpness is not supported")]]` (`nvsdk_ngx_defs.h:60,:296`) and the eval helper
  still copies `InSharpness` into the parameter block, but nothing consumes it (sharpening was removed
  in DLSS 3.x). The shim keeps `InSharpness = 0`.
  - Our pass: NVIDIA **NIS** (NVIDIA Image Scaling SDK 1.0.3, MIT, `LICENSE-NIS.txt` shipped) in
    **sharpen-only** mode — what the DLSS programming guide recommends in place of the removed NGX
    sharpening. `native\nis\NIS_Scaler.h` (HLSL) is vendored and embedded by CMake as a byte array
    (`nis_scaler_hlsl.h`); at runtime the shim prepends the `NIS_Main.hlsl` bindings
    (`NIS_SCALER 0`, `NIS_HDR_MODE 0`, cbuffer `b0`, `samplerLinearClamp s0`, `in_texture t0`,
    `out_texture u0`) and an `NVSharpen` entry, compiles `cs_5_0` with `D3DCompile`
    (`d3dcompiler_47.dll`), cached. Block/group = `NISOptimizer(isUpscaling=false, NVIDIA_Generic)`
    from `native\nis\NIS_Config.h`: 32×32 px per block, 128 threads → dispatch ⌈w/32⌉×⌈h/32⌉.
    Constants = `NISConfig` filled by `NVSharpenUpdateConfig(cfg, s, 0,0,w,h, w,h, 0,0)`
    (256 B, aligned), slider/100 passed straight in (NIS' own 0..1 slider). Runs inside event 2
    right after a successful `NGX_D3D11_EVALUATE_DLSS_EXT`, in place on the output UAV, reading an
    SRV scratch copy (`CopyResource(scratch, output)`; in-place read+write is a hazard). Views are
    dropped on event 3 (before the driver frees RTs).
  - Fallback: AMD FidelityFX **RCAS** (`kRcasHlsl`, from the public formula, epsilon-guarded, denoise
    on; mapping `con = exp2(-2·(1−s))`) only if the NIS source fails to compile on this machine.
    `Dlss_Sharpener()` → 1 NIS / 2 RCAS / −1 failed, shown as `sharpen=` in `DlssDriver.Status`.
    A failed setup sets `Dlss_LastError() = DLSS_ERR_SHARPEN` (−3) and disables the pass, never the
    DLSS frame. ABI otherwise unchanged: the existing `sharpness` arg of `Dlss_SetFrame` feeds it;
    s=0 skips the pass. Probe evaluates with 0.5 and asserts `lastError == 0` AND sharpener == NIS.
    Not applied in Passthrough (no release event there).
  - Managed: `DlssConfig.Sharpness = 40` (0..100, localized "Sharpness"/"Резкость"), read by the
    driver EVERY frame (no re-create). `RenderforgeMod.SetSharpness(int)` = PPCLI/keypress substitute.
  - Slider: `GraphicsPanel.BuildSlider` clones the panel's own ShadowDistance row (same
    TextAndSlider prefab as VideoPanel's rows) right under the DLSS picker, label "SHARPNESS" /
    "РЕЗКОСТЬ", whole 0..100 + readout, immediate apply + `SaveConfig()` like the picker; greyed
    (CanvasGroup 0.35 + non-interactable) when the picker is Off, re-evaluated on every picker change.
  - Verified live (DLAA 1280×720, Instance2, `build\shots\7-nis-{0,50,100}.png`, `Status` says
    `sharpen=NIS`): mean |luma gradient| over the scene 10.95 → 12.45 → 13.72 (the earlier RCAS run on
    another camera, `6-sharp-*.png`: 7.22 → 8.29 → 11.02); 100 visibly crisper on texture/edges, no
    ringing halos at 50 (crop `crop-7-nis.png`). Frame-time delta read off the overlay's 0.5 s average is
    noise-level (≤0.5 ms at 1280×720). Panel: `6-panel-sharp.png` / `6-panel-sharp-off.png`.
    Persisted in `ModConfig.json` (`"Sharpness": 100` after the run).
- **Texture mip bias** (`src\MipBias.cs`, 2026-09-02). Unity 2019.4 has no global LOD bias, so the
  driver sweeps `Resources.FindObjectsOfTypeAll<Texture2D>()` and sets `mipMapBias =
  log2(renderW/outW)` (DLSS programming guide: Performance −1.0, Quality −0.585, DLAA/Passthrough 0)
  on every mipmapped texture when a generation goes Live, again 2 s later (level content still
  streaming), and writes 0 back on release. Skipped: `mipmapCount <= 1` and names containing
  `lut|noise|dither|ramp|gradient` (PPv2 LUT, Amplify blue noise). Idempotent (sweeps only when the
  effective bias changes); no dictionary of originals — the first sweep samples 20 textures and
  logs `originals max|bias|`, live = 0.000, vanilla ships 0. PPCLI switch
  `call Renderforge.MipBias.SetEnabled(false)`. Measured live (Performance 640×360→1280×720, Instance2):
  1813 textures set, 753 skipped, 9–15 ms per sweep; `build\shots\8-mip-{off,on}.png`: mean |luma
  gradient| over two rock regions 2.93 → 3.25, rock grain visible in the 4× crops
  `crop-8-mip-{off,on}.png`, HUD/overlay text identical (ScreenSpaceOverlay, sprites have no mips).
- **Benchmark overlay** (`src\Overlay.cs`, `ShowOverlay=false`, `OverlayPosition=TopCenter` of
  `TopLeft|TopCenter|TopRight|BottomCenter`): one ScreenSpaceOverlay canvas, sortingOrder 30000,
  no GraphicRaycaster, `raycastTarget=false`, HUD font (first `Text` found) else Arial, size
  14·Screen.height/1080, 6 px margin. Lines: upscaler (nvngx file version via `version.dll`;
  Mono's `FileVersionInfo` is empty for native DLLs), mode + `render -> out`, AA (DLSS / live
  `PostProcessLayer.antialiasingMode`), FPS = 0.5 s moving average, refreshed 4×/s.
  TopCenter is empty in tactical and geoscape; TopLeft covers the objectives title and
  BottomCenter the ability-bar key labels in tactical — user's choice, not a default.

### Data flow per frame

```
Cinemachine → PPv2 OnPreCull (reset) → [postfix: jitter, targetTexture=colorRT(render res)]
→ opaque + MV pass + transparent → [CB BeforeImageEffects: copy MV, copy depth]
→ PPv2 (render res, into colorRT) → [CB AfterEverything: DLSS eval → outRT (native res)]
→ [OnPostRender: ResetProjectionMatrix, GL.InvalidateState]
→ present camera [CB: Blit outRT → backbuffer]
→ Unity draws ScreenSpaceOverlay HUD at native res
```

## Failure handling

- No NVIDIA / no RTX / old driver: `Dlss_Init` ≠ success → mod dormant, one log line, picker
  hidden, nothing patched. Must never throw into the game's frame.
- NGX error at create/eval: the frame already rendered offscreen cannot be rewound, so the present
  camera blits `colorRT` (stretched) for that frame; from the next frame the driver is off
  (`Mode` shown as Off with reason in log) and the camera renders directly again.
- Native DLL missing/unloadable: same dormant path.
- Render-thread callback must be exception-free C; all pointers validated non-null, sizes cached.

## Testing

- `dlss_probe.exe` green on this machine (RTX 5070 Ti, driver 596.49) before any game work.
- In-game via PPCLI on `D:\PP-Instance2`: `plans\start-mission.json` → `connect screenshot`
  with Mode=Off vs DLAA vs Performance; compare edge crispness + check HUD unaffected + no
  ghosting on a moving unit (two screenshots one turn apart). `connect call` reads
  `DlssDriver.Status` for render/out res, feature handle, last NGX code.
- FPS: `var` / `call` read of frame time with Off vs Performance at 4K window.
- User gate (ping): visual quality judgement + real play session.

## Phases

1. Repo + native shim + probe → probe green. Commit. (DONE `e707936`; skeleton `da37f7d`,
   in-game `DLSS available` on Instance2.)
2. Managed driver in three screenshot-gated steps (Codex's ordering — prove Unity resource
   lifetime and presentation before NGX):
   a. **Passthrough**: scene camera → `colorRT` (native res) → event 2 is a no-op → present
      camera → backbuffer. Screenshot: scene identical, HUD sharp; `Mode=Off` restores the camera.
   b. **Debug views**: `DebugView = Depth | MotionVectors` blits `depthRT`/`mvRT` to the screen
      instead of `outRT`; screenshot with a moving unit shows sane MVs (uniform on camera pan).
   c. **DLAA** (render res = out res). Screenshot: edges anti-aliased, no HUD damage, no
      ghosting on a unit that moved one turn. **Ping user here.**
3. Upscale modes + Auto + SMAA auto-off + resolution-change handling.
4. Graphics-panel picker + ModConfig + dormant path on non-NVIDIA.
5. README, NVIDIA notice, Workshop packaging (Workshop id TBD by user, tags `Gameplay`?). Release.
6. Experiment (separate decision): `-force-d3d12` viability → Frame Gen.

## D3D12 probe (2026-09-02, LIVE on Instance2) — `-force-d3d12` WORKS

- Launch `PhoenixPointWin64.exe -force-d3d12 -mods` → `Player.log`: `Forcing GfxDevice: Direct3D 12`,
  `d3d12: loaded!`, `Version: Direct3D 12 [level 12.1]`. Menu + tactical mission
  (`ALN_PLT_Nest_48x48_A`, via PPCLI `start-mission.json`) render correctly: PPv2, lighting,
  HUD, units all fine (screenshots taken through `connect screenshot`). BUT the tactical scene is
  washed out / fully lit vs the dark cave under D3D11 (same map, same seed) — see RCA below.
- **Vulkan is DEAD**: `-force-vulkan` → `Forced GfxDevice 'Vulkan' was not built from editor,
  shaders will not be available` → `InitializeEngineGraphics failed`, exit 1. No SPIR-V in the
  build. Only D3D11 and D3D12 exist for this game; DX12 works because it reuses the DXBC blobs.
- ONE breakage, spammed per frame: `ArgumentException: Kernel 'MultiScaleVODownsample1' not found.`
  = PPv2 `AmbientOcclusion` in `MultiScaleVO` mode — its compute shader has no D3D12 platform data
  in this build. Fix = under D3D12 switch the AO mode to `ScalableAmbientObscurance` (pixel shader)
  or disable AO; trivial Harmony/PPv2-settings tweak.
- Consequence: a D3D12 backend is viable → unlocks Frame Generation (DLSS-FG via Streamline,
  FSR FG, XeSS-FG — all D3D12-only) and official FSR 3.1/4 + cross-vendor XeSS (both D3D12-only
  in the current SDKs; the D3D11 XeSS DLL is Intel-Arc-only, FSR has NO official D3D11 backend).
  Native shim needs a D3D12 path: `GetNativeTexturePtr()` returns `ID3D12Resource*`, NGX D3D12
  entry points, resource-state transitions.

## Idea backlog (user, not scheduled)

- **Color grading preset / LUT** (2026-09-02, "like Cyberpunk's natural-grey look"): PPv2 already
  ships `ColorGrading` with `LDR LUT` / `External LUT` modes — drop in a LUT strip (1024×32) or
  3D LUT with `contribution`, or build the look from temperature / saturation / contrast /
  lift-gamma-gain without a file. Delivery = our own high-priority `PostProcessVolume` or a Harmony
  tweak of the existing `ColorGrading` settings. Independent of DLSS (grading runs before the
  upscale in the colour buffer). Could be a "Color preset" picker in this mod or a separate mod.

## Future scope (user, 2026-09-01)

DLSS first; later FSR and XeSS through the same driver (the native shim's ABI is upscaler-shaped:
init / optimal / create / set-frame / event / release). Folder and mod name `DLSS` were
provisional; renamed to the vendor-neutral `Renderforge` on 2026-09-02. Publish sequence:
in-game success → GitHub repo → Steam Workshop → Nexus.

## Repo

`E:\DEV\PhoenixPoint\Renderforge\` = own inner repo (`UberMorgott/PhoenixPoint-Mod-Renderforge`, branch
`main`, ignored by the outer `.gitignore`). Layout: `src\` (C#), `native\` (C++, CMake),
`native\probe\`, `Renderforge.csproj`, `deploy.ps1` (builds both, copies bundle to `<PPRoot>\Mods\Renderforge`),
`docs\`. `refs\DLSS-sdk` stays in the outer workspace (not committed; deploy copies the DLL from it).
