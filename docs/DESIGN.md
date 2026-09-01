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
- `DlssDriver : MonoBehaviour` on the scene camera. Per-frame:
  1. `OnPreCull`: `ResetProjectionMatrix()`; `nonJitteredProjectionMatrix = projectionMatrix`;
     add Halton(2,3) jitter `(jx,jy)` in render-pixel units (phase count = 8·(out/render)²) to
     `projectionMatrix[0,2]/[1,2]` as `2·j/renderW`, `-2·j/renderH` (sign verified live via
     the sharpness of static geometry — the single most likely thing to get wrong);
     `depthTextureMode |= Depth | MotionVectors`; `targetTexture = colorRT` (render-res,
     RGBA16F = PPv2 HDR path... verified by reading `PostProcessLayer` state; else ARGB32).
  2. A `CommandBuffer` on `CameraEvent.AfterEverything` (see ordering below) copies inputs and
     runs the DLSS eval.
  3. `OnPostRender`: `ResetProjectionMatrix()` so picking/UI raycasts see the clean matrix, then
     `Graphics.Blit(outRT, (RenderTexture)null)` writes the upscaled frame to the backbuffer.
- Ordering (v1, simplest that works): camera renders everything incl. PPv2 into `colorRT` at
  render res. The `AfterEverything` command buffer does:
  `Blit(BuiltinRenderTextureType.MotionVectors → mvRT RG16F)`,
  `Blit(BuiltinRenderTextureType.Depth → depthRT RFloat)` (raw hardware depth via a copy shader,
  reversed-Z kept), then `IssuePluginEventAndData(evalFn, 2, framePtr)` writing `outRT`
  (`enableRandomWrite=true` → UAV). The screen write is NOT in the command buffer (the camera
  target is `colorRT`); it is the `OnPostRender` blit above. Overlay HUD draws afterwards at
  native res, untouched.
  Consequence accepted for v1: PPv2 effects (bloom/DoF/SSR/AO) run at render res and get
  upscaled with the frame. v2 (only if visibly worse than SMAA): Harmony PPv2 so DLSS runs before
  the uber pass.
- `InReset=1` on the first frame after create, on level change, and when the camera teleports
  (Cinemachine cut: `CinemachineBrain.ActiveBlend == null && cameraChanged`). `InMVScale` =
  `(-renderW, -renderH)` pending live sign check (Unity MV = previous→current NDC delta? verify
  by the ghosting direction; both options in one flag).
- Auto mode table (output height): ≤1200 → DLAA; ≤1600 → Quality; else → Performance.
- SMAA: postfix `LightingManager.ApplyPostProcessOptions` → when DLSS active force
  `antialiasingMode = None` (SMAA on top of DLSS = blur). Restored on Off.
- Resolution change / alt-tab / fullscreen switch: `Screen.width/height` diff vs cached →
  release + recreate feature (event 3 then 1), reallocate RTs.
- Options panel: postfix `UIModuleGraphicsOptionsPanel.Init/Show` → clone the TextureQuality
  `ArrowPickerController` GameObject, label "DLSS", items = mode names, value ↔
  `DlssConfig.Mode`, on change `DlssDriver.Apply` + `Config.Save`. Hidden when
  `Available=false` or `ShowInGraphicsOptions=false`. Native widget, no custom UI.
- `OnModDisabled`: driver off, RTs released, camera `targetTexture=null`, feature released,
  Harmony unpatch. Game must look byte-identical to no-mod.

### Data flow per frame

```
Cinemachine → camera matrices → [OnPreCull: jitter, targetTexture=colorRT(render res)]
→ opaque + MV pass + transparent + PPv2 (render res, in colorRT)
→ [CB AfterEverything: copy MV, copy depth, DLSS eval → outRT (native res)]
→ [OnPostRender: ResetProjectionMatrix, Blit outRT → backbuffer]
→ Unity draws ScreenSpaceOverlay HUD at native res
```

## Failure handling

- No NVIDIA / no RTX / old driver: `Dlss_Init` ≠ success → mod dormant, one log line, picker
  hidden, nothing patched. Must never throw into the game's frame.
- NGX error at create/eval: driver disables itself (`Mode` shown as Off with reason in log),
  camera restored to direct rendering the same frame.
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

1. Repo + native shim + probe → probe green. Commit.
2. Managed driver, DLAA only (render res = out res) → in-game screenshot shows AA, no HUD damage.
   **Ping user here.**
3. Upscale modes + Auto + SMAA auto-off + resolution-change handling.
4. Graphics-panel picker + ModConfig + dormant path on non-NVIDIA.
5. README, NVIDIA notice, Workshop packaging (Workshop id TBD by user, tags `Gameplay`?). Release.
6. Experiment (separate decision): `-force-d3d12` viability → Frame Gen.

## Repo

`E:\DEV\PhoenixPoint\DLSS\` = own inner repo (`UberMorgott/PhoenixPoint-Mod-DLSS`, branch
`main`, ignored by the outer `.gitignore`). Layout: `src\` (C#), `native\` (C++, CMake),
`native\probe\`, `DLSS.csproj`, `deploy.ps1` (builds both, copies bundle to `<PPRoot>\Mods\DLSS`),
`docs\`. `refs\DLSS-sdk` stays in the outer workspace (not committed; deploy copies the DLL from it).
