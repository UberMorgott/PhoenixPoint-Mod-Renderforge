# Renderforge for Phoenix Point — design (2026-09-01)

Renderforge (working name `DLSS` until the rename on 2026-09-02). Mod id `com.morgott.Renderforge`,
folder `Mods\Renderforge`, DLLs `Renderforge.dll` + `RenderforgeNative.dll`. Adds NVIDIA DLSS Super Resolution + DLAA to
Phoenix Point (Unity 2019.4.31f1, built-in pipeline, D3D11, PostProcessing v2) as a plain
Workshop mod. Exactly one game-file write: `RenderforgeNative.dll` is copied into
`PhoenixPointWin64_Data\Plugins\x86_64\` (see "Native plugin staging"). Subscribe → enable → play; DLSS on by default (Auto),
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
  amd_fidelityfx_{loader,upscaler}_dx12.dll   AMD FSR runtime (D3D12), verbatim from the SDK
  libxess.dll              Intel XeSS-SR runtime (D3D12, cross-vendor DP4a / Intel XMX), verbatim from the SDK
  meta.json, LICENSE-NVIDIA.txt, LICENSE-NIS.txt, LICENSE-AMD.txt, LICENSE-INTEL.txt, README.md
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
D3D11 render thread owns it; `IssuePluginEventAndData` runs there). The plugin is pinned with
`LoadLibraryW` by the managed side before the first P/Invoke, so `[DllImport("RenderforgeNative")]`
resolves against the already-loaded module.

### Native plugin staging (`Native.EnsureStaged`, `src\Native.cs`)

Unity calls `UnityPluginLoad` — the only way to get `IUnityGraphicsD3D12v5` — solely for modules it
resolves out of `<install>\PhoenixPointWin64_Data\Plugins\x86_64\`, never for a DLL the mod
`LoadLibraryW`s from `Mods\Renderforge\`. So `OnModEnabled`, before `Native.Load`, on every
graphics API, copies `<modDir>\RenderforgeNative.dll` to `<Application.dataPath>\Plugins\x86_64\`
when the target is missing or differs (length + last-write time), creating the folder if needed, and
logs `staged native shim into Plugins\x86_64 (takes effect after restart)`. Unity loads the Plugins copy at
startup, so it is locked in every process: a mapped DLL cannot be overwritten but can be renamed, so an
update moves it to `RenderforgeNative.dll.old` (or `.old<ticks>`), copies the new one in, and every run
sweeps `.old*` leftovers best-effort. `Native.Load` prefers the Plugins copy only when it
matches the mod copy; a stale one is skipped with a warning. Under D3D12 with `Dlss_UnityIface()==0`
the Availability reason is "Native plugin staged — restart the game" and the overlay shows
`Upscaler: off (restart required)`. Not removed on disable (the module is loaded); Steam "verify
integrity" leaves extra files alone.

Offline check: `native\probe\dlss_probe.exe` — creates a bare D3D11 device, runs Init → Create
(1920×1080→3840×2160, Quality) → Release, prints NGX status. Fails loudly if SDK/link/driver
is wrong before we ever touch the game.

### Upscaler providers

The shim carries one `IDevice` implementation per (API, vendor) pair. `Dlss_SetProvider(int)` picks one **before**
`Dlss_Init`; the choice is latched until `Dlss_Shutdown`. `Dlss_Provider()` reports the latched id,
`Dlss_ProviderVersion(char*,int)` the live provider's version string, `Dlss_SetCamera(near,far,fovY)` feeds the
frustum FSR/XeSS need (copied into every frame slot; NGX ignores it).

- **2026-09-03 live provider switch (no restart).** The UPSCALER row / PPCLI `SetUpscaler ["FSR"]` →
  `RenderforgeMod.SetUpscaler` → `DlssDriver.SwitchProvider(kind)`: `BeginRelease` (FG chain via `FrameGen.Release`,
  RELEASE event, mip bias), `Releasing` waits its two frames + `TeardownResources`, then `Idle` waits until
  `Dlss_Status` reports the feature dead (the RELEASE event runs on the render thread; racing it with a main-thread
  Shutdown double-destroyed the ffx context = access violation) and calls `RenderforgeMod.ReinitNative(kind)` =
  `Dlss_Shutdown` → `SetProvider` → `Init` on the same probe texture (a failed Init falls back to the previous
  provider, `Upscalers.Failed`/`FailedCode` keep the row's reason); the normal Idle path re-creates the generation and
  `FrameGen.Retry` re-arms FG. Belt on the native side: `IDevice::BeginDestroy/EndDestroy` (interlocked) make every
  backend's feature/context destroy single-entry. Measured on Instance2 D3D12 1280x720: menu DLSS→FSR live in
  168 ms, →XeSS 331 ms, →DLSS 1424 ms (NGX feature create), same in a tactical mission; 15 back-to-back switches in
  692 ms without a leak; FG (FSR-FG 2x) back live after each switch, ratio 2.1; D3D11 refuses FSR/XeSS with
  "Requires DirectX 12 — switch Renderer" and DLSS stays live; debug layer: 0 `D3D12 ERROR` naming our lists/resources
  across 3 switches, `Application.Quit` exit code 0, no dump. `Availability.NeedsRestart` (first-run staging) stays.

| Provider | id | API | Backend | SDK | Quality modes | Sharpening |
|---|---|---|---|---|---|---|
| DLSS SR / DLAA | 0 | D3D11 + D3D12 | `Device11`, `Device12` (NGX) | DLSS 3.10.7, `nvngx_dlss.dll` 310.7.129 | DLAA, Quality, Balanced, Performance, Ultra Performance | the shim's own NIS pass (RCAS fallback) |
| FSR | 1 | D3D12 only | `Fsr12` (ffx-api) | FidelityFX SDK 2.3, `amd_fidelityfx_{loader,upscaler}_dx12.dll` | Native AA, Quality, Balanced, Performance, Ultra Performance | FSR's built-in RCAS (`enableSharpening`) |
| XeSS SR | 2 | D3D12 only | `Xess12` (`libxess.dll`) | XeSS SDK 3.0.2, `libxess.dll` 2.0.2.68 | Native AA (1.0x), Ultra Quality Plus (1.3x), Ultra Quality (1.5x), Quality (1.7x), Balanced (2.0x), Performance (2.3x), Ultra Performance (3.0x) | the shim's own NIS pass (XeSS has no sharpness) |

All three D3D12 providers share `D3D12Ring` (`native\D3D12Ring.h`: 4 command allocators/lists, submitted through
`IUnityGraphicsD3D12v5::ExecuteCommandList` with a resource-state array, a slot recycled only after BOTH our own fence
signalled on Unity's queue and the frame-fence value ExecuteCommandList returned have retired; measured 2026-09-02:
the returned value == `GetNextFrameFenceValue()`, i.e. the current frame's, never seen unretired when ours was).

**D3D12 resource states — the owned-resource contract (`native\D3D12Owned.h`, 2026-09-02).** Unity 2019.4's D3D12
state tracker cannot be shared with a vendor SDK: with NGX/FSR/XeSS reading Unity RenderTextures directly, the debug
layer showed the RT's pre-state varying per frame (RENDER_TARGET, GENERIC_READ, COPY_DEST, DEPTH_WRITE on the same
RT), so every SDK barrier mismatched (`id=527`, ~400 per run) and the device was removed after 1–5 min. The shim now
OWNS the four resources the SDKs touch (`OwnedSet12`: colorIn/depthIn/mvIn at render res, out at output res with
`ALLOW_UNORDERED_ACCESS`, fully typed twins of the Unity formats — `R8G8B8A8_TYPELESS` → `R8G8B8A8_UNORM`,
`R32_FLOAT`, `R16G16_FLOAT`, created in `COMMON`, named `Renderforge colorIn` etc. so debug-layer messages name them)
and a Unity RT is only ever the source or destination of a `CopyResource` inside our list. Unity does NOT transition
an RT to the `expected` state a plugin declares (measured: our copy found every RT in the state Unity's LAST use
left it in), but that state is deterministic per RT because the driver uses each one the same way every frame —
`colorRT` (camera target) `RENDER_TARGET` 623/628 frames, `depthRT` (Blit target) `RENDER_TARGET`, `mvRT`
(CopyTexture target) `COPY_DEST` 614/614, `outRT` (present Blit source) `GENERIC_READ` 617/617 incl. the first
frame; the 5 `DEPTH_WRITE` outliers are Unity's own illegal barrier on a colour RT (`id=524`). So per frame, in ONE
ring list: Unity RT `pre-state → COPY_SOURCE` (inputs) / `GENERIC_READ → COPY_DEST` (outRT) `→ pre-state` again, declared
as `expected == current == pre-state`; owned inputs `COMMON → COPY_DEST` (copy in) `→ NON_PIXEL_SHADER_RESOURCE` (SDK)
`→ COMMON`; owned out `COMMON → UNORDERED_ACCESS` (SDK + our sharpen pass writes it) `→ COPY_SOURCE` (copy out) `→ COMMON`.
Every owned resource starts and ends each list in `COMMON`, so nothing depends on another list. Result with
`-force-d3d12-debug`: zero `id=527/538` on our lists (`Renderforge ring N`) or resources (`Renderforge colorIn` …).
Two Unity-side shortcuts were measured dead:
`CommandBuffer.CopyTexture` refuses a `Texture2D.CreateExternalTexture` wrapper under D3D12 (`can only copy between
same texture format groups (d3d12 base formats: src=27 dst=0)`), and `CreateExternalTexture` views the resource with
its own format, so a TYPELESS one removes the device (`id=28`). Remaining `id=527/538` under DLSS are Unity's own
(shadow cubemaps, its depth copies, on its own lists; 0/min with the mod Off, ~1600/min with a generation live —
they are triggered by the Unity-side setup, not by our list) and do not remove the device (10-min DLAA soak +
mission loads, see Phase 2 plan Task 8).
- **Format/view rule + the 2x-dark frame (2026-09-03).** Every copy in the contract is bit-exact (`R8G8B8A8_TYPELESS`
  Unity RT ↔ `R8G8B8A8_UNORM` twin, NGX/ffx/XeSS view the twins as UNORM, the sharpen pass views `SharpenViewFormat` =
  UNORM), so the shim never changes the gamma of a byte — and it was not the culprit of the D3D12 frame coming out
  ~2x darker (menu mean luma 28 vs 56 on D3D11 / D3D12-Off; `DebugView.Passthrough`, a bare `CopyResource
  colorRT → outRT`, measured the same 28). Root cause is Unity's D3D12 present path: `Blit(outRT → CameraTarget)`
  decodes the sRGB SRV read but does not sRGB-encode the backbuffer write (D3D11 does both), one net decode. Fix in
  `DlssDriver.Make`: `outRT` is created `RenderTextureReadWrite.Linear` on D3D12 only (`R8G8B8A8_UNorm`), so the
  already-encoded bytes pass through untouched; `colorRT` stays Default (its flag changed nothing — Linear measured 28
  too). D3D11 keeps Default (Linear there double-encodes). After: D3D12 DLAA 56.5, Quality 56.3, FSR Quality 56.1,
  XeSS Quality 56.2, D3D11 DLAA 56.7 (`docs\shots\darkfps\fix-*.png`); debug-layer run 0 errors on `Renderforge ring N`.

The AMD ffx-api is loaded at **runtime** (`FfxLoader.cpp`: `LoadLibraryW` of both DLLs by absolute path from the
mod folder, then `ffxLoadFunctions`). No AMD import library is linked, so `RenderforgeNative.dll` loads and DLSS
works even when the AMD DLLs are absent. `amd_fidelityfx_upscaler_dx12.dll` holds FSR 4.1.1 (ML, needs an AMD
RX 7000/9000-class GPU) and the 3.1.5 / 2.3.4 legacy providers; the shim does **not** force a version — it lets
the DLL choose and reports the result through `Dlss_ProviderVersion` (`ffxQueryGetProviderVersion`), which the
overlay prints as `Upscaler: FSR 4.1.1` / `FSR 3.1.5`. Observed on the dev RTX 5070 Ti (`dlss_probe --fsr`,
2026-09-02): version list `3.1.5 2.3.4` (no 4.x entry on a non-AMD device), context created on **3.1.5**.

XeSS (`native\Xess12.cpp`) links `libxess.lib` **delay-loaded** (`/DELAYLOAD:libxess.dll`): `Init` pins
`<modDir>\libxess.dll` with `LoadLibraryW` before the first `xess*` call, so the delay-load thunks bind to the mod's
copy and the shim still loads when it is absent (`DLSS_ERR_NO_PROVIDER_DLL`). `xessD3D12CreateContext` decides
support (`XESS_RESULT_ERROR_UNSUPPORTED_DEVICE` = no SM 6.4 + DP4a → `DLSS_ERR_NOT_AVAILABLE`,
`_UNSUPPORTED_DRIVER` → `DLSS_ERR_NEEDS_DRIVER`); `xessD3D12Init` is a blocking CPU call on the render thread (no
ring slot); `xessD3D12Execute` records into the ring list on the owned set with the same copy-in/copy-out contract as
`Device12` (XeSS restores nothing, `OwnedSet12::Leave` transitions back), and the shim's NIS pass (`native\D3D12Sharpen.h`, shared with `Device12`) runs after it because XeSS has
no sharpness of its own. Execution path: `xessGetIntelXeFXVersion` = `0.0.0` off Intel = cross-vendor DP4a,
non-zero = Intel XMX; `Dlss_ProviderVersion` reports `2.0.2 DP4a` / `2.0.2 XMX` and the overlay prints
`Upscaler: XeSS 2.0.2 DP4a`. xess results map onto `NVSDK_NGX_Result` like ffx's (`XESS_RESULT_SUCCESS` is 0, NGX's
is 1), `Dlss_LastError() = DLSS_ERR_XESS` (-6). Observed on the dev RTX 5070 Ti (`dlss_probe --xess`, 2026-09-02):
`xessGetVersion 2.0.2`, XeFX `0.0.0` (DP4a), 1920x1080 Quality → 1130x636, UQ+ → 1477x831, AA → 1920x1080, 3
executes + NIS sharpen green. **The Intel XMX path is implemented but unverified — no Arc GPU here.** The jitter
signs (`kJitterSignX/Y` in `Xess12.cpp`, `(-X, +Y)` like FSR) are derived, not yet measured in-game.

Managed side: `UpscalerKind { Off, Auto, DLSS, FSR, XeSS }` (`src\Upscaler.cs`), config field `Upscaler`. Auto
resolves from hardware facts before init (D3D11: NVIDIA → DLSS; D3D12: NVIDIA → DLSS, Intel → XeSS, else FSR when
the AMD DLLs are present, else XeSS when `libxess.dll` is, else Off); if the resolved provider fails `Dlss_Init`
under D3D12 with Auto, the mod calls `Dlss_Shutdown` and retries with the next one (`Upscalers.NextFallback`: FSR,
then XeSS). `RenderforgeMode` gained `UltraQuality` / `UltraQualityPlus` (appended, ordinal-serialised); the quality
row offers them only while XeSS is the resolved upscaler, DLSS/FSR run them as Quality.
Contract notes: `docs\research\fsr-ffx-api-d3d12-contract.md`, `docs\research\xess-d3d12-contract.md`.

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
  `ARGBHalf` still with `IsHDR=0` — this is exactly the `D3D12HalfColor` path: linear LDR FP16, NGX
  `IsHDR=0`, `DLSS_F_HDR` is informational only — FSR never sets `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE`
  and XeSS always sets `XESS_INIT_FLAG_LDR_INPUT_COLOR`, the values are display-referred 0..1; `IsHDR=1`
  without an exposure source was temporally unstable), `AutoExposure=0`. `InMVScale = (-renderW, -renderH)`
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
- RCA (live, PPCLI reflection): the washed-out look = PPv2 `PostProcessLayer` aborting every
  frame, which also drops `PhoenixPoint.Tactical.FogOfWar.FogOfWarPostProcess` and grading.
  Two compute shaders have no D3D12 kernels: `MultiScaleVODownsample1` (AO, MSVO mode) and,
  once AO is off, `KGenLut3D_AcesTonemap` (HDR ColorGrading 3D-LUT baker). Control: on D3D11
  `PostProcessLayer.enabled=false` reproduces the D3D12 look exactly. Verified fix on D3D12:
  AO `enabled=false` + `PostProcessResources.computeShaders.lut3DBaker=null` (→ LDR 2D-LUT
  path) → exceptions stop, dark cave + FoW back. Seam: `Base.Lighting.LightingManager
  .ApplyPostProcessOptions` (`LightingManager.cs:163-187`, `:173` `EnableEffect<AmbientOcclusion>`).
  `gradingMode` is re-blended from volume profiles every frame — patch resources/profile, not
  the bundle. Residual: `Mesh can not have more than 65000 vertices` (8x), 3 idle crashes seen
  during the RCA (cause unknown, measure after the fix). Full spec:
  `docs\superpowers\specs\2026-09-02-multi-vendor-d3d12-design.md`.
- Consequence: a D3D12 backend is viable → unlocks Frame Generation (DLSS-FG via Streamline,
  FSR FG, XeSS-FG — all D3D12-only) and official FSR 3.1/4 + cross-vendor XeSS (both D3D12-only
  in the current SDKs; the D3D11 XeSS DLL is Intel-Arc-only, FSR has NO official D3D11 backend).
  Native shim needs a D3D12 path: `GetNativeTexturePtr()` returns `ID3D12Resource*`, NGX D3D12
  entry points, resource-state transitions.

## Vendor SDKs on disk (downloaded 2026-09-02, all in gitignored `E:\DEV\PhoenixPoint\refs\`)

| SDK | Tag / date | Folder | Runtime DLLs (FileVersion, Authenticode Valid) |
|---|---|---|---|
| AMD FidelityFX SDK | v2.3.0 / 2026-06-24 | `refs\FidelityFX-SDK\` (shallow clone; release zip is samples-only) | `Kits\FidelityFX\signedbin\amd_fidelityfx_loader_dx12.dll` 2.3.0.2740; `amd_fidelityfx_upscaler_dx12.dll` 4.1.1.2740 (27 MB, FSR 4.1.1 + 3.1.5 fallback); `amd_fidelityfx_framegeneration_dx12.dll` 4.0.1.2740 (38 MB). Headers `Kits\FidelityFX\api\include\`, licence `docs\license.md` — **integrated in Phase 3** (upscaling only; frame generation stays Phase 5) |
| Intel XeSS SDK | v3.0.2 / 2026-07-24 | `refs\XeSS-sdk\` | `bin\libxess.dll` 2.0.2.68 (74 MB, D3D12 cross-vendor); `bin\libxess_fg.dll` 1.3.1.78 (22 MB); `bin\libxell.dll` 1.3.2.10. `inc\`, `LICENSE.txt` — **SR integrated in Phase 4** (`libxess.dll` only; `libxess_dx11.dll` is Intel-Arc-only and not shipped, FG stays Phase 5) |
| NVIDIA Streamline | v2.12.0 / 2026-06-23 | `refs\Streamline\` | `bin\x64\sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `sl.dlss.dll` — all 2.12.0.0. **SDK's `nvngx_dlssg.dll` is 310.7.0 = STALE**; ship `refs\Streamline\latest-dll\nvngx_dlssg.dll` 310.7.129.0 (7.5 MB, NVIDIA-signed, from the TechPowerUp FG DLL DB). `include\`, `license.txt` |

