# FSR through ffx-api on D3D12 — the contract Renderforge implements

Source: `E:\DEV\PhoenixPoint\refs\FidelityFX-SDK` (FidelityFX SDK 2.3), read 2026-09-02.
Companion to the outer repo's `docs\research\dlss-ngx-d3d11-contract.md`; only the differences from NGX are spelled out.

## 1. Surface

- 5 functions, all in `amd_fidelityfx_loader_dx12.dll`: `ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`,
  `ffxQuery`, `ffxDispatch` (`api/include/ffx_api.h`).
- Load them with `LoadLibrary` + `GetProcAddress` — AMD's own recommendation (`docs/getting-started/ffx-api.md:15`);
  `ffx_api_loader.h` ships `ffxLoadFunctions(ffxFunctions*, void* module)` to do it. An import lib exists
  (`signedbin/amd_fidelityfx_loader_dx12.lib`) but linking it makes the host DLL unloadable without the AMD DLLs.
- **The loader resolves effect DLLs by bare name** (`amd_fidelityfx_upscaler_dx12.dll` etc., seen in its strings).
  Pre-load the effect DLL by absolute path first; the loader's bare-name lookup then binds to that module
  (`native/FfxLoader.cpp`).
- Everything is a descriptor chain: `ffxApiHeader {type, pNext}`. **Pointers in the create chain must stay alive
  until `ffxDestroyContext`** (`ffx_api.h:140`) — store the descs, never build them on the stack.

## 2. Creating an upscale context (D3D12)

Chain: `ffxCreateContextDescUpscale` → `ffxCreateContextDescUpscaleVersion` → `ffxCreateBackendDX12Desc`.

- `ffxCreateContextDescUpscaleVersion.version = FFX_UPSCALER_VERSION` is **mandatory since SDK 2.1**
  (`docs/techniques/super-resolution-ml.md:99`, `ffx_upscale.h:225`). Omitting it is undefined behaviour.
- `ffxCreateBackendDX12Desc.device` = the `ID3D12Device` (from `ID3D12Resource::GetDevice` on a Unity RT).
- `fpMessage` is a `void(uint32_t type, const wchar_t*)` callback; wire it to a log even without
  `FFX_UPSCALE_ENABLE_DEBUG_CHECKING`, and add that flag while debugging — it names the bad parameter.
- Context creation needs **no command list** (unlike `NGX_D3D12_CREATE_DLSS_EXT`), so it does not go through the
  ring: the probe counts `ExecuteCommandList` calls = dispatches only.
- `RfDbg::Attach(device)` is called in `Fsr12::Init` (after `ring.Attach`) for D3D12 debug-layer message routing.

## 3. Flag mapping (Unity/BiRP → ffx)

| Unity fact | ffx create flag |
|---|---|
| colour RT is FP16 HDR | `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE` |
| reversed-Z depth (`SystemInfo.usesReversedZBuffer`) | `FFX_UPSCALE_ENABLE_DEPTH_INVERTED` |
| finite far plane | **no** `FFX_UPSCALE_ENABLE_DEPTH_INFINITE` (the debug checker warns when INFINITE meets a small `cameraFar`) |
| MVs at render resolution | **no** `FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS` (that flag means display-res MVs) |
| no exposure texture | `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE` |
| MVs carry no jitter | **no** `FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` |
| linear colour space | **no** `FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE` |

## 4. Conventions

- **Motion vectors**: current → previous, screen space, range `[<-w,-h> … <w,h>]`
  (`super-resolution-ml.md:129`) — the same direction NGX wants, so the driver's `mvScale = (-renderW, -renderH)`
  (Unity's UV-space `current - previous`) is passed straight into `motionVectorScale`.
- **Jitter**: unit pixel space, composited as `projX = 2*J.x/renderWidth`, `projY = -2*J.y/renderHeight`
  (`super-resolution-ml.md:233`). **Y is negated relative to X, and relative to NGX.** Renderforge's driver applies
  `proj[0,2] += 2*jx/w`, `proj[1,2] += 2*jy/h` and stores NGX's `(-jx, -jy)`, so the ffx dispatch gets
  `jitterOffset = (jitterSignX * fp.jitterX, jitterSignY * fp.jitterY)` with defaults `(-1, +1)`. Getting this wrong
  doubles thin edges instead of resolving them. **UNVERIFIED in-game** until the Phase 3 screenshots land — the sign
  was derived, not observed. Env override `RENDERFORGE_FSR_JITTER_SIGN` = `"sx,sy"` (each in {-1,1}), read once at
  `Fsr12::Init`, logged `FSR: jitterSign=%d,%d`.
- **Jitter phase count**: `ffxQueryDescUpscaleGetJitterPhaseCount` / `...GetJitterOffset` are available (null-context
  queries) but unused — the driver's own Halton(2,3) with `phases = 8*ratio²` matches the documented sequence
  lengths (Quality 18, Balanced 23, Performance 32, Ultra Performance 72).
- **Depth**: single float, render resolution, must be jittered like everything else.
- **`frameTimeDelta` is MILLISECONDS** (~16.6 at 60 fps); `preExposure` must be `> 0`.
- **Camera**: `cameraNear`, `cameraFar`, `cameraFovAngleVertical` (radians) are required dispatch fields; they reach
  the shim through `Dlss_SetCamera` (cached, copied into every frame slot by `Dlss_SetFrame`).
- **Owned-resource model** (D3D12, `D3D12Owned.h`): the shim OWNS the four textures ffx touches
  (`colorIn` R8G8B8A8_UNORM, `depthIn` R32_FLOAT, `mvIn` R16G16_FLOAT, `out` +UAV, same-family typed twin of the
  Unity RT). Unity RenderTextures are only the SOURCE or DESTINATION of a `CopyResource` in our list — never handed
  to ffx. Why: Unity 2019.4's D3D12 state tracker varied the pre-state of a RT per frame (measured debug-layer
  id=527, 2026-09-02), so every barrier the SDK recorded on it mismatched.
