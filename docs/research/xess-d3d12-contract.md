# Intel XeSS-SR on D3D12 — the contract Renderforge implements

Source: `E:\DEV\PhoenixPoint\refs\XeSS-sdk` (XeSS 3 SDK 3.0.2, `libxess.dll` 2.0.2.68, Intel Authenticode-signed),
headers `inc\xess\xess.h` + `inc\xess\xess_d3d12.h`, guide `doc\xess_sr_developer_guide_english.md`. Read 2026-09-02.
Companion to `dlss-ngx-d3d11-contract.md` (outer repo) and `fsr-ffx-api-d3d12-contract.md`; only the differences are
spelled out. Implementation: `native\Xess12.cpp`.

## 1. Init

- `xessD3D12CreateContext(ID3D12Device*, xess_context_handle_t*)` (`xess_d3d12.h:124`) is the support check: it
  returns `XESS_RESULT_ERROR_UNSUPPORTED_DEVICE` (-1, "an SM 6.4 capable GPU is required" — DP4a) or
  `XESS_RESULT_ERROR_UNSUPPORTED_DRIVER` (-2) when XeSS cannot run at all (`xess.h:377`). Renderforge maps them to
  `DLSS_ERR_NOT_AVAILABLE` / `DLSS_ERR_NEEDS_DRIVER`.
- `xessIsOptimalDriver` → `XESS_RESULT_WARNING_OLD_DRIVER` (2) is advisory; logged, not fatal.
- `xessGetVersion` (global) + `xessGetIntelXeFXVersion(ctx)` (`xess.h:216-218`): the XeFX version is `0.0.0` on
  non-Intel platforms — **that zero is the cross-vendor DP4a path**, non-zero is the Intel XMX path. Nothing else
  reports it (`xessGetProperties` returns descriptor/heap sizes only). Shown as `2.0.2 DP4a` / `2.0.2 XMX`.
- `xessD3D12Init(ctx, const xess_d3d12_init_params_t*)` (`xess_d3d12.h:161`): blocking CPU call, no command list,
  JITs the kernels. Fields (`xess_d3d12.h:84-112`): `outputResolution` is an `xess_2d_t {x,y}` — **there is no
  `outputWidth`/`outputHeight`; the guide's sample is stale** — `qualitySetting`, `initFlags`, `creationNodeMask`/
  `visibleNodeMask` (1), `pTempBufferHeap`/`pTempTextureHeap`/`pPipelineLibrary` NULL (internal allocation).
  Re-init on a live context is allowed once no command list is pending; Renderforge instead destroys the context on
  release and recreates it on the next create.
- `xessD3D12BuildPipelines` skipped: it only moves the JIT earlier than `xessD3D12Init`, and create runs off the hot path.
- `xessSetLoggingCallback(ctx, XESS_LOGGING_LEVEL_WARNING, cb)` — callback may run on another thread and the message
  dies at return (`xess.h:196`), so it only `OutputDebugString`s.

## 2. Init flags (`xess.h:106-127`, mapped from `DLSS_F_*` in `ToXessInitFlags`)

| Unity fact | flag |
|---|---|
| colour RT is ARGB32 LDR (no `DLSS_F_HDR`) | `XESS_INIT_FLAG_LDR_INPUT_COLOR` ("disable tonemapping for input and output"; guide: exposure 1.0, no auto-exposure) |
| reversed-Z (`DLSS_F_DEPTH_INVERTED`) | `XESS_INIT_FLAG_INVERTED_DEPTH` |
| MVs at render resolution (`DLSS_F_MV_LOW_RES`) | default; its ABSENCE sets `XESS_INIT_FLAG_HIGH_RES_MV`. Low-res MVs make the depth texture required (`xess_d3d12.h:37`) — we always pass it |
| MVs carry no jitter | no `XESS_INIT_FLAG_JITTERED_MV` (set only with `DLSS_F_MV_JITTERED`) |
| `DLSS_F_AUTO_EXPOSURE` | `XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE` (the driver never sets it) |
| never | `USE_NDC_VELOCITY` (MVs are UV-space, scaled to pixels), `EXPOSURE_SCALE_TEXTURE`, `RESPONSIVE_PIXEL_MASK`, `EXTERNAL_DESCRIPTOR_HEAP` (XeSS manages its own heap) |

The enum spelling is `XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE` (`xess.h:114`); `xess_d3d12.h:40` calls it
`XESS_INIT_FLAG_EXPOSURE_TEXTURE` in prose. The header enum wins.

## 3. Presets (`xess.h:92-101`, scale factors = XeSS 1.3+ defaults, guide "Fixed Input Resolution")

