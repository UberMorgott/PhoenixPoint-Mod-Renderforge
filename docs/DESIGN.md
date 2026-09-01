# DLSS for Phoenix Point — design (2026-09-01)

Mod id `com.morgott.DLSS`, folder `Mods\DLSS`. Adds NVIDIA DLSS Super Resolution + DLAA to
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
Ray Reconstruction, mip-bias retune of every texture (v2 if visibly soft).

## Architecture — two DLLs, one mod folder

```
Mods\DLSS\
  DLSS.dll          managed mod (Harmony + MonoBehaviour driver)      ← src\
  DlssNative.dll    C++ shim: NGX D3D11 init/create/evaluate          ← native\
  nvngx_dlss.dll    NVIDIA runtime, verbatim from SDK (rel)
  meta.json, LICENSE-NVIDIA.txt, README.md
```

### DlssNative.dll (C++, ~400 LOC, VS2022 Build Tools + CMake)

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
`LoadLibraryW(<modFolder>\DlssNative.dll)` by the managed side before the first P/Invoke, so
`[DllImport("DlssNative")]` resolves without touching the game's `Plugins` folder.

Offline check: `native\probe\dlss_probe.exe` — creates a bare D3D11 device, runs Init → Create
(1920×1080→3840×2160, Quality) → Release, prints NGX status. Fails loudly if SDK/link/driver
is wrong before we ever touch the game.

### DLSS.dll (C#, ~700 LOC)

- `DlssMod : ModMain` — `OnModEnabled`: probe NVIDIA (native init on a 1×1 `Texture2D`), if
  unavailable log + stay dormant (`Available=false`). Otherwise Harmony patch + attach driver when
  `CameraManager.Camera` exists (level start hook `OnLevelStart`, plus lazy attach in a Harmony
  postfix on `CameraManager` camera assignment). `OnConfigChanged` → `DlssDriver.Apply(config)`.
- `DlssConfig : ModConfig` — public fields (this is also the mod-manager settings UI):
  `DlssMode Mode = Auto` (`Off, Auto, DLAA, Quality, Balanced, Performance, UltraPerformance`),
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

## Future scope (user, 2026-09-01)

DLSS first; later FSR and XeSS through the same driver (the native shim's ABI is upscaler-shaped:
init / optimal / create / set-frame / event / release). Folder and mod name `DLSS` are
provisional and get renamed to something vendor-neutral before publishing. Publish sequence:
in-game success → GitHub repo → Steam Workshop → Nexus.

## Repo

`E:\DEV\PhoenixPoint\DLSS\` = own inner repo (`UberMorgott/PhoenixPoint-Mod-DLSS`, branch
`main`, ignored by the outer `.gitignore`). Layout: `src\` (C#), `native\` (C++, CMake),
`native\probe\`, `DLSS.csproj`, `deploy.ps1` (builds both, copies bundle to `<PPRoot>\Mods\DLSS`),
`docs\`. `refs\DLSS-sdk` stays in the outer workspace (not committed; deploy copies the DLL from it).