`refs\DLSS-sdk` `nvngx_dlss.dll` 310.7.129.0 is still the newest SR DLL. Rule (same trap twice
now): after any SDK update, compare every `nvngx_*.dll` FileVersion against the TechPowerUp DLL
databases and ship the newest NVIDIA-signed build, never the SDK copy blindly.

## Renderer switch (Phase 1, 2026-09-02)

- Config `DlssConfig.Renderer` (`RendererMode { Auto, DirectX11, DirectX12 }`, `Auto == DirectX11`) is the
  DESIRED API; `SystemInfo.graphicsDeviceType` (`RendererSwitch.Running`) is the running one. They differ
  only until the next launch.
- UI: three cloned `ArrowPickerController` rows under TEXTURE QUALITY, built by `src\Pickers.cs` —
  RENDERER, UPSCALER (Off/DLSS/FSR/XeSS), FRAME GENERATION (Off/2x/3x/4x). `GraphicsPanel` then places
  its DLSS QUALITY row and the SHARPNESS slider after them, so the order lives in one place.
- RENDERER is deferred like the panel's own settings: Harmony postfixes on
  `UIModuleGraphicsOptionsPanel.HasChanges` (`:124`) / `Apply` (`:137`) light and commit it, `Deinit`
  (`:107`) + `Init` (`:86`) reset the pending value from the config so a choice the user backed out of is
  never committed later. `HasChanges` = picker ≠ config OR (picker moved AND picker ≠ running API) —
  the second clause is what relights APPLY after a "No" to the restart dialog, when the config already
  says DirectX 12 but the process still runs D3D11.