`AA` 106 1.0x, `ULTRA_QUALITY_PLUS` 105 1.3x, `ULTRA_QUALITY` 104 1.5x, `QUALITY` 103 1.7x, `BALANCED` 102 2.0x,
`PERFORMANCE` 101 2.3x, `ULTRA_PERFORMANCE` 100 3.0x. Never hardcode: `xessGetOptimalInputResolution(ctx, &out, q,
&opt, &min, &max)` (`xess.h:269`) is the source and also yields the dynamic range; `xessGetInputResolution` is
deprecated since 1.2; `xessForceLegacyScaleFactors` is not used. `DLSS_Q_*` mapping: DLAA → AA, 5 → UQ, 6 → UQ+.
Observed (RTX 5070 Ti, 2026-09-02): 1920x1080 Quality → **1130x636** (range 1130x636..1920x1080), UQ+ → 1477x831,
AA → 1920x1080. Mip bias `log2(render/out)` and Halton phases `8*ratio²` already match the guide (-1.202 at
Performance, ≥72 at Ultra Performance).

## 4. Evaluate — `xessD3D12Execute(ctx, cl, const xess_d3d12_execute_params_t*)` (`xess_d3d12.h:184`)

Records into a caller-owned list; no GPU work of its own, so it goes through `D3D12Ring` like NGX. Field mapping from
`FrameParams` (`xess_d3d12.h:31-77`): `pColorTexture`=color, `pVelocityTexture`=mv, `pDepthTexture`=depth,
`pOutputTexture`=Unity's RT or the sharpen target, `pExposureScaleTexture`/`pResponsivePixelMaskTexture`=NULL,
`jitterOffsetX/Y` (see §5), `exposureScale`=preExposure (1.0), `resetHistory`=reset, `inputWidth/Height`=render size,
every `*Base` coordinate `(0,0)` (RTs are exactly the input/output size), `pDescriptorHeap`=NULL.
Per-context knobs set once at create: `xessSetJitterScale(1,1)`, `xessSetExposureMultiplier(1.0)`,
`xessSetVelocityScale(mvScaleX, mvScaleY)` pushed by the first evaluate and on change.

## 5. Conventions

- **Motion vectors**: "screen-space motion in pixels from the current frame to the previous frame" (guide "Motion
  Vectors") — the same direction NGX and FSR want, so the driver's `(-renderW, -renderH)` scale (Unity's UV-space
  `current - previous`) goes straight into `xessSetVelocityScale`. **No extra sign flip.** Format `R16G16_FLOAT`
  = Unity `RGHalf`, exact match.
- **Jitter**: range `[-0.5, 0.5]` in input pixels. The guide composites in row-vector form
  `ProjectionMatrix[2][0] += Jx*2/InputWidth; [2][1] -= Jy*2/InputHeight`; the driver applies `proj[0,2] += 2*jx/W`,
  `proj[1,2] += 2*jy/H` and stores NGX's `(-jx, -jy)`, so XeSS gets `jitterOffset = (-fp.jitterX, +fp.jitterY)` —
  the same `(-X, +Y)` flip FSR needed. Compile-time constants `kJitterSignX = -1`, `kJitterSignY = +1` in
  `Xess12.cpp`. **Measured 2026-09-02** (build `fd394fe`, ALN_PLT_Nest_48x48_A seed 12345, Ultra Performance
  427x240 → 1280x720, 4 sign combos `-1,1 / 1,1 / -1,-1 / 1,-1`): all four indistinguishable on stills — path
  dashes, health bars, unit diamonds, dashed cover outlines resolve equally cleanly, no doubling/serration.
  Limitation: `start-mission` with same scene+seed lands a different camera/unit each launch, so a still-frame A/B
  cannot separate the signs. Defaults `(-1, +1)` kept. To settle definitively: fixed camera pose or moving-camera
  capture. Shots: `docs\shots\jitter-ab\xess-{-1_1,1_1,-1_-1,1_-1}.png`.
  Env override `RENDERFORGE_XESS_JITTER_SIGN` = `"sx,sy"` (each in {-1,1}), read once at `Xess12::Init`, logged
  `XeSS: jitterSign=%d,%d`.
- **Depth**: `R32_FLOAT`, render resolution, smaller = closer unless `INVERTED_DEPTH`.
- **Colour**: only `UNORM` integer formats are accepted as colour input; output must be the same format/colour space
  as the input (`ARGB32` in, `ARGB32` out — matches).
- **Exposure**: LDR → `exposureScale 1.0`, no auto-exposure.
- **Thread safety**: "all calls must be done from the thread where XeSS was initialized" — `xessD3D12Init` and every
  execute run on Unity's render thread (plugin event); the context for the main-thread `GetOptimal` is created
  lazily there and is never `Init`ed on that thread (release destroys it, create rebuilds it on the render thread).

## 6. Resource states & sharpening

- **Owned-resource model** (D3D12, `D3D12Owned.h`): the shim OWNS the four textures XeSS touches
  (`colorIn` R8G8B8A8_UNORM, `depthIn` R32_FLOAT, `mvIn` R16G16_FLOAT, `out` +UAV, same-family typed twin of the
  Unity RT). Unity RenderTextures are only the SOURCE or DESTINATION of a `CopyResource` in our list — never handed
  to XeSS. Why: Unity 2019.4's D3D12 state tracker varied the pre-state of a RT per frame (measured debug-layer
  id=527, 2026-09-02), so every barrier the SDK recorded on it mismatched.