- **Measured Unity RT pre-states** (deterministic per RT, 614-628 frames sampled):
  `colorRT` RENDER_TARGET, `depthRT` RENDER_TARGET, `mvRT` COPY_DEST, `outRT` GENERIC_READ.
  Our list transitions each Unity RT from its pre-state to COPY_SOURCE/COPY_DEST for the copy and puts it BACK;
  `OwnedSet12::Declare` sets `expected == current == pre-state` so Unity's tracker agrees.
- **Per-frame barrier sequence** (one ring list):
  Unity colorRT/depthRT/mvRT: pre-state -> COPY_SOURCE (copy in) -> pre-state.
  Unity outRT: GENERIC_READ -> COPY_DEST (copy out) -> GENERIC_READ.
  Owned colorIn/depthIn/mvIn: COMMON -> COPY_DEST (copy in) -> NON_PIXEL_SHADER_RESOURCE (ffx) -> COMMON.
  Owned out: COMMON -> UNORDERED_ACCESS (ffx) -> COPY_SOURCE (copy out) -> COMMON.
  Every owned resource starts and ends in COMMON (the state it was created in).
- **State restoration by ffx is still guaranteed**: `UnregisterResourcesDX12` (`ffx_dx12.cpp:2413-2426`) walks every
  app-provided resource back to the `state` declared in `FfxApiResource` and flushes the barriers. The owned inputs
  are declared `FFX_API_RESOURCE_STATE_COMPUTE_READ`, owned output `FFX_API_RESOURCE_STATE_UNORDERED_ACCESS`, so
  `OwnedSet12::Leave` starts from those SDK states.
- **Optional inputs**: exposure, reactive and transparency-and-composition masks are optional for FSR 3.1 and FSR 4
  (`super-resolution-ml.md:154`); `ffxQueryDescUpscaleGetResourceRequirements` reports what a given provider really
  wants. Renderforge passes none.
- **Sharpening**: `enableSharpening` + `sharpness` (0..1) runs FSR's own RCAS inside the dispatch — no extra copy,
  so the shim's NIS/RCAS pass is skipped for FSR and `Dlss_Sharpener()` reports `DLSS_SHARPEN_RCAS` (2).
- **Mip bias**: `log2(render/display) - 1.0`, same formula the DLSS path already uses.

## 5. Quality modes

`FfxApiUpscaleQualityMode`: `NATIVEAA` 1.0x, `QUALITY` 1.5x, `BALANCED` 1.7x, `PERFORMANCE` 2.0x,
`ULTRA_PERFORMANCE` 3.0x. `ffxQueryDescUpscaleGetRenderResolutionFromQualityMode` converts display → render size;
as a **null-context** query it must have an `ffxCreateBackendDX12Desc` chained in `pNext`, or it returns
`FFX_API_RETURN_NO_PROVIDER` (`ffx-api.md:133`). Verified: 1920x1080 Quality → 1280x720. FSR has no dynamic
min/max range without `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION`, so `Dlss_GetOptimal` reports min = max = render.

## 6. Version selection and reporting

- `amd_fidelityfx_upscaler_dx12.dll` (28 MB) contains FSR 4.1.1 (ML) plus the 3.1.5 and 2.3.4 providers
  (`ffx-api.md:27`). **FSR 4 needs an AMD RX 7000 or 9000-series GPU** (`super-resolution-ml.md:325`).
- `ffxQueryDescGetVersions` (null context, embeds `device`) enumerates the providers available for that device:
  call it once for the count, once for the arrays. `ffxOverrideVersion` in the create chain forces one.
  **Renderforge forces nothing** — the documented default path — and reads the result back with
  `ffxQueryGetProviderVersion` on the created context.
- Version *names* live in ffx global memory and a later query may overwrite them (`ffx-api.md:243`): copy the
  string immediately.
- Observed on the dev machine (NVIDIA RTX 5070 Ti, 2026-09-02, `dlss_probe.exe . --fsr`): the version list is
  `3.1.5 2.3.4` — **the 4.x provider does not even appear** for a non-AMD device — and the created context reports
  **3.1.5**. The FSR 4.1.1 ML path is untestable without RDNA3/RDNA4 hardware.

## 7. Gotchas found while integrating

1. `_WINDOWS` (or `PLATFORM_WINDOWS`) must be defined or `ffxLoadFunctions` compiles to an empty body and every
   function pointer stays NULL (`ffx_api_loader.h:38`). Set as a CMake compile definition for both targets.
2. `ffx_api.h` declares the five entry points `__declspec(dllexport)`; including it in a DLL that then resolves
   them by `GetProcAddress` is fine, but do not also link the AMD `.lib` — pick one.
3. The create-descriptor chain must outlive the context (see §1) — a stack-local chain "works" until it does not.
4. `bool` fields (`enableSharpening`, `reset`) are C `bool` in a C header: `stdbool.h` is pulled in by
   `ffx_api_types.h`, so the struct layout matches only when the caller is C++ or C99.
5. ffx return codes are mapped onto `NVSDK_NGX_Result` so the existing ABI/UI keeps working: `OK` → Success,
   `ERROR_PARAMETER` → `FAIL_InvalidParameter`, `ERROR_MEMORY` → `FAIL_OutOfGPUMemory`, `NO_PROVIDER` →
   `FAIL_FeatureNotSupported`, anything else → `FAIL_PlatformError`; `Dlss_LastError()` = `DLSS_ERR_FFX` (-5).