- Apply → `RendererSwitch.Confirm`: the GAME'S dialog (`GameUtl.GetMessageBox().ShowSimplePrompt`,
  `MessageBox.cs:77`, `MessageBoxButtons.YesNo`). Yes → `RendererSwitch.Restart`; No → the row keeps the
  value and shows "(restart pending)". `ModSettingsFilter` scopes filtering to
  `UIModuleModManager.SelectModSettingsSection`; `DlssConfig.GetConfigFields` removes Renderer and the other
  regular Graphics/Screen controls only in that UI call. Mods → Renderforge exposes just overlay and hotkey
  preferences. Developer diagnostics are not config fields at all: `Diagnostics.Reset` selects the verified
  production path on every enable, and PPCLI can alter it only for the current diagnostic session. The loader's
  save/load calls still receive every player setting. Each player-facing setting has one UI and one apply path.
  The FPS setting is an output/presented ceiling: `RenderforgeMod.ApplyFrameRate` divides it by
  `FrameGen.OutputMultiplier` only while a provider is actually live, and reapplies the full ceiling immediately
  if FG stops or fails. Integer division rounds down so the requested output cap is never intentionally exceeded.
  Shot: `docs\shots\renderer-restart-dialog.png`.
- Relaunch = `powershell.exe -WindowStyle Hidden -Command "Wait-Process -Id <this pid>; Start-Process
  <exe> -ArgumentList '<current argv minus -force-d3d1*, plus -force-d3d12 when DX12>'"` then
  `Application.Quit()`. WHY the detour: the game is single-instance ("Another instance is already running"
  fatal), so a child started while this process is still tearing down dies at once; the hidden shell waits
  for the pid to vanish and only then starts the new one (~1 s after quit, no console flash with
  `CreateNoWindow`). Args are re-quoted by MSVC rules (`RendererSwitch.Quote`); DirectX 11 = no flag.