- **Measured Unity RT pre-states** (deterministic per RT, 614-628 frames sampled):
  `colorRT` RENDER_TARGET, `depthRT` RENDER_TARGET, `mvRT` COPY_DEST, `outRT` GENERIC_READ.
  Our list transitions each Unity RT from its pre-state to COPY_SOURCE/COPY_DEST for the copy and puts it BACK;
  `OwnedSet12::Declare` sets `expected == current == pre-state` so Unity's tracker agrees.
- **Per-frame barrier sequence** (one ring list):
  Unity colorRT/depthRT/mvRT: pre-state -> COPY_SOURCE (copy in) -> pre-state.
  Unity outRT: GENERIC_READ -> COPY_DEST (copy out) -> GENERIC_READ.
  Owned colorIn/depthIn/mvIn: COMMON -> COPY_DEST (copy in) -> NON_PIXEL_SHADER_RESOURCE (XeSS) -> COMMON.
  Owned out: COMMON -> UNORDERED_ACCESS (XeSS) -> COPY_SOURCE (copy out) -> COMMON.
  Every owned resource starts and ends in COMMON (the state it was created in).
- Required SDK states: inputs `NON_PIXEL_SHADER_RESOURCE`, output `UNORDERED_ACCESS` (`xess_d3d12.h:33-47`);
  "XeSS-SR does not perform any memory synchronization" and never promises to restore states — `OwnedSet12::Leave`
  transitions them back to COMMON ourselves.
- XeSS has **no sharpness**: the shim's NIS/RCAS pass (`native\D3D12Sharpen.h`, `SharpenPass12`, shared with
  `Device12`) runs on the same list right after the execute — XeSS writes `sharpen.target`, the pass writes Unity's
  RT. `Dlss_Sharpener()` reports NIS (1). XeSS binds its own heap/root signature/PSO; the pass re-binds all three.
- Heap tier: devices from `D3D12_RESOURCE_HEAP_TIER_1` (`xess_d3d12.h:154`) — no gate needed.

## 7. Release & shutdown

No feature handle apart from the context: a preset/resolution change is a re-init. `ReleaseFeature` = `ring.WaitIdle`
+ `xessDestroyContext` ("the user must ensure that any pending command lists are completed", `xess.h:293`);
`Shutdown` also releases the ring, the sharpen pass and the device. `libxess.dll` stays resident (delay-load thunks
are bound to it).

## 8. Linking & redistribution

- `lib\libxess.lib` linked with `/DELAYLOAD:libxess.dll` + `delayimp` (both the shim and `dlss_probe`). `Xess12::Init`
  pins `<modDir>\libxess.dll` by absolute path with `LoadLibraryW` BEFORE the first `xess*` call, so the delay-load
  helper's bare-name lookup binds to the mod's copy (an already-loaded module wins) and the shim loads fine without
  the DLL (`DLSS_ERR_NO_PROVIDER_DLL`). An implicit link would resolve against the game's exe folder at load time.
- Requires `msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` (MSVC redist 14.40.33810+; guide "Deployment") —
  not vendored; the dev box has 14.51.36247.0 in System32.
- Ships `libxess.dll` only (`libxess_dx11.dll` is Intel-Arc-only, `libxess_fg.dll` is Phase 5). Intel Simplified
  Software License shipped verbatim as `LICENSE-INTEL.txt`; signature (`CN=Intel Corporation`) asserted in
  `build-native.ps1`.

## 9. Result codes

`XESS_RESULT_SUCCESS` is **0** (NGX's success is 1). Mapped onto `NVSDK_NGX_Result` (`Xess12::Map`): success + the
two warnings → `Success`; `UNINITIALIZED`/`WRONG_CALL_ORDER` → `FAIL_NotInitialized`; `INVALID_ARGUMENT` →
`FAIL_InvalidParameter`; `DEVICE_OUT_OF_MEMORY` → `FAIL_OutOfGPUMemory`; `UNSUPPORTED_DEVICE`/`UNSUPPORTED`/
`NOT_IMPLEMENTED` → `FAIL_FeatureNotSupported`; `UNSUPPORTED_DRIVER` → `FAIL_OutOfDate`; `CANT_LOAD_LIBRARY` →
`FAIL_UnableToInitializeFeature`; else `FAIL_PlatformError`. `Dlss_LastError() = DLSS_ERR_XESS` (-6); the raw
`xess_result_t` goes to the `RENDERFORGE_D3D12_DEBUG` log.

## 10. Verified on this machine (RTX 5070 Ti, `dlss_probe.exe . --xess`, 2026-09-02)

`xessGetVersion 2.0.2`, `xessGetIntelXeFXVersion 0.0.0` (cross-vendor DP4a path), `xessIsOptimalDriver 0`,
`Dlss_Init code=0 provider=2 version='2.0.2 DP4a'`, create (`xessD3D12Init`) with zero `ExecuteCommandList` calls,
3 executes green, 3 submissions, NIS sharpen active, release → re-create green. **The Intel XMX path is implemented
but UNVERIFIED — no Arc GPU available.** In-game jitter A/B: 2026-09-02, indistinguishable on stills, defaults kept (see §5).
