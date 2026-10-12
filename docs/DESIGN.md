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
| DLSS SR / DLAA | 0 | D3D11 + D3D12 | `Device11`, `Device12` (NGX) | DLSS SDK 310.9.1, `nvngx_dlss.dll` 310.9.1 | DLAA, Quality, Balanced, Performance, Ultra Performance | the shim's own NIS pass (RCAS fallback) |
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
signs (`kJitterSignX/Y` in `Xess12.cpp`) are `(+1, +1)` on the driver's NGX-convention offsets, measured 2026-10-12
(`docs\research\xess-d3d12-contract.md` §5).

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
     History reset (1.6.4): a position jump > 50 units or a per-frame FOV change > 5 % (cut / zoom snap); the 1.6.3
     `Mathf.Approximately` FOV test reset on every frame of a Cinemachine blend / `FrameModifier` lens animation.
  2. `CommandBuffer` at `CameraEvent.BeforeImageEffects` (depth/MV transients are alive there):
     Only while MV/depth are consumed (1.6.4, `DlssDriver.NeedsMvDepth`): a real upscaler feature, live frame
     generation, or the Depth / MotionVectors debug view. A passthrough generation (upscaler Off with sharpen / LUT /
     grade / style) neither forces `Depth | MotionVectors` (the MV pass re-draws every skinned renderer) nor adds this
     buffer; it follows FG coming up or going mid-generation. Status: `mvCopy=`.
     `CopyTexture(BuiltinRenderTextureType.MotionVectors → mvRT RGHalf)` (same size/format,
     no resample, no Y flip); depth: material-less `Blit(<depth source> → depthRT RFloat)` point-sampled, no
     conversion (`DepthInverted` flag from `SystemInfo.usesReversedZBuffer`). The source follows the camera's
     `actualRenderingPath` (`DlssDriver.BuildCopy`): `ResolvedDepth` under deferred, `Depth` under forward — see
     "Scene depth, D3D12 frame pairing, jitter signs (2026-10-12)".
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
- Auto mode table (output height): ≤1200 → DLAA; ≤2160 → Quality; else → Balanced (2026-10-12: was ≤1600 → Quality,
  else Performance - 1080p → 4K read as blurry; quality before fps by default).
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
    (`nis_scaler_hlsl.h`); the build-time tool `native\shadergen\rf_shadergen.cpp` prepends the
    `NIS_Main.hlsl` bindings (`NIS_SCALER 0`, `NIS_HDR_MODE 0|1`, cbuffer `b0`, `samplerLinearClamp s0`,
    `in_texture t0`, `out_texture u0`) and an `NVSharpen` entry and compiles `cs_5_0` with `D3DCompile`
    (O3). 1.6.4: every post-pass variant (NIS LDR, NIS HDR-linear, analytic grade; HLSL in
    `native\SharpenHlsl.h`) is compiled at BUILD time into `sharpen_dxbc.h` - nothing compiles on the
    render thread, and D3D11 shaders / D3D12 PSOs stay resident per variant, so a LUT/style/colour-vision/
    grade toggle switches without a recompile or a GPU wait (was a 100+ ms hitch). Block/group = `NISOptimizer(isUpscaling=false, NVIDIA_Generic)`
    from `native\nis\NIS_Config.h`: 32×32 px per block, 128 threads → dispatch ⌈w/32⌉×⌈h/32⌉.
    Constants = `NISConfig` filled by `NVSharpenUpdateConfig(cfg, s, 0,0,w,h, w,h, 0,0)`
    (256 B, aligned), slider/100 passed straight in (NIS' own 0..1 slider). Runs inside event 2
    right after a successful `NGX_D3D11_EVALUATE_DLSS_EXT`, in place on the output UAV, reading an
    SRV scratch copy (`CopyResource(scratch, output)`; in-place read+write is a hazard). Views are
    dropped on event 3 (before the driver frees RTs).
  - AMD FidelityFX **RCAS** (from the public formula, epsilon-guarded, denoise on; mapping
    `con = exp2(-2·(1−s))`) lives only inside the analytic grade shader now; the standalone RCAS fallback
    for "NIS fails to compile" was dropped in 1.6.4 (a compile failure now fails the build).
    `Dlss_Sharpener()` → 1 NIS / 2 RCAS(grade) / −1 failed, shown as `sharpen=` in `DlssDriver.Status`.
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
  **1.6.4 sweep cache:** every texture is classified once per instance id (skip / normal / UI; `name`, `mipmapCount`
  and the Sprite scan only on first sight). A sweep writes unseen textures, plus the normal or UI class only when its
  bias differs from what it last wrote, so the level-start and +2 s re-sweeps stop rewriting every texture (the
  9–15 ms hitch). `Resweep` (UI pin toggled) clears the cache; `Sprite_Create_Patch` marks the textures it pins as UI.
  Log: `MipBias: bias=… applied to N textures (… new A, unchanged K, skipped S[, full]) in T ms`.
- **Benchmark overlay** (`src\Overlay.cs`, `ShowOverlay=false`, `OverlayPosition=TopCenter` of
  `TopLeft|TopCenter|TopRight|BottomCenter`): one ScreenSpaceOverlay canvas, sortingOrder 30000,
  no GraphicRaycaster, `raycastTarget=false`, HUD font (first `Text` found) else Arial, size
  14·Screen.height/1080, 6 px margin. Lines: upscaler (nvngx file version via `version.dll`;
  Mono's `FileVersionInfo` is empty for native DLLs), mode + `render -> out`, AA (DLSS / live
  `PostProcessLayer.antialiasingMode`), FPS = 0.5 s moving average, refreshed 4×/s.
  TopCenter is empty in tactical and geoscape; TopLeft covers the objectives title and
  BottomCenter the ability-bar key labels in tactical — user's choice, not a default.

### Colour vision (daltonization, 1.4.0)

Fixed-strength daltonization stage in the analytic post shader, after `Grade()` and `Stylize()`:
`ColorVision(Grade(Stylize(p,c)))` at `native\Sharpen.cpp:74`. Tactical missions AND the geoscape (1.5.0;
was tactical-only through 1.4.1 via `RenderforgeMod.TacticalActive`, deleted — the post pass runs on the one
scene camera in both states, see line 17). `ColorVisionPanel.Active` = mode in Deuteranopia..Tritanopia, nothing
else; the LUT is gated the same way in `DlssDriver.Step` (`lutActive`).

- **Formula.** `D = I + R·(I − S)`, applied column-vector in linear RGB (`v' = D·v`), stored row-major.
  `S` = Machado, Oliveira & Fernandes 2009 simulation matrices at severity 1.0, from the authors' own
  table (https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html) — verified
  exact match. `R` = error redistribution:
  - **protanopia:** Fidaner, Lin & Ozguven 2005 `err2mod` (`conv_img.m`), which corrects `errorp` only.
  - **deuteranopia:** the same `err2mod`, on the authority of `daltonize/daltonize.py:125` (maintained
    Python port, https://github.com/joergdietrich/daltonize), where the single matrix is applied for
    every `color_deficit` value; the type argument only selects the `simulate()` matrix (`:120`). The
    ixora distinct deutan R (`1 0.7 0 / 0 0 0 / 0 0.7 1`) was rejected — it rests on the same
    unverified secondary table as the tritan row.
  - **tritanopia:** per-deficiency redistribution from the matrix table at
    https://ixora.io/projects/colorblindness/color-blindness-simulation-research.html. **Secondary
    source, coefficients NOT independently verified.** The page cites Simon-Liedtke & Farup (JVCI 2016)
    as justification for using a per-type matrix; it is not the published origin of these numbers.
    Used because `err2mod` would push the tritan error onto B — the channel a tritan cannot
    discriminate.
- **Matrices** live in `native\ColorVision.h` (constexpr `CvCorrection(mode)`). Every row of every `D`
  sums to 1 within 1e-6, so neutral grey/white is a fixed point.
- **Constant buffer.** `uint colorVision` at byte 40, `float4 cvRow0/cvRow1/cvRow2` at 48/64/80 of
  the existing 256-byte block (`native\Sharpen.cpp`). The three float4 rows are 16-byte aligned; no
  member straddles a register boundary.
- **Two colour-space branches** in the HLSL, keyed on `styleLinear` (the same uniform the LUT and style
  stages branch on):
  - UNORM path (`styleLinear == 0`): `saturate` → exact piecewise sRGB decode → `D·v` → `saturate` in
    linear → piecewise sRGB encode.
  - FP16-linear path (`styleLinear != 0`, the D3D12 half-colour allocation): `max(0)` → `D·v` →
    `max(0)`, no encode — overbrights preserved.
  Mode 0 is a uniform-branch bit-exact bypass (no decode/encode, no clamp).
- **Activation predicate.** `PostShaderEnabled(preset, strength, style, colorVision)` at
  `native\Sharpen.h` replaces the former two-term `ColorGradeEnabled || SceneStyleEnabled` at all 11
  native call sites (`Sharpen.cpp`, `Device11.cpp`, `D3D12Sharpen.h`, `Device12.cpp`, `Fsr12.cpp`,
  `Xess12.cpp`). Without this, LUT=Off + style=Off + sharpness=0 would bypass the post pass and
  silently skip the correction.
- **Managed side.** `ColorVisionMode` enum (`src\DlssConfig.cs`), `Dlss_SetColorVision` native export
  (`src\Native.cs`), `ColorVisionPanel` picker row (`src\ColorVisionPanel.cs`) cloned from the LUT
  recipe, placed between LUT and Scene style in Options → Graphics. Console setter:
  `RenderforgeMod.SetColorVision(<mode>)`.
- **Probe.** `native\probe\colour_vision_probe.cpp` — D3D11 WARP, production HLSL, 3 modes, 4913-colour
  cube + 196 sRGB-knee samples + 8 gamut corners on both colour-space paths, matrices verified against
  the independent Python reference (`native\probe\colour_vision_ref.py`), LUT+style composition on
  both paths.

### Levels / Contrast / Clarity (1.5.0, ReShade parity)

Three more stages of the same analytic post shader (`native\Grade.h` `RF_GRADE_HLSL`, function `Adjust(p, c)`),
one dispatch as before. Final chain in `native\Sharpen.cpp`: RCAS → `Stylize` → `Grade` (LUT) → **`Adjust`
(Levels → Contrast → Clarity)** → `ColorVision` — the daltonization compensation stays last so it corrects what the
player actually sees. Runs wherever the pass runs (tactical + geoscape); the HUD is composited after it.

- **Knobs** (`DlssConfig`, ints, all default = off): `LevelsBlack` 0–40 (0), `LevelsWhite` 215–255 (255),
  `Contrast` 50–150 (100), `Clarity` 0–100 (0). Sliders = `src\GradePanel.cs` (four ShadowDistance-row clones,
  the LUT-strength recipe), immediate apply + `SaveConfig`; the driver reads them every frame, no generation
  restart. Console setter `RenderforgeMod.SetGrade(black, white, contrast, clarity)`; `-1` for any argument keeps
  that knob's current value (`SetGrade(-1,-1,-1,100)` = A/B one knob).
- **ABI.** `Dlss_SetGrade(slot, black, white, contrast, clarity)` — same slot contract as `Dlss_SetColorVision`
  (after `Dlss_SetFrame`, before the event). Values arrive normalised: black = slider/255 (0..40/255), white =
  slider/255 (215/255..1), contrast = slider/100 (0.5..1.5), clarity = slider/100 (0..1); out of range or NaN =
  that knob's Off. **Zero is Off by construction:** `Dlss_SetGrade` re-encodes into `GradeParams { black,
  whiteDrop = 1 − white, contrastDelta = contrast − 1, clarity }`, every field 0 = off, so an all-zero struct
  (`Dlss_SetFrame`'s memset), a default `GradeParams{}` and a zero-filled constant block (the NIS/RCAS layouts,
  a probe that never packs the grade) are all the bit-exact bypass — no reset step to forget. `GradePanel.Active`
  = any knob off default; it is a third term in `DlssDriver.Step`'s `needsPipeline` and `GradeEnabled` (any field
  `!= 0`) a fourth term in `PostShaderEnabled` (no default argument there, so a call site that forgets it fails
  to compile instead of skipping the stage).
- **Constant buffer.** One `float4` appended at byte 96 (`fp[24..27]`) after `cvRow2`: `levelsBlack` @96,
  `whiteDrop` @100, `contrastDelta` @104, `clarity` @108 — 112 of the 256 bytes used, both backends already
  allocate 256 (D3D11 `ByteWidth = 256`, D3D12 root CBV 256 B per ring slot; no root-signature change).
- **Formulas** (display-referred on both colour-space paths — the FP16-linear path goes through the same
  `pow(2.2)` pair as `Stylize`; only negatives are clipped, UNORM clamps on store, FP16 keeps overbrights):
  - Levels: `d = max((d - b) / max(1 - wd - b, 1e-4), 0)` per channel, b = black/255, wd = whiteDrop
    (= 1 − white/255).
  - Contrast: `d = max((d - 0.5) * (1 + cd) + 0.5, 0)` per channel, cd = contrastDelta (= contrast/100 − 1) —
    the same per-channel form the LUT presets use (`(g-0.5)*con+0.5`), not a luma-preserving variant.
  - Clarity: luma unsharp mask. `y` = luma of the input at s, `blur` = mean luma of s plus 12 Poisson-disc taps
    (13 taps, equal weights) read from the SAME input SRV (`src`, t0 — the scratch/target copy, never the UAV),
    radius `r = 10 px × H/1080`, offsets `int2(round(tap × r))` through `L()`'s bounds clamp (`.Load`; the pass
    declares no sampler). Gain `d *= 1 + 0.6 × clarity × clamp((y − blur) / max(y, 1e-3), −1, 1)` — both
    lumas come from the input so a preceding grade/levels shift cannot bias the mask; slider 100 = ±60 %.
    `s = p`, except under an active PixelArt style where `s = StyleBlockCentre(p)` (the same snap `Stylize`
    draws the block's one sample from, `SceneStyle.h`): the mask is then one value per block, so Clarity cannot
    re-introduce per-pixel detail inside the blocks (`scene_style_probe` asserts 0 non-uniform pixels at
    Clarity 100 + 4-px blocks, gamma and linear). Luma (`ClarityLuma`) = `StyleLuma(max(L, 0))` then ONE scalar
    `pow(·, 1/2.2)` on the linear path — luma-of-linear-then-gamma is not gamma-then-luma, but the mask only
    needs a monotonic brightness estimate, and it is 13 scalar pows per pixel instead of 39 (three per tap).
  - Each stage branches on its uniform (`levelsBlack != 0 || whiteDrop != 0`, `contrastDelta != 0`,
    `clarity != 0`) and the whole function early-outs when all four are zero: bit-exact bypass, no divergence.
- **Config writes are debounced (1.6.4).** `RenderforgeMod.SaveConfig()` only marks the config dirty and stamps the
  time; `ConfigSaver` (a `HideAndDontSave` MonoBehaviour created on the first call) writes in `LateUpdate` once no
  change came for 0.5 s (`SaveModConfig` serialises every mod's config, so the 1.6.3 once-per-frame write hitched a
  slider drag). `FlushConfig()` writes at once on `OnApplicationQuit`, `OnModDisabled` and `OnLevelEnd`. A slider drag
  = one `SaveModConfig` (PPCLI: `RenderforgeMod.ConfigWrites` goes up by 1).
- **Verified:** `build-native.ps1` green (the production HLSL compiles in the `--fake=2` post-only probe:
  `grade=32711` pixels differ from the copy); managed build 0/0; `qgate -All -Full` green. In-game per-knob
  screenshots and the 1440p frame-time delta are the acceptance step still owed (plan Track C).

### Exposure / Brightness / Vibrance / Saturation + Reset (1.6.0)

Four more stages of the same `Adjust(p, c)` (`native\Grade.h`), inserted around the 1.5.0 ones so the chain is
**Exposure → Levels → Brightness → Contrast → Clarity → Vibrance → Saturation**, still after `Grade` (LUT) and before
`ColorVision` in `Sharpen.cpp` (the existing stages did not move). Same display-referred maths, same `!= 0` bypass per
stage, same early-out when all eight uniforms are zero.

- **Knobs** (`DlssConfig`, ints, identity by default): `Exposure` -40–40 (0, tenths of EV), `Brightness` -100–100
  (0), `Vibrance` -100–100 (0), `Saturation` 0–200 (100). Rows = `GradePanel.Knobs` in chain order; the UI shows the
  raw integers. `Active` = any of the eight knobs off its default. Console: `RenderforgeMod.SetImage(exposure,
  brightness, saturation, vibrance)` (all four written — negatives are legal, so no keep sentinel) and
  `RenderforgeMod.ResetImage()`.
- **ABI.** `Dlss_SetGrade(slot, black, white, contrast, clarity, exposure, brightness, saturation, vibrance)` —
  exposure -4..4 EV, brightness -1..1, saturation 0..2, vibrance -1..1, out of range / NaN = Off. Re-encoded zero-is-off:
  `GradeParams { …, exposure, brightness, saturationDelta = saturation − 1, vibrance }`. Constant buffer: a second
  `float4` at byte 112 (`fp[28..31]`) — 128 of 256 bytes used, no layout change on either backend.
- **Formulas** (d = display-referred value, luma = Rec.709 `StyleLuma`):
  - Exposure: `d *= exp2(EV / 2.2)` — 1 EV is ×2 in LINEAR light; on the gamma-encoded value that is `2^(EV/2.2)`
    (`pow(c·2^EV, 1/2.2)`), identical on both colour-space paths. A plain `×2^EV` on the display value would have made
    +1 EV ≈ ×4.6 linear.
  - Brightness: `d = pow(max(d, 0), exp2(-B))` — midtone gamma, endpoints fixed; +1 → exponent 0.5, −1 → 2.
  - Vibrance: `sat = (max(d) − min(d)) / max(max(d), 1e-4)`, `d = lerp(luma, d, 1 + V·(1 − sat))`.
  - Saturation: `d = lerp(luma, d, S)`; S = 1 is the bit-exact bypass (`saturationDelta == 0`).
- **Reset row** (`GradePanel.BuildReset`): `UIModuleGraphicsOptionsPanel` ships no standalone button, so the row is a
  `TextureQualityPicker` clone (the prefab every Renderforge picker row uses) with `PreviousArrow`/`NextArrow`
  hidden, `Title` = "Reset image settings" (label left like every row), `CurrentItem` = the short verb "Reset"
  (the long label wrapped and clipped inside the button) and `CentralButton.PointerClicked` assigned
  directly (`ArrowPickerController.Init` is NOT called — it binds the central button to `NextOption`,
  `ArrowPickerController.cs:45-48`). Click = `GradePanel.ResetAll`: Sharpness 40, LUT strength 100, the eight knobs
  to their defaults, `SaveConfig` + `ApplyLutSettings`, then `GradePanel.Sync` / `GraphicsPanel.SyncSharpness` /
  `LutPanel.Sync` repaint the rows; one `LogInfo` line. Immediate like every Renderforge slider (no Apply flow); the LUT
  filter, colour vision and scene style selections are untouched. Every image tooltip now ends in `Default: N.`

### Crisp fonts (1.4.x) — REMOVED in 1.5.0

- Measurements 2026-09-07 (`docs\research\2026-09-05-crisp-ui-measurement.md`): Sobel ON/OFF = 1.00 at 1440p (glyphs
  already rasterise 1:1 at `fontSize × scaleFactor`), only 13/34 labels passed the glyph-mapping gate, ~600 LOC, and
  1.4.1 shipped a glyph-drift regression. Code, probes, UI row and strings deleted; `DlssConfig.CrispFonts` stays as
  a hidden legacy key so old `ModConfig.json` files round-trip (`OnModEnabled` logs once when it is `true`).

### Crisp icons — evaluated and dropped (2026-09-07)

- Trilinear + aniso 8 on the UI atlas and `mipMapBias` −0.5/−1.0 were built, measured at 2560x1440 and deleted before
  release: strict pixel diff = frame noise (`QualitySettings.anisotropicFiltering` is already `ForceEnable`; mip LOD
  0.585 blends mips that differ by 2–4/channel). Numbers, atlas inventory and rejected levers:
  `docs\research\2026-09-07-ui-sharpness-measurements.md`.

### Pixel-perfect UI (1.5.1; UI texture pin + default On, 1.6.1)

- `DlssConfig.PixelPerfectUi` (default **On** since 1.6.1; a saved `false` survives — `ModConfig.LoadFromRawConfig`
  overwrites every field present in the profile's `ModConfig.json`, verified live on Instance2: saved `false` + new
  default `true` → `Cfg.PixelPerfectUi == false` after load), row `RenderforgePixelPerfectUi` under Options → Screen
  (`src\VideoPanel.cs`, pending/HasChanges/Apply like the FPS rows). `src\PixelPerfectUi.cs` (rewritten 2026-09-15):
  the snap is done by two Harmony postfixes, `Graphic.GetPixelAdjustedRect` (Image quads: simple/sliced/sprite-mesh)
  and `Graphic.PixelAdjustPoint` (UI.Text rounding offset), with OUR OWN math (`SnapPoint`: local →
  `TransformPoint` → `Mathf.Round` world xy → `InverseTransformPoint`; a ScreenSpaceOverlay root canvas lives in pixel
  units in world space, so that IS the pixel snap; accepted only if the round trip lands within 0.01 px, so the
  `localScale.z = 0` prefabs keep the unsnapped value) for graphics whose root canvas is `ScreenSpaceOverlay` (same
  guards as UGUI's own: canvas present, `scaleFactor != 0`; a canvas that already has `pixelPerfect` is left to UGUI).
  NOT via `RectTransformUtility.PixelAdjustRect/Point`: those natives read `canvas.pixelPerfect` themselves and are
  identity while it is off (live-probed, `snap-experiments.md`: `PixelAdjustPoint(0,0)` = (0,0) off, (−0.495,−0.495)
  on) — the first attempt through them measured exactly the "mip0 alone" bleed row (`verify.md`). **`Canvas.pixelPerfect`
  is never set any more**: UGUI 2019.4 reads that flag in exactly those two managed methods (ilspy of
  `UnityEngine.UI.dll`), and everything else the flag enables is native per-frame canvas work that measured
  0.5–0.6 ms/frame even on an idle screen — 336 vs 399 fps geoscape idle, 287 vs 345 roster at 1440p, growing to
  ~1.65 ms under geoscape fast-forward (`docs\research\pixelperfect-motion-cost-2026-09-15\results.md`; the pin
  measured 0). With the postfixes the snap costs only at mesh rebuild. `Apply(on)` flips a static and
  `SetVerticesDirty()`s every scene `Graphic` once (log `Pixel-perfect UI on|off: N graphics rebuilt`); graphics
  created later build with the snap on their first mesh, so there is no per-level re-apply. The empty-rect fallback
  for singular matrices (`localScale.z = 0` prefabs) lives in the same postfix.
- **UI texture pin (1.6.1)** — `PixelPerfectUi.Apply(on)` sets `MipBias.UiPin` and calls `MipBias.Resweep()`. In the
  sweep, textures behind any `Sprite` (`Resources.FindObjectsOfTypeAll<Sprite>()`, distinct `.texture`) get
  `min(dlssBias, log2(uiScale))` where `uiScale` = the smallest `scaleFactor` over root overlay canvases (the game's
  3840x2160 reference → 0.6667 at 1440p → −0.585; a mod canvas with a 1080p reference scales 1.33 and never wins;
  pin clamped to ≤ 0, so 4K = 0). Everything else keeps the DLSS bias. `MipBias.Reapply` (level start, +2 s after a
  generation) sweeps when the pin is on even at bias 0. Harmony postfix `Sprite_Create_Patch` (`src\Patches.cs`) on
  the 8-argument `Sprite.Create(Texture2D, Rect, Vector2, float, uint, SpriteMeshType, Vector4, bool)` — every shorter
  overload chains into it (verified: a sprite made through the 3-argument overload came back with −0.585) — pins
  textures a mod creates after the last sweep. Log: `MipBias: bias=0.000 applied to N textures (ui=-0.585 on M sprite
  textures, skipped K)`.
- **Why the pin:** `docs\research\icon-bleed-2026-09-08.md`. A 64² icon inside an unpadded runtime atlas (or a
  Repeat-wrapped full-rect texture) minified 0.667x by the canvas samples mip 1 (`MIN_MAG_LINEAR_MIP_POINT`), whose
  kernel reaches 0.125 texel past the sprite rect → a 1 px line of the neighbour, magenta intensity 138/255; at mip 0 a
  fractional quad edge still reaches it (118). Measured at 2560x1440: base 138/136/0, mip0 alone 118/115/0,
  pixelPerfect alone 61/61/61, mip0 + pixelPerfect **0/0/0** at every fractional position, both synthetic cases;
  vanilla atlases (SpriteAtlas padding) never bled (control 104 flat in every mode) and are merely sharpened. DLSS
  Quality hid the bug by coincidence: its bias −0.585 = log2(0.667); DLAA/Off wrote 0 → bleed.
- **Why optional:** `docs\research\font-remeasure-2026-09-07\results.md` — at 2560x1440 UI.Text Sobel +2–7%
  (digits 1.05–1.06, `time 00:00` 1.07), but elements shift onto the pixel grid (sub-pixel snap; pixel-value maxDelta up to 180 in the diff), so animated
  panels step by whole pixels. The only UI lever that measured above noise; Crisp fonts (1.00) and Crisp icons were dropped.

### Resolution list dedup (1.6.0)

- Vanilla `UIModuleVideoOptionsPanel.InitResolutionPicker()` (decompile `PhoenixPoint.Common.View.ViewModules\UIModuleVideoOptionsPanel.cs:125`,
  `private void InitResolutionPicker()`) stores `_resolutions = Screen.resolutions` (:133) — every size once per refresh
  rate — then builds labels (`Resolution.ToString()`), finds the current index and calls
  `ResolutionPicker.Init(_resolutions.Length, num, OnResolutionChanged)` (:149); `OnResolutionChanged` (:153) maps
  index → `_resolutions[index]`.
- `src\VideoPanel.cs`: Harmony transpiler on that method swaps the single `Screen.resolutions` getter call for
  `VideoPanel.UniqueResolutions()` — GroupBy (width,height), keep the highest `refreshRate`, ordered by width then
  height. The game's own loop then runs on the short array, so range, labels, initial selection and index mapping
  stay consistent by construction. Labels keep Unity's `w x h @ NHz` format.
- Refresh rate was never applied: `OptionsManager.cs:523` calls `Screen.SetResolution(w, h, mode)` without one; the
  frame-rate limiter owns pacing. Log line: `Resolution list: N entries -> M unique sizes`.

### Geoscape markers after reconstruction (2026-09-07)

- **Symptom:** every temporal upscaler trails the globe markers when the geoscape rotates
  (`docs\research\geoscape-dlss-2026-09-07\results.md`; the bias-current-colour mask is ignored by preset K,
  `bias-mask\results.md`). **Correction to that diagnosis:** the markers are NOT the ~48 WorldSpace "PopUp" canvases
  (those are empty, 0x0, no Graphic). Each `GeoSite` carries a `GeoSiteVisualsController` (decompile
  `PhoenixPoint.Geoscape.View\GeoSiteVisualsController.cs:16`) whose `VisualsContainer` (:38, the `GS_*(Clone)` prefab)
  holds MeshRenderer quads + TextMesh: `LocationIconParent` (:29, `Location_Icon`: site icon, frame, shadow, lock /
  infested / owner icons, `Site_ID`), `LocationDetailsParent` (:26, the empty `PopUp` canvas = `CanvasIcons` :32) and the
  `FoV` rows (resources, recruit, soldiers, timer, scanner progress). Shaders `Unlit/Colored, Animated Mask`,
  `Unlit/Transparent`, `GUI/3D Cull Back Text Shader`, queues 2450-3000, `FovControllableBehavior` billboarding, no
  motion-vector pass (`RenderforgeMod.DumpHierarchy` dumps such a tree). `SiteAddon` (:50/:52 containers, 3D haven-zone
  models) is real geometry and stays in the upscaler.
- **Mechanism (`src\GeoMarkerOverlay.cs`):** while a real (non-passthrough) generation is Live on `GeoscapeCamera`,
  every active site's `VisualsContainer` subtree (minus the `SiteAddon` subtree) is moved to the one free Unity layer
  (the game names every layer but **15**; picked by `LayerMask.LayerToName`), `GeoscapeCamera.cullingMask` loses that
  bit (re-asserted every frame), and the driver's own `DlssPresent` camera draws the layer at output resolution: its
  outRT blit moves from `AfterEverything` to `BeforeForwardOpaque`, a depth-only `ClearRenderTarget` follows it, the
  camera goes `Forward` with `cullingMask = 1 << 15` and `clearFlags = Depth` (the blit writes colour only, so the
  camera's own depth clear replaces a second command buffer), and a `Sync` MonoBehaviour copies GeoscapeCamera's pose,
  rect, fov, near/far and `projectionMatrix` in its `OnPreCull` - pose only. Every LAYER-OWNERSHIP change (mask
  re-assert, seam events, fallback re-walk, horizon classification) runs in GeoscapeCamera's own pre-cull (`Camera.onPreCull` filtered to
  it), BEFORE it culls, so both cameras draw one frame with one near/far split (done from the present camera's
  OnPreCull, after GeoscapeCamera rendered, a side change dropped or doubled a marker for a frame). Bit 15 is stripped
  from EVERY enabled camera but the present one (`Camera.GetAllCameras`, at activation and every Live frame) and given back on
  release. `CanvasIcons.worldCamera` = that camera; site picking is untouched (physics raycast on `PickingCollider`,
  layer 20, `ProbeMarkerClick` verifies pixel parity through both cameras). Everything is restored in
  `DlssDriver.Detach` (release, level end, mode Off, provider switch) or the moment a gate closes: layers,
  worldCamera, every camera mask, command-buffer event, and the present camera's full snapshot (rect, fov,
  orthographic/size, near/far, projection, mask, clear flags, path, depth, local pose) taken before the first Sync.
  An exception anywhere in the overlay logs once, restores, and sets `Diagnostics.MarkerOverlay = false` for the
  session - the upscaler never goes down for a marker walk.
- **Gates** (`GeoMarkerOverlay.Blocker`, checked every Live frame, closing one releases at once): scene camera is
  `GeoscapeCamera`, generation not passthrough, **D3D11 or D3D12**, and **`Cfg.ColorVision == None`** - the markers
  bypass the shim's post pass, so daltonisation would stop covering the colour-coded icons; every other combination
  keeps the vanilla path unchanged. Frame generation is no longer a gate ("with frame generation" below). **D3D12 (un-gated 2026-10-12):** the pass needed no change - on the production FP16 path
  (`D3D12HalfColor`, outRT `R16G16B16A16_SFloat`) the present camera's backbuffer write encodes and blends exactly
  like D3D11. Measured on Instance3, 2560x1440, Performance 1280x720, same camera pose on both APIs: badge orange
  `255,152,0` on both (vanilla path `216,116,0` / `224,126,0`), semi-transparent label gradient `82,79,94` on both,
  `ProbeMarkerClick` `sameSite=True`, armed under DLSS / FSR / XeSS, restore + re-arm across a campaign restart,
  0 Renderforge errors. Before that D3D12 logged `not armed - API D3D12` and the site icons / % labels were
  reconstructed from render resolution (blurry at 4K Performance). Plus the curtain / mask gates below. Each
  "not armed" reason is logged once.
- **With frame generation (un-gated 2026-10-12).** The gate dated from b5255e3 (2026-09-07) and rested on a guess
  ("interpolated frames would lack the markers"), written before the FG input contract existed. What the generators
  are handed ("FG input contract" / "HUD" below): hud-less colour = the upscaler's `out` twin, final frame = Unity's
  back buffer. The markers are drawn after the outRT blit, so they are in the back buffer and not in the hud-less
  colour - the HUD's class, which every SDK documents as kept out of the interpolation:
  DLSS-G `kBufferTypeHUDLessColor` = "the scene color before any UI/HUD elements are drawn", "without any HUD/UI
  elements in it" (`refs\Streamline\docs\ProgrammingGuideDLSS_G.md:247`, `:268`; tagged `FgStreamline.cpp:557`);
  XeSS-FG `XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS` = "interpolates on the HUD-less color texture and extracts the
  UI from the back buffer" (`refs\XeSS-sdk\doc\xess_fg_developer_guide_english.md:602-605`; `FgXess.cpp:158`);
  FSR-FG `HUDLessColor` = "the frame interpolation shader will detect UI areas in the frame and suppress distortion
  in those areas" (`refs\FidelityFX-SDK\Kits\FidelityFX\docs\techniques\frame-interpolation-swap-chain.md:91-92`;
  `FgFsr.cpp:246`). Nothing in the native side changed. Measured (`Fg_Capture`, D3D12, 2560x1440, Performance, FG 2x,
  crop 260x220 around the base marker + parked aircraft, pixels differing by more than 24/255 between tagged hud-less
  and back buffer): DLSS-G 14.6 %, XeSS-FG 15.2 %, FSR-FG 16.5 % (the markers and the aircraft; whole frame 10.3-10.5 %
  with the HUD) - the hud-less crop shows the bare globe. Overlay armed, site and aircraft picking unchanged, 0 errors
  under all three; XeSS-FG and FSR-FG presented 401 / 399 fps from 193 / 286 unfocused.
  **The one FG gate left:** a chain whose frames carry no hud-less colour (`FgTwins::HudPair`: the back buffer's
  format has no typed UAV, or is not the `out` twin's picture). The generator then interpolates the back buffer
  itself, and an opaque aircraft that is in neither the depth nor the motion vectors would be warped with the
  globe's. The providers report what they tagged (`FgNoteHudless`), `Fg_Hudless()` = 1 / 0 / -1 (nothing prepared
  yet), and `Blocker` releases the overlay on 0 ("frame generation without a hud-less colour").
  **Known limit:** composited UI updates at the rendered rate - "certain UI elements (such as waypoints or enemy
  markers) may stutter" (XeSS guide `:521-522`): under an orbit the markers step at the base rate over an
  interpolated globe. Before this, FG users got render-resolution markers through the upscaler (blurred at
  Performance, FSR worst: Laplacian 37.6 on the icon crop against 63.4 drawn after reconstruction) AND interpolated
  as scene. The alternative - markers inside the hud-less colour, so they interpolate with the globe's motion
  vectors - needs them composited into the native `out` twin before the generators read it (on D3D12 Unity's own
  draws of frame N reach the GPU after the shim's list of frame N, so nothing Unity draws can be in that twin);
  not built. **Not observable here:** the generated frames themselves - the test bench must not take the
  foreground or capture the screen, DLSS-G only interpolates focused, and no diagnostic reads the shadow chain.
- **Aircraft and globe markers (2026-10-12).** Two marker families live outside every `VisualsContainer`:
  - `GeoVehicle` (`PhoenixPoint.Geoscape.Entities\GeoVehicle.cs`): `GlobeOffset` (`VisualsRoot` :316) holds `Visuals`
    (`MeshRoot` :317 - `Model` (Standard, opaque, lit), `UI` base disc, `Geo_Shadow`, `Selected_Circle`, the
    `UI_GS_Selection(Clone)` quads, disengage visuals :378), `Trails` (two `TrailRenderer`s) and the
    `RangeIndicator` decal. Left to the scene camera the model was a render-resolution picture UNDER every site
    label (the overlay draws last). `MeshRoot` now rides the marker layer: the model is opaque and writes depth in
    the present camera's forward pass, so labels behind it are hidden again as in the scene.
  - `GlobeMarker` (`PhoenixPoint.Geoscape.View\GlobeMarker.cs`, e.g. `UI_GS_TravelTo(Clone)`): the pulsing ground
    ring of a destination / current site, a sibling of the site's `SphereOffset`. It was the soft ring left behind
    every sharp icon.
  Both are "loose" items (`GeoMarkerOverlay.Loose`): found by `FindObjectsOfType` at activation (and on a fallback
  pass only when a seam is missing), plus seams `GeoVehicle.Initialize` :312 and `GlobeMarker.Awake` :46; tested
  against the globe every frame with the sites' segment/sphere test on their pivot (an aircraft's carries the flight
  altitude), behind the globe they go back to their own layers and the globe's depth hides them (pop at the limb,
  like a site); one is re-walked when its root's `childCount` changes, on either side of the globe: the vehicle
  selection quads are ONE instance without a `GlobeMarker` (`UIStateVehicleSelected.cs:714`) re-parented under the
  selected aircraft's `MeshRoot` (:724) - taken with a near aircraft, they have to go back to their own layer when
  the next selection is behind the globe, or the present camera draws them through it. `WalkGuarded` leaves a
  GameObject with a `Collider` on its layer (`PickingCollider`, 20: picking is a physics raycast, and
  GeoscapeCamera's mask is `0xFFFFFFFF`, so "was it ever drawn by it" cannot tell) and never enters a
  `MaterialDecalComponent` subtree. `ProbeCraftClick` = `ProbeMarkerClick` for aircraft.
  **Left with the upscaler, and why:** range rings and travel paths are `MaterialDecals` (a projection box that
  needs the deferred G-buffer and the globe's depth - in a forward pass it would draw as a box); aircraft `Trails`
  (a ribbon along the flight path can reach behind the limb and the cleared depth has no globe to hide it); the
  hover name label (`SiteDetails_Visuals`, a WorldSpace canvas under `Markers` with `SoftMask`: needs a mouse hover
  to verify, which the unfocused test bench cannot give).
- **Lighting of the marker layer (`LightLayer`).** Forward and deferred disagree twice here. `Geoscape_Lights`
  (directional, 1.2, soft shadows) lists its layers (`0x7FFF6F37`) and the free layer 15 is not among them: every
  light that lights Default gains the bit while armed. And `QualitySettings.pixelLightCount` is **0** in this game:
  the forward pass shades only the brightest directional light per pixel and folds the other three (`Yellow SUN` 4,
  `Geoscape_Lights` 1.2, `BacklightLight` 1, `Pale SUN` 0.6) into spherical harmonics - the aircraft came out flat.
  The directional lights go `LightRenderMode.ForcePixel` (per light, not the global setting) **for the present
  camera's render only**: set in its `OnPreCull` (`BeginPresent`), back to `Auto` in its `OnPostRender`
  (`EndPresent`). Held for the whole frame (as first built) the mode also applied to GeoscapeCamera: every
  forward-lit surface it draws (transparent Standard, shaders without a deferred pass) got three ForwardAdd passes
  at render resolution and a different specular look. The mask bit stays while armed - no other camera draws the
  layer. `EndPresent` is idempotent and also runs from GeoscapeCamera's next pre-cull, `Tick` and `Restore`, so a
  present-camera render that never reaches `OnPostRender` leaves nothing behind. Laplacian on the aircraft crop,
  D3D12 Performance: 24.4 as taken → 28.9 with the mask → 35.3 with both (vanilla upscaled 41.9, upscaler off 51.1:
  the rest is PPv2 - SAO, tonemap, NIS - which nothing drawn after reconstruction gets). The mask is put back on
  release (only where the value is still the one set). Lights are collected at activation and on every fallback
  pass: one created later joins within 2 s. Frame cost, D3D12 DLSS Quality 2560x1440, one launch, 3x10 s each
  (`SetMarkerOverlayLights(mask)`: 1 = mask, 2 = per-pixel, 4 = shadows on the present camera): all on 377 / 371
  fps, no shadows 368, no per-pixel 377, unlit 369, no aircraft 337 (a loaded sample), overlay off 363 / 355 -
  nothing in the lighting, the shadows or the aircraft costs a measurable frame; the overlay is not slower than
  markers through the upscaler.
- **Geoscape frame rate across the overlay commits (2026-10-12).** A "116 fps instead of 330" reading was the test
  route, not the overlay: after PPCLI `start-mission` → `end-mission {lobby}` → `start-campaign` the process stays
  CPU-bound - WITHOUT Renderforge (D3D11, TFTV + PPBridge only, `targetFrameRate=-1`) menu 1238 → 135 fps, geoscape
  378 → 115; with it 324 → 114, the same with the overlay off or the upscaler off. Fresh geoscape (`start-campaign`
  from the menu), one launch per build and API, 3x10 s, foreign CPU load 1-11 cores, DLSS Quality with the overlay
  on / off: D3D12 `02560d9` 331 / 327, `a9d21b4` 329 / 324, `14f4204` 322 / 306, overlay fixes 319 / 334; D3D11 353 /
  333, 333 / 342, 378 / 323, 312 / 366; DLAA 267-343, upscaler off 326-397. No build is outside the others' spread.
  One launch per cell only - the repeat pass and the final native build were taken with another game sharing the GPU
  and are not counted; the perf verdict for the final build is open until a re-run on an idle machine.
- **D3D12 draws the layer one frame late (`PoseLag`).** The outRT the present camera blits in frame N holds what
  GeoscapeCamera rendered in frame N-1 ("D3D12 frame pairing" below). Drawn with frame N's camera the markers led
  the globe by one frame of camera motion. Measured with `DumpScreen` pairs under an orbit at 30 fps (template match
  of the base marker against its projected pivot; `SetDumpAnchor` pins the site): marker minus pivot (-3.2, 9.4) px
  with the overlay off (the truth: marker in the scene), (0.5..1.6, -1.7..-2.1) with this frame's pose, (-1.9..-3.1,
  8.7..10.2) with the previous frame's. D3D11, same test: (-6.1, -3.0) off, (-5.7, -2.7) with this frame's pose,
  (+8..13, +3..5) with the previous one - no lag there. So on D3D12 `Sync` hands the present camera the previous
  frame's pose, fov and projection, and an aircraft that moved since the last frame is put on its previous world
  pose for the present camera's cull and render only (`ShiftCrafts` in `OnPreCull`, `UnshiftCrafts` in
  `OnPostRender` and in `Restore`): in flight at 3600x game speed it sat a frame ahead of its own trails and range
  ring (54 px in a frame stretched by a dump), now at the head of the trail. A/B: `SetMarkerOverlayOptions(aircraft,
  poseDelay)` (-1 by API, 0, 1), runtime-only. The same holds for every loose item, not only aircraft: a
  `GlobeMarker` added for a `GeoActor` hangs under the actor's own transform (`GeoscapeGlobeMarkers.AddMarker(GeoActor,
  ..)` :92), not under `MeshRoot`, and flies with a vehicle. And for the near/far split: ownership changes in
  GeoscapeCamera's pre-cull of frame N, while the picture under the markers is frame N-1's - a marker crossing the
  limb was missing for one frame on its way out and drawn twice on its way in. With `PoseLag` the side changes of
  the frame are recorded (`flips`), undone for the present camera's render and re-applied after it (`BeginPresent` /
  `EndPresent`). The shift follows the same split: `ShiftCrafts` moves what the present camera draws in this frame,
  i.e. what was near in frame N-1 (`Near != (FlipFrame == frame)`), and a loose item that crosses near -> far takes
  its previous pose at the flip (`CapturePose`). Before, such an item was back on the marker layer for that one
  render but skipped by the pose capture and the shift (both keyed on the NEW side): drawn at frame N's position
  over frame N-1's picture, one frame of its own motion at the limb (Codex review 2, static; in-game check pending).
- **`MotionVectorFilter` and the layer.** The filter keys on `Renderer`, the overlay on `Transform` layers; the
  `Rotator`s on `Selected_Circle` / `Quad_2` stay in the filter's map (mode Camera) while their objects are on the
  marker layer, where no camera with a motion-vector pass draws them. Either can be released first. After
  upscaler Off on both APIs: `AuditMvModes` `decal/Object=434 rotator/Object=95`, `saved=0`, overlay `inactive`.
- **A third screen camera does not work here** (measured, `marker-cam\results.md`): a separate marker camera after
  DlssPresent (any clearFlags, any depth) leaves the present blit out of the buffer the HUD and end-of-frame
  `ReadPixels` see - stale frames with marker trails; disabling it brings the blit back. Drawing with the present
  camera itself is the design that survives.
- **Arming gate:** `GeoMarkerOverlay.Tick` (from `DlssDriver.Step`, Live) arms only once
  `LevelSwitchCurtainController.IsCurtainLifted` (`Base.Utils\LevelSwitchCurtainController.cs:113`, after the lift
  fade) and `cullingMask != 0` (`CullEverythingController.cs:19` zeroes it under the curtain). Armed under the curtain
  the takeover captured mask 0 and never saw the sites.
- **Far side:** vanilla hid far-side markers with the globe's depth buffer, which the cleared depth cannot; a
  segment/sphere test (centre origin, `GlobeUnits.GlobeRadius`, `PhoenixPoint.Common.Core\GlobeUnits.cs:28`) hands a
  site whose pivot is behind the globe back to its original layers (drawn by GeoscapeCamera again, depth-hidden as
  before), a quarter of the ~425 active sites per frame; a newly seen site is classified by the same test before it
  is layered, so a far-side site never gets a frame through the globe. No second hidden layer exists.
- **New markers (1.6.4, event-driven):** Harmony postfixes on `GeoSiteVisualsController` (bottom of
  `src\GeoMarkerOverlay.cs`) mark ONE site dirty: `OnEnable` :167 (site spawned by `GeoActorSpawner` or re-activated -
  the 1.6.3 rescan missed a site that toggled active without changing the hierarchy count), `AddSiteDetailsVisuals`
  :175, `RefreshAnimations` :404 (highlight :410, only when `_highlight` went null → new), `RefreshMissionVisuals` :595
  (diplomatic :601 / mission :627, only when either field holds a NEW object - a day tick refreshes every site at
  once), `OnDestroy` (drop dead sites from the site lists). The dirty set is drained in GeoscapeCamera's pre-cull, so
  the spawning frame already draws the object on the marker layer. Haven-zone addons (:514/:519) land in the pruned
  containers; `RefreshSiteAddons` :542 has no caller. A seam whose method/field is gone skips itself (`Prepare`).
  Fallback for spawners without a seam: every 2 s the geoscape `Transform.hierarchyCount` is read; a change queues
  every near site for a re-walk, 8 sites per frame (no ~5 ms burst); a shrink also prunes destroyed keys from every
  map; without the `OnEnable` seam the scene-wide `FindObjectsOfType` runs there too. Bit 15 is stripped from new
  cameras every frame (`Tick`). The scene camera's name is read once per camera object. Status counters
  (`GetMarkerOverlayStatus`): `hooked`, `fullScans` (1 per activation), `eventWalks`, `fallbackPasses`, `rewalkQueue`.
  The `SiteSpecialAddonContainer` / `SiteUniqueAddonContainer` subtrees (:50/:52) are excluded by direct reference.
- **Known look difference:** the markers no longer pass through PPv2's HDR tonemapping/grade: whites reach 255
  (vanilla caps at 246), the unknown-site disk reads as a translucent grey instead of crushed black
  (`marker-cam\still2-site116-on-vs-vanilla.png`). A parked aircraft sat under the soldier row instead of over it
  until the aircraft joined the layer (2026-10-12); the aircraft model itself is now forward-lit without PPv2 (flatter
  contrast than the scene's, unanti-aliased 1-pixel edges; "Lighting of the marker layer"). Trails are gone; A/B lever `RenderforgeMod.SetMarkerOverlay(bool)` (runtime-only).
- Diagnostics: `GetMarkerOverlayStatus`, `ProbeMarkerClick`, `DumpScreen(path)` (raw RGB24 of the real backbuffer at
  end of frame + `.txt` sidecar - PPCLI's `screenshot` re-renders the cameras itself and shows the level-curtain art
  once the present camera draws objects), `DumpHierarchy(name, path)`.

### Motion vectors of spun props and decal volumes (2026-10-12)

- **Symptom:** the geoscape was soft and shimmering under every upscaler, DLAA included, with the camera standing
  still; the land dot grid was wiped.
- **Cause:** Unity's object-MV pass re-draws every renderer in `MotionVectorGenerationMode.Object` whose transform
  changed, with the renderer's own mesh. Two vanilla families move every frame without the picture moving:
  - `MaterialDecals.MaterialDecalComponent` (decompile `MaterialDecals\MaterialDecalComponent.cs`): the mesh is the
    decal's projection BOX, never the surface it lands on. `Vehicles/…/GlobeOffset/RangeIndicator` (`Material
    Decals/Decal lit` q=2014, cube scale 5, spun (0,-3,0)°/s) wrote a screen-wide roll field. The game's other decal
    system already sets Camera (`LlockhamIndustries.Decals\ProjectionRenderer.cs:381`); MaterialDecals forgot.
  - `MirzaBeig.ParticleSystems.Rotator` (`Rotator.cs:44/:48`), the FX pack's idle spinner:
    `GeoscapeVisual_Showroom/Core` (`PlanetCoreMaterial`, Standard q=2000, (0,0,-0.7)°/s) wrote motion over the whole
    globe disc.
  - Only opaque queues write: with everything else on Camera, switching ONE renderer back to Object gave Core
    75.4 % of the frame (max 0.066 px), RangeIndicator 99.8 % (max 0.32 px), and 0 % for every transparent spinner
    (`Atmosphere`, `Atmosphere_2`, `Atmosphere_4_clouds` q=3000-5000, `Selected_Circle (1)`, `Quad_2`) - the clouds
    never had motion vectors to lose.
- **Rule (`src\MotionVectorFilter.cs`):** while the MV/depth pass is consumed (`DlssDriver.KeepCameraState`,
  `MotionVectorFilter.Set(copyAttached)` - real upscaler, frame generation on top of it, Depth / MotionVectors debug
  views), every `MaterialDecalComponent` renderer and every renderer on a `Rotator`'s own GameObject (not its
  children: a child may be real geometry riding along) is set to `MotionVectorGenerationMode.Camera`. By component
  type, no names. The original mode of each touched renderer is saved and written back in `DlssDriver.Detach`
  (mode Off, provider switch, level end), when the pass stops being needed (passthrough generation), or by the A/B
  lever. A passthrough generation never arms it.
- **Seams, no per-frame search:** decals at arm time from `MaterialDecalComponent.SceneDecals` (the game's own
  registry, :59) and afterwards from a Harmony postfix on `MaterialDecalComponent.OnEnable` (:136 - a new
  `RangeIndicator` per vehicle, travel-path decals); rotators from one `Resources.FindObjectsOfTypeAll<Rotator>` at
  arm time (inactive ones included - they already ran `Start`) and a postfix on `Rotator.Start` (:19). A seam whose
  method is gone skips itself (`Prepare`); an exception disables the filter for the session and restores, never the
  upscaler. Dead keys (destroyed travel paths) are pruned when the map doubles.
- **Measured** (Instance2, 2560x1440 borderless, vsync off, fresh campaign, day-side pose `ChaseGeoTarget (2,5,9)`,
  still camera; `MvStats` = share of mvRT texels above 0.01 px; Laplacian of crop 1500,700-1900,1100 of
  `DumpColorIn` / `DumpOut`; diff = two `DumpOut` 0.3 s apart, pixels differing by more than 8; filter off → on):

  | API | Provider / mode | moving texels | Laplacian in | Laplacian out | diff >8 |
  |---|---|---|---|---|---|
  | D3D11 | native (Passthrough view) | - | 5.30 | 5.28 | 0.00 % |
  | D3D11 | DLSS DLAA | 78.3 % → 0 | 5.3 | 4.08 → 7.50 | 1.12 → 0.03 % |
  | D3D11 | DLSS Quality | 77.7 % → 0 | 3.97 | 4.22 → 7.50 | 1.38 → 0.06 % |
  | D3D11 | DLSS Performance | 76.8 % → 0 | 3.2 | 7.37 → 6.91 | 2.69 → 0.24 % |
  | D3D12 | native (Passthrough view) | - | 5.14 | 5.15 | 0.00 % |
  | D3D12 | DLSS DLAA | 83.7 % → 0 | 5.27 | 2.13 → 8.43 | 0.11 → 0.15 % |
  | D3D12 | DLSS Quality | 83.4 % → 0 | 3.9 | 2.47 → 5.50 | 0.03 → 0.27 % |
  | D3D12 | DLSS Performance | 82.4 % → 0 | 3.2 | 3.88 → 6.31 | 2.18 → 0.33 % |
  | D3D12 | XeSS Quality | 82.8 % → 0 | 3.7 | 1.69 → 2.40 | 0.02 → 0.03 % |
  | D3D12 | FSR Quality | 83.5 % → 0 | 3.8 | 2.73 → 3.94 | 0.05 → 0.00 % |

  With the range ring in view (start pose, D3D11 DLAA) 99.8 % of the frame moved, max 0.32 px. Frame generation
  (D3D12, FSR Quality + DLSS-G x2 live): 83.1 % → 0. XeSS and FSR do not run on D3D11. Low "diff" beside a low
  Laplacian (D3D12 filter off) is a stable blur, not a stable picture.
- **Frame rate of these numbers:** per-frame vectors scale with 1/fps. The table was taken under the NVIDIA driver's
  240 fps cap (fps not recorded then; ~235 inferred from the re-run's px/s). Re-run uncapped, caps read live:
  `Application.targetFrameRate=-1`, `QualitySettings.vSyncCount=0`, `Cfg.LimitFrameRate=False`, `runInBackground=True`,
  window unfocused. Filter off, largest vector, fps-independent (render-res px/s; source spin Core 0.7°/s, range
  ring 3°/s); filter on = 0 moving texels in every row:

  | API | Pose | DLAA 2560x1440 | Quality 1707x960 | Performance 1280x720 |
  |---|---|---|---|---|
  | D3D11 | start (range ring in view) | 73.7 px/s at 378 fps | 51.4 at 398 | 37.8 at 393 |
  | D3D11 | day side (Core only) | 22.6 px/s at 390 fps | 14.8 at 401 | 11.6 at 401 |
  | D3D12 | start (range ring in view) | 87.3 px/s at 304 fps | 55.5 at 351 | 36.7 at 367 |
  | D3D12 | day side (Core only) | 35.8 px/s at 306 fps | 29.3 at 362 | 22.7 at 366 |

  The Laplacian / diff columns above were NOT re-taken uncapped.
- **Motion kept:** camera orbit feeds the pure camera field (100 % of texels, up to 2.5 px, back to exactly 0 once
  settled; dots and coastline hold in a mid-orbit dump where the unfiltered one smears). A travelling aircraft keeps
  its object motion (`Model`, Standard q=2000, neither family: a 41x41 px island of 0.02-0.05 px at geoscape time
  scale 600, nothing else moving).
- **Restore, asserted:** `AuditMvModes` after mode Off on both APIs: `decal/Object=434 rotator/Object=95`, saved 0;
  armed: `decal/Camera=433 rotator/Camera=95`, `savedModes=Object:467`.
- **Tactical:** nothing to fix and nothing touched. `ALN_PLT_Nest_48x48_A`, `NJR_PLT_IND_48x48_SSYN16`,
  `SCV_PLT_56x56_SSYN12`: no `MaterialDecalComponent` and no `Rotator` in the scene (`saved=0`), the still world
  reads 0 moving texels, the only motion is the squad's idle animation (0.3 % of the frame, same with the filter on
  or off). Ground markers there are transparent (`_PX_UI/SimpleUIXRay`, Standard q=3000) and write nothing.
- **Left alone:** the animated coastline glow (shader time, no transform motion: the native picture itself differs
  by ~4 % between two frames there; no motion vector can describe it) and every transparent spinner (no MV pass).
- Diagnostics (PPCLI): `MvStats(path)` (whole-frame motion field + PNG), `SurveyMovers(path, frames)` (renderers
  whose transform moves on a still picture, with the vector at their centre), `GetMvFilterStatus`, `AuditMvModes`,
  `SetMvFilter(bool)` (A/B lever, runtime-only). Dumps: session scratchpad `mvfix\`.

### Scene depth, D3D12 frame pairing, jitter signs (2026-10-12)

Three independent defects behind "soft / stair-stepped on D3D12, XeSS bad in a different way". All measured at
2560x1440 borderless, `targetFrameRate=-1`, `vSyncCount=0`, mod limiter off, 280–400 fps on the geoscape (60–100 in
the tactical map), no other Phoenix Point process alive. Dumps + scripts: session scratchpad `xess\`.

- **Depth was the colour buffer (D3D11 and D3D12, every provider, FG too).** `Blit(BuiltinRenderTextureType.Depth →
  depthRT)` logged `built-in render texture type 3 not found while executing DLSS copy depth+mv (Blit source)` and
  copied whatever `_MainTex` still held: `DepthStats` read `corrColorR=1.0000` on the geoscape and in
  `ALN_PLT_Nest_48x48_A`, both APIs. Unity 2019.4 `BuiltinRenderTextureType`: `Depth` = "Camera's depth texture.",
  `ResolvedDepth` = "Resolved depth buffer from deferred." — both cameras render `DeferredShading`, which has no camera
  depth texture. Measured per path (D3D11 geoscape, `renderingPath` forced through PPCLI):

  | path | `Depth` | `ResolvedDepth` |
  |---|---|---|
  | DeferredShading | colour (corr 1.0000) | real: centre 0.017413 vs 0.017372 predicted by a physics ray |
  | Forward | real: centre 0.017397 | colour (corr 1.0000) |

  Fix: `DlssDriver.BuildCopy` picks the source from `cam.actualRenderingPath` and `KeepCameraState` rebuilds the
  buffer (with a history reset) when the path changes. Tactical after the fix: centre 0.005860 vs 0.005863 predicted.
  **Contract for consumers (FG included):** `depthRT` is `RFloat` at render resolution, raw device depth, reversed-Z
  on D3D (near = 1, far = 0; `DLSS_F_DEPTH_INVERTED` / `INVERTED_DEPTH` / `DEPTH_INVERTED` declare exactly that), same
  size and row order as `colorRT` and `mvRT` (all three are written by the same camera, no flip between them).
- **D3D12 evaluated frame N-1 with frame N's jitter.** A list handed to `IUnityGraphicsD3D12v5::ExecuteCommandList`
  runs on the GPU before Unity's own still-open command list of that frame, and v5 (the only version 2019.4 has)
  offers no flush. So what our list copies out of `colorRT` / `depthRT` / `mvRT` at frame N's event is what Unity
  rendered in frame N-1. Evidence: `DumpPair` under an orbit at 20 fps — `outRT` of a passthrough copy trails
  `colorRT` read back in the same frame by 5.70 px, then 1.09, 1.11 as the orbit decays; 0.00 standing still and 0.00
  on D3D11. Reporting the previous frame's jitter (`SetJitterReportDelay 1`) lifted the high-pass correlation with a
  native frame from 0.855 to 0.937 (DLSS), 0.773 to 0.919 (XeSS), 0.789 to 0.944 (FSR), Performance mode.
  Fix: `PairD3D12` in `RenderforgeNative.cpp` — on D3D12 jitter, reset, frame time, pre-exposure and the camera
  frustum come from the previous event's block; resources and everything applied to the output (sharpness, LUT,
  style, grade, colour vision) stay this frame's. The first evaluate on a new colour RT is a reset.
  **Remaining consequence:** the upscaled picture on D3D12 is one frame behind the scene Unity renders (3 ms at
  330 fps). `Fg_SetFrame` still sends frame N's matrices and jitter next to twins that hold frame N-1 — for the FG fix.
- **Jitter signs.** Every SDK takes the same value, the driver's NGX offsets `(-jx, -jy)`: XeSS was `(-X, +Y)`,
  FSR `(+X, -Y)`, each with one axis wrong. Constant-jitter test (`SetJitterConst ±0.45`, sharpen 0, Quality; a
  correct sign leaves the output where it was, a wrong one moves it by twice the input shift): DLSS 0.09 / 0.19 px,
  XeSS 3.11 px in X before and 0.06 / 0.00 after, FSR 2.84 px in Y before and 0.01 / 0.01 after. With the Halton
  sequence and the pairing fix the shipped signs beat the other combinations against a native frame in Quality and
  Performance (XeSS 0.914 / 0.935 vs 0.817–0.866; FSR 0.944 / 0.942 vs 0.825–0.910; DLSS 0.950 / 0.942 vs 0.826 /
  0.833 flipped). Before the pairing fix the same test favoured the *opposite* signs (the base-2 / base-3 Halton
  values of consecutive frames are anti-correlated, so a flipped sign approximated the previous frame's jitter) —
  which is why earlier still-frame and "shake" A/Bs could not settle it.
- **XeSS `LDR_INPUT_COLOR` stays.** Guide "Color": LDR input sets the flag, exposure 1.0, no auto-exposure. A/B
  without the flag (FP16 linear treated as HDR): 0.925 / 0.933 against 0.914 / 0.935 with it — no difference.

Result, static geoscape, day side, crop 1500,700–1900,1100, Laplacian of the SDK input → output (`s0` = Sharpness 0,
`s40` = default), consecutive-output pixels differing by more than 8/255:

| API / provider / mode | before s40 | after s0 | after s40 | consecutive diff (after) | match with native, before → after |
|---|---|---|---|---|---|
| D3D12 DLSS Quality | 3.54 → 5.18 | 3.49 → 3.40 | 3.58 → **7.51** | 0.00 % / 0.16 % | 0.852 → 0.960 |
| D3D12 DLSS Performance | — | 2.99 → 3.57 | 3.00 → 7.50 | 0.00 % / 1.34 % | 0.866 → 0.942 |
| D3D12 XeSS Quality | 3.42 → 2.14 | 3.37 → 2.28 | 3.20 → 2.80 | 0.00 % / 0.11 % | 0.882 → 0.913 |
| D3D12 XeSS Performance | 2.59 → 2.11 | 2.52 → 2.36 | 2.49 → 2.63 | 0.00 % / 0.27 % | 0.843 → 0.938 |
| D3D12 FSR Quality | 3.49 → 3.72 | 3.63 → 3.37 | 3.69 → 4.39 | 0.00 % / 0.00 % | 0.869 → 0.948 |
| D3D12 FSR Performance | — | 2.99 → 2.82 | 2.99 → 3.75 | 0.00 % / 0.00 % | 0.812 → 0.942 |
| D3D11 DLSS Quality | 7.50 (earlier run) | 3.90 → 3.86 | 3.91 → 7.28 | 0.00 % / 0.06 % | 0.955 |

- D3D12 DLSS now equals D3D11 (7.51 vs 7.28). The old "5.50 vs 7.50" was the pairing defect.
- Sharpening: DLSS and XeSS use the shim pass (NIS, output resolution, same strength on both APIs — `Dlss_Sharpener`
  = NIS in every row); FSR uses its own RCAS with the same 0..1 value. NIS is contrast-adaptive, so its gain follows
  what the upscaler resolved: +121 % on DLSS, +23 % on XeSS at 40.
- **Open: XeSS Quality stays below its own (aliased) input on this crop** (2.80 vs 3.20). The crop is a field of
  1–2 px halftone dots; XeSS 2.0.2 DP4a reconstructs them regularly but at lower contrast than DLSS / FSR. No
  integration error is left that the SDK documents: jitter registered (0.06 px), pairing fixed, depth real, flags per
  guide. Coastlines at XeSS Performance are clean (`xess\_fin.png`); before, XeSS and FSR showed horizontal combing
  on near-horizontal edges (`xess\_xp.png`, `xess\_xin.png`).
- Tactical (`ALN_PLT_Nest_48x48_A`, D3D12, Quality, s40): DLSS 2.39 → 3.40, XeSS 2.14 → 2.64, FSR 2.41 → 2.39; the
  12–14 % consecutive diff there is the scene (pulsing deployment zone, idle soldiers): native passthrough reads
  12.99 %, the unlit 74 % of the crop 0.00–0.15 %.
- Motion vectors under an orbit at 60 fps (1.2–1.3 px/frame): output/input Laplacian DLSS 0.53, XeSS 0.54, FSR 0.56,
  the same ratios as standing still on that content — XeSS shows no smear of its own; scale `(-renderW, -renderH)`
  unchanged (guide "Motion Vectors": pixels, current → previous).
- Diagnostics (PPCLI, runtime-only): `DepthStats`, `SetDepthSource(0|1|2)`, `SetJitterReportDelay(-1|0|1)`,
  `DumpPair(in, out)`; `DumpOut` / `DumpColorIn` now sRGB-encode an FP16 source, so D3D12 and D3D11 dumps compare 1:1.

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

### Init state machine (post-only)

When NGX init fails but the D3D device is still alive, the post pass (LUT grades, scene styles,
colour-vision, NIS sharpen, mip bias) can still run — no NGX feature is needed for any of it.

Init outcomes (native `Dlss_Init`, `native\RenderforgeNative.cpp:142-170`):

| provider `Init()` returns | `PostAlive()` | `Dlss_Init` returns | managed |
|---|---|---|---|
| `DLSS_OK` (0) | yes | `DLSS_OK` | `Available`, upscaler runs |
| `DLSS_ERR_INIT_FAILED` (2) / `NOT_AVAILABLE` (3) / `NEEDS_DRIVER` (4) | **yes** | **`DLSS_OK_POST_ONLY` (8)** | `Available` + `PostOnly`, mode forced Off, post pass runs |
| same codes | no | that code, unchanged | unavailable, as today |
| `NO_DEVICE` (1) / `NO_UNITY_IFACE` (5) / `NO_PROVIDER_DLL` (6) / `PROVIDER_UNSUPPORTED` (7) | no | that code, unchanged | unavailable, as today |

Native invariants:
- `ngxInitialized` stays **success-only** (`Device11.cpp:186`, `Device12.cpp:109`): set to 1 only
  after a real `NVSDK_NGX_SUCCEEDED(r)`. `Shutdown()` therefore never calls
  `NVSDK_NGX_*_Shutdown1` on a device that never initialised NGX (`Device11.cpp:314`,
  `Device12.cpp:270`).
- `Shutdown()` releases the device **unconditionally** in both backends (`Device11.cpp:315`,
  `Device12.cpp:271`), so a post-only device is always freed on shutdown / provider switch.
- FSR/XeSS never reach post-only: they release the device on every failure path
  (`Fsr12.cpp:147,156`; `Xess12.cpp:202,212`), and their `PostAlive()` is the base default
  `false` (`Device.h:81`). Deliberate — the Auto loop reaches D3D11/D3D12 NGX on the machines
  that matter, and a second post-only carrier buys nothing.

Managed state (`src\RenderforgeMod.cs`):
- `PostOnly` ⇒ `Available=true` (`:159-160`), `Upscalers.Running=Off` (`:169`).
- The live carrier sits in `Upscalers.PostCarrier` (its own field, `src\Upscaler.cs:23`), never
  derived from `Upscalers.Failed`.
- `Failed`/`FailedCode` hold the carrier kind + the retained `DLSS_ERR_*` (2/3/4, never 8) so
  the picker greys with the true reason (`Dlss_PostOnlyReason`, `RenderforgeNative.cpp:317`).
- `ReinitNative` (`:178-204`) never trades a working upscaler for post-only: if `PostOnly &&
  prev != Off` the init is treated as a failure and rolls back. On rollback it restores the
  carrier from `PostCarrier` (`:186,199`), not from `Failed` (which is about to be overwritten).

Driver (`src\DlssDriver.cs`):
- `Apply` (`:137`) forces `wantMode = Off` under `PostOnly` — every generation is a passthrough
  carrying LUT / style / colour-vision / NIS.
- Mip bias = 0 in passthrough (`:241`).
- Sharpness zeroed only for `DebugView.Passthrough` (`:500`), not for the post-only Off
  passthrough — the user's NIS slider still applies.
- Vanilla SMAA kept: `KeepCameraState` (`:400-405`) and `AfterApplyPostProcessOptions`
  (`:549-553`) skip the `Antialiasing.None` assignment when `passthrough`.
- FrameGen gated off in post-only (`src\Availability.cs:84`): `PostOnly` → "Turn an upscaler on
  first".
- Overlay (`:160-170`): prints `Upscaler: off (<reason>)` via `Availability.Reason`; hides the
  D3D12 `GPU:` line because `live` requires `!Passthrough` (`:138`).

### Dev switch `RENDERFORGE_FAKE_INIT`

**Dev-only.** Injects an NGX failure while the device is alive — tests the post-only path
without a non-NVIDIA GPU. Read once per process (`RfFakeInitCode`, `GetEnvironmentVariableA`,
`native\RenderforgeNative.cpp:119-127`); cannot be armed mid-session.

| Value | What it replaces | GPU requirement |
|---|---|---|
| `2` | `*_Init_with_ProjectID` is never called; `r = NVSDK_NGX_Result_FAIL_PlatformError` instead. The PRODUCTION failure branch runs unchanged. | any |
| `3` | `SuperSampling_Available` query skipped, `available` stays 0. | NVIDIA (needs NGX up) |
| `4` | `NeedsUpdatedDriver` query skipped, `needsDriver = 1` injected. | NVIDIA (needs NGX up) |

Never overwrites a successful init — it **replaces the NGX call**, not the result, so no
context leaks and `ngxInitialized` stays correct (`Device11.cpp:175-176`, `Device12.cpp:98-99`).

Regression gate: `dlss_probe.exe <dir> --fake=2` in `build-native.ps1:90` (sinusoid pattern
upload + readback: copy == input, sharpen != input, grade != input). `--fake=3|4` are manual,
NVIDIA-only. Exit 3 of the ordinary probe now also covers a `DLSS_OK_POST_ONLY` init.

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
| NVIDIA Streamline | v2.14.1 / 2026-09-08 (release zip `streamline-sdk-v2.14.1.zip`, refreshed 2026-09-28) | `refs\Streamline\` | `bin\x64\sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `sl.dlss.dll` — all 2.14.1.0. SDK's `bin\x64\nvngx_dlssg.dll` is 310.9.1.0 (= DLSS SDK 310.9.1 copy, same hash); `latest-dll\nvngx_dlssg.dll` holds that build and is what ships. Headers vs 2.12.0: `sl::Resource` `reserved` u32 split into `internalFlags`+`reserved` u16 (same size), new buffer/feature consts only — ABI-compatible. `include\`, `license.txt` |
| NVIDIA DLSS SDK | v310.9.1 / 2026-09-08 (git clone, refreshed 2026-09-28) | `refs\DLSS-sdk\` | `lib\Windows_x86_64\rel\nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll` — all 310.9.1.0. `nvsdk_ngx_helpers.h` split into `*_d3d.h` / `*_cuda.h` (still included by it); native builds unchanged |

`refs\DLSS-sdk` `nvngx_dlss.dll` 310.9.1.0 is the newest SR DLL (2026-09-28). Rule (same trap twice
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
via Streamline 2.14.1 manual hooking, `nvngx_dlssg.dll` 310.9.1, Reflex `eLowLatency` + 6 PCL markers,
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
| XeSS-FG | `libxess_fg.dll` 1.3.1 + `libxell.dll` 1.3.2 | `InitFromSwapChainDesc` on child, `pApplicationSwapChain=NULL` | XeLL mandatory, 6 markers, sleep at main-thread frame start, cap `minimumIntervalUs` | 2x off Intel Arc | `maxSupportedInterpolations=1` on non-Intel |
| DLSS-G | Streamline 2.14.1 + `nvngx_dlssg.dll` 310.9.1 | proxy factory `CreateSwapChainForHwnd` on child + proxy device queue | Reflex `eLowLatency` + 6 PCL markers, sleep at main-thread frame start, cap `frameLimitUs` | 2x-4x on RTX 50 (`numFramesToGenerateMax=5`) | NVIDIA focus gate: unfocused -> real only, no error |

### Per-frame sequence

1. Main thread: `FrameGen.Apply` -> `Fg_Init` (once); every frame while frames flow: PlayerLoop `FrameStart` ->
   `Fg_FrameBegin` (latency SDK sleep + `SIMULATION_START`, "FG pacing" below), `SimEnd` -> `Fg_SimEnd`; then
   `Fg_SetFrame` (jitter, camera, matrices, dt, the frame's pace id; returns the stored block, which travels as the
   `data` of that frame's `DLSS_EV_FG_PREPARE`).
2. Render thread, `DLSS_EV_FG_PREPARE` (`CameraEvent.AfterEverything`): `FgHostPrepare(block)` -> row-flipped copies
   of the owned twins (`FgTwins.h`) -> `PairFrame` -> `provider.Prepare` on a recording command list submitted
   through `ExecuteCommandList`. Providers tag `FgDepth12()` / `FgMv12()` / `FgHudless12()` (all resting in COMMON),
   never the Unity RTs and never the upscaler's own twins.
3. Unity's D3D12 submission thread (not the render thread; serialised with step 2 by `Pin`), Present hook:
   `FgHostOnPresent` copies Unity backbuffer (PRESENT -> COPY_SOURCE -> PRESENT)
   into shadow chain -> `provider.Generate` (FSR manual path only) -> `provider.BeforePresent` (DLSS: markers
   after copy fence wait) -> shadow `Present` (vendor paces internally) -> `provider.AfterPresent` (XeSS/DLSS:
   end markers) -> Unity's original `Present` (sync 0, ALLOW_TEARING).

### FG input contract (2026-10-12)

What `Fg_SetFrame` carries and what the providers tag (`native\Fg.h` `FgFrame`). Before this the generators got the
upscaler's inputs as they were: reprojecting the tagged depth with the constants **as sent** missed the tagged motion
vectors by 5226 px (tactical, pan + rotate + zoom, 53 px/frame) and 149 px (geoscape orbit, 2.6 px/frame) — the
"image shake growing toward the edges" with frame generation on. Four defects, one contract:

- **Orientation: top-down, like the back buffer.** A camera with a target texture renders through
  `GL.GetGPUProjectionMatrix(p, true)` (y negated), so `colorRT` / `depthRT` / `mvRT` and the upscaler's `out` twin
  hold image row 0 at the bottom; the present Blit turns the picture over into the back buffer (row 0 = top). The
  upscalers never see the back buffer; a generator warps exactly that. Measured with `Fg_Capture` (raw rows of what
  was tagged + Unity's back buffer of the same Present): hud-less vs back buffer correlation 0.08 same rows / 0.39
  mirrored before. No SDK has a flag for bottom-up inputs, so `FgTwins.h` copies depth, motion vectors and the
  hud-less colour row-flipped on the prep list (one compute pass each) and the providers tag those copies. After:
  0.50–0.78 same rows / −0.06–0.16 mirrored on lit scenes (the rest is the HUD; one geoscape frame read 0.78 / 0.58, the globe being nearly symmetric); a 7 px `PixelArt` block grid, anchored at row 0 of
  the `out` twin, lands on rows ≡ 5 (mod 7) = 1440 mod 7 in the tagged hud-less **and** in the back buffer; picture
  edge strength on the tagged depth's silhouettes 0.20–0.23 same rows vs 0.01–0.06 mirrored (tactical).
- **Matrices: Direct3D row-vector, left-handed view, reversed Z.** `view` = `worldToCameraMatrix` with z mirrored
  (view z = `cam.transform.forward`), `proj` = `GL.GetGPUProjectionMatrix(nonJittered, false)` with the z column
  mirrored (clip y up, w = +z, near = 1), both transposed (`clip = v * view * proj`). Was: Unity's GL projection
  (z in −1..1, w = −z) next to `depthInverted` / `INVERTED_DEPTH`, and for XeSS-FG column-vector matrices where its
  guide memcpy's an `XMFLOAT4X4` (`xess_fg_developer_guide_english.md:903-909`; read the other way the sent pair is
  716 px off). Streamline: `sl_consts.h:183-199`, chain order `sl_matrix_helpers.h:206-214`; the left-handed view is
  the basis `cameraRight` / `cameraUp` / `cameraFwd` span (`sl_matrix_helpers.h:185-190`): recomputing
  `clipToPrevClip` from the sent camera vectors by that helper matches the sent matrix to 6e-5. FSR-FG takes no
  matrices: camera position + right / up / forward (world space, `ffx_framegeneration.h:240-243`), near, far, fov.
- **Jitter and motion-vector scale follow the rows.** Jitter = the NGX offsets every upscaler takes, y mirrored:
  `(-jx, +jy)`; FSR documents that sign (`super-resolution-upscaler.md:259-261`: a clip-space translation by
  `+2jx/w`, `−2jy/h`), Streamline (`sl_consts.h:201-202`) and XeSS-FG (`xefg_swapchain.h:158-161`) only say "pixels" /
  "[-0.5, 0.5]" — same value as their upscalers get, mirrored. MV scale `(-renderW, +renderH)`: stored UV-space
  motion → render pixels, current → previous; Streamline gets it divided by the render size (`mvecScale`, [-1, 1]).
- **Pairing: the constants of the frame the twins hold.** Same ordering as `PairD3D12` (above): at frame N's event
  the twins — and the back buffer of frame N's Present — hold frame N-1. `Fg_SetFrame` returns its block, the driver
  passes it as the event's `data`, and `FgHostPrepare` → `PairFrame` hands the provider the **previous** event's
  view / proj / jitter / camera / reset / dt beside this event's resources and frame id (no previous block = reset).
  Left alone this was 0.62 px (tactical) / 0.05 px (geoscape) of residual at every radius.

Residual after (median, output pixels, by radius r/rmax; `fgana.py` on an `Fg_Capture` under camera motion; D3D12,
2560x1440 borderless, DLSS Quality 1707x960, FG 2x, `targetFrameRate=-1`, `vSyncCount=0`, limiter off):

| scene / provider | motion | 0–.2 | .2–.4 | .4–.6 | .6–.8 | .8–1 | base / presented fps |
|---|---|---|---|---|---|---|---|
| tactical, before, DLSS-G | 53 px | 5226.5 | 5224.8 | 5228.3 | 5227.6 | 5227.5 | 55–104 / 54 |
| geoscape, before, DLSS-G | 2.6 px | 148.6 | 148.8 | 149.2 | 150.1 | – | 270 / 192–248 |
| tactical DLSS-G | 36 px | 0.016 | 0.018 | 0.021 | 0.025 | 0.028 | 101 / 111 |
| tactical XeSS-FG | 34 px | 0.020 | 0.022 | 0.024 | 0.028 | 0.032 | 125 / 292 |
| tactical FSR-FG | 50 px | 0.012 | 0.015 | 0.018 | 0.023 | 0.027 | 105 / 237 |
| geoscape DLSS-G | 8.3 px | 0.003 | 0.003 | 0.003 | 0.003 | – | 109 / 104 |
| geoscape XeSS-FG | 8.4 px | 0.002 | 0.003 | 0.002 | 0.002 | – | 98 / 177 |
| geoscape FSR-FG | 18 px | 0.008 | 0.008 | 0.007 | 0.005 | – | 90 / 167 |

(`–` = the globe does not reach the corners: no depth there.) DLSS-G rows are measured unfocused — the production
plugin interpolates only with the window in the foreground (below); XeSS-FG and FSR-FG generate unfocused, 2.0x.

Diagnostics: `RenderforgeMod.FgCapture(dir)` / `FgCaptureStatus()` / `FgCaps()` (PPCLI) → `Fg_Capture`: `fgcap.txt`
(the constants each provider sent, the paired block and the previous one) + `fgcap.{depth,mv,hud,bb}.bin` (raw rows,
memory order). `RENDERFORGE_FG_DEBUG=1` turns FidelityFX's debug checker on (`fsr dbg[...]` lines in
`renderforge_fg.log`).

### FG pacing (2026-10-12)

The FPS LIMIT setting is the **presented** ceiling for every provider. One pacer at a time:

| State | who caps | how |
|---|---|---|
| FG off | Unity | `targetFrameRate = limit` (unchanged, nothing of the pacing code runs) |
| chain live, frames **flowing**, XeSS-FG | XeLL | `xellSetSleepMode.minimumIntervalUs = 1e6 / limit`; XeLL halves the rendered rate itself; Unity `targetFrameRate = -1` |
| chain live, frames flowing, DLSS-G | Reflex | `ReflexOptions::frameLimitUs = 1e6 / limit` (presented interval, see below); Unity `targetFrameRate = -1` |
| chain live, frames flowing, FSR-FG | Unity | `targetFrameRate = floor(limit / multiplier)`; the FSR swapchain only spaces the presents |
| chain live, **nothing fed** (main menu without a camera, loading, `HoldPrepare`) | Unity | `targetFrameRate = limit`; DLSS-G: Reflex `eOff`, no `frameLimitUs` (`slReflexSleep` + simulation markers still issued, they return at once); XeSS-FG: no XeLL frame at all |

- **Flowing** (`FrameGen.Flowing`) = the driver issued a `DLSS_EV_FG_PREPARE` within the last 3 frames (`FrameGen.Fed`).
  Before this a live XeSS-FG chain with no prepare events had no pacer at all (`Fg_SetFrameLimit` answered 1, Unity's
  limiter went off, `xellSleep` only ran inside `Prepare`): limit 120, `HoldPrepare` → 574 fps. Now 119 fps
  (`targetFrameRate=120`); menu with the camera feeding: 60 / 120.
- **Sleep at the start of the main thread's frame.** While a chain is live `FrameGen` keeps two systems in Unity's
  PlayerLoop (`UnityEngine.LowLevel.PlayerLoop`, present in the game's `UnityEngine.CoreModule` 2019.4.31):
  `FrameStart` first in `Initialization` (before `PlayerUpdateTime`, long before `EarlyUpdate.UpdateInputManager`) →
  `Fg_FrameBegin` → `IFgProvider::FrameBegin`: new frame id, `slReflexSleep` / `xellSleep`, `SIMULATION_START`;
  `SimEnd` last in `PreLateUpdate` (after `ScriptRunBehaviourLateUpdate`) → `SIMULATION_END`. The id travels in
  `FgFrame::paceId`; the render thread issues `RENDERSUBMIT_START`, the constants and tags, `RENDERSUBMIT_END`,
  `PRESENT_START/END` with the same id. Sources: XeLL "at the beginning of each frame, before sampling user input …
  Only one call per frame" (`xell_developer_guide_english.md:294-298`); Reflex guide §3.4.2 / §3.5 (sleep at the start
  of the simulation thread's frame, `SIMULATION_START` after it), Streamline `ProgrammingGuideDLSS_G.md:25,951`
  (frame index of the constants = the present markers'). No chain = no PlayerLoop systems (removed on teardown).
- **Pacing watchdogs (`FrameGen`, review 2, 2026-10-12):**
  - *PlayerLoop replaced by another mod* — `FrameStart` runs first in `Initialization`, before `PlayerUpdateTime`
    advances `Time.frameCount`, so it records N − 1 in frame N; `Pump` sees a frame it did not open in that same frame
    (`Time.frameCount - startFrame > 1`), re-inserts both systems and runs `FrameStart` late:
    a fresh pace id, the fed / flowing state kept. No idle present, no stale pace id (test lever `ResetPlayerLoop()`).
  - *A pacing system threw* (`PacingFailed`) — the chain could never be paced again (every prepare refused for lack
    of a pace id), so FG goes off: flag only inside the system, the next `Pump` releases the chain, `Apply` keeps it
    off, the picker shows `LastReason`; forcing a provider (`SetFgProvider`) re-arms.
  - *Fed but nothing prepared* (`Starved`) — FLOWING is the managed feed; the shim's word is `Fg_PreparedCount`. 120
    consecutive fed frames and 2 s without one prepared frame (twins that cannot be built, a refused option, a
    provider prepare failing for good) → the provider is marked failed for the session like a failed init, the chain
    is released (`Pump`), Auto moves on / the picker greys FG. Otherwise FSR-FG would sit at `floor(limit / 2)`
    rendered frames with nothing generated (test lever `SetFgFailPrepare(true)`).
- **One frame is fed once.** `DlssDriver.AfterPostProcessPreCull` feeds the chain on the first cull of the bound layer
  in a `Time.frameCount` only (`FgCaps`: `dupPreCull`, `dupFeeds`); `FgHostPrepare` refuses a prepare event whose pace
  id it already took BEFORE its pin and reset (`Fg_Status` `dup=`). A second event used to wait the full 100 ms pin
  timeout for a Present that could not come, then discard the first prepare.
- **Streamline frame tokens are not kept.** `slGetNewFrameToken(idx)` hands out one of 6 handles and a request for a
  different index rotates the ring (`source\core\sl.api\sl.cpp:1120`), so both threads carry the index and ask for
  its token where they use it. The first fed frame after an idle stretch has no id (the pacer starts with the next
  frame start) and is left unprepared: one more idle present.
- **`frameLimitUs` counts presented frames.** Measured with DLSS-G generating (development plugins +
  `sl.dlss_g.json {"runWhenNoFocus":true}`, geoscape, uncapped base 138): `frameLimitUs = 16666` → 30 rendered / 60
  presented at 2x; `8333` → 60 / 120 (2x), 40 / 120 (3x), 30 / 120 (4x); `4166` → 120 / 240, 80 / 240, 60 / 240. With
  nothing generated (production plugin, window not in the foreground) `8333` → 120 / 120. So the limit goes in
  undivided for every multiplier. Docs: `sl_reflex.h:45-50` ("the driver would be aware"), `ProgrammingGuideDLSS_G.md:769`.
- **Reflex low latency only while frames flow:** `Fg_FrameBegin(pace)` reaches the provider on EVERY frame of a live
  chain. `pace = 1` → `eLowLatency` + `frameLimitUs`; `pace = 0` (nothing fed) → `eOff`, cap 0 — a cap left set would
  also throttle Unity's own presents (`nvapi.h:18331`: the interval applies even without `NvAPI_D3D_Sleep`), and
  low-latency mode is never left on without its sleep. `slReflexSleep` and `SIMULATION_START/END` go out in both
  states (`ProgrammingGuideReflex.md:171,247`: "must always be called even when Reflex Low Latency mode is Off");
  `slReflexSetOptions` only when mode or cap change. `Create` starts `eOff`, `Destroy` ends `eOff`. XeLL is left
  alone while unfed (no `xellSleep`, no markers: its ids must be consecutive and each needs all six markers - see
  "Every XeLL frame id" below); its sleep mode is only set at the host's quiescent point ("FG host threading and
  retirement" below).
- **XeLL calls are serialised** (`FgXess.cpp` `XellLock`): markers come from three threads (main, render, presenting)
  and neither the XeLL guide nor `xell.h` states thread safety. One lock around every XeLL call except `xellSleep`,
  which blocks for the pacing interval and is never called under it.
- **DLSS-G `eOff` while idle:** after 30 presents without a prepared frame the host calls `IFgProvider::Idle` and
  DLSS-G goes `eOff` (`ProgrammingGuideDLSS_G.md:29`: paused / loading / menus); the next prepared frame re-issues
  `eOn`. `eRetainResourcesWhenOff` is set (`:593`). A resize detaches the chain, and `SetEnabled(false)` turns DLSS-G
  off before the chain is released (`:756,954`).
- **FSR-FG:** "rendered frame rate slightly below half the desired output frame rate"
  (`frame-interpolation-swap-chain.md:144`); Unity's limiter at `floor(limit / 2)` lands on 59.7–59.8 / 119–120 and
  119.9 / 239, i.e. just under — kept.
- **Prepare and Present run on different threads** (Unity's D3D12 backend presents from its submission thread:
  `host: first prepare tid` ≠ `first hooked present tid`). `FgHost.cpp` `Pin` serialises them: a prepare waits (≤ 100 ms)
  for the previous prepared frame's Present, a Present for a prepare in flight. Before, an overlapping prepare was
  refused or superseded the unpresented frame and the next Present went the idle way (FSR-FG tactical uncapped: 124 of
  2901 prepares overlapped). Counters in `Fg_Status`: `prepare=ok/events presents= idled= waited= maxWaitMs= stalls=
  dup=` (`stalls` = pin timeouts, `maxWaitMs` = the longest prepare wait). A waiter is woken by `Unpin`, by a detach
  (any thread), by `SetEnabled(0)` and by the pump, so it never sits out its timeout on a chain that went away;
  `presentStalled` is cleared only by a Present of the chain's own app swapchain.
- **Tagged inputs are valid "now" only, for every provider.** The three flipped twins (`FgTwins.h`) are one set of
  textures rewritten by every Prepare, and a frame whose Present does not come within the pin timeout is superseded
  by the next one. XeSS-FG was tagged `XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT`, which promises the resource untouched
  until "the `Present`/`Present1` call with the matching frame ID" (`xess_fg_developer_guide_english.md` "Resource
  Tagging", `xefg_swapchain.h:72-75`) - a promise the host cannot keep. Now `XEFG_SWAPCHAIN_RV_ONLY_NOW` with the
  prep list: "XeSS-FG will record a copy operation to the provided command list", reusable "as soon as the tagging
  command list has been submitted". DLSS-G already was `eOnlyValidNow` on the same list (`FgStreamline.cpp:573-575`).
  FSR-FG copies depth / motion vectors in its prepare dispatch on the list, but reads `HUDLessColor` from inside its
  Present (the latest `Configure`), queued after the list that wrote it: `FgTwins::Ensure` therefore releases the old
  twins on a size / format change only after the prep ring AND Unity's queue have drained (`D3D12Ring::DrainQueue`).
- **`xellSetSleepMode` only at the quiescent point.** "application must ensure any GPU activity is finished before
  `xellSetSleepMode` is called" (`xell_developer_guide_english.md:131`). Every call - the first one (low latency on)
  and each cap change - is issued by the presenting thread inside the Present hook, behind a proven drain of Unity's
  queue; see "FG host threading and retirement" below. (Until review 2 it ran on the main thread under a pin plus a
  drain whose failure was only logged, and `Create` issued the first one while Unity was rendering.)
- **Every XeLL frame id gets all six markers** (`xell.h:74-79` "required"; guide "Markers": "must be delivered each
  frame for XeLL to correctly calculate and execute the `xellSleep` API call"). `FrameBegin` opens an id for every
  paced frame, and some never reach Prepare: the frames of the grace period after the feed stops (`FrameGen`
  `IdleAfter`), a refused, failed or superseded prepare, a prepare event that never got the pin. The presenting
  thread closes them in id order (`ProviderXess::OpenSkipped` / `EndSkipped`): before a prepared frame's own markers
  every lower id, and on a Present without a prepared frame (`IFgProvider::UnpreparedPresent`) every id that can no
  longer be prepared - nothing was fed in its frame (`FrameUnfed`, told by the host at the next `FgHostFrameBegin`)
  or its prepare event has run (`PrepareDone`). An id whose event is still to come stops the walk (an
  age-in-presents rule closed ids too early when the presenting thread fell behind: 467 prepares refused in a 150
  ms-delay run). `xess: destroyed (... XeLL ids N, closed without a prepared frame M)` reports it: 9482 ids / 942
  closed in a run with every 20th Present delayed 150 ms (941 idle presents), prepares 9460 of 9461 events (measured
  before review 2; to be re-measured).
- **The provider's on/off runs under the pin, or not yet** - see "FG host threading and retirement" below.
- **`Fg_Capture` never frees what the queue may still use** - same section.
- Test lever `RENDERFORGE_FG_TEST_PRESENT_DELAY=ms,every` (env): every `every`-th hooked Present is held back `ms`
  before it pins - above 100 ms the next prepare times out and supersedes the unpresented frame.
- Diagnostics (PPCLI): `FrameStats(true|false)` (base-frame count, fps, p50 / p99 / max ms), `FgLatency()` (Reflex /
  XeLL frame reports), `SetFgMainPacing(false)` (A/B: sleep on the render thread inside `Prepare`, the old placement).

Measured, final build, D3D12, 2560x1440 borderless, DLSS Quality 1707x960, FG 2x, `vSyncCount=0`, game at High
priority, window in the background, foreign CPU load 60–100 % of all cores (other jobs on the machine). Base = Unity
frames (`Time.unscaledDeltaTime`, 5–6 s), presented = Present-hook counter + the provider's own count.

| scene / provider | limit 120: base / presented, p50 / p99 ms | limit 240 | limiter off |
|---|---|---|---|
| geoscape FG off | 119.5 / –, 8.33 / 8.76 | 237.1 / –, 4.17 / 4.75 | 330.6 / –, 3.02 / 4.01 |
| geoscape XeSS-FG | 60.0 / 120, 16.67 / 16.71 | 119.8 / 240, 8.34 / 8.44 | 186.0 / 380, 5.23 / 8.23 |
| geoscape FSR-FG | 59.7 / 118–120, 16.67 / 17.40 | 119.9 / 239, 8.33 / 8.54 | 223.4 / 440, 4.42 / 5.92 |
| geoscape DLSS-G, generating (dev plugin) | 60.0 / 120, 16.67 / 16.69 | 119.9 / 240, 8.33 / 8.34 | 137.5 / 275, 7.29 / 8.53 |
| geoscape DLSS-G 3x / 4x (dev plugin) | 40.0 / 120 · 30.0 / 120 | 80.0 / 240 · 60.0 / 240 | 117 / 351 · 103.5 / 412 |
| geoscape DLSS-G, production, unfocused (no generation) | 120.0 / 120, 8.33 / 8.34 | 239.3 / 240, 4.17 / 4.18 | 272.8 / 272, 3.65 / 4.38 |
| tactical FG off | 98 / – (CPU-bound; limit 60: 59.8, 16.67 / 17.06) | 98 / – | 98.1 / –, 10.45 / 11.81 |
| tactical XeSS-FG | 59.9 / 120, 16.67 / 16.71 | 94.6 / 196 (CPU-bound) | 96.9 / 199, 10.45 / 12.53 |
| tactical FSR-FG | 59.8 / 120, 16.67 / 17.21 | 98.5 / 197 (CPU-bound) | 95.8 / 193, 10.51 / 13.20 |
| tactical DLSS-G, production, unfocused (no generation) | 103.1 / 97–107 (CPU-bound; limit 60: 60.0 / 60, 16.67 / 16.83) | 101.2 / 98 (CPU-bound) | 99.1 / 101, 10.38 / 12.00 |

Before (HEAD `8f13fd2`, tactical, limit 120): XeSS-FG 60.0 / 120 p99 19.02; FSR-FG 59.5 / 118 p99 17.77; DLSS-G 59.7 / 59
p99 17.68 (Unity's limiter + Reflex sleep).

Latency reports (frame-start sleep, simulation start → present end, mean of the SDK's last 64 frames): geoscape limit
240 — Reflex 6.3–6.5 ms (2x generating: 6.4, 3x 6.8, 4x 7.0), XeLL 4.9 ms; tactical limit 120 — Reflex 14.1 ms, XeLL
10.8–14.4 ms. The render-thread placement cannot be compared through the same report: its `SIMULATION_*` markers
bracketed the prepare event (simulation 0.00 ms, start → present end 1.3–2.7 ms), i.e. it never measured the
simulation. What can be compared is pacing: DLSS-G 2x limit 240 generating, render-thread sleep 111.3 / 222, p99
10.28 ms; main-thread sleep 119.9 / 240, p99 8.35 ms.

- **Test-rig trap: an occluded game window.** Windows 11 stops honouring a process's timer resolution while its
  window is fully covered, `Sleep` falls back to 15.6 ms, and every `Sleep`-based limiter lands on half its target:
  Unity `targetFrameRate` 60 → 34 fps, 120 → 65 fps (FG off too), Reflex `frameLimitUs` 16666 → 55 fps with 30 ms
  frames, XeSS-FG's present pacing stretches. A player's foreground window is not affected. Unattended runs opt the
  game process out first (`SetProcessInformation(ProcessPowerThrottling)`, `ControlMask =
  PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`, `StateMask = 0`); every number above was taken that way.
- **Not observed:** DLSS-G generating with the PRODUCTION plugin (needs the window in the foreground); input lag
  by feel; presented-frame intervals (no provider exposes them — FrameView `MsBetweenDisplayChange` is the tool).
- **Open:** a Present of an unprepared frame that arrives after the next frame's prepare event consumes that
  prepare's constants (one frame early) — only around idle transitions, unchanged from before; exact pairing needs a
  per-frame id on the Present side.

### FG host threading and retirement (2026-10-12, review 2)

Rule: **a call that needs a synchronisation it did not get is not issued.** The wish is stored and applied at the
next proven-safe point of the thread that owns it. No "log and proceed", no longer waits, no lock held across an SDK
sleep. Three threads: main (PlayerLoop, `Fg_*` API), Unity's render thread (`DLSS_EV_FG_PREPARE`), Unity's D3D12
submission thread (it executes Unity's command lists - ours handed to `ExecuteCommandList` included - and calls
Present, so the Present hook runs on it: `host: first prepare tid` ≠ `first hooked present tid`).

| # | Invariant | Enforced at | Deferred request, safe point |
|---|---|---|---|
| 1 | A fence wait reports "retired" only after `GetCompletedValue() >= value` was read - never on the event alone. | `D3D12Ring::WaitOn` (`native\D3D12Ring.h`) | – (the wait runs to its original deadline) |
| 2 | `xellSetSleepMode` is issued only behind a successful `DrainQueue` made in the same Present hook, on the submission thread, with the chain pinned and nothing of that Present queued yet; never while a thread is inside `xellSleep`. | `ServeQuiescent` (`native\FgHost.cpp`) → `ProviderXess::Quiescent`, `SleepGate` (`native\FgXess.cpp`) | `wantUs` (main, `SetFrameLimit`) → next Present; no proof = no call, retried 30 presents later |
| 3 | `xefgSwapChainSetEnabled(1)` only once a low-latency sleep mode is set on that XeLL context - set = the SDK ACCEPTED it (review 3, "SDK results decide state"). | `ProviderXess::Quiescent` / `EnableFg` / `SetEnabled`; `Prepare` refuses until then | `enabled && !fgOn` → the first quiescent point of the chain |
| 4 | `IFgProvider::SetEnabled` runs on a live chain only between a successful `PinMain` and its `Unpin` (no Prepare, no Present inside). | `ApplyEnabled` (`native\FgHost.cpp`) | `H.provWant`, gate `H.enabled = 0` meanwhile → `FgHostPump`, every frame, no wait |
| 5 | No pace id a prepare event was issued for stays "event still to come" after that event returned. | `EventRan` on every exit of `FgHostPrepare`; `PrepareDone(H.evDone)` from the next pinned thread | – |
| 6 | Every skipped XeLL id's `PRESENT_START` / `PRESENT_END` bracket one real Present call. | `ProviderXess::OpenSkipped` / `EndSkipped`; `FG_PRESENT_HELD` + `FgHostAfterForwardedPresent` (`native\FgHook.cpp`) | – |
| 7 | What a timed-out capture list touches of ours outlives it: allocator, list, readback buffers, wait event, a reference on each twin it reads. | `CapPending` / `CapturePendingRetire` (`native\FgHost.cpp`) | released by the next capture or the chain's `ReleaseGpu` once its fence completed |

- **1 - stale fence registrations (blocker).** A ring has one auto-reset event for all its waits, and
  `SetEventOnCompletion` cannot be withdrawn. A wait for value 1 that timed out left its registration armed; when 1
  retired later the event was set, and the next wait - for value 2, or for a value on Unity's frame fence - returned
  `WAIT_OBJECT_0` at once with 2 still executing. Consumers: allocator reuse (`Begin` → `WaitSlot`), `WaitIdle` before
  resources are released, `DrainQueue` (`FgTwins::Ensure`, the quiescent point), the DLSS-G copy wait. Now a wake only
  means "read the fence again" and the wait continues until the deadline it was given. Proof without the game:
  `native\probe\retirement_probe.cpp` reproduces the old result on a WARP fence (`defect reproduced: the da602a4 wait
  reports 2 retired on the stale wake for 1`) and checks the new one (times out at its deadline; keeps waiting past a
  stale wake until the value retires).
- **2 - the quiescent point.** `xellSetSleepMode` used to run on the main thread at the start of a frame, under
  `PinMain` + `DrainQueue`: (a) a failed or timed-out drain was logged and the call made anyway, (b) even a good drain
  proved little - a `Signal` from the main thread covers what is on the queue at that instant, not what Unity's
  submission thread queues a moment later (`D3D12Ring.h` `unityVal`), and the pin keeps out our Prepare / Present, not
  Unity's submission, (c) `Create` made the first call while Unity was rendering. The call moved to where the proof
  exists: `FgHostOnPresent`, right after the pin, on the submission thread - Unity's frame is on the queue and nothing
  else gets there while that thread sits in the hook. `ServeQuiescent` asks `IFgProvider::WantsQuiescent`, drains
  with the copy ring's fence (`DrainQueue`, ≤ 100 ms) and only then calls `Quiescent`. A refused drain (timeout,
  failed `Signal`, quarantined ring) = no call; the request stays with the provider and is retried 30 presents later,
  so a stuck GPU costs one bounded wait per retry, not one per Present. Cost when served: the presenting thread waits
  for the GPU to finish the frame it just submitted, once per cap change and once per chain start.
  `xellSleep` (main thread, not under `XellLock`) and `xellSetSleepMode` exclude each other through `SleepGate`, a
  state word under a lock that is never held across either call: the mode setter waits ≤ 100 ms for the sleeper to
  leave (one pacing interval at most; the submission thread stays in the hook meanwhile, so the proof holds), a
  sleeper arriving during the mode call waits for that call (microseconds). The main thread never waits on the GPU.
  Not covered, and not coverable from the application: what the XeSS-FG proxy's own present thread still does with
  the previous frame. Assumption the proof rests on: Present is called by the thread that executes Unity's lists
  (evidence: the tids above; `ExecuteCommandList` "executes a given command list on a worker thread",
  `IUnityGraphicsD3D12.h`; the back-buffer copy and `Fg_Capture` already run on Unity's queue from this hook and
  the debug layer names no state error on our resources). The served log line carries Unity's frame-fence values
  for the regression run to look at. Review 3 narrowed what is claimed and added runtime measurements for the
  rest: "Present thread = submission thread" below.
- **3 - XeSS-FG chain start.** `Create` issues no sleep mode. XeLL runs in pass-through (`xellSleep` and markers
  still go out - the guide's own "disabled" shape, `:112-115`) and the proxy presents the application's frames
  (`xess_fg_developer_guide_english.md:652`) until the chain's first Present: sleep mode on, then
  `xefgSwapChainSetEnabled(1)` (`:237`, `:648`). `Prepare` returns false until then - nothing is tagged to a disabled
  chain; that frame goes out the idle way and its id is closed there. If the quiescent point can never be served, no
  frame is ever prepared and the managed starvation watchdog (`Starved`) drops the provider.
- **4 - the provider's on/off.** `slDLSSGSetOptions` "is NOT thread safe" (`sl_dlss_g.h:207`) and has to be ordered
  against Present (`ProgrammingGuideDLSS_G.md:486-494`); the render thread issues it from Prepare. `FgHostSetEnabled`
  closes the gate (`H.enabled = 0`: no Prepare and no Present enters any more), records the wish and tries
  `PinMain` (≤ 100 ms). No pin = the switch is NOT issued (before: logged and issued next to the thread inside);
  `FgHostPump` retries every frame without waiting, and the gate opens only behind the provider's own switch. A chain
  that went away meanwhile needs nothing: `DestroyChain` switches its provider off detached and unpinned.
  `Fg_Status`: `pendingEnable=` (-1 none, 0 / 1 the deferred wish).
- **5 - prepare events that never got the pin.** `Pin(true)` can time out behind a Present (or find the gate
  closed) and the event returned before `PrepareDone`: with the feed stopping right after, that id stayed unsettled
  and `OpenSkipped` stalled on it until 16 newer ids existed - i.e. for the whole idle stretch. `EventRan` now records
  the id on every exit (plain data, no pin), and the next pinned thread - the event's own tail or the Present hook -
  hands the provider the highest one.
- **6 - present markers around the Present.** "`XELL_PRESENT_START` and `XELL_PRESENT_END` markers delimit
  `IDXGISwapChain::Present`" (`xell_developer_guide_english.md:339`). Skipped ids got all four markers back to back
  before the call. Now `PRESENT_START` before and `PRESENT_END` after: the proxy's Present next to a prepared frame
  (`BeforePresent` / `AfterPresent`), Unity's own forwarded Present otherwise - `FgHostOnPresent` returns
  `FG_PRESENT_HELD`, the hook forwards and calls `FgHostAfterForwardedPresent`, and the pin stays with the presenting
  thread across that one call (as it already does across the shadow Present). Only a provider that opened something
  asks for it (XeSS); DLSS-G and FSR-FG take the old path.
- **7 - `Fg_Capture`.** A readback list that did not retire in 5 s kept its allocator and buffers, but not the twins
  it reads: `FgTwins::Ensure` / `ReleaseGpu` release those on the prep and copy rings' fences, which know nothing of
  that list. They are referenced now, together with the wait event its fence still holds registered, and everything
  is released when the fence completes (next capture or teardown); a new capture is refused while one is in flight.
  Unity's back buffer is deliberately not referenced - an outstanding reference makes Unity's `ResizeBuffers` fail,
  and Unity resizes behind a flush of the same queue.
- **Counters:** `Fg_Status` `quiesce=served/refused pendingEnable=`; log lines `host: quiescent point #n served …`,
  `host: quiescent point refused …`, `host: enabled=N deferred …`, `xess: XeLL sleep mode: low latency on, frame cap …`,
  `xess: frame generation on (behind the XeLL sleep mode)`.
- **Status:** built and probed without the game (`retirement_probe`, WARP; 397 checks with review 4); in-game
  regression pending - scenarios in the session's `regression-checklist.md`.

### SDK results decide state (2026-10-12, review 3)

Rule: **a provider's "applied" state says what the SDK accepted, never what was asked.** A refused call changes
nothing, the request stays pending, the retry is bounded and backed off, and a provider that cannot get there
FAILS where the managed side can see it - it prepares nothing, `FrameGen.Starved` (`src\FrameGen.cs:112`, 120 fed
frames and 2 s without a prepared one) marks it failed for the session, `Pump` (`:245`) releases the chain,
`Pacing(false)` + `ApplyFrameRate` restore Unity's limiter, Auto moves on / the picker shows `LastReason`. No
managed change was needed.

| Call | State it guards | On refusal | Bound | Where |
|---|---|---|---|---|
| `xellSetSleepMode` | `appliedUs` (>= 0 = low latency IS on) | `appliedUs` unchanged, FG not enabled behind it, `Quiescent` returns false | host back-off 30 presents; 8 in a row → `dead` | `native\FgXess.cpp:330` |
| `xefgSwapChainSetEnabled(1)` | `fgOn` (Prepare accepts frames on it) | `fgOn` stays 0, no frame tagged | same; 8 in a row → `dead` | `EnableFg` `:364` |
| `xefgSwapChainSetEnabled(0)` | `fgOn` | stays 1 (true to the SDK); Prepare also wants `enabled`, host gate closed | – | `SetEnabled` `:576` |
| XeSS tags / constants | – (per frame) | frame presented, SDK skips its interpolation | 120 frames in a row → `dead` | `:524` |
| `slReflexSetOptions` | `g_sl.reflex.mode/us` (`FgReflexLatch`) | unchanged; `reflex.bad` while a low-latency request stands refused → Prepare feeds nothing, `SetFrameLimit` returns false | that request retried every 30 asks (was: every frame); another request at once | `ReflexApply` `native\FgStreamline.cpp:155`, `native\FgLatch.h:24` |
| `slDLSSGSetOptions` (Prepare) | `isOn`, `sentW/H` | unchanged; Prepare returns false - no token / constants / tags for a DLSS-G that is off or sized for other inputs | that request (mode + size) retried every 30 prepares (was: every frame, frame tagged anyway); another request at once | `IssueOptions` `:420`, `:425` |
| `slSetConstants` / `slSetTagForFrame` | – (per frame) | warn, frame presented | 120 frames in a row → `dead` | `:639` |
| `ffxConfigure` / prepare dispatch | none kept (the wish travels in each Configure) | Prepare returns false, `preparedId` unchanged | watchdog | `native\FgFsr.cpp:256` |
| `CreateFence` (present-queue handoff) | `c.xfence` | chain published detached, destroyed by the ordinary path (was: DLSS-G copies on Unity's queue) | – | `native\FgHost.cpp:666` |

- Also: `maxSupportedInterpolations` / `numFramesToGenerateMax` are cached for the process only when the query
  succeeded (a failed query used to pin the caps at 2x until restart); XeLL marker, `SetPresentId`,
  `SetUiCompositionState`, `xellDestroyContext`, FSR pre-destroy `Configure` / wait results are logged, capped.
- XeSS-FG warnings are positive codes (`XEFG_SWAPCHAIN_RESULT_WARNING_*`): the call took effect (`XefgOk`).
- `dead` is per chain (`Zero()` clears it): a rebuilt chain tries again; the watchdog is what makes it per session.
- **Every refusal state has its way out (review 4, 2026-10-12).** A "refused" flag, a back-off and an "in a row"
  counter describe ONE request; they end with (a) an accepted call, (b) a request for what the SDK already
  accepted - no call is made, the accepted state is the wanted one, (c) the chain's destroy / recreate. The bug
  this rule comes from: cap 120 working → 144 refused once → back to 120 returned "in effect" from the cache and
  left `reflexBad` up - Prepare fed nothing, `Starved` dropped a healthy DLSS-G for the session.
  - Reflex (`FgReflexLatch::Apply`, `native\FgLatch.h:24`, invariant in its header): `bad` set by a refused or
    backed-off low-latency ask; cleared by any accepted call, by an ask for the accepted mode + cap, and by
    `NewChain` (`FgStreamline.cpp:375`, Create). The back-off is keyed to the refused request and survives an
    accepted other one (eOff between two low-latency asks does not turn it into a per-frame retry).
  - DLSS-G options: `optSkip` is keyed to the refused mode + size (`failOn/failW/failH`) - a size switched back,
    or "on" after `Idle`, is issued at once; an accepted call zeroes it; `Zero()` per chain.
  - XeLL: `modeFails` also ends when the wanted cap is the applied one (`FgXess.cpp:361`), `enableFails` when
    the "on" request is withdrawn (`SetEnabled(false)`, `:584`) - spread-out refusals no longer add up to
    `dead`. `inputFails` (both providers) already restarted on every good frame.
- **Test lever** `RENDERFORGE_FG_TEST_SDK_FAIL=<call>[,<count>][,skip=<n>]` (`FgTestSdkFail`,
  `native\FgHost.cpp:792`, read once in `FgHostInit`): `xell_mode`, `xefg_enable`, `sl_options`, `sl_reflex`,
  `sl_reflex@<frameLimitUs>` (only the Reflex requests for that cap: `sl_reflex@6944` = 144 fps),
  `fsr_configure`. The named call is NOT made and an error is reported in its place; `<count>` = that many
  calls, omitted = every call; `skip=<n>` = the first n matching calls go through first (a mid-run failure).
  Unset = one branch per call site. Log: `host: TEST forced SDK failure #n: <call>`.
- Log lines: `xess: xellSetSleepMode(...) REFUSED rc (n of 8 in a row)`, `xess: xefgSwapChainSetEnabled(1) REFUSED`,
  `xess: ... kept failing ... provider FAILED for this chain`, `sl: slReflexSetOptions(...) REFUSED`,
  `sl: slDLSSGSetOptions(n) REFUSED rc ... frames are not prepared`. `Fg_Latency` (XeSS): `wantUs= fgOn= dead=
  sdkFails=mode/enable/input markFails=`.
- Probe: the host's half (`Quiescent` false → not served, request kept, back-off, retried) and the Reflex latch
  with the lever as the SDK (`retirement_probe.cpp:308`: 120 → 144 refused → 120 feeds again, keyed back-off,
  eOff in between, new chain, `skip=`). The other provider halves need the vendor DLLs: in-game, with the lever.

### FG chain epoch (2026-10-12, review 3)

Defect: `FrameGen.Apply` (`src\FrameGen.cs:231`) releases and rebuilds the chain on a multiplier change and does not
wait for the render thread's queue. A prepare event queued for the old chain could run on the new one. Its
`paceId` (say 500) numbers frames of a provider that is gone; the new chain's ids start at 1. `EventRan(500)`
declared every pending id of the new chain settled and XeSS `OpenSkipped` closed them before their Prepare, or the
old event prepared the wrong id / superseded a prepared frame.

- `H.epoch` (`native\FgHost.cpp`, under `g_lock`) = generation of the chain in `H.c`. Written only by
  `PublishChain` (`:584`), in the critical section that makes the new chain pinnable, together with `evDone` and
  `lastEvPace`. Never 0 once a chain was built.
- Every block carries the epoch it was fed to: `FgFrame::epoch` (`native\Fg.h:96`), stamped by `FgHostSetFrame`
  under the lock (`:868`); 0 = no live chain then.

| # | Invariant | Enforced at |
|---|---|---|
| E1 | A prepare event whose block carries another generation touches nothing: not the duplicate filter, the pin, `prepared`, `EventRan`, the provider. The block is copied under the lock the ring is written under. | `FgHostPrepare` `:919`-`:952` |
| E2 | The generation is re-checked after every wake of the pin wait, in the critical section that takes the pin. A pinned thread sees one generation until it unpins (a new chain is published only after `DestroyDetached` saw nobody pinned). | `Pin(forPrepare, epoch, &stale)` `:311` |
| E3 | A pace id is recorded as "event ran" only for the generation that issued it. | `EventRan(epoch, id)` `:912` |
| E4 | A capture is noted and written only by the generation it was armed on; a rebuilt chain may be armed at once. | `FgCaptureArmed` `:1033`, `FgHostOnPresent` `:1170`, `FgHostCapture` |
| E5 | The result of Unity's own Present (reported after the pin is gone) detaches only the generation that Present belonged to. Per thread - no single-presenting-thread assumption. | `t_presentEpoch` `:1120`, `Detach(why, epoch)` `:259` |
| E6 | Backstop: a provider refuses a block of another generation. | `Prepare` in `FgXess.cpp:468`, `FgStreamline.cpp:532`, `FgFsr.cpp:235` (`FgSetup::epoch`) |

- Not epoch-stamped, by design: `DLSS_EV_RELEASE` / `DLSS_EV_CREATE`. The driver keeps exactly one RELEASE
  outstanding (`Dlss_ReleaseStatus` 0 = queued, re-issued only after -1; `DlssDriver.TryRetire`), queues CREATE only
  behind its acknowledgement, and `FrameGen.Retry` does not build a chain while the driver is retiring. A RELEASE
  that meets a live chain answers -1 (`FgHostRetired`) - the safe direction.
- Main-thread calls (`Fg_FrameBegin`, `Fg_SimEnd`, `Fg_SetFrameLimit`, `Fg_SetEnabled`) run on the thread that
  builds the chain: never stale. Resize / fullscreen / window teardown mean "whatever is live now".
- `Fg_Status`: `epoch=` `stale=` (dropped events). Log: `host: stale prepare event dropped (#n): block of chain
  epoch a (pace id p, frame f), live epoch b`.
- Probe (`retirement_probe`): stale event after a republish settles and prepares nothing; dropped without the pin
  wait behind a Present; generation replaced during the pin wait (second thread) → dropped after the wake; control
  event of the live generation prepared; capture and Present-result cases.

### Present thread = submission thread (2026-10-12, review 3)

The quiescent point (review 2, invariant 2) needs "nothing reaches Unity's queue while the presenting thread sits
in the Present hook". That holds if the thread calling Present is the one that submits Unity's command lists.
**It is an assumption.** `FgHookInstall` (`native\FgHook.cpp`) patches the DXGI vtable; it controls no thread
affinity, and `ID3D12CommandQueue::ExecuteCommandLists` is not hooked (no hook was added for this).

- **Proven at the point, independent of the assumption:** Unity's queue was empty at one instant (`DrainQueue`);
  our own Prepare cannot hand Unity a list meanwhile (pin); nothing of this Present is queued yet.
- **Made independent:** the generation a Present result belongs to is per thread (E5).
- **Still resting on it:** that the queue STAYS empty for the microseconds of `xellSetSleepMode`. If another
  thread submits Unity's lists, work can start on the GPU during the call - what xell guide `:131` forbids, with
  the consequence the SDK does not document (review 2 saw `0x887A0005` once before the quiescent point existed).
- **Measured instead** (always on; `Fg_Status`):

| Counter | What it is | Reads as |
|---|---|---|
| `quiesceBusy=` | Served quiescent points where a `Signal` right BEFORE the provider's call completed at once (control) and one right BEHIND it did not (`D3D12Ring::SignalDone`, `native\D3D12Ring.h:226`; `ServeQuiescent` `native\FgHost.cpp:761`). | > 0 = work reached the queue during the SDK call: assumption FALSE (or the XeSS proxy's own present thread used Unity's queue then - the log line of that point tells which frame). 0 over all served points = no violation observed. |
| `fenceMoved=` | Hooked Presents during which `GetNextFrameFenceValue()` changed between entry and exit of the host (`FenceWatch`, `:1103`). Two interface calls per Present. | > 0 = another thread advanced Unity's frame bookkeeping while the presenting thread sat in the hook: Present is not the only thread driving Unity's frames. Indicator, not proof of a submission. |
| `presentTid=tid/n` `prepareTid=tid/n` | Thread of the last hooked Present / prepare event and how often it changed. | `n` = 0 and the two tids differ = one dedicated presenting thread (consistent with the assumption). `presentTid` n > 0 = Present moves between threads: assumption false as stated. |

- Served log line: `host: quiescent point #n served: ... next a -> b; queue empty before the call 1, behind it 1) tid=…`;
  a violation appends `WORK REACHED THE QUEUE DURING THE CALL`.
- **What the regression run must read** (each provider, after a soak that includes cap changes, and once under
  `RENDERFORGE_FG_TEST_PRESENT_DELAY=300,2`, which holds the presenting thread in the hook 300 ms): `quiesceBusy=0`,
  `fenceMoved=0`, `presentTid=…/0`, every served line `before the call 1, behind it 1`. Control `0` on a served
  line = the probe was inconclusive there (an idle queue did not complete a Signal within 200 µs) - note it, it
  is not a violation.
- **Falsifiers:** `quiesceBusy > 0`; `presentTid` switches; `fenceMoved > 0` together with either. Then the
  quiescent point is not a proof on this engine build and the sleep-mode call needs a real submission gate - a
  hook on `ID3D12CommandQueue::ExecuteCommandLists` of Unity's queue (count + thread id, hold submissions across
  the call). Deliberately not built on a guess.
- **Residual risk if all three read clean:** a submitting thread that happened never to overlap a served point or a
  hooked Present in that run. Not excluded by measurement; excluded only by the hook above.
- Probe: an empty queue behind the call is not reported, a queue left waiting on a fence is (`quiesceBusy == 1`).

### One pacer at every instant (2026-10-12, review 5)

- **Invariant:** at every instant exactly one pacer enforces the user's limit. The provider's latency SDK is that
  pacer only while it has ACCEPTED the cap; in every other state Unity's limiter (`Application.targetFrameRate`) is.
- **Defect it replaces (regression FAIL 2):** `Fg_SetFrameLimit` answered with the WISH. XeSS returned `true`
  unconditionally, DLSS-G `!reflex.bad` - which is still `false` on a fresh chain whose first Reflex request has not
  been made. Unity's limiter went off, the SDK then refused (or had not been asked yet) and nothing re-evaluated:
  uncapped at limit 60 (`xell_mode` every call: 627 fed frames in 2.0 s; `sl_reflex`: 519 in 2.0 s; a fresh 3x chain:
  330 idle presents in ~1 s).
- **State, not wish:** `IFgProvider::PacerState()` -> `FG_PACER_HELD` | `FG_PACER_GENERATING` (`native\Fg.h`).
  - HELD - XeSS: `wantUs > 0 && appliedUs == wantUs` (`appliedUs` moves only on `xellSetSleepMode` SUCCESS).
    DLSS-G: the Reflex latch's accepted mode is low latency with exactly `LimitUs()`, not `bad`, and the frame is paced.
    A fresh chain, a cap change waiting for its quiescent point, a refused or backed-off request: NOT held.
  - GENERATING - XeSS: `fgOn` (the SDK accepted `xefgSwapChainSetEnabled(1)`). DLSS-G: `isOn && !dead && !reflex.bad`
    (Prepare's own gate). FSR: always (no latency SDK; never HELD).
- **Decision** (`RenderforgeMod.ApplyFrameRate`): `Fg_SetFrameLimit` records the cap and returns the bits.
  `providerPaces = flowing && HELD` -> `targetFrameRate = -1`. Otherwise Unity caps at `ceiling / multiplier` when
  GENERATING, and at the UNDIVIDED ceiling when not (nothing is generated: rendered = presented).
- **Re-evaluation:** the answer arrives on other threads (the presenting thread's quiescent point, the render
  thread's options). `FrameGen.FrameStart` polls `Fg_PacerState` every frame and calls `ApplyFrameRate` when the bits
  differ from the ones the last decision used (`FrameGen.NotePacer`). One frame of latency, no SDK call in the poll.
- **The 72 of J2** (limit 144, DLSS-G 2x, `sl_reflex@6944` refused for good): Unity capped at `144 / 2 = 72` because
  the old code divided by the chain's multiplier whenever frames FLOWED - but a refused Reflex request gates the feed
  (`reflex.bad`), nothing is generated, so 72 rendered = 72 presented. Now GENERATING is clear there and Unity caps
  at 144. Reflex's previously accepted cap (120) stays in effect inside the SDK - a refused change cannot take it
  back - so the frame rate is min(120, 144) until the watchdog drops the provider (~2 s): never above the limit.
- **`sdkFails` of XeSS (FAIL 3):** `modeFails` was reset only inside `Quiescent()`, and a refused cap change that is
  taken back needs no quiescent point (`wantUs == appliedUs`) - the host never went there, `sdkFails=1/0/0` stayed.
  `WantsQuiescent()` (presenting thread, pinned, every present) now ends the run when the wanted cap is the accepted one.

### One slDLSSGSetOptions per Streamline frame (2026-10-12, review 5)

- sl.log: `Repeated slDLSSGSetOptions() call for the frame N. A redundant call or a race condition with Present()`,
  once per DLSS-G chain start after the first (warnings = chains - 1).
- Streamline's frame advances with the PROXY's Present and only with it (measured: N = proxy presents + 1, unchanged
  across 30 unprepared presents). Options "take effect in the next Present() call that executes after" them
  (`ProgrammingGuideDLSS_G.md:487-488`).
- The two calls: the teardown's eOff (`Off()`, main thread, required before the chain goes, `:745-748`) - no Present
  follows it - and the next chain's eOn in its first Prepare. Same pair: `Idle()`'s eOff and the eOn of the resume.
- Fix (`FgStreamline.cpp`): `SlCore::presents` counts proxy Presents (`AfterPresent`), `optStamp` = presents + 1 at
  every set that reached the SDK. `IssueOptions` returns `kOptHold` when a set is already out for this frame: the
  frame is prepared (token, constants, tags, markers) and presented through the proxy with DLSS-G as it is (off:
  passed through), which ends the Streamline frame; the eOn goes out with the next prepare. Refusing the frame would
  deadlock - an unprepared frame bypasses the proxy. Cost: the first frame of a chain / after an idle stretch is not
  interpolated. `isOn` still follows the SDK's answer only; the `sl_options` lever does not stamp.
- Not covered: eOn followed by a teardown before any Present (the eOff cannot wait).

### A vendor Present that does not return (2026-10-12) - OPEN

- **Incident:** FSR-FG, fast off/on (900/1600 ms), 7th rebuild (epoch 17), video memory overcommitted by a second
  game instance. Player.log `D3D12Fence::Wait(16038) error: got 16037. Possible device removal.`, then Unity's render
  thread broke into the debugger after its 30 s fence timeout. Not reproduced in 35 later cycles.
- **What the dump proves** (`cdb` on crash.dmp, all threads):
  - Unity's D3D12 submission thread: `HookPresent1` -> `FgHostOnPresent` -> `c.shadow->Present` - inside AMD's proxy
    `Present` (the SECOND one of a fresh chain; the first returned S_OK), spinning in `waitForFenceValue`
    (`FrameInterpolationSwapchainDX12_Helpers.cpp:97-118`, no timeout) on "AMD FSR CompositionFence CPU", wanted 2,
    completed 0, for 30.09 s.
  - "AMD FSR Interpolation Thread": the same spin on "AMD FSR InterpolationFence", wanted 1, completed 0, 30.10 s -
    the fence `presentInterpolated` signalled on Unity's queue in Present #1 (`FrameInterpolationSwapchainDX12.cpp:1938`).
  - "AMD FSR Presenter Thread": parked on its pacer event (never woken: the interpolation thread never got there).
  - Unity's render thread: its own fence 16038 - most likely never SUBMITTED, the submission thread being stuck.
  - No frame of ours on any other thread; `g_lock` not held; the pin is held by the presenting thread across Present
    (by design), every waiter on it is bounded (`kPinWaitMs`), and the render thread was not waiting on it.
  - Device not removed (fences read 0, not UINT64_MAX; `removed=0x00000000` in the log); no TDR event.
  - The queue was alive at the end of the previous teardown (AMD's `shutdown()` signals and CPU-waits on it; it took 110 ms).
- **What it does not prove:** why a Signal on Unity's queue did not complete for 30 s. Not a queue-level Wait of ours
  (FSR presents on Unity's queue: `CopyBackBuffer` issues none there). Candidates: the GPU scheduler holding the
  queue while it pages under overcommit (no TDR for that), or a failing `Signal` AMD does not check.
- **Teardown order vs AMD's docs** (`frame-interpolation-api.md:318-337`, `frame-interpolation-swap-chain.md:262-268`):
  Configure(off) -> DestroyContext(FG) -> DestroyContext(swapchain) -> release the FINAL proxy reference. Ours had one
  deviation: the host released its proxy reference BEFORE the provider's Destroy (the XeSS-FG rule, applied to all).
  Now per provider (`IFgProvider::ProxyOutlivesDestroy`, FSR = true). Cannot explain the hang (it was the 2nd Present
  of the NEXT chain, the queue was alive in between) - conformance, not the fix.
- **Mitigations in the build** (none of them can interrupt AMD's spin):
  - `host: provider=... , vram local <usage>/<budget> MB [OVER BUDGET], shared ...` at every chain start.
  - Present watchdog (own thread, reads and logs only): a shadow Present that has not returned for 2 s ->
    `host: WATCHDOG shadow Present #n (...) has not returned for N ms ...: removed=..., Unity queue probe COMPLETED |
    DID NOT COMPLETE in 1 s, vram ...`. The probe is a fence of our own signalled on Unity's queue: COMPLETED = the
    queue retires work and the provider waits on something of its own; DID NOT COMPLETE = the queue itself stands still.
  - Test lever `RENDERFORGE_FG_TEST_VRAM_MB=<n>`: commits n MB of throwaway textures before the first chain.
- **Residual risk:** the hang can recur under video-memory overcommit; the process then dies in Unity's 30 s fence
  timeout. Not mitigated: keeping the FSR proxy across off/on (Configure(off) instead of a rebuild), refusing FG over budget.

### XeLL marker refusals (2026-10-12) - OPEN, instrumented

- `xess: xellAddMarkerData(id N, marker 2..5) -4` in 2 sessions (id 4 right after the first quiescent point of a
  chain; ids 63/64 mid-chain). -4 = `XELL_RESULT_ERROR_INVALID_ARGUMENT` (`xell.h:54`); markers 2..5 =
  RENDERSUBMIT_START/END, PRESENT_START/END. SIMULATION_* of the same ids were accepted.
- Ruled out: an id used on another context (ids and the context are per provider, ids restart per chain), marker
  order (sleep returns before the markers, START before END - by construction; guide `:296-298`, `:320-329`).
- Unverified hypothesis: XeLL forgets an id once the sleeping thread is two ids ahead of it. The refusal line now
  carries `newest slept id` and the thread; the first 32 are logged. No fix without that evidence.

### Debug-layer attribution (2026-10-12)

- Every D3D12 object of ours is named `Renderforge ...` (rings: lists, allocators, fence, timestamp heap; post pass;
  FG twins; FG capture; the DLSS-G present queue; the queue handoff fence; under `RENDERFORGE_D3D12_DEBUG` also the
  shadow chain's back buffers). A message that names none of them is not about our lists or resources.
- `renderforge-d3d12.log` records, per chain, the first `FG copy:` lines (Unity back buffer pointer, queue, thread)
  and - now for FSR generations too - the four Unity RTs a generation declares (`RfDbg::Resource` / `States`).
- Regression sessions (S1 geo / S3 tactical / L1): E527 `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH` 71702 / 9821 / 1039
  and E538 `INVALID_SUBRESOURCE_STATE` 40791 / 16233 / 1090, ALL on unnamed (Unity) lists; none names a ring list,
  none names one of the four declared RTs (checked by pointer), the same signatures hit RT sizes we never create
  (2048x1024, 192x192, 1024x32) and come from ~27 submitting threads. They are Unity's own state tracking for its
  depth copies and ping-pong targets under the driver's command buffers (see the 2026-09 note above).
- Back buffer: 1-2 messages per session in 88-123 chain starts (`Draw` with `BackBufferN_0` in PRESENT on a worker
  thread; once `Draw` + barrier with it in COPY_SOURCE on the submission thread). Our only access is `CopyBackBuffer`:
  PRESENT -> COPY_SOURCE -> PRESENT inside ONE list, and validation is per `ExecuteCommandLists` call, so no other
  list can observe the intermediate state; no message names `Renderforge ring`. Unattributed - not shown to be ours.
- E1315 x2, W954, W1361, W1380, W680: our code has no such call (our descriptor heaps are SHADER_VISIBLE, we never
  pass 0 barriers, create no sampler / graphics PSO / placed resource) - Unity or vendor.

### Geoscape click probe (2026-10-12)

- `ProbeMarkerClick` / `ProbeCraftClick` replicate the game's pick: cursor ray vs the GLOBE layer (10000 units,
  `GeoscapeView.cs:912-914`), then `GeoMap.PickObjectOnPosition<T>(point, true)` - a ray from the globe's centre
  through that point vs the PICKING layer (`GeoMap.cs:822-851`). The old probe cast the camera ray against every
  layer and took the nearest collider, which near the limb belongs to another site than the game picks.
- Every near-side site / aircraft on screen: `n / same / globeMiss / none / other`; PASS = `other == 0`
  (globeMiss = the pixel is off the globe, none = no enabled actor under it: the game picks nothing either way).

### HUD

The present path gives every SDK what it needs: the upscaler's `out` twin = upscaled scene WITHOUT the HUD, back
buffer = same frame WITH it. The geoscape markers and aircraft drawn after reconstruction (`GeoMarkerOverlay`) are
in the back buffer only, i.e. HUD for the generators ("Geoscape markers after reconstruction" → "With frame
generation"). The generators get the host's row-flipped copy of it in the back buffer's format
(`FgTwins.h`; an FP16 `out` is sRGB-encoded into 8 bits on the way): FidelityFX's `HUDLessColor`, XeSS's UI mode 4
(`BACKBUFFER_HUDLESS`) and DLSS-G's `kBufferTypeHUDLessColor`. No UI render target is built.

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
  `RENDERFORGE_XESS_JITTER_SIGN`, `RENDERFORGE_D3D12_DEBUG`, `RENDERFORGE_FAKE_INIT` (§ Failure
  handling → Dev switch).

### Known limits

- Windowed/borderless only; exclusive fullscreen -> FG torn down.
- NVIDIA focus gate: unfocused window -> real frames only, no error, no log. Production plugin requires
  the window in the foreground — the normal state when a player plays.
- Frame-pacing metric (CoV of `MsBetweenDisplayChange` via PresentMon) unmeasured.
- Debug layer: no message names one of OUR lists or resources (`Renderforge ring N`, `Renderforge fg *`). The
  remaining `id=527/538` (resource state mismatch) and `id=1315` (descriptor heap not shader-visible) come from
  unnamed lists and 10–21 of them name `BackBufferN_0`, mostly with FSR-FG, also before the review fixes. **Not
  attributed:** "Unity's own" was an assumption, never shown. Our only back-buffer access is `CopyBackBuffer`
  (`FgHost.cpp`, PRESENT → COPY_SOURCE → PRESENT on a named ring list) and the `FgTwins` heap is shader-visible; a
  link to the one XeSS-FG `DEVICE_REMOVED` / `INVALID_CALL` (seen once at `dc5a881`) is neither shown nor excluded.
  Needs the full messages (`RENDERFORGE_D3D12_DEBUG`) and DRED breadcrumbs.
- Full contract note: `E:\DEV\PhoenixPoint\docs\research\framegen-d3d12-contract.md`.

## Packaging (Phase 6, 2026-09-02)

- **Build stamp (2026-10-12).** Both DLLs carry `RFSTAMP|kind|commit|dirty|src|END` (`build\stamp.ps1`,
  `Rf_BuildStamp()` in the shim, `AssemblyMetadata("RfStamp")` + `BuildStamp.Value` in the mod; logged at load,
  `RenderforgeMod.BuildInfo()`). `deploy.ps1` and `build\release.ps1` read it back out of the files and refuse a binary
  that was not built from the tree on disk; a release additionally needs a clean tree and a `-Clean` native rebuild.
  Native objects follow source CONTENT (per-build-dir manifest, changed files are re-dated), and the quality gate
  builds into `build\gate\` - never into the dirs a deploy links from. Runbook: `docs\OPERATIONS.md` §Build stamp.
- `build\release.ps1` is the only packaging path. It reads the version from `meta.json`, walks an
  ordered pack table, and emits `Renderforge-{Core,NVIDIA,AMD,Intel,Full}-<v>.zip` plus
  `SHA256SUMS.txt` into `build\release\`.
- **Layout rule:** every zip has ONE top-level `Renderforge/` folder and is extracted into
  `<Phoenix Point>\Mods\`. Core + any vendor packs overlay into a single `Mods\Renderforge\` in any
  order. (1.0.0's zip was flat and extracted INTO `Mods\Renderforge\`; 1.1.0 changed it, because an
  overlay cannot work without the prefix.)
- Pack contents: **Core** = `Renderforge.dll`, `RenderforgeNative.dll`, `rf-exposure-d3d12.bundle`,
  `meta.json`, `README.md`, `LICENSE`, `LICENSE-NIS.txt`. **NVIDIA** = `nvngx_dlss.dll`, `nvngx_dlssg.dll`,
  `sl.{interposer,common,dlss,dlss_g,reflex,pcl}.dll` 2.14.1 + `LICENSE-NVIDIA.txt`. **AMD** =
  `amd_fidelityfx_{loader,upscaler,framegeneration}_dx12.dll` + `LICENSE-AMD.txt`. **Intel** = `libxess.dll`,
  `libxess_fg.dll`, `libxell.dll` + `LICENSE-INTEL.txt`. **Full** = the union. Every runtime is always packed —
  the `-WithFrameGen` opt-in was removed after a bare run produced a 122 MB Full zip without FG (1.5.1 prep).
- Each zip also carries `manifest-<pack>.json`: mod id, version, generation timestamp, and per file
  the name, FileVersion, byte size, SHA-256, required Authenticode signer and licence file. Per-pack
  names, not one shared `manifest.json`, because vendor packs are extracted on top of Core.
- Every vendor DLL is Authenticode-asserted before packing — subject must contain `NVIDIA Corporation`
  / `Advanced Micro Devices` / `Intel Corporation`, status must be `Valid`; anything else fails the
  build. NVIDIA reports FileVersion with commas (`310,7,129,0`), so all comparisons normalise
  `-replace '[ ,]', '.'`.
- Stale-DLL guard: `$NewestKnownNgx` pins `nvngx_dlss.dll` and `nvngx_dlssg.dll` at `310.9.1.0`;
  a mismatch is a WARNING telling the operator to check the TechPowerUp DLL databases. Warning, not
  error — a newer DLL is legitimate, it just has to be a deliberate choice.
- Missing vendor DLLs are a supported state at runtime: `Availability.Reason` returns
  `DLL missing: <name> — install the <Vendor> pack` (RU: `Нет файла: <name> — установите пакет
  <Vendor>`), the row greys out, and everything else keeps working.
- Release checklist: `docs\RELEASING.md`. GitHub release and Steam Workshop upload are user-gated.
- Steam Workshop item: **TBD — filled in by the Workshop publish task**. Uses PerkOracle's
  SteamworksPy publisher (`PerkOracle\docs\OPERATIONS.md`), appid 839770, content = the Full pack.

## Quality knobs (1.4.0)

`src\QualityKnobs.cs` (state + patches), `src\QualityPanel.cs` (rows). Config: `Vignette`,
`ShadowResolution`, `Anisotropic`, `LodBias` on `DlssConfig`, hidden from the Mods menu.

- **Vanilla = write the captured baseline back, never "skip the write."** A skipped write would leave
  the mod's value in place until the next preset change.
- **Baseline capture.** Vanilla writes `QualitySettings` inside `OptionsManager.UsePreset`
  (`OptionsManager.cs:384`), which nests `ChangeGraphicsQuality` (`:391`) → `OnGraphicsSettingsChangedEvent`
  (`:406`) → `LightingManager.ApplyPostProcessOptions` (`LightingManager.cs:53-55`). A Harmony prefix
  raises `inUsePreset`, a `HarmonyFinalizer` lowers it — always — and re-applies the knobs; it snapshots
  `{anisotropicFiltering, lodBias, shadowResolution}` **only when the original method did not throw**
  (`__exception == null`), and returns `__exception` unchanged so the failure is rethrown, not swallowed. Finalizer, not
  postfix: a throw must not latch the guard. Success-gated snapshot, because `UsePreset` can throw at
  `DefinedPresetIndexToQualityIndex` (`OptionsManager.cs:377,447`) *before* `SetQualityLevel` (`:384`) — at that point the
  live `QualitySettings` are still the mod's own overrides, and snapshotting them would record them as "Vanilla".
- **Fallback capture.** If no snapshot exists at the first mod write (preset applied before Harmony was
  installed), the current values are captured right before that write. An uninitialised snapshot is never restored.
- **Vignette** is captured per volume, before the first override, and written on `profile` — the runtime
  clone vanilla writes (`LightingManager.cs:172-178`) — never `sharedProfile`. The write is followed by the
  same invalidation vanilla performs at `LightingManager.cs:167`:
  `PostProcessManager.NeedUpdateSettings = true` (`PostProcessManager.cs:25`). The volume is reacquired on
  every seam because `LightingSettingsDef.ApplyTo` re-instantiates the lights prefab each level
  (`LightingSettingsDef.cs:21-22`).
- **Anisotropic restore** = the snapshot value plus `Texture.SetGlobalAnisotropicFilteringLimits(-1, -1)`,
  the engine default. Coexistence with another mod's limits is not attempted.
- **Seams:** `OptionsManager.UsePreset` finalizer (scalars + vignette), `LightingManager.ApplyPostProcessOptions`
  postfix (vignette only — scalars are suppressed while `inUsePreset`), `RenderforgeMod.OnLevelStart` (everything).
- **LOD slider positions are contiguous, values are not.** Valid biases are `0` (Vanilla) and `1.0 … 4.0`, so the slider
  runs `0..31` and maps position → value (`1.0 + (pos - 1) * 0.1`) instead of scaling. A scaled `0..40` slider would snap
  positions 1–9 back to 10 and trap keyboard/controller decrement at `1.0`.
- **Row order in Options → Graphics:** LUT → Colour vision → Exposure / Black point / White point / Brightness / Contrast / Clarity / Vibrance / Saturation → Reset image settings → Scene style → Quality rows.
- **No "Extreme" shadow tier.** Unity 2019.4 caps custom shadow maps at 4096 dir / 2048 spot / 1024 point,
  so a tier above `VeryHigh` would be meaningless. 11 of 71 lights cast shadows, all `FromQualitySettings`.

## Idea backlog (user, not scheduled)

- **Color grading preset / LUT** (2026-09-02, "like Cyberpunk's natural-grey look"): PPv2 already
  ships `ColorGrading` with `LDR LUT` / `External LUT` modes — drop in a LUT strip (1024×32) or
  3D LUT with `contribution`, or build the look from temperature / saturation / contrast /
  lift-gamma-gain without a file. Delivery = our own high-priority `PostProcessVolume` or a Harmony
  tweak of the existing `ColorGrading` settings. Independent of DLSS (grading runs before the
  upscale in the colour buffer). Could be a "Color preset" picker in this mod or a separate mod.

- **Reflex standalone (FG off)** (2026-09-05, reviewed with Codex, deferred): Streamline
  `Reflex|PCL` init on the game device + sleep/markers in the Present hook. Ship only with a real
  `Update` simulation marker + input boundary (not the synthetic pair at `FgStreamline.cpp:375`),
  measured input-to-photon gain, `lowLatencyAvailable` gate (GeForce 900+), and one shared SL
  lifecycle with FG (`FgStreamline.cpp:117`). Spec: `docs\superpowers\specs\2026-09-05-quality-knobs-colour-vision-design.md` §C.

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