- PPModEnabler on GOG/Epic uses UnityDoorstop and sets `DOORSTOP_INITIALIZED=TRUE`. A child process
  inherits that marker; Doorstop then skips its entry point and the relaunched game has no mod loader.
  `RendererSwitch.PrepareChildEnvironment` removes `DOORSTOP_INITIALIZED` and `DOORSTOP_DISABLE` from
  the waiting PowerShell environment before it creates the new game, preserving all enabled mods.
- A normal store launch cannot carry the saved graphics flag. When config says D3D12 but the process runs
  D3D11, `RendererSwitch.ArmStartupRestart` reconciles it automatically once the MessageBox exists; no
  question appears merely because the game was launched. `RENDERFORGE_RENDERER_RESTART=1` is inherited by
  the child and suppresses a second automatic attempt if Unity did not honor the flag, preventing loops.
- Availability: `src\Availability.cs` is the ONLY place that decides whether DLSS/FSR/XeSS/FG can run and
  why not. `Reason(f) == null` means available; anything else is shown as the greyed value's tooltip
  (`UITooltipText`, the game's own component — never a custom overlay) and, for DLSS, in the overlay.
  Picking an unavailable UPSCALER snaps the row back to the applied value and leaves the reason as its
  tooltip. NVIDIA without DLSS (GTX) = `InitCode == DLSS_ERR_NOT_AVAILABLE` → "Requires an NVIDIA RTX GPU".
  Shot: `docs\shots\renderer-tooltip.png`.
- D3D12 PPv2 repair: `src\D3D12Fix.cs`, driven by the existing `LightingManager.ApplyPostProcessOptions`
  postfix and by `OnLevelStart`. It nulls `PostProcessResources.computeShaders.lut3DBaker` (reached through
  `PostProcessLayer`'s private `m_Resources`, `PostProcessLayer.cs:55`) so HDR ColorGrading routes to
  `RenderHDRPipeline2D` (`ColorGradingRenderer.cs:33,44` — still HDR, 2D LUT via pixel shader instead of
  the compute baker), and switches AO from MSVO (compute) to **SAO** (`ScalableAmbientObscurance`, pixel
  shader). SAO decision: AO kept, not disabled — the SAO tactical shot matches D3D11 (dark cave + fog of war)
  and `Player.log` has no kernel errors; `D3D12Fix.DisableAo` stays as the PPCLI-switchable fallback.
  Shots: `docs\shots\d3d12-tactical.png` vs `docs\shots\d3d11-tactical.png`.
- **D3D12 exposure restoration** (`assets/rf-exposure-d3d12.bundle`, 20992 B, shipped in the Core
  pack beside `Renderforge.dll`): PPv2 `AutoExposure` + `ExposureHistogram` compute shaders target
  renderer 2 (D3D11 only) and have no kernels on D3D12, leaving `autoExposureTexture` = White (1.0)
  vs the measured D3D11 value of 22.6273518 (+4.5 EV) — tactical scenes near-black. The bundle
  carries the same two shaders with identical DXBC/bindings, only `targetRenderer` 2 -> 18. Loader:
  `D3D12Fix.FixExposure` loads the bundle once from `RenderforgeMod.ModDir` and assigns the shaders
  only when the stock ones lack kernels AND the bundle ones report all 4 kernels. Safe failure = one
  WARN line, existing `HasKernel` guard path. Full evidence:
  `docs/research/2026-09-05-d3d12-exposure-restoration.md`.
- Under D3D12 the mod stays fully active (pickers, overlay, PPv2 fix) but the NGX init is skipped; the
  overlay says `Upscaler: off (DLSS on D3D12 comes in Phase 2)`.

## Frame generation (Phase 5, 2026-09-02) — D3D12 only

Three providers behind one seam (`native\Fg.h` `IFgProvider`): `FgFsr.cpp` (FidelityFX FG, analytical
3.1.6 pinned via `ffxOverrideVersion`, FG-swapchain 3.1.7, 2x), `FgXess.cpp` (XeSS-FG 1.3.1 + mandatory
XeLL 1.3.2, `InitFromSwapChainDesc`, `BACKBUFFER_HUDLESS`, 2x on non-Intel), `FgStreamline.cpp` (DLSS-G/MFG
via Streamline 2.12 manual hooking, `nvngx_dlssg.dll` 310.7.129, Reflex `eLowLatency` + 6 PCL markers,
proxy device/queue/factory, per-frame token FIFO, CPU-wait copy fence, 2x-4x on RTX 50). Auto picks
DLSS-G on NVIDIA, FSR-FG elsewhere; `RenderforgeMod.SetFgProvider` forces one for testing.

### Child-HWND contract

- **HWND chain on Unity's window = `E_ACCESSDENIED` (`0x80070005`)** — DXGI refuses a second flip-model
  chain on a window that already owns one (measured on Instance3, every retry).
- **Composition chain works only with Unity's Present forwarded** — without it, `GetCurrentBackBufferIndex()`
  freezes, debug layer `id=907` -> device removal (measured).
- **Shipped design = child HWND** (`FgWnd.cpp`): subclassed Unity WndProc (once, never restored),
  `HTTRANSPARENT` child (`WS_CHILD|WS_VISIBLE`, `CS_OWNDC`, `WS_EX_NOPARENTNOTIFY`), parent gets
  `WS_CLIPCHILDREN`. Each vendor SDK creates its own swapchain on this child.
- Exactly one Unity Present per frame (hook always forwards, sync 0, `ALLOW_TEARING` only).
- `RENDERFORGE_FG_CHAIN=composition` falls back to DComp visual + manual-dispatch FSR.

### Per-vendor summary

| Provider | SDK versions | Swapchain | Latency | Caps | Notes |
|---|---|---|---|---|---|
| FSR-FG | FG-swapchain 3.1.7 / model 3.1.6 | SDK proxy on child HWND (manual dispatch fallback for composition) | none | 2x | Paced by SDK's present thread |
| XeSS-FG | `libxess_fg.dll` 1.3.1 + `libxell.dll` 1.3.2 | `InitFromSwapChainDesc` on child, `pApplicationSwapChain=NULL` | XeLL mandatory, 6 markers | 2x off Intel Arc | `maxSupportedInterpolations=1` on non-Intel |
| DLSS-G | Streamline 2.12 + `nvngx_dlssg.dll` 310.7.129 | proxy factory `CreateSwapChainForHwnd` on child + proxy device queue | Reflex `eLowLatency` + 6 PCL markers | 2x-4x on RTX 50 (`numFramesToGenerateMax=5`) | NVIDIA focus gate: unfocused -> real only, no error |

### Per-frame sequence

1. Main thread: `FrameGen.Apply` -> `Fg_Init` (once) -> `Fg_SetFrame` (Unity RTs retained, jitter, camera, dt).
2. Render thread, `DLSS_EV_FG_PREPARE` (`CameraEvent.AfterEverything`): `FgHostPrepare` -> `provider.Prepare`
   on a recording command list submitted through `ExecuteCommandList` with state arrays. Providers read the
   shim-owned twins (`FgOwned12()`, all resting in COMMON), never the Unity RTs.
3. Render thread, Present hook: `FgHostOnPresent` copies Unity backbuffer (PRESENT -> COPY_SOURCE -> PRESENT)
   into shadow chain -> `provider.Generate` (FSR manual path only) -> `provider.BeforePresent` (DLSS: markers
   after copy fence wait) -> shadow `Present` (vendor paces internally) -> `provider.AfterPresent` (XeSS/DLSS:
   end markers) -> Unity's original `Present` (sync 0, ALLOW_TEARING).

### HUD

The present path gives every SDK what it needs for free: `outRT` = upscaled scene WITHOUT the HUD, backbuffer =
same frame WITH it. FidelityFX's `HUDLessColor`, XeSS's UI mode 4 (`BACKBUFFER_HUDLESS`) and DLSS-G's
`kBufferTypeHUDLessColor` + `enableUserInterfaceRecomposition`. No UI render target is built.

### Overlay

Real fps counted in `Update`; presented fps counted in the Present hook (`FgPresentedFps()`). Overlay shows
`FPS: 62 / 118 (16.1 ms)` and a `FG:` line naming provider and multiplier.

### Lifecycle / teardown

- Init (main thread, render idle): `FgHostInit` -> `FgWndCreate` (child on parent thread via `WM_APP+0x51`)
  -> provider `Create` -> `FgHostSetEnabled(1)`.
- Teardown triggers: `ResizeBuffers` hook, shadow Present failure, `SetFullscreenState` (exclusive fullscreen
  -> FG torn down), `WM_NCDESTROY`, display change, manual `SetFrameGen(Off)`.
- Mission loads: `BeginRelease` -> `FgHostSetEnabled(0)` -> teardown; `FrameGen.Retry()` rebuilds once the
  new camera is live. Verified across 3 consecutive loads.
- Per-vendor destroy: FSR `Configure(enabled=false)` + `WAIT_FOR_PRESENTS` + context destroy; XeSS
  `xefgSwapChainDestroy` (host releases shadow ref first) + `xellDestroyContext`; DLSS `eOff` before
  chain release, `slInit` stays for the process.
- `SRWLOCK` guards state; render thread pins/unpins around Prepare/OnPresent; teardown waits for unpin.

### Picker / availability

- Greyed rows stay visible with native tooltip reasons (`UITooltipText`).
- 3x/4x grey with "Not supported by this GPU" from `Fg_Caps()` when the provider's caps lack them.
- Missing DLLs -> "DLL missing: <name> -- install the <Vendor> pack" (EN/RU).
- `Availability.Reason(Feature.FrameGen)`: requires D3D12, requires an upscaler on.
- Env knobs: `RENDERFORGE_FG_CHAIN=composition`, `RENDERFORGE_FSR_JITTER_SIGN`,
  `RENDERFORGE_XESS_JITTER_SIGN`, `RENDERFORGE_D3D12_DEBUG`.

### Known limits

- Windowed/borderless only; exclusive fullscreen -> FG torn down.
- NVIDIA focus gate: unfocused window -> real frames only, no error, no log. Production plugin requires
  the window in the foreground — the normal state when a player plays.
- Frame-pacing metric (CoV of `MsBetweenDisplayChange` via PresentMon) unmeasured.
- Debug layer: 0 mismatches on our lists/resources; remaining `id=527/538/1315` are Unity's own.
- Full contract note: `E:\DEV\PhoenixPoint\docs\research\framegen-d3d12-contract.md`.

## Packaging (Phase 6, 2026-09-02)

- `build\release.ps1` is the only packaging path. It reads the version from `meta.json`, walks an
  ordered pack table, and emits `Renderforge-{Core,NVIDIA,AMD,Intel,Full}-<v>.zip` plus
  `SHA256SUMS.txt` into `build\release\`.
- **Layout rule:** every zip has ONE top-level `Renderforge/` folder and is extracted into
  `<Phoenix Point>\Mods\`. Core + any vendor packs overlay into a single `Mods\Renderforge\` in any
  order. (1.0.0's zip was flat and extracted INTO `Mods\Renderforge\`; 1.1.0 changed it, because an
  overlay cannot work without the prefix.)
- Pack contents: **Core** = `Renderforge.dll`, `RenderforgeNative.dll`, `rf-exposure-d3d12.bundle`,
  `meta.json`, `README.md`, `LICENSE`, `LICENSE-NIS.txt`. **NVIDIA** = `nvngx_dlss.dll` + `LICENSE-NVIDIA.txt` (with
  `-WithFrameGen`: `nvngx_dlssg.dll` 310.7.129 and `sl.{interposer,common,dlss,dlss_g,reflex,pcl}.dll`
  2.12.0). **AMD** = `amd_fidelityfx_{loader,upscaler}_dx12.dll` + `LICENSE-AMD.txt` (with
  `-WithFrameGen`: `amd_fidelityfx_framegeneration_dx12.dll`). **Intel** = `libxess.dll` +
  `LICENSE-INTEL.txt` (with `-WithFrameGen`: `libxess_fg.dll`, `libxell.dll`). **Full** = the union.
- Each zip also carries `manifest-<pack>.json`: mod id, version, generation timestamp, and per file
  the name, FileVersion, byte size, SHA-256, required Authenticode signer and licence file. Per-pack
  names, not one shared `manifest.json`, because vendor packs are extracted on top of Core.
- Every vendor DLL is Authenticode-asserted before packing — subject must contain `NVIDIA Corporation`
  / `Advanced Micro Devices` / `Intel Corporation`, status must be `Valid`; anything else fails the
  build. NVIDIA reports FileVersion with commas (`310,7,129,0`), so all comparisons normalise
  `-replace '[ ,]', '.'`.
- Stale-DLL guard: `$NewestKnownNgx` pins `nvngx_dlss.dll` and `nvngx_dlssg.dll` at `310.7.129.0`;
  a mismatch is a WARNING telling the operator to check the TechPowerUp DLL databases. Warning, not
  error — a newer DLL is legitimate, it just has to be a deliberate choice.
- Missing vendor DLLs are a supported state at runtime: `Availability.Reason` returns
  `DLL missing: <name> — install the <Vendor> pack` (RU: `Нет файла: <name> — установите пакет
  <Vendor>`), the row greys out, and everything else keeps working.
- Release checklist: `docs\RELEASING.md`. GitHub release and Steam Workshop upload are user-gated.
- Steam Workshop item: **TBD — filled in by the Workshop publish task**. Uses PerkOracle's
  SteamworksPy publisher (`PerkOracle\docs\OPERATIONS.md`), appid 839770, content = the Full pack.

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
