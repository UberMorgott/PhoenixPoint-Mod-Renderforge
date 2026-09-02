# Phase 5 — Frame generation on D3D12 (FSR-FG, XeSS-FG, DLSS-FG/MFG) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Under `-force-d3d12`, present interpolated frames from any of the three vendor SDKs on disk — AMD FidelityFX FG 4.0.1/3.1.6 (`amd_fidelityfx_framegeneration_dx12.dll`), Intel XeSS-FG 1.3.1 + XeLL 1.3.2 (`libxess_fg.dll`, `libxell.dll`), NVIDIA DLSS-G/MFG through Streamline 2.12.0 (`sl.*.dll` + `nvngx_dlssg.dll` 310.7.129) — so the FRAME GENERATION picker row stops being greyed and the overlay reads `FPS: 62 / 118 (16.1 ms)` (real / presented).

**Architecture:** Unity's plugin API does not hand out the swapchain (`IUnityGraphicsD3D12v5` has `GetDevice`, `GetFrameFence`, `GetNextFrameFenceValue`, `ExecuteCommandList`, `SetPhysicalVideoMemoryControlValues`, `GetCommandQueue`, `TextureFromRenderBuffer` — and nothing else; see Grounding), and every one of the three SDKs owns presentation. So the shim grows a **presentation host** (`native/FgHook.cpp` + `native/FgHost.cpp`): it patches `IDXGISwapChain::Present` / `Present1` / `ResizeBuffers` in the shared DXGI vtable (read off a throwaway 8×8 swapchain we create ourselves), identifies Unity's swapchain as the first `this` whose `OutputWindow` is not our dummy window, then creates a **second, FG-owned swapchain on the same HWND** through the active provider's own creation entry point. From then on every Unity `Present` is intercepted: we copy Unity's finished backbuffer (scene + HUD) into the FG swapchain's backbuffer, let the provider interpolate against the hud-less `outRT` the DLSS driver already produces, present the FG swapchain, and return `S_OK` without ever calling Unity's original `Present`. Per-frame resource tagging/prepare that needs a recording command list runs in a **new render event** (`DLSS_EV_FG_PREPARE`) inside Unity's `AfterEverything` command buffer, submitted through `IUnityGraphicsD3D12v5::ExecuteCommandList` with a state array, exactly like the DLSS evaluate of Phase 2.

**Tech Stack:** C++17, MSVC (VS 2022 Build Tools), CMake ≥ 3.15, D3D12 + DXGI (`dxgi1_6.h`), AMD FidelityFX SDK 2.3 ffx-api (`refs/FidelityFX-SDK/Kits/FidelityFX/`), Intel XeSS 3 SDK (`refs/XeSS-sdk/`), NVIDIA Streamline 2.12.0 (`refs/Streamline/`), Unity 2019.4 native plugin API headers (vendored `native/unity/`), C# `net472` / `LangVersion=latest` (`Renderforge.csproj`), PowerShell 7 (`build-native.ps1`, `deploy.ps1`), PPCLI for in-game verification on `D:\PP-Instance2`.

**Depends on:** Phase 2 (`docs/superpowers/plans/2026-09-02-phase2-dlss-d3d12.md`) — `native/Device.h` (`IDevice`, `FrameParams`, `CreateParams`), `native/Device12.cpp`, `native/unity/IUnityGraphicsD3D12.h`, the global `IUnityGraphicsD3D12v5* g_unityD3D12` set by `UnityPluginLoad`, and `Dlss_Api()` returning 12. Phase 3 (FSR SR) and Phase 4 (XeSS SR) plans **do not exist yet** and this phase does **not** depend on them: frame generation is a separate `Fg_*` export family, as the spec requires ("FG is swapchain/pacing-shaped, not upscaler-shaped" — `docs/superpowers/specs/2026-09-02-multi-vendor-d3d12-design.md:88-90`). FG works with DLSS SR, with no upscaler, or with any later SR provider.

---

## Grounding — facts this plan rests on (all read from real source on disk, 2026-09-02)

### The swapchain problem

- `native/unity/IUnityGraphicsD3D12.h` (vendored in Phase 2, commit `8c2e7b4`; full member list at `docs/superpowers/plans/2026-09-02-phase2-dlss-d3d12.md:331-350`): `IUnityGraphicsD3D12v5` exposes `GetDevice`, `GetFrameFence`, `GetNextFrameFenceValue`, `ExecuteCommandList`, `SetPhysicalVideoMemoryControlValues`, `GetCommandQueue`, `TextureFromRenderBuffer`. **There is no swapchain getter, in v2, v3, v4 or v5.** v6/v7 (Unity 2023+) do not exist in 2019.4. → option (c) of the brief is dead, verified in the header we ship.
- DXGI has no enumeration API for existing swapchains. `IDXGIFactory` creates them; `IDXGIDebug::ReportLiveObjects` only prints. → option (a) is dead.
- `RenderTexture.GetNativeTexturePtr()` returns `ID3D12Resource*` for render textures only; Unity never exposes the backbuffer as a `RenderTexture` (`BuiltinRenderTextureType.CameraTarget` is a command-buffer token, not an object). → option (d) is dead.
- → **option (b), the vtable Present hook, is the only mechanism.** All COM objects created by one DXGI runtime share one vtable, so patching the vtable read off *our own* throwaway swapchain also patches Unity's.

### AMD FidelityFX — frame generation

Paths below are relative to `E:\DEV\PhoenixPoint\refs\FidelityFX-SDK\Kits\FidelityFX\`.

- Entry points are 5 C exports resolved from **`signedbin\amd_fidelityfx_loader_dx12.dll`** (2.3.0.2740, Authenticode Valid, `CN=Advanced Micro Devices`) by `api\include\ffx_api_loader.h:50-61`: `ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch`. The loader dispatches to `amd_fidelityfx_framegeneration_dx12.dll` (4.0.1.2740) internally.
- `api\include\ffx_api.h:142-165` — `ffxCreateContext(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*)`, `ffxDestroyContext`, `ffxConfigure(ffxContext*, const ffxConfigureDescHeader*)`, `ffxQuery(ffxContext*, ffxQueryDescHeader*)`, `ffxDispatch(ffxContext*, const ffxDispatchDescHeader*)`. `ffxContext` = `void*` (`:45`), `ffxReturnCode_t` = `uint32_t` (`:46`), `FFX_API_RETURN_OK = 0` (`:33`).
- `api\include\ffx_api.h:54` — every desc starts with `ffxApiHeader { ffxStructType_t type; ffxApiHeader* pNext; }`; descs are chained through `pNext`.
- `api\include\dx12\ffx_api_dx12.h:48-52` — `FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12`, `struct ffxCreateBackendDX12Desc { ffxCreateContextDescHeader header; ID3D12Device* device; }`.
- `api\include\dx12\ffx_api_dx12.h:204` — `static inline FfxApiResource ffxApiGetResourceDX12(ID3D12Resource* pRes, uint32_t state = FFX_API_RESOURCE_STATE_COMPUTE_READ, uint32_t additionalUsages = 0)`; `:68` `ffxApiGetSurfaceFormatDX12(DXGI_FORMAT)`.
- `api\include\ffx_api_types.h:199` — `FfxApiResource { void* resource; FfxApiResourceDescription description; uint32_t state; }`; `:92` `FFX_API_RESOURCE_STATE_COMPUTE_READ = (1<<2)`, `..._UNORDERED_ACCESS = (1<<1)`, `..._PRESENT = (1<<7)`, `..._COPY_DEST = (1<<5)`; `:140` `FfxApiDimensions2D{width,height}`; `:147` `FfxApiFloatCoords2D{x,y}`; `:162` `FfxApiRect2D{left,top,width,height}`.
- `framegeneration\include\ffx_framegeneration.h`:
  - `:28-30` FG effect version **4.0.1** (ML); the analytical **3.1.6** model lives in the same DLL.
  - `:38` create flags `FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT (1<<0)`, `..._DISPLAY_RESOLUTION_MOTION_VECTORS (1<<1)`, `..._MOTION_VECTORS_JITTER_CANCELLATION (1<<2)`, `..._DEPTH_INVERTED (1<<3)`, `..._DEPTH_INFINITE (1<<4)`, `..._HIGH_DYNAMIC_RANGE (1<<5)`, `..._DEBUG_CHECKING (1<<6)`.
  - `:49` dispatch flags `FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES (1<<0)`, `..._RESET_INDICATORS (1<<1)`, `..._DRAW_DEBUG_VIEW (1<<2)`, `..._NO_SWAPCHAIN_CONTEXT_NOTIFY (1<<3)`, `..._DRAW_DEBUG_PACING_LINES (1<<4)`.
  - `:66` `FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION`, struct `ffxCreateContextDescFrameGeneration { header; uint32_t flags; FfxApiDimensions2D displaySize; FfxApiDimensions2D maxRenderSize; uint32_t backBufferFormat; }`.
  - `:107-121` `FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION`, struct `ffxConfigureDescFrameGeneration { header; void* swapChain; FfxApiPresentCallbackFunc presentCallback; void* presentCallbackUserContext; FfxApiFrameGenerationDispatchFunc frameGenerationCallback; void* frameGenerationCallbackUserContext; bool frameGenerationEnabled; bool allowAsyncWorkloads; FfxApiResource HUDLessColor; uint32_t flags; bool onlyPresentGenerated; FfxApiRect2D generationRect; uint64_t frameID; }`.
  - `:220-243` `FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2`, struct `ffxDispatchDescFrameGenerationPrepareV2 { header; uint64_t frameID; uint32_t flags; void* commandList; FfxApiDimensions2D renderSize; FfxApiFloatCoords2D jitterOffset; FfxApiFloatCoords2D motionVectorScale; float frameTimeDelta; bool reset; float cameraNear; float cameraFar; float cameraFovAngleVertical; float viewSpaceToMetersFactor; FfxApiResource depth; FfxApiResource motionVectors; float cameraPosition[3]; float cameraUp[3]; float cameraRight[3]; float cameraForward[3]; }`. The V1 `..._PREPARE` (`:128`) and `..._PREPARE_CAMERAINFO` (`:192`) are marked **DEPRECATED** — use V2.
  - `:261-266` `FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION`, `ffxCreateContextDescFrameGenerationVersion { header; uint32_t version; }` — must be chained with `FFX_FRAMEGENERATION_VERSION`.
- `framegeneration\include\dx12\ffx_api_framegeneration_dx12.h`:
  - `:26-28` FG-swapchain-DX12 version **3.1.7**.
  - `:34-40` `ffxCreateContextDescFrameGenerationSwapChainWrapDX12 { header; IDXGISwapChain4** swapchain; ID3D12CommandQueue* gameQueue; }` — **`swapchain` is in/out: the existing swapchain goes in, the proxy comes back in the same pointer, and the SDK releases the original internally.**
  - `:52-61` `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 { header; IDXGISwapChain4** swapchain; HWND hwnd; DXGI_SWAP_CHAIN_DESC1* desc; DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc; IDXGIFactory* dxgiFactory; ID3D12CommandQueue* gameQueue; }` — **creates a fresh swapchain; `swapchain` is out-only.**
  - `:64-70` `ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 { header; FfxApiResource uiResource; uint32_t flags; }`.
  - `:72-83` `ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12 { header; void** pOutCommandList; }` and `...InterpolationTextureDX12 { header; FfxApiResource* pOutTexture; }`.
  - `:86` `ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 { header; }` (no payload).
  - `:92-106` `ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 { header; uint64_t key; uint64_t u64; void* ptr; }` with keys `FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK = 0`, `..._FRAMEPACINGTUNING = 2`.
  - `:129-133` `ffxCreateContextDescFrameGenerationSwapChainVersionDX12 { header; uint32_t version; }` — must be chained.
- `docs\techniques\frame-interpolation-swap-chain.md:57-60` — Wrap / New / ForHwnd are the three creation shapes; the shipped sample (`Samples\...\fsrapirendermodule.cpp:1807-1841`) uses **ForHwnd**: it `AddRef`s the engine swapchain, detaches it from the engine, releases it, and only then creates the proxy.
- UI composition, three mutually exclusive modes (`docs\techniques\frame-interpolation-api.md:83-93`): (1) `presentCallback` — we composite per presented frame; (2) a registered UI texture; (3) **`HUDLessColor` in `ffxConfigureDescFrameGeneration`** — same resolution as the backbuffer, pre-UI, FG recovers the UI by differencing against the composed backbuffer. **Mode (3) is exactly our pipeline** (`outRT` is hud-less at output resolution; the backbuffer is the same frame with the HUD on top).
- Runtime toggle: set `frameGenerationEnabled = false` and call `ffxConfigure`; the call drains pending GPU work internally, no context destroy needed (`frame-interpolation-swap-chain.md:223-226`). On resize: disable, destroy the FG context, destroy the swapchain context, recreate both.
- Interpolated frame count: `ffxDispatchDescFrameGeneration.numGeneratedFrames` with `FfxApiResource outputs[4]` (`ffx_framegeneration.h:90-102`), but the shipped sample sets **1** (`fsrapirendermodule.cpp:1612`) and no doc on disk describes >1 for either model. → **FSR-FG ships 2x only.**
- Model selection: `ffxQueryDescGetVersions` (`ffx_api.h:97`) enumerates version ids/names for `createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION`; chain `ffxOverrideVersion { header; uint64_t versionId; }` (`:108`) onto the create desc to pick one. FG 4.0.1 (ML) requires **Windows 11, DX12 Agility SDK 1.4.9, AMD RDNA4 (RX 9000) or later, CS_6_6, ≥30 fps input** (`docs\techniques\frame-interpolation-ml.md:75`); FG 3.1.6 (analytical) needs only typed UAV load + `R16G16B16A16_UNORM`, ≥60 fps input. → **on the RTX 5070 Ti here only 3.1.6 can run; we pick it explicitly and never let the loader silently choose the ML model.**
- Async compute is optional (`FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT` + `allowAsyncWorkloads`). No separate AMD Anti-Lag component appears anywhere in the FG headers or docs on disk.

### Intel XeSS-FG + XeLL

Paths relative to `E:\DEV\PhoenixPoint\refs\XeSS-sdk\`.

- `inc\xess_fg\xefg_swapchain.h:62` `xefg_swapchain_handle_t`; `inc\xell\xell.h:146` `xell_context_handle_t`.
- `inc\xess_fg\xefg_swapchain_d3d12.h`:
  - `:120` `xefgSwapChainD3D12CreateContext(ID3D12Device*, xefg_swapchain_handle_t*)`
  - `:168-174` `xefgSwapChainD3D12GetProperties(handle, const xefg_swapchain_d3d12_init_params_t*, uint32_t backBufferWidth, uint32_t backBufferHeight, DXGI_FORMAT backBufferFormat, xefg_swapchain_properties_t*)`
  - `:190-191` `xefgSwapChainD3D12InitFromSwapChain(handle, ID3D12CommandQueue*, const xefg_swapchain_d3d12_init_params_t*)`
  - `:216-218` `xefgSwapChainD3D12InitFromSwapChainDesc(handle, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory2*, const xefg_swapchain_d3d12_init_params_t*)`
  - `:232` `xefgSwapChainD3D12GetSwapChainPtr(handle, REFIID, void**)`
  - `:255-256` `xefgSwapChainD3D12TagFrameResource(handle, ID3D12CommandList*, uint32_t presentId, const xefg_swapchain_d3d12_resource_data_t*)`
  - `:34-81` `xefg_swapchain_d3d12_init_params_t { IDXGISwapChain* pApplicationSwapChain; uint32_t initFlags; uint32_t maxInterpolatedFrames; uint32_t creationNodeMask; uint32_t visibleNodeMask; ID3D12Heap* pTempBufferHeap; uint64_t bufferHeapOffset; ID3D12Heap* pTempTextureHeap; uint64_t textureHeapOffset; ID3D12PipelineLibrary* pPipelineLibrary; xefg_swapchain_ui_mode_t uiMode; }`
  - `:89-103` `xefg_swapchain_d3d12_resource_data_t { xefg_swapchain_resource_type_t type; xefg_swapchain_resource_validity_t validity; xefg_swapchain_2d_t resourceBase; xefg_swapchain_2d_t resourceSize; ID3D12Resource* pResource; D3D12_RESOURCE_STATES incomingState; }`
- `inc\xess_fg\xefg_swapchain.h`: `:365-366` `xefgSwapChainTagFrameConstants(handle, uint32_t presentId, const xefg_swapchain_frame_constant_data_t*)`; `:377` `xefgSwapChainSetEnabled(handle, uint32_t)`; `:390` `xefgSwapChainSetPresentId(handle, uint32_t)`; `:401` `xefgSwapChainGetLastPresentStatus(handle, xefg_swapchain_present_status_t*)`; `:421` `xefgSwapChainDestroy(handle)`; `:431` `xefgSwapChainSetLatencyReduction(handle, void* hXeLLContext)`; `:486-487` `xefgSwapChainSetNumInterpolatedFrames(handle, uint32_t)`; `:511-512` `xefgSwapChainSetUiCompositionState(handle, xefg_swapchain_ui_composition_state_t)`.
- `xefg_swapchain.h:152-170` `xefg_swapchain_frame_constant_data_t { float viewMatrix[16]; float projectionMatrix[16]; float jitterOffsetX; float jitterOffsetY; float motionVectorScaleX; float motionVectorScaleY; uint32_t resetHistory; float frameRenderTime; }` (row-major).
- `xefg_swapchain.h:103-111` resource types `XEFG_SWAPCHAIN_RES_HUDLESS_COLOR = 0, ..._DEPTH, ..._MOTION_VECTOR, ..._UI, ..._BACKBUFFER`; `:70-77` validity `XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT = 0, ..._ONLY_NOW`; `:82-98` init flags incl. `XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH (1<<0)`, `..._USE_NDC_VELOCITY (1<<3)`, `..._JITTERED_MV (1<<4)`; `:280-294` UI modes `..._AUTO=0, ..._NONE=1, ..._BACKBUFFER_UITEXTURE=2, ..._HUDLESS_UITEXTURE=3, ..._BACKBUFFER_HUDLESS=4, ..._BACKBUFFER_HUDLESS_UITEXTURE=5`; `:195-255` results with `XEFG_SWAPCHAIN_RESULT_SUCCESS = 0` and negative errors.
- `doc\xess_fg_developer_guide_english.md:623` — mode table: **`BACKBUFFER_HUDLESS` (4) requires hud-less, does not use a UI texture** → our mode, same as FSR's `HUDLessColor`.
- `doc\xess_fg_developer_guide_english.md:270-276` — `InitFromSwapChain` "Queries parameters from the provided swap chain … The application must provide a swap chain pointer with **reference counter equal to 1** … **The application-provided swap chain will be released!** … After successful call, the provided swap chain pointer will be invalid … Creates a new swap chain for the underlying window." **Unity holds its own reference and keeps calling `GetBuffer`/`ResizeBuffers` on it, so `InitFromSwapChain` is unusable for us. `InitFromSwapChainDesc` (which takes `pApplicationSwapChain = NULL`) is the only viable XeSS-FG path.**
- `doc\xess_fg_developer_guide_english.md:229-231` — "XeSS-FG proxy swap chain will fail to initialize **without XeLL context**. Frame generation will be disabled if XeLL is not initialized and enabled." → **XeLL is mandatory**, wired with `xefgSwapChainSetLatencyReduction`.
- `inc\xell\xell_d3d12.h:28` `xellD3D12CreateContext(ID3D12Device*, xell_context_handle_t*)`; `inc\xell\xell.h:153` `xellDestroyContext`, `:161` `xellSetSleepMode(ctx, const xell_sleep_params_t*)`, `:177` `xellSleep(ctx, uint32_t frame_id)`, `:186` `xellAddMarkerData(ctx, uint32_t frame_id, xell_latency_marker_type_t)`, `:86-99` `xell_sleep_params_t { uint32_t minimumIntervalUs; uint32_t bLowLatencyMode : 1; uint32_t bLowLatencyBoost : 1; uint32_t reserved : 30; }`, `:72-83` markers `XELL_SIMULATION_START=0, XELL_SIMULATION_END=1, XELL_RENDERSUBMIT_START=2, XELL_RENDERSUBMIT_END=3, XELL_PRESENT_START=4, XELL_PRESENT_END=5, XELL_INPUT_SAMPLE=6` — the first six are marked **required**.
- Mandatory per-frame tags: motion vectors + depth (`xess_fg_developer_guide_english.md:136-137, 667-668`); hud-less and UI are optional and mode-dependent. Shutdown order: release proxy refs → `xefgSwapChainDestroy` → `xellDestroyContext` (`:1080`).
- Interpolated frames: `maxInterpolatedFrames` in the init params; **on non-Intel GPUs the maximum is 1** (`:87`), Intel Arc reports more through `xefg_swapchain_properties_t.maxSupportedInterpolations`. → **XeSS-FG ships 2x on this machine; 3x/4x only on Arc.**
- Linkage: `XEFG_SWAPCHAIN_API` is `__declspec(dllexport)`/empty (`xefg_swapchain.h:20-25`) → import libs `lib\libxess_fg.lib`, `lib\libxell.lib`, delay-loaded so a missing DLL is a greyed picker, not a failed process start.
- Runtime DLLs (all Authenticode **Valid**, `CN=Intel Corporation`): `bin\libxess_fg.dll` 1.3.1.78 (22 MB), `bin\libxell.dll` 1.3.2.10 (406 KB).

### NVIDIA Streamline — DLSS-G / MFG

Paths relative to `E:\DEV\PhoenixPoint\refs\Streamline\`.

- SDK 2.12.0 (`include\sl_version.h:24-26`).
- `include\sl_core_api.h:50-71` — `PFun_slInit(const sl::Preferences&, uint64_t sdkVersion)`, `PFun_slShutdown()`, `PFun_slIsFeatureSupported(sl::Feature, const sl::AdapterInfo&)`, `PFun_slIsFeatureLoaded`, `PFun_slSetFeatureLoaded`, `PFun_slSetD3DDevice(void*)`, `PFun_slUpgradeInterface(void** baseInterface)`, `PFun_slGetNativeInterface(void* proxy, void** base)`, `PFun_slSetConstants(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&)`, `PFun_slSetTagForFrame(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*)`, `PFun_slGetNewFrameToken(sl::FrameToken*&, const uint32_t*)`, `PFun_slGetFeatureFunction(sl::Feature, const char*, void*&)`, `PFun_slGetFeatureRequirements`.
- `include\sl_core_types.h:549-588` `sl::Preferences` fields in order: `showConsole`, `logLevel`, `pathsToPlugins`, `numPathsToPlugins`, `pathToLogsAndData`, `allocateCallback`, `releaseCallback`, `logMessageCallback`, `flags`, `featuresToLoad`, `numFeaturesToLoad`, `applicationId`, `engine`, `engineVersion`, `projectId`, `renderAPI`.
- `include\sl_core_types.h:509-542` `sl::PreferenceFlags`: `eDisableCLStateTracking (1<<0)`, `eDisableDebugText (1<<1)`, **`eUseManualHooking (1<<2)`**, `eAllowOTA (1<<3)`, `eBypassOSVersionCheck (1<<4)`, `eUseDXGIFactoryProxy (1<<5)`, `eLoadDownloadedPlugins (1<<6)`, `eUseFrameBasedResourceTagging (1<<7)`.
- `include\sl_core_types.h:222-262` features: `kFeatureDLSS=0`, `kFeatureReflex=3`, `kFeaturePCL=4`, **`kFeatureDLSS_G=1000`**. `:66-214` buffer types: `kBufferTypeDepth=0`, `kBufferTypeMotionVectors=1`, `kBufferTypeHUDLessColor=2`, `kBufferTypeUIColorAndAlpha=23`, `kBufferTypeBackbuffer=53`, `kBufferTypeUIAlpha=69`. `:386-394` `ResourceLifecycle { eOnlyValidNow, eValidUntilPresent, eValidUntilEvaluate }`. `:401-418` `ResourceTag { Resource resource; BufferType type; ResourceLifecycle lifecycle; Extent extent; }`. `:329-378` `Resource { ResourceType type; void* native; void* memory; void* view; uint32_t state; ... }`.
- `include\sl_consts.h:182-258` `sl::Constants`: `cameraViewToClip`, `clipToCameraView`, `clipToLensClip`, `clipToPrevClip`, `prevClipToClip`, `jitterOffset`, `mvecScale`, `cameraPinholeOffset`, `cameraPos/Up/Right/Fwd`, `cameraNear/Far/FOV/AspectRatio`, `motionVectorsInvalidValue`, `depthInverted`, `cameraMotionIncluded`, `motionVectors3D`, `reset`, `orthographicProjection`, `motionVectorsDilated`, `motionVectorsJittered`.
- `include\sl_dlss_g.h`: `:34-41` `DLSSGMode { eOff=0, eOn, eAuto, eDynamic }`; `:43-53` `DLSSGFlags` incl. `eRetainResourcesWhenOff (1<<3)`; `:72-122` `DLSSGOptions` with `mode`, **`numFramesToGenerate`** (1 = 2x, 2 = 3x, 3 = 4x), `flags`, `dynamicResWidth/Height`, `numBackBuffers`, `mvecDepthWidth/Height`, `colorWidth/Height`, `colorBufferFormat`, `mvecBufferFormat`, `depthBufferFormat`, `hudLessBufferFormat`, `uiBufferFormat`, `onErrorCallback`, `queueParallelismMode`, `enableUserInterfaceRecomposition`, `dynamicTargetFrameRate`; `:149-183` `DLSSGState` with `status`, `minWidthOrHeight`, `numFramesActuallyPresented`, **`numFramesToGenerateMax`**, `estimatedVRAMUsageInBytes`; `:125-143` `DLSSGStatus` incl. `eFailReflexNotDetectedAtRuntime (1<<1)`; `:197-222` `PFun_slDLSSGGetState(const ViewportHandle&, DLSSGState&, const DLSSGOptions*)`, `PFun_slDLSSGSetOptions(const ViewportHandle&, const DLSSGOptions&)` — both fetched via `slGetFeatureFunction(kFeatureDLSS_G, "slDLSSGSetOptions", ...)`.
- `include\sl_pcl.h:60-87` `PCLMarker { eSimulationStart=0, eSimulationEnd=1, eRenderSubmitStart=2, eRenderSubmitEnd=3, ePresentStart=4, ePresentEnd=5, ... }`; `:122-143` `PFun_slPCLSetMarker(PCLMarker, const FrameToken&)`. **This SDK has `slPCLSetMarker`, not `slReflexSetMarker`** — markers moved into `sl.pcl.dll`. `include\sl_reflex.h:31-39` `ReflexMode { eOff, eLowLatency, eLowLatencyWithBoost }`, `:42-62` `ReflexOptions`, `:156-200` `PFun_slReflexSetOptions`, `PFun_slReflexSleep(const FrameToken&)`.
- `docs\ProgrammingGuideDLSS_G.md:676-681` — mandatory per-frame order: new frame token → `eSimulationStart`/`eSimulationEnd` → `slReflexSleep` → `eRenderSubmitStart` → `slSetConstants` → `slSetTagForFrame` → `eRenderSubmitEnd` → `ePresentStart` → `Present()` → `ePresentEnd`; the present markers must carry the same frame index as `slSetConstants` or DLSS-G logs "common constants cannot be found for frame N". Required tags: `kBufferTypeDepth`, `kBufferTypeMotionVectors`; quality-critical: `kBufferTypeHUDLessColor` + `kBufferTypeUIColorAndAlpha`/`kBufferTypeUIAlpha`. **DLSS-G never goes through `slEvaluateFeature`; it runs inside the hooked `Present`.**
- **`docs\ProgrammingGuideManualHooking.md:208-215` is the decisive line for this phase:** manual hooking requires `slUpgradeInterface(&factory)` on the **DXGI factory** and then `proxyFactory->CreateSwapChain…()`. Every swapchain hook (`Present`, `GetBuffer`, `ResizeBuffers`) only exists on swapchains created through that proxy factory, and `sl_hooks.h:48-50` lists `CreateSwapChain` / `CreateSwapChainForHwnd` / `CreateSwapChainForCoreWindow` as mandatory hooks. **There is no API and no documented path for upgrading a swapchain that already exists — `slUpgradeInterface` is shown only for the D3D12 device and the DXGI factory.** `ProgrammingGuideDLSS_G.md:192-193`: "DLSS-G will automatically attach to any swap-chain created by the application unless manual hooking is used." → under manual hooking we must create the swapchain ourselves, through the proxy factory. That is precisely the shadow-swapchain shape this plan builds for the other two providers, so DLSS-G fits the same host.
- `ProgrammingGuideManualHooking.md:110-111` — the D3D device may be created **before or after** `slInit`; only the swapchain must go through the proxy. Our mod loads long after Unity created the device — that is fine.
- `ProgrammingGuideDLSS_G.md:964-1015` — with DLSS-G on, the app renders into off-screen buffers and Streamline hands out proxy back buffers; the swapchain must be recreated when DLSS-G is toggled. `:747-748` turn DLSS-G off before any fullscreen/resize transition. `:738-743` HDR must be `R10G10B10A2` (BT.2100); FP16/scRGB unsupported. `:1082-1087` VSync only when `DLSSGState::bIsVsyncSupportAvailable`.
- MFG support (`ProgrammingGuideDLSS_G.md:53-64, :1167`): RTX 40 reports `numFramesToGenerateMax = 1` (2x only); RTX 50 reports ≥ 3 (up to 6x theoretical). → **3x/4x are DLSS-only and only on RTX 50**; the RTX 5070 Ti in this machine can test them.
- DLLs: `bin\x64\sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` — all 2.12.0.0, Authenticode Valid, `CN=NVIDIA Corporation`. `bin\x64\nvngx_dlssg.dll` is **310.7.0.0 = stale**; ship `latest-dll\nvngx_dlssg.dll` **310.7.129.0**, Authenticode Valid, `CN=NVIDIA Corporation` (verified on disk 2026-09-02). No DLSS-G plugin source ships (`source\plugins\` has no `sl.dlss_g`).

### The game and the existing mod

- `src/DlssDriver.cs:8-10` documents the present path and `:227-238` builds it: a second camera `DlssPresent` at `cam.depth + 1`, `cullingMask = 0`, `clearFlags = Nothing`, with one `CommandBuffer` on `CameraEvent.AfterEverything` that blits `outRT` → `BuiltinRenderTextureType.CameraTarget`. `docs/DESIGN.md:242-249`: after that camera, "Unity draws ScreenSpaceOverlay HUD at native res" straight to the backbuffer, then presents. → **at `Present` time the backbuffer is the composed frame and `outRT` is the hud-less version of the same frame.** That is the hud-less contract all three SDKs want, for free.
- `src/DlssDriver.cs:215-220` — `colorRT` (render res, ARGB32), `depthRT` (render res, RFloat), `mvRT` (render res, RGHalf), `outRT` (out res, ARGB32, `enableRandomWrite`), with cached `IntPtr`s. `:355-356` the per-frame call already carries jitter `(-jx,-jy)`, MV scale `(-renderW,-renderH)`, `reset`, `Time.unscaledDeltaTime*1000f`.
- `src/Overlay.cs:18-20` — `public static int FgFps;` already exists with the comment "Phase 5's FG provider writes it and the FPS line turns into real / presented"; `:150-152` already renders `FPS: <real>[ / <FgFps>] (<ms>)`.
- `src/Availability.cs:7` `enum Feature { Dlss, Fsr, Xess, FrameGen }`; `:42-47` `Feature.FrameGen` currently returns "Not implemented yet" on D3D12.
- `src/Pickers.cs:39-42, :66-69, :136-142, :179-187` — the FRAME GENERATION row exists with labels `Off/2x/3x/4x`, is built, greyed from `Availability.Reason(Feature.FrameGen)`, and `OnFrameGen` writes nothing.
- `src/DlssConfig.cs` — `ModConfig` fields drive both `ModConfig.json` and the settings UI; every field needs an entry in the `Ru` dictionary (`:51-65`).
- `deploy.ps1:45-48` copies a fixed file list into `<PPRoot>\Mods\Renderforge`; `build-native.ps1:15-16` is the Authenticode pattern to copy for every vendor DLL.

---

## Decisions taken in this plan (and why)

1. **Swapchain acquisition = vtable `Present` hook (brief option (b)), and it is the *only* mechanism.** Options (a), (c) and (d) are each disproven above against a header or an API on disk. The vtable is read off a throwaway 8×8 swapchain we create on Unity's own command queue, because all DXGI swapchains in one process share one vtable; the hook then recognises Unity's swapchain as the first `this` whose `GetDesc().OutputWindow` is not our dummy window.
2. **We never hand Unity's swapchain to any SDK; we build a *shadow* swapchain on the same HWND and present that instead.** Reason, from the SDKs themselves: FSR's `WrapDX12` "releases the original internally" (`ffx_api_framegeneration_dx12.h:34-40` + `frame-interpolation-swap-chain.md:57-60`) and XeSS's `InitFromSwapChain` demands the app's swapchain have **refcount 1** and then destroys it (`xess_fg_developer_guide_english.md:270-276`). Unity keeps its reference and keeps calling `GetBuffer`/`ResizeBuffers` on it every frame, so in-place wrapping would leave Unity driving a swapchain the SDK considers dead. Streamline independently forces the same shape: the swapchain must be created through the `slUpgradeInterface`-upgraded DXGI factory (`ProgrammingGuideManualHooking.md:208-215`). One mechanism therefore serves all three providers: `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12` / `xefgSwapChainD3D12InitFromSwapChainDesc` / `proxyFactory->CreateSwapChainForHwnd`.
3. **The whole phase is gated on one HRESULT.** Two swapchains on one HWND is not documented as legal by DXGI. Task 1 Step 6 calls `CreateSwapChainForHwnd` a second time on the live game window and logs the result. `S_OK` ⇒ the architecture above is sound and Tasks 2-5 proceed. Failure ⇒ **stop** and run the fallback ladder in the next decision. Nothing else in this plan is written before that number exists.
4. **Fallback ladder, in order.**
   a. `CreateSwapChainForHwnd` on the game HWND fails → try `IDXGIFactory2::CreateSwapChainForComposition` + `DirectComposition` visual on the same HWND (a second presentation surface that does not claim the HWND). Spiked in Task 1 Step 7; if it works the host swaps one creation call and everything else in the plan is unchanged.
   b. Both fail → **Task 8 (contingency): a `dxgi.dll` shim in the game root** that owns swapchain creation from process start. This is the architecture all three SDKs actually expect, it is what `sl.interposer.dll` is for, and it is the only remaining option. It writes a file **outside `Mods\`**, so it is a user decision, not an implementation detail — Task 8 stops and asks before touching the install.
   c. User declines the shim → frame generation ships **unavailable**: the FRAME GENERATION row stays greyed with the reason string "Frame generation needs the Renderforge presentation shim — see README", and Phase 5 lands as documentation plus the overlay's presented-fps counter only.
5. **Hud-less mode everywhere, no UI texture.** Our present path already produces a hud-less `outRT` and a composed backbuffer of the same frame, which is FSR's `HUDLessColor` mode, XeSS's `XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS` (mode 4, "hud-less required, UI texture not used" — `xess_fg_developer_guide_english.md:623`) and DLSS-G's `kBufferTypeHUDLessColor` with `enableUserInterfaceRecomposition`. Building a separate UI render target would mean re-parenting the game's `ScreenSpaceOverlay` canvases — a large, fragile change for no gain here.
6. **Prepare/tag runs in a render event, present runs in the hook.** Both FSR (`ffxDispatchDescFrameGenerationPrepareV2.commandList`) and XeSS (`xefgSwapChainD3D12TagFrameResource(…, ID3D12CommandList*, …)`) need a *recording* command list with the depth/MV resources in a known state. Only Unity knows those states, and only `IUnityGraphicsD3D12v5::ExecuteCommandList(list, stateCount, states)` can declare them. So a new render event `DLSS_EV_FG_PREPARE` (id 4) records prepare/tagging on the shim's own list at `CameraEvent.AfterEverything`, exactly like Phase 2's evaluate. The hook then only copies the backbuffer, sets the frame id and presents.
7. **`Fg_*` is a separate export family, not new fields on `Dlss_*`.** The spec asks for it (`spec:88-90`) and the Phase 2 rule that no existing exported signature changes still holds.
8. **Frame-count capability is per provider, queried at runtime, never assumed.** FSR ships 2x (sample and docs show `numGeneratedFrames = 1`). XeSS asks `xefg_swapchain_properties_t.maxSupportedInterpolations` and gets 1 on non-Intel. DLSS asks `DLSSGState::numFramesToGenerateMax` and gets 1 on RTX 40, ≥3 on RTX 50. `Fg_Caps()` returns the mask; the picker greys 3x/4x when the mask lacks them.
9. **FSR model is pinned to 3.1.6 (analytical), not auto.** The ML 4.0.1 model needs RDNA4 (`frame-interpolation-ml.md:75`); letting the loader choose on an NVIDIA GPU is an untested path with no way to verify here. We enumerate versions with `ffxQueryDescGetVersions` and chain `ffxOverrideVersion` for the entry whose name contains `3.1`.
10. **Vendor DLLs are delay-loaded or `GetProcAddress`-loaded, never hard-linked.** A player with only the Core zip must still start the game. FSR uses `ffxLoadFunctions` on `amd_fidelityfx_loader_dx12.dll`; Streamline uses `GetProcAddress` on `sl.interposer.dll`; XeSS uses its import libs under `/DELAYLOAD` behind an explicit `LoadLibraryW` probe.

---

## File structure

| File | Status | Responsibility |
|---|---|---|
| `native/Fg.h` | create | FG seam: `FgFrame`, `FgSetup`, `IFgProvider`, provider factories, `FgLog` |
| `native/FgHook.cpp` | create | DXGI vtable patch, dummy swapchain, game-swapchain discovery, spike diagnostics, presented-fps counter |
| `native/FgHost.cpp` | create | shadow swapchain lifetime, per-frame backbuffer copy, provider dispatch, `Fg_*` implementation |
| `native/FgFsr.cpp` | create | FidelityFX FG provider (ffx-api loader DLL) |
| `native/FgXess.cpp` | create | XeSS-FG + XeLL provider |
| `native/FgStreamline.cpp` | create | Streamline DLSS-G / MFG provider (manual hooking + Reflex/PCL) |
| `native/RenderforgeNative.h` | modify | `DLSS_EV_FG_PREPARE`, `FG_*` enums, `Fg_*` exports |
| `native/RenderforgeNative.cpp` | modify | route `DLSS_EV_FG_PREPARE` to the FG host |
| `native/CMakeLists.txt` | modify | new sources, vendor include dirs, XeSS delay-load, `dcomp` link |
| `src/Native.cs` | modify | `Fg_*` P/Invoke + `FG_*` constants |
| `src/FrameGen.cs` | create | managed FG driver: enable/disable, per-frame feed, status |
| `src/DlssDriver.cs` | modify | issue `DLSS_EV_FG_PREPARE` and feed `Fg_SetFrame` |
| `src/DlssConfig.cs` | modify | `FrameGenMode FrameGen` config field + RU labels |
| `src/Availability.cs` | modify | real `Feature.FrameGen` reasons + per-multiplier availability |
| `src/Pickers.cs` | modify | FG row applies the setting, greys 3x/4x per `Fg_Caps` |
| `src/Overlay.cs` | modify | drive `FgFps` from `Fg_PresentedFps()`, add the `FG:` line |
| `src/RenderforgeMod.cs` | modify | create/destroy the FG driver, `SetFrameGen` PPCLI entry |
| `build-native.ps1` | modify | Authenticode-verify every vendor DLL, stage them into `build\out` |
| `deploy.ps1` | modify | copy the vendor DLLs into the mod folder |
| `docs/DESIGN.md` | modify | Frame generation section |
| `README.md` | modify | FG row in the feature matrix, vendor DLL notice |
| `E:\DEV\PhoenixPoint\docs\research\framegen-d3d12-contract.md` | create | the cross-vendor FG contract (outer repo) |
| `E:\DEV\PhoenixPoint\docs\research\README.md` | modify | index entry for the new note |

---
### Task 1: Present-hook spike — the go/no-go for the whole phase

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\native\Fg.h`
- Create: `E:\DEV\PhoenixPoint\Renderforge\native\FgHook.cpp`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\RenderforgeNative.h`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\RenderforgeNative.cpp`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\CMakeLists.txt`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Native.cs`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\RenderforgeMod.cs`

This task ships **no frame generation**. It answers four questions with numbers from the running game, and nothing after it is written until they are answered:
1. Can we patch the DXGI swapchain vtable and see Unity's `Present` come through?
2. What is Unity's swapchain really: format, buffer count, swap effect (flip model?), flags, windowed?
3. Can we call `Present` ourselves through the saved original pointer (i.e. is a forwarding hook transparent)?
4. **Can a second swapchain be created on the game's HWND** — the number decision 3 hangs on. Plus the composition fallback (decision 4a).

- [x] **Step 1: Write `native\Fg.h`**

```cpp
// Fg.h - the frame-generation seam. FgHook.cpp owns the DXGI vtable patch and the present counter;
// FgHost.cpp owns the shadow swapchain and drives one IFgProvider; FgFsr/FgXess/FgStreamline implement it.
// Everything here runs on the render thread except FgHost::Init/Shutdown (main thread, render idle).
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>

// One line into <modDir>\renderforge_fg.log. Never throws, never allocates after the first call.
void FgLog(const char* fmt, ...);
void FgLogInit(const wchar_t* logDir);

// ---------------------------------------------------------------- hook (FgHook.cpp)

// Patch IDXGISwapChain::Present/Present1/ResizeBuffers in the shared DXGI vtable. Idempotent.
// `queue` = Unity's command queue (needed to create the throwaway swapchain the vtable is read from).
// Returns true when the three slots are patched.
bool FgHookInstall(ID3D12CommandQueue* queue);
void FgHookRemove(void);

// The application's swapchain, discovered on the first hooked Present. NULL until then.
IDXGISwapChain3* FgAppSwapChain(void);
HWND             FgAppHwnd(void);
const DXGI_SWAP_CHAIN_DESC1* FgAppDesc(void);   // NULL until discovered

// Present the application's swapchain through the saved original vtable entry (never re-enters the hook).
HRESULT FgOriginalPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags);
HRESULT FgOriginalResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);

// Presented frames per second over a 0.5 s window, counted in the hook (includes generated frames). 0 = no data.
int  FgPresentedFps(void);
long long FgPresentCount(void);

// Spike diagnostics, filled by FgHookSpike(). Read by Fg_SpikeStatus.
struct FgSpike
{
    int   installed;          // vtable patched
    int   sawPresent;         // the hook ran at least once
    int   flipModel;          // app swap effect is FLIP_DISCARD or FLIP_SEQUENTIAL
    int   waitable;           // app flags have DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
    unsigned format;          // DXGI_FORMAT of the app backbuffer
    unsigned bufferCount;
    unsigned swapEffect;
    unsigned scFlags;
    unsigned width, height;
    int   windowed;
    long   secondSwapChainHr; // HRESULT of a second CreateSwapChainForHwnd on the game HWND
    long   compositionHr;     // HRESULT of CreateSwapChainForComposition as the 4a fallback
    long   forwardedPresentHr;// HRESULT the saved original Present returned
};
const FgSpike* FgSpikeResult(void);
void FgHookSpike(void);       // runs the two creation probes once, on the render thread
```

- [x] **Step 2: Write `native\FgHook.cpp`**

```cpp
// FgHook.cpp - the DXGI vtable patch. All swapchains produced by one DXGI runtime share one vtable, so
// patching the vtable read off a throwaway 8x8 swapchain of our own also patches Unity's. The hook
// recognises the application swapchain as the first `this` whose OutputWindow is not our dummy window.
//
// Vtable indices (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, IDXGISwapChain 8-17,
// IDXGISwapChain1 18-...): Present = 8, ResizeBuffers = 13, Present1 = 22. Verified against dxgi.h
// declaration order; asserted at runtime by comparing the saved original against the vtable slot.
#include "Fg.h"

#include <stdio.h>
#include <stdarg.h>

namespace {

typedef HRESULT (STDMETHODCALLTYPE *PfnPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PfnPresent1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PfnResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

const int kVtPresent = 8, kVtResizeBuffers = 13, kVtPresent1 = 22;

PfnPresent       g_origPresent = NULL;
PfnPresent1      g_origPresent1 = NULL;
PfnResizeBuffers g_origResize = NULL;

IDXGISwapChain3* g_app = NULL;              // NOT AddRef'd: Unity owns it, we only observe
HWND             g_appHwnd = NULL;
DXGI_SWAP_CHAIN_DESC1 g_appDesc = {};
int              g_appDescValid = 0;

HWND             g_dummyHwnd = NULL;
IDXGISwapChain1* g_dummy = NULL;
ID3D12CommandQueue* g_queue = NULL;
int              g_installed = 0;

FgSpike          g_spike = {};
int              g_spikeDone = 0;

// Presented-frame counter (0.5 s window).
LARGE_INTEGER    g_freq = {};
LARGE_INTEGER    g_windowStart = {};
long long        g_windowCount = 0;
long long        g_total = 0;
volatile long    g_fps = 0;

FILE*            g_log = NULL;

void CountPresent()
{
    if (g_freq.QuadPart == 0) { QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_windowStart); }
    ++g_total; ++g_windowCount;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - g_windowStart.QuadPart) / double(g_freq.QuadPart);
    if (dt >= 0.5) {
        g_fps = (long)(double(g_windowCount) / dt + 0.5);
        g_windowStart = now; g_windowCount = 0;
    }
}

// Remember the application's swapchain the first time a foreign `this` presents.
void NoteApp(IDXGISwapChain* sc)
{
    if (g_app) return;
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || d.OutputWindow == NULL || d.OutputWindow == g_dummyHwnd) return;
    IDXGISwapChain3* sc3 = NULL;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
        FgLog("hook: app swapchain is not IDXGISwapChain3 - FG needs it, giving up on discovery");
        return;
    }
    sc3->Release();                 // do not hold a reference; Unity owns the object
    g_app = sc3;
    g_appHwnd = d.OutputWindow;
    IDXGISwapChain1* sc1 = NULL;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
        if (SUCCEEDED(sc1->GetDesc1(&g_appDesc))) g_appDescValid = 1;
        sc1->Release();
    }
    g_spike.sawPresent = 1;
    g_spike.format = (unsigned)d.BufferDesc.Format;
    g_spike.bufferCount = d.BufferCount;
    g_spike.swapEffect = (unsigned)d.SwapEffect;
    g_spike.scFlags = d.Flags;
    g_spike.width = d.BufferDesc.Width;
    g_spike.height = d.BufferDesc.Height;
    g_spike.windowed = d.Windowed ? 1 : 0;
    g_spike.flipModel = (d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) ? 1 : 0;
    g_spike.waitable = (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ? 1 : 0;
    FgLog("hook: app swapchain %p hwnd %p %ux%u fmt %u buffers %u swapEffect %u flags 0x%X windowed %d flip %d waitable %d",
          (void*)sc, (void*)g_appHwnd, g_spike.width, g_spike.height, g_spike.format,
          g_spike.bufferCount, g_spike.swapEffect, g_spike.scFlags, g_spike.windowed,
          g_spike.flipModel, g_spike.waitable);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync, UINT flags)
{
    NoteApp(self);
    HRESULT hr = g_origPresent(self, sync, flags);
    if (self == (IDXGISwapChain*)g_app) { CountPresent(); g_spike.forwardedPresentHr = (long)hr; }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    NoteApp(self);
    HRESULT hr = g_origPresent1(self, sync, flags, pp);
    if ((IDXGISwapChain*)self == (IDXGISwapChain*)g_app) { CountPresent(); g_spike.forwardedPresentHr = (long)hr; }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    if (self == (IDXGISwapChain*)g_app) {
        FgLog("hook: app ResizeBuffers %ux%u count %u fmt %d flags 0x%X", w, h, count, (int)fmt, flags);
        g_appDescValid = 0;
    }
    return g_origResize(self, count, w, h, fmt, flags);
}

bool PatchSlot(void** vt, int idx, void* fn, void** outOrig)
{
    DWORD old = 0;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *outOrig = vt[idx];
    vt[idx] = fn;
    DWORD tmp = 0;
    VirtualProtect(&vt[idx], sizeof(void*), old, &tmp);
    return true;
}

// An 8x8 off-screen window that never becomes visible; the throwaway swapchain hangs off it.
HWND MakeDummyWindow()
{
    static const wchar_t kClass[] = L"RenderforgeFgDummy";
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);          // duplicate registration is harmless, we ignore the result
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, kClass, WS_POPUP,
                           0, 0, 8, 8, NULL, NULL, inst, NULL);
}

} // namespace

// ---------------------------------------------------------------- log

void FgLogInit(const wchar_t* logDir)
{
    if (g_log || !logDir) return;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\renderforge_fg.log", logDir);
    _wfopen_s(&g_log, path, L"w");
}

void FgLog(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- install

bool FgHookInstall(ID3D12CommandQueue* queue)
{
    if (g_installed) return true;
    if (!queue) { FgLog("hook: no command queue"); return false; }
    g_queue = queue;

    g_dummyHwnd = MakeDummyWindow();
    if (!g_dummyHwnd) { FgLog("hook: dummy window failed (%lu)", GetLastError()); return false; }

    IDXGIFactory2* factory = NULL;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&factory);
    if (FAILED(hr)) { FgLog("hook: CreateDXGIFactory2 0x%08X", (unsigned)hr); return false; }

    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = 8; d.Height = 8;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(queue, g_dummyHwnd, &d, NULL, NULL, &g_dummy);
    factory->Release();
    if (FAILED(hr) || !g_dummy) { FgLog("hook: dummy swapchain 0x%08X", (unsigned)hr); return false; }

    void** vt = *(void***)g_dummy;
    bool ok = PatchSlot(vt, kVtPresent,       (void*)&HookPresent,       (void**)&g_origPresent)
           && PatchSlot(vt, kVtResizeBuffers, (void*)&HookResizeBuffers, (void**)&g_origResize)
           && PatchSlot(vt, kVtPresent1,      (void*)&HookPresent1,      (void**)&g_origPresent1);
    if (!ok) { FgLog("hook: VirtualProtect failed"); return false; }

    g_installed = 1;
    g_spike.installed = 1;
    FgLog("hook: installed (vtable %p, Present %p, Present1 %p, ResizeBuffers %p)",
          (void*)vt, (void*)g_origPresent, (void*)g_origPresent1, (void*)g_origResize);
    return true;
}

void FgHookRemove(void)
{
    if (!g_installed) return;
    void** vt = *(void***)g_dummy;
    void* dummy = NULL;
    PatchSlot(vt, kVtPresent,       (void*)g_origPresent,  &dummy);
    PatchSlot(vt, kVtResizeBuffers, (void*)g_origResize,   &dummy);
    PatchSlot(vt, kVtPresent1,      (void*)g_origPresent1, &dummy);
    g_dummy->Release(); g_dummy = NULL;
    DestroyWindow(g_dummyHwnd); g_dummyHwnd = NULL;
    g_installed = 0; g_app = NULL; g_appDescValid = 0;
    FgLog("hook: removed");
}

IDXGISwapChain3* FgAppSwapChain(void) { return g_app; }
HWND FgAppHwnd(void) { return g_appHwnd; }
const DXGI_SWAP_CHAIN_DESC1* FgAppDesc(void) { return g_appDescValid ? &g_appDesc : NULL; }

HRESULT FgOriginalPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    return g_origPresent ? g_origPresent(sc, sync, flags) : E_FAIL;
}

HRESULT FgOriginalResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    return g_origResize ? g_origResize(sc, count, w, h, fmt, flags) : E_FAIL;
}

int FgPresentedFps(void) { return (int)g_fps; }
long long FgPresentCount(void) { return g_total; }
const FgSpike* FgSpikeResult(void) { return &g_spike; }

// The two probes decision 3 and decision 4a hang on. Runs once; safe to call every frame.
void FgHookSpike(void)
{
    if (g_spikeDone || !g_app || !g_queue) return;
    g_spikeDone = 1;

    IDXGIFactory2* factory = NULL;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&factory);
    if (FAILED(hr)) { FgLog("spike: CreateDXGIFactory2 0x%08X", (unsigned)hr); return; }

    DXGI_SWAP_CHAIN_DESC1 d = {};
    const DXGI_SWAP_CHAIN_DESC1* app = FgAppDesc();
    if (app) d = *app; else {
        d.Width = g_spike.width; d.Height = g_spike.height;
        d.Format = (DXGI_FORMAT)g_spike.format; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 3;
    }
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;      // FG needs the flip model regardless of Unity's choice
    d.Flags = 0;
    if (d.BufferCount < 2) d.BufferCount = 3;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    // Probe 1 (decision 3): a second swapchain on the game's own HWND.
    IDXGISwapChain1* second = NULL;
    hr = factory->CreateSwapChainForHwnd(g_queue, g_appHwnd, &d, NULL, NULL, &second);
    g_spike.secondSwapChainHr = (long)hr;
    FgLog("spike: second CreateSwapChainForHwnd on game hwnd -> 0x%08X", (unsigned)hr);
    if (second) { second->Release(); second = NULL; }

    // Probe 2 (decision 4a): a composition swapchain, which never claims the HWND.
    DXGI_SWAP_CHAIN_DESC1 c = d;
    c.Scaling = DXGI_SCALING_STRETCH;
    c.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    IDXGISwapChain1* comp = NULL;
    hr = factory->CreateSwapChainForComposition(g_queue, &c, NULL, &comp);
    g_spike.compositionHr = (long)hr;
    FgLog("spike: CreateSwapChainForComposition -> 0x%08X", (unsigned)hr);
    if (comp) comp->Release();

    factory->Release();
}
```

- [x] **Step 3: Add the spike exports to `native\RenderforgeNative.h`**

Append immediately before the closing `#ifdef __cplusplus` / `}` block:

```cpp
// Render event id for the frame-generation prepare/tag pass (records into the shim's own command list,
// submitted through IUnityGraphicsD3D12v5::ExecuteCommandList exactly like DLSS_EV_EVALUATE).
enum { DLSS_EV_FG_PREPARE = 4 };

// Fg_Init return codes.
enum { FG_OK = 0, FG_ERR_NOT_D3D12 = 1, FG_ERR_NO_HOOK = 2, FG_ERR_NO_SWAPCHAIN = 3,
       FG_ERR_NO_PROVIDER = 4, FG_ERR_PROVIDER_FAILED = 5, FG_ERR_UNSUPPORTED_MULTIPLIER = 6 };

// Fg_Caps bitmask: which multipliers the active provider can do on this GPU.
enum { FG_CAP_2X = 1, FG_CAP_3X = 2, FG_CAP_4X = 4 };

// Provider ids (Fg_Init / Fg_Provider).
enum { FG_PROVIDER_NONE = 0, FG_PROVIDER_FSR = 1, FG_PROVIDER_XESS = 2, FG_PROVIDER_DLSS = 3 };

// Main thread. Patches the DXGI swapchain vtable so the shim sees Unity's Present. Idempotent.
// Requires the D3D12 backend (Dlss_Api() == 12) because the hook needs Unity's command queue.
DLSS_API int __cdecl Fg_HookInstall(const wchar_t* logDir);
// Presented frames per second counted in the Present hook (includes generated frames). 0 = no data yet.
DLSS_API int __cdecl Fg_PresentedFps(void);
// Spike diagnostics as one flat line (static buffer, main thread only).
DLSS_API const char* __cdecl Fg_SpikeStatus(void);
```

- [x] **Step 4: Implement the three spike exports in `native\RenderforgeNative.cpp`**

Add `#include "Fg.h"` next to the existing includes, then append at the end of the exports section (before `Dlss_Shutdown`):

```cpp
// ---------------------------------------------------------------- frame generation (Phase 5)

int __cdecl Fg_HookInstall(const wchar_t* logDir)
{
    if (!S.dev || S.dev->Api() != 12) return FG_ERR_NOT_D3D12;
    if (!g_unityD3D12) return FG_ERR_NOT_D3D12;
    FgLogInit(logDir);
    return FgHookInstall(g_unityD3D12->GetCommandQueue()) ? FG_OK : FG_ERR_NO_HOOK;
}

int __cdecl Fg_PresentedFps(void) { return FgPresentedFps(); }

const char* __cdecl Fg_SpikeStatus(void)
{
    static char buf[512];
    const FgSpike* s = FgSpikeResult();
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "installed=%d sawPresent=%d presents=%lld fps=%d %ux%u fmt=%u buffers=%u swapEffect=%u flags=0x%X "
        "windowed=%d flip=%d waitable=%d secondSwapChain=0x%08X composition=0x%08X forwardedPresent=0x%08X",
        s->installed, s->sawPresent, FgPresentCount(), FgPresentedFps(), s->width, s->height, s->format,
        s->bufferCount, s->swapEffect, s->scFlags, s->windowed, s->flipModel, s->waitable,
        (unsigned)s->secondSwapChainHr, (unsigned)s->compositionHr, (unsigned)s->forwardedPresentHr);
    return buf;
}
```

And in the render-event dispatch function, add the spike trigger so the two probes run on the render thread (`FgHookSpike` is a no-op after the first call):

```cpp
    case DLSS_EV_FG_PREPARE:
        FgHookSpike();
        break;
```

- [x] **Step 5: Wire the new source into `native\CMakeLists.txt`**

Replace the `add_library(RenderforgeNative SHARED ...)` source list with:

```cmake
add_library(RenderforgeNative SHARED
    RenderforgeNative.cpp
    RenderforgeNative.h
    Sharpen.h
    Sharpen.cpp
    Fg.h
    FgHook.cpp
    unity/IUnityInterface.h
    unity/IUnityGraphics.h
    unity/IUnityGraphicsD3D12.h)
```

- [x] **Step 6: Managed side — one P/Invoke trio and a PPCLI entry point**

In `E:\DEV\PhoenixPoint\Renderforge\src\Native.cs`, add after `Dlss_Status`:

```csharp
        public const int FG_OK = 0, FG_ERR_NOT_D3D12 = 1, FG_ERR_NO_HOOK = 2, FG_ERR_NO_SWAPCHAIN = 3,
                         FG_ERR_NO_PROVIDER = 4, FG_ERR_PROVIDER_FAILED = 5, FG_ERR_UNSUPPORTED_MULTIPLIER = 6;
        public const int FG_CAP_2X = 1, FG_CAP_3X = 2, FG_CAP_4X = 4;
        public const int FG_PROVIDER_NONE = 0, FG_PROVIDER_FSR = 1, FG_PROVIDER_XESS = 2, FG_PROVIDER_DLSS = 3;
        public const int DLSS_EV_FG_PREPARE = 4;

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern int Fg_HookInstall([MarshalAs(UnmanagedType.LPWStr)] string logDir);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Fg_PresentedFps();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, EntryPoint = "Fg_SpikeStatus")]
        private static extern IntPtr Fg_SpikeStatusPtr();

        public static string Fg_SpikeStatus() => Marshal.PtrToStringAnsi(Fg_SpikeStatusPtr()) ?? "";
```

In `E:\DEV\PhoenixPoint\Renderforge\src\RenderforgeMod.cs`, add a PPCLI-reachable spike command after `GetStatus`:

```csharp
        /// <summary>Phase 5 Task 1 spike: {"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"FgSpike"}.
        /// Installs the Present hook (D3D12 only) and returns the diagnostics line once frames have gone through it.</summary>
        public static string FgSpike()
        {
            if (!Availability.IsD3D12) return "not D3D12";
            int rc = Native.Fg_HookInstall(ModDir);
            return "hookInstall=" + rc + " " + Native.Fg_SpikeStatus();
        }
```

- [x] **Step 7: Build**

```powershell
powershell -NoProfile -Command "Set-Location E:\DEV\PhoenixPoint\Renderforge; .\build-native.ps1"
```
Expected: `nvngx_dlss.dll 310.7.129.0 ...`, cmake configure + build with no warnings from `FgHook.cpp`, both probes green, `build-native: OK`.

- [x] **Step 8: Run the spike in-game and READ THE FOUR NUMBERS**

```powershell
powershell -NoProfile -Command "Set-Location E:\DEV\PhoenixPoint\Renderforge; .\deploy.ps1"
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods','-force-d3d12'
Start-Sleep -Seconds 60
Set-Location E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["DLAA","None"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"FgSpike"}'
Start-Sleep -Seconds 5
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"FgSpike"}'
Get-Content 'D:\PP-Instance2\Mods\Renderforge\renderforge_fg.log'
```
Expected on the second `FgSpike` call: `hookInstall=0 installed=1 sawPresent=1 presents=<a few hundred> fps=<~60> ... forwardedPresent=0x00000000`.

**Record all four answers in this file before writing another line of code:**

| Question | Where the answer is | Meaning |
|---|---|---|
| Hook works | `installed=1 sawPresent=1 presents>0` | vtable patch is valid, `Present` flows through us |
| Swapchain shape | `fmt= buffers= swapEffect= flags= flip= waitable=` | `flip=1` is required by every FG SDK. `flip=0` ⇒ the shadow swapchain must be created as `FLIP_DISCARD` anyway (the plan already does), and this is expected to be fine because our swapchain is ours |
| Forwarding is transparent | `forwardedPresent=0x00000000` and the game still renders | we may call `Present` ourselves through the saved pointer |
| **Second swapchain on the HWND** | `secondSwapChain=0x00000000` | **GO** for decision 3 → Task 2 |
| Composition fallback | `composition=0x00000000` | decision 4a is available |

> **2026-09-02 result:** run on `D:\PP-Instance3` (profile `...593`, `-mods -force-d3d12`), mission
> `ALN_PLT_Nest_48x48_A` seed 12345, `SetMode DLAA/None`, provider **DLSS**
> (`upscaler available provider=DLSS version= api=12 unityIface=3 renderer=D3D12`).
>
> - 1st `FgSpike` (immediately after install):
>   `hookInstall=0 installed=1 sawPresent=0 presents=0 fps=0 0x0 fmt=0 buffers=0 swapEffect=0 flags=0x0 windowed=0 flip=0 waitable=0 secondSwapChain=0x00000000 composition=0x00000000 forwardedPresent=0x00000000`
> - 2nd `FgSpike` (+5 s):
>   `hookInstall=0 installed=1 sawPresent=1 presents=583 fps=127 1280x720 fmt=28 buffers=3 swapEffect=3 flags=0x802 windowed=1 flip=1 waitable=0 secondSwapChain=0x00000000 composition=0x00000000 forwardedPresent=0x00000000`
> - 3rd `FgSpike` (confirmation): `presents=1959 fps=118`, all other fields identical.
>
> `renderforge_fg.log`:
> `hook: installed (vtable ..., Present ..., Present1 ..., ResizeBuffers ...)` and
> `hook: app swapchain ... hwnd 0000000000C20826 1280x720 fmt 28 buffers 3 swapEffect 3 flags 0x802 windowed 1 flip 1 waitable 0`.
>
> Four answers: **hook works** (`installed=1 sawPresent=1 presents>0`); **swapchain shape** is
> `DXGI_FORMAT_R8G8B8A8_UNORM` (28), 3 buffers, `FLIP_DISCARD` (swapEffect 3), `flags 0x802`,
> windowed, **`flip=1`**, not waitable; **forwarding is transparent** (`forwardedPresent=0x00000000`,
> game kept rendering — `docs\shots\fg-spike-instance3.png`); **`secondSwapChain=0x00000000`**;
> **`composition=0x00000000`**. No crash, no `DEVICE_REMOVED`.

- [x] **Step 9: Go/no-go**

> **Verdict: GO with fallback 4a (measured in Task 2)**
>
> **2026-09-02 correction:** the `secondSwapChain=0x00000000 composition=0x00000000` result above was an artefact — `FgHookSpike` only ran inside `FgHostPrepare` (called from `DLSS_EV_FG_PREPARE`), which the driver issues only when `FrameGen.Live` is true; that was never true in Task 1, so the two probes never executed and zero-initialised HRESULTs read as `S_OK`.
> - Measured in Task 2 (HEAD `55dbf41`): `CreateSwapChainForHwnd` on Unity's HWND → `0x80070005` (`E_ACCESSDENIED`) every time.
> - `CreateSwapChainForComposition` → `S_OK` (commit `e4915dd`) → decision 4a (DComp visual on the game HWND) is the shipped path.
> - Hook must forward Unity's own `Present` every frame (commit `8a2b8a2`) or the flip-chain buffer index freezes (debug layer id=907 → `DEVICE_REMOVED` `0x887A002B`).

- `secondSwapChain=0x00000000` → **GO**. Proceed to Task 2 unchanged.
- `secondSwapChain != 0` and `composition == 0` → **GO with fallback 4a**. In Task 2 Step 2 replace `CreateSwapChainForHwnd` with `CreateSwapChainForComposition` + a DirectComposition device/target/visual bound to `FgAppHwnd()` (`dcomp.lib` is already linked by Step 5 of Task 2). Everything else in Tasks 2-5 is identical.
- Both fail → **NO-GO for the in-process design.** Stop here, skip Tasks 2-7, and go to **Task 8** (the `dxgi.dll` shim contingency), which begins by asking the user.

- [x] **Step 10: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(fg): DXGI Present hook spike - swapchain discovery, present counter, two creation probes"
```

---
### Task 2: FG ABI, shadow-swapchain host, managed driver, presented fps — with a "null FG" provider

**Files:**
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\Fg.h`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\FgHook.cpp`
- Create: `E:\DEV\PhoenixPoint\Renderforge\native\FgHost.cpp`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\RenderforgeNative.h`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\RenderforgeNative.cpp`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\CMakeLists.txt`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Native.cs`
- Create: `E:\DEV\PhoenixPoint\Renderforge\src\FrameGen.cs`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\DlssConfig.cs`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\DlssDriver.cs`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\RenderforgeMod.cs`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Overlay.cs`

The whole plumbing with **no vendor SDK involved**: a shadow swapchain of our own, a per-frame backbuffer copy, presents through the shadow, and the overlay reading real/presented fps. `FG_PROVIDER_NONE` presents 1:1, so if the picture, the HUD and the frame rate are unchanged the transport is correct and every later provider is a drop-in.

- [x] **Step 1: Extend `native\Fg.h` with the provider seam**

Append to `Fg.h` (after the hook declarations):

```cpp
// ---------------------------------------------------------------- provider seam

// Everything a provider needs to build its own swapchain on the game's window.
struct FgSetup
{
    ID3D12Device*         device;
    ID3D12CommandQueue*   queue;
    IDXGIFactory2*        factory;
    HWND                  hwnd;
    DXGI_SWAP_CHAIN_DESC1 desc;        // FLIP_DISCARD, app format, app size, >= 3 buffers
    unsigned              multiplier;  // 2, 3 or 4 (total frames presented per rendered frame)
    const wchar_t*        dllDir;      // mod folder: where the vendor DLLs live
};

// One rendered frame's inputs. Filled on the main thread by Fg_SetFrame, read on the render thread.
struct FgFrame
{
    ID3D12Resource* hudless;      // outRT: upscaled scene WITHOUT the HUD, output resolution
    ID3D12Resource* depth;        // depthRT, render resolution
    ID3D12Resource* mv;           // mvRT, render resolution, current->previous, pixels
    float    jitterX, jitterY;
    float    mvScaleX, mvScaleY;
    float    cameraNear, cameraFar, cameraFovY;   // fov in radians
    float    dtMs;
    int      reset;
    unsigned renderW, renderH, outW, outH;
    unsigned long long frameId;
    float    view[16];            // worldToCamera, row-major
    float    proj[16];            // non-jittered projection, row-major
    float    camPos[3], camUp[3], camRight[3], camFwd[3];
};

struct IFgProvider
{
    virtual ~IFgProvider() {}
    virtual int      Id() const = 0;                    // FG_PROVIDER_*
    virtual unsigned Caps() const = 0;                  // FG_CAP_* mask, valid after Create
    virtual const char* Name() const = 0;
    // Build the FG-owned swapchain on s.hwnd. Returns FG_OK or an FG_ERR_*.
    virtual int      Create(const FgSetup& s, IDXGISwapChain4** outSwapChain) = 0;
    // Render thread, inside the DLSS_EV_FG_PREPARE render event, on a recording DIRECT command list.
    virtual void     Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f) = 0;
    // Render thread, inside the Present hook, after the backbuffer copy and before the shadow Present.
    virtual void     BeforePresent(const FgFrame& f) = 0;
    // Render thread, right after the shadow Present. Returns how many frames were actually presented.
    virtual int      AfterPresent(void) = 0;
    virtual void     SetEnabled(bool on) = 0;
    virtual void     Destroy(void) = 0;
};

IFgProvider* MakeFgProviderNone(void);
IFgProvider* MakeFgProviderFsr(void);
IFgProvider* MakeFgProviderXess(void);
IFgProvider* MakeFgProviderStreamline(void);

// ---------------------------------------------------------------- host (FgHost.cpp)

int  FgHostInit(int provider, unsigned multiplier, const wchar_t* dllDir);  // main thread
void FgHostSetEnabled(int on);
void FgHostSetFrame(const FgFrame& f);       // main thread, ring-buffered
void FgHostPrepare(void);                    // render thread, DLSS_EV_FG_PREPARE
void FgHostShutdown(void);                   // main thread, render idle
unsigned FgHostCaps(void);
int  FgHostProvider(void);
const char* FgHostStatus(void);

// Called from the Present hook. Returns true when the host presented the frame itself (the hook must
// then NOT call the original Present) and writes the HRESULT to *outHr.
bool FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags, HRESULT* outHr);
// Called from the ResizeBuffers hook before the original runs.
void FgHostOnResize(unsigned w, unsigned h);
// Add n presented frames to the counter (providers report their generated frames through this).
void FgPresentedAdd(int n);
```

- [x] **Step 2: Write `native\FgHost.cpp`** (as built: both command-list rings are `D3D12Ring` instances — `prep` via Unity `ExecuteCommandList` with the ring's own persistent state arrays, `copy` via the new `D3D12Ring::EndDirect(queue)`; the plan's hand-rolled ring mixed our fence with Unity's frame-fence value and passed a stack state array, which `D3D12Ring.h` documents as the 2026-09-02 DEVICE_REMOVED root cause. Vendor factories return NULL until Tasks 3-5 and the host falls back to the pass-through provider with a log line.)

```cpp
// FgHost.cpp - the presentation host. Owns the FG-owned "shadow" swapchain on the game's HWND, copies
// Unity's finished backbuffer into it every frame, drives one IFgProvider and presents. Unity's own
// swapchain is never presented again while FG is on; Unity keeps rendering into its buffer 0, which is
// exactly what it does anyway because our interception freezes its back-buffer index.
//
// Threading: Init/Shutdown on the main thread with the render thread idle; SetFrame on the main thread
// into a 4-deep ring; Prepare and OnPresent on the render thread.
#include "Fg.h"
#include "RenderforgeNative.h"
#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphics.h"
#include "unity/IUnityGraphicsD3D12.h"

#include <stdio.h>

extern IUnityGraphicsD3D12v5* g_unityD3D12;   // RenderforgeNative.cpp

namespace {

const int kRing = 3;

struct Host
{
    IFgProvider*        prov;
    IDXGISwapChain4*    shadow;
    IDXGIFactory2*      factory;
    ID3D12Device*       device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator*    alloc[kRing];
    ID3D12GraphicsCommandList* list[kRing];
    ID3D12Fence*        fence;
    UINT64              fenceVal;
    UINT64              slotFence[kRing];
    HANDLE              fenceEvent;
    unsigned            slot;
    unsigned            multiplier;
    int                 enabled;
    int                 lastError;
    unsigned            outW, outH;
    // per-frame ring, main thread writes, render thread reads
    FgFrame             frames[4];
    volatile long       frameIdx;
    FgFrame             cur;          // render-thread copy
    char                status[512];
} H = {};

bool WaitSlot(unsigned s)
{
    if (!H.fence || H.slotFence[s] == 0) return true;
    if (H.fence->GetCompletedValue() >= H.slotFence[s]) return true;
    if (FAILED(H.fence->SetEventOnCompletion(H.slotFence[s], H.fenceEvent))) return false;
    WaitForSingleObject(H.fenceEvent, 5000);
    return true;
}

// Open list `s`, barrier src PRESENT->COPY_SOURCE and dst PRESENT->COPY_DEST, CopyResource, barrier back.
bool CopyBackBuffer(ID3D12Resource* src, ID3D12Resource* dst)
{
    unsigned s = H.slot;
    if (!WaitSlot(s)) return false;
    if (FAILED(H.alloc[s]->Reset())) return false;
    if (FAILED(H.list[s]->Reset(H.alloc[s], NULL))) return false;

    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = src;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource = dst;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    H.list[s]->ResourceBarrier(2, b);

    H.list[s]->CopyResource(dst, src);

    for (int i = 0; i < 2; ++i) {
        D3D12_RESOURCE_STATES t = b[i].Transition.StateAfter;
        b[i].Transition.StateAfter = b[i].Transition.StateBefore;
        b[i].Transition.StateBefore = t;
    }
    H.list[s]->ResourceBarrier(2, b);

    if (FAILED(H.list[s]->Close())) return false;
    ID3D12CommandList* lists[1] = { H.list[s] };
    H.queue->ExecuteCommandLists(1, lists);
    H.slotFence[s] = ++H.fenceVal;
    H.queue->Signal(H.fence, H.fenceVal);
    H.slot = (H.slot + 1) % kRing;
    return true;
}

void ReleaseGpu()
{
    if (H.fence) {
        for (int i = 0; i < kRing; ++i) WaitSlot(i);
    }
    for (int i = 0; i < kRing; ++i) {
        if (H.list[i])  { H.list[i]->Release();  H.list[i] = NULL; }
        if (H.alloc[i]) { H.alloc[i]->Release(); H.alloc[i] = NULL; }
        H.slotFence[i] = 0;
    }
    if (H.fence)      { H.fence->Release(); H.fence = NULL; }
    if (H.fenceEvent) { CloseHandle(H.fenceEvent); H.fenceEvent = NULL; }
    if (H.shadow)     { H.shadow->Release(); H.shadow = NULL; }
    if (H.factory)    { H.factory->Release(); H.factory = NULL; }
}

// ---------------------------------------------------------------- the pass-through provider

struct ProviderNone : IFgProvider
{
    IDXGISwapChain4* sc;
    ProviderNone() : sc(NULL) {}
    int Id() const { return FG_PROVIDER_NONE; }
    unsigned Caps() const { return FG_CAP_2X; }        // reported, never used: NONE generates nothing
    const char* Name() const { return "none"; }
    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        IDXGISwapChain1* sc1 = NULL;
        DXGI_SWAP_CHAIN_DESC1 d = s.desc;
        HRESULT hr = s.factory->CreateSwapChainForHwnd(s.queue, s.hwnd, &d, NULL, NULL, &sc1);
        if (FAILED(hr) || !sc1) { FgLog("none: CreateSwapChainForHwnd 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }
        hr = sc1->QueryInterface(__uuidof(IDXGISwapChain4), (void**)out);
        sc1->Release();
        if (FAILED(hr)) return FG_ERR_NO_SWAPCHAIN;
        sc = *out;
        return FG_OK;
    }
    void Prepare(ID3D12GraphicsCommandList*, const FgFrame&) {}
    void BeforePresent(const FgFrame&) {}
    int  AfterPresent(void) { return 1; }
    void SetEnabled(bool) {}
    void Destroy(void) { sc = NULL; }
};

ProviderNone g_none;

} // namespace

IFgProvider* MakeFgProviderNone(void) { return &g_none; }

// ---------------------------------------------------------------- host

int FgHostInit(int provider, unsigned multiplier, const wchar_t* dllDir)
{
    if (H.prov) return FG_OK;
    if (!g_unityD3D12) return FG_ERR_NOT_D3D12;

    IDXGISwapChain3* app = FgAppSwapChain();
    HWND hwnd = FgAppHwnd();
    if (!app || !hwnd) return FG_ERR_NO_SWAPCHAIN;    // hook has not seen a Present yet: caller retries

    IFgProvider* p = NULL;
    switch (provider) {
    case FG_PROVIDER_FSR:  p = MakeFgProviderFsr();        break;
    case FG_PROVIDER_XESS: p = MakeFgProviderXess();       break;
    case FG_PROVIDER_DLSS: p = MakeFgProviderStreamline(); break;
    default:               p = MakeFgProviderNone();       break;
    }
    if (!p) return FG_ERR_NO_PROVIDER;

    H.device = g_unityD3D12->GetDevice();
    H.queue  = g_unityD3D12->GetCommandQueue();
    if (!H.device || !H.queue) return FG_ERR_NOT_D3D12;

    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&H.factory);
    if (FAILED(hr)) { FgLog("host: CreateDXGIFactory2 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }

    DXGI_SWAP_CHAIN_DESC ad = {};
    app->GetDesc(&ad);
    FgSetup s = {};
    s.device = H.device; s.queue = H.queue; s.factory = H.factory; s.hwnd = hwnd;
    s.multiplier = multiplier < 2 ? 2 : (multiplier > 4 ? 4 : multiplier);
    s.dllDir = dllDir;
    const DXGI_SWAP_CHAIN_DESC1* a1 = FgAppDesc();
    if (a1) s.desc = *a1;
    else {
        s.desc.Width = ad.BufferDesc.Width; s.desc.Height = ad.BufferDesc.Height;
        s.desc.Format = ad.BufferDesc.Format; s.desc.SampleDesc.Count = 1;
        s.desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    }
    s.desc.SampleDesc.Count = 1; s.desc.SampleDesc.Quality = 0;
    s.desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    s.desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;    // every FG SDK on disk requires the flip model
    s.desc.Scaling = DXGI_SCALING_STRETCH;
    s.desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    s.desc.Flags = 0;                                      // no waitable object: we drive the pacing
    if (s.desc.BufferCount < 3) s.desc.BufferCount = 3;
    H.outW = s.desc.Width; H.outH = s.desc.Height;
    H.multiplier = s.multiplier;

    IDXGISwapChain4* shadow = NULL;
    int rc = p->Create(s, &shadow);
    if (rc != FG_OK || !shadow) { H.factory->Release(); H.factory = NULL; H.lastError = rc; return rc; }
    H.shadow = shadow;
    H.prov = p;
    H.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    for (int i = 0; i < kRing; ++i) {
        if (FAILED(H.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&H.alloc[i])) ||
            FAILED(H.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, H.alloc[i], NULL, __uuidof(ID3D12GraphicsCommandList), (void**)&H.list[i])) ||
            FAILED(H.list[i]->Close()))
        {
            FgLog("host: command list %d failed", i);
            FgHostShutdown();
            return FG_ERR_PROVIDER_FAILED;
        }
    }
    if (FAILED(H.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&H.fence))) {
        FgHostShutdown(); return FG_ERR_PROVIDER_FAILED;
    }
    H.fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    H.slot = 0; H.fenceVal = 0;
    FgLog("host: provider=%s multiplier=%u shadow=%p %ux%u caps=0x%X",
          p->Name(), H.multiplier, (void*)H.shadow, H.outW, H.outH, p->Caps());
    return FG_OK;
}

void FgHostSetEnabled(int on)
{
    H.enabled = on ? 1 : 0;
    if (H.prov) H.prov->SetEnabled(on != 0);
    FgLog("host: enabled=%d", H.enabled);
}

void FgHostSetFrame(const FgFrame& f)
{
    long i = (H.frameIdx + 1) & 3;
    H.frames[i] = f;
    H.frameIdx = i;
}

void FgHostPrepare(void)
{
    FgHookSpike();                       // Task 1 probes; no-op after the first call
    if (!H.prov || !H.enabled) return;
    H.cur = H.frames[H.frameIdx & 3];
    if (!H.cur.hudless || !H.cur.depth || !H.cur.mv) return;

    unsigned s = H.slot;
    if (!WaitSlot(s)) return;
    if (FAILED(H.alloc[s]->Reset()) || FAILED(H.list[s]->Reset(H.alloc[s], NULL))) return;
    H.prov->Prepare(H.list[s], H.cur);
    if (FAILED(H.list[s]->Close())) return;

    // Unity owns the states of hudless/depth/mv. We ask for the state every FG SDK reads them in and
    // hand them back unchanged, exactly like Phase 2's DLSS evaluate.
    UnityGraphicsD3D12ResourceState st[3];
    st[0].resource = H.cur.hudless; st[0].expected = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; st[0].current = st[0].expected;
    st[1].resource = H.cur.depth;   st[1].expected = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; st[1].current = st[1].expected;
    st[2].resource = H.cur.mv;      st[2].expected = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; st[2].current = st[2].expected;
    H.slotFence[s] = g_unityD3D12->ExecuteCommandList(H.list[s], 3, st);
    H.slot = (H.slot + 1) % kRing;
}

bool FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags, HRESULT* outHr)
{
    if (!H.prov || !H.enabled || !H.shadow) return false;
    if (app != (IDXGISwapChain*)FgAppSwapChain()) return false;

    IDXGISwapChain3* app3 = FgAppSwapChain();
    ID3D12Resource* src = NULL;
    ID3D12Resource* dst = NULL;
    if (FAILED(app3->GetBuffer(app3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&src)) ||
        FAILED(H.shadow->GetBuffer(H.shadow->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&dst)))
    {
        if (src) src->Release();
        if (dst) dst->Release();
        return false;
    }
    bool copied = CopyBackBuffer(src, dst);
    src->Release(); dst->Release();
    if (!copied) return false;

    H.prov->BeforePresent(H.cur);
    HRESULT hr = H.shadow->Present(syncInterval, flags);
    int presented = H.prov->AfterPresent();
    FgPresentedAdd(presented < 1 ? 1 : presented);
    *outHr = hr;
    return true;
}

void FgHostOnResize(unsigned w, unsigned h)
{
    if (!H.prov) return;
    FgLog("host: resize %ux%u - tearing the FG chain down, the driver rebuilds it", w, h);
    IFgProvider* p = H.prov;
    H.enabled = 0;
    p->SetEnabled(false);
    ReleaseGpu();
    p->Destroy();
    H.prov = NULL;
}

void FgHostShutdown(void)
{
    if (H.prov) { H.prov->SetEnabled(false); }
    ReleaseGpu();
    if (H.prov) { H.prov->Destroy(); H.prov = NULL; }
    H.enabled = 0;
    FgLog("host: shutdown");
}

unsigned FgHostCaps(void) { return H.prov ? H.prov->Caps() : 0u; }
int FgHostProvider(void) { return H.prov ? H.prov->Id() : FG_PROVIDER_NONE; }

const char* FgHostStatus(void)
{
    _snprintf_s(H.status, sizeof(H.status), _TRUNCATE,
        "provider=%s enabled=%d multiplier=%u shadow=%p out=%ux%u caps=0x%X lastError=%d presented=%lld fps=%d frameId=%llu",
        H.prov ? H.prov->Name() : "-", H.enabled, H.multiplier, (void*)H.shadow, H.outW, H.outH,
        FgHostCaps(), H.lastError, FgPresentCount(), FgPresentedFps(),
        (unsigned long long)H.frames[H.frameIdx & 3].frameId);
    return H.status;
}
```

- [x] **Step 3: Let the hook call the host — edit `native\FgHook.cpp`**

Replace the bodies of `HookPresent`, `HookPresent1` and `HookResizeBuffers` with:

```cpp
HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync, UINT flags)
{
    NoteApp(self);
    HRESULT hr = S_OK;
    if (self == (IDXGISwapChain*)g_app && FgHostOnPresent(self, sync, flags, &hr)) return hr;
    hr = g_origPresent(self, sync, flags);
    if (self == (IDXGISwapChain*)g_app) { CountPresent(); g_spike.forwardedPresentHr = (long)hr; }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    NoteApp(self);
    HRESULT hr = S_OK;
    if ((IDXGISwapChain*)self == (IDXGISwapChain*)g_app && FgHostOnPresent((IDXGISwapChain*)self, sync, flags, &hr)) return hr;
    hr = g_origPresent1(self, sync, flags, pp);
    if ((IDXGISwapChain*)self == (IDXGISwapChain*)g_app) { CountPresent(); g_spike.forwardedPresentHr = (long)hr; }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    if (self == (IDXGISwapChain*)g_app) {
        FgLog("hook: app ResizeBuffers %ux%u count %u fmt %d flags 0x%X", w, h, count, (int)fmt, flags);
        FgHostOnResize(w, h);
        g_appDescValid = 0;
    }
    return g_origResize(self, count, w, h, fmt, flags);
}
```

and add the counter entry point at the bottom of the file:

```cpp
void FgPresentedAdd(int n)
{
    for (int i = 0; i < n; ++i) CountPresent();
}
```

- [x] **Step 4: Exports in `native\RenderforgeNative.h`**

Append after the `Fg_SpikeStatus` declaration from Task 1:

```cpp
// Main thread. Builds the FG chain: hook (if not yet), provider, shadow swapchain. Retry-safe: returns
// FG_ERR_NO_SWAPCHAIN until the hook has seen at least one Present, so the caller may call it per frame.
DLSS_API int __cdecl Fg_Init(int provider, unsigned multiplier, const wchar_t* dllDir);
// Main thread. Turns interpolation on/off without tearing the chain down.
DLSS_API void __cdecl Fg_SetEnabled(int on);
// Main thread. One rendered frame's inputs. All resources are ID3D12Resource*. `view`/`proj` are 16 floats
// row-major; `cam` is 12 floats: pos[3], up[3], right[3], forward[3]. fovY in radians.
DLSS_API void __cdecl Fg_SetFrame(void* hudless, void* depth, void* mv,
                                  float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                                  float cameraNear, float cameraFar, float cameraFovY,
                                  float dtMs, int reset,
                                  unsigned renderW, unsigned renderH, unsigned outW, unsigned outH,
                                  unsigned long long frameId,
                                  const float* view, const float* proj, const float* cam);
// FG_CAP_* mask of the ACTIVE provider on THIS GPU. 0 = no chain.
DLSS_API unsigned __cdecl Fg_Caps(void);
// FG_PROVIDER_* of the active chain.
DLSS_API int __cdecl Fg_Provider(void);
// One diagnostic line (static buffer, main thread only).
DLSS_API const char* __cdecl Fg_Status(void);
// Main thread, render idle. Destroys the chain; the Present hook stays installed and inert.
DLSS_API void __cdecl Fg_Shutdown(void);
```

- [x] **Step 5: Implement them in `native\RenderforgeNative.cpp`**

Append after `Fg_SpikeStatus`:

```cpp
int __cdecl Fg_Init(int provider, unsigned multiplier, const wchar_t* dllDir)
{
    if (!S.dev || S.dev->Api() != 12 || !g_unityD3D12) return FG_ERR_NOT_D3D12;
    FgLogInit(dllDir);
    if (!FgHookInstall(g_unityD3D12->GetCommandQueue())) return FG_ERR_NO_HOOK;
    return FgHostInit(provider, multiplier, dllDir);
}

void __cdecl Fg_SetEnabled(int on) { FgHostSetEnabled(on); }

void __cdecl Fg_SetFrame(void* hudless, void* depth, void* mv,
                         float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                         float cameraNear, float cameraFar, float cameraFovY,
                         float dtMs, int reset,
                         unsigned renderW, unsigned renderH, unsigned outW, unsigned outH,
                         unsigned long long frameId,
                         const float* view, const float* proj, const float* cam)
{
    FgFrame f;
    memset(&f, 0, sizeof(f));
    f.hudless = (ID3D12Resource*)hudless;
    f.depth   = (ID3D12Resource*)depth;
    f.mv      = (ID3D12Resource*)mv;
    f.jitterX = jitterX; f.jitterY = jitterY;
    f.mvScaleX = mvScaleX; f.mvScaleY = mvScaleY;
    f.cameraNear = cameraNear; f.cameraFar = cameraFar; f.cameraFovY = cameraFovY;
    f.dtMs = dtMs; f.reset = reset;
    f.renderW = renderW; f.renderH = renderH; f.outW = outW; f.outH = outH;
    f.frameId = frameId;
    if (view) memcpy(f.view, view, sizeof(f.view));
    if (proj) memcpy(f.proj, proj, sizeof(f.proj));
    if (cam) {
        memcpy(f.camPos,   cam + 0, sizeof(f.camPos));
        memcpy(f.camUp,    cam + 3, sizeof(f.camUp));
        memcpy(f.camRight, cam + 6, sizeof(f.camRight));
        memcpy(f.camFwd,   cam + 9, sizeof(f.camFwd));
    }
    FgHostSetFrame(f);
}

unsigned __cdecl Fg_Caps(void) { return FgHostCaps(); }
int __cdecl Fg_Provider(void) { return FgHostProvider(); }
const char* __cdecl Fg_Status(void) { return FgHostStatus(); }
void __cdecl Fg_Shutdown(void) { FgHostShutdown(); }
```

and change the `DLSS_EV_FG_PREPARE` arm added in Task 1 Step 4 from `FgHookSpike();` to:

```cpp
    case DLSS_EV_FG_PREPARE:
        FgHostPrepare();
        break;
```

Also call `FgHostShutdown(); FgHookRemove();` at the top of `Dlss_Shutdown`.

- [x] **Step 6: `native\CMakeLists.txt` — new source and the DirectComposition link (fallback 4a)** (added `FgHost.cpp` + `dcomp` to the existing multi-provider list; the snippet below predates the FSR/XeSS sources)

```cmake
add_library(RenderforgeNative SHARED
    RenderforgeNative.cpp
    RenderforgeNative.h
    Sharpen.h
    Sharpen.cpp
    Fg.h
    FgHook.cpp
    FgHost.cpp
    unity/IUnityInterface.h
    unity/IUnityGraphics.h
    unity/IUnityGraphicsD3D12.h)
target_include_directories(RenderforgeNative PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${DLSS_SDK}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(RenderforgeNative PRIVATE "${NGX_LIB}" d3d11 d3d12 dxgi dcomp d3dcompiler)
```

- [x] **Step 7: Managed — the config field** (placed after `Upscaler`, the last field at HEAD)

In `E:\DEV\PhoenixPoint\Renderforge\src\DlssConfig.cs`, add the enum next to `RendererMode`:

```csharp
    /// <summary>Frame generation multiplier. Off = the game presents every rendered frame and nothing else.
    /// 3x/4x exist only on DLSS-G with an RTX 50 GPU; the picker greys what Fg_Caps does not report.</summary>
    public enum FrameGenMode { Off, X2, X3, X4 }
```

the field, after `Renderer`:

```csharp
        [ConfigField("Frame generation", "Off / 2x / 3x / 4x. DirectX 12 only. 3x and 4x need DLSS-G on an RTX 50 GPU.")]
        public FrameGenMode FrameGen = FrameGenMode.Off;
```

and the RU entry inside the `Ru` dictionary:

```csharp
            { nameof(FrameGen), new[] { "Генерация кадров", "Выкл / 2x / 3x / 4x. Только DirectX 12. 3x и 4x — DLSS-G на видеокарте RTX 50." } },
```

- [x] **Step 8: Managed — `src\FrameGen.cs`** (plus `Release()`: driver teardown keeps the wish so the next live generation's `Retry()` rebuilds the chain; `Stop()` = `Release()` + forget)

```csharp
using System;
using UnityEngine;

namespace Renderforge
{
    /// <summary>Managed side of the frame-generation chain. The native shim owns the Present hook and the
    /// shadow swapchain; this class only decides WHICH provider and WHEN, retries the init until the hook has
    /// seen a Present (it cannot build the chain before that), and exposes the status for PPCLI and the overlay.</summary>
    internal static class FrameGen
    {
        private static int wantProvider, wantMultiplier;
        private static bool live;
        private static float retryAt;
        private static int lastRc = -1;

        internal static bool Live => live;
        internal static int Provider => live ? Native.Fg_Provider() : Native.FG_PROVIDER_NONE;
        internal static uint Caps => live ? Native.Fg_Caps() : 0u;
        internal static int LastResult => lastRc;

        internal static string ProviderName(int id)
        {
            switch (id)
            {
                case Native.FG_PROVIDER_FSR: return "FSR";
                case Native.FG_PROVIDER_XESS: return "XeSS";
                case Native.FG_PROVIDER_DLSS: return "DLSS";
                default: return "none";
            }
        }

        internal static int Multiplier(FrameGenMode m) => m == FrameGenMode.X4 ? 4 : m == FrameGenMode.X3 ? 3 : m == FrameGenMode.X2 ? 2 : 0;

        /// <summary>Which vendor drives FG on this GPU: NVIDIA -> DLSS-G, everything else -> FSR-FG (cross-vendor),
        /// with XeSS-FG as the second cross-vendor option. Mirrors the spec's Auto order for D3D12.</summary>
        internal static int AutoProvider()
        {
            if (Availability.IsNvidia) return Native.FG_PROVIDER_DLSS;
            return Native.FG_PROVIDER_FSR;
        }

        /// <summary>Called from RenderforgeMod on config change and from the driver every frame while live.</summary>
        internal static void Apply(DlssConfig cfg)
        {
            int mult = Multiplier(cfg.FrameGen);
            if (!Availability.IsD3D12 || mult == 0 || Availability.Reason(Feature.FrameGen) != null)
            {
                Stop();
                return;
            }
            wantProvider = AutoProvider();
            wantMultiplier = mult;
            if (live) { Native.Fg_SetEnabled(1); return; }
            Retry();
        }

        /// <summary>The chain can only be built after the Present hook has seen a frame, so the first attempts
        /// legitimately return FG_ERR_NO_SWAPCHAIN. Retry four times a second, log once per distinct code.</summary>
        internal static void Retry()
        {
            if (live || wantMultiplier == 0) return;
            if (Time.unscaledTime < retryAt) return;
            retryAt = Time.unscaledTime + 0.25f;
            int rc = Native.Fg_Init(wantProvider, (uint)wantMultiplier, RenderforgeMod.ModDir);
            if (rc != lastRc)
            {
                lastRc = rc;
                RenderforgeMod.Instance?.Logger.LogInfo("FG init " + ProviderName(wantProvider) + " " + wantMultiplier + "x -> " + rc);
            }
            if (rc != Native.FG_OK) return;
            live = true;
            Native.Fg_SetEnabled(1);
            RenderforgeMod.Instance?.Logger.LogInfo("FG live: " + Native.Fg_Status());
        }

        internal static void Stop()
        {
            wantMultiplier = 0;
            if (!live) return;
            Native.Fg_SetEnabled(0);
            Native.Fg_Shutdown();
            live = false;
            lastRc = -1;
            Overlay.FgFps = 0;
        }

        internal static string Status() => (live ? "live " : "off ") + Native.Fg_Status();
    }
}
```

- [x] **Step 9: Managed — feed the driver (`src\DlssDriver.cs`)** (`BeginRelease` calls `FrameGen.Release()`, not `Stop()`)

At the end of the `try` block in `AfterPostProcessPreCull`, right after `cbEval.IssuePluginEventAndData(...)` and before `frames++`, add:

```csharp
                if (FrameGen.Live)
                {
                    var v = cam.worldToCameraMatrix;
                    var pr = cam.nonJitteredProjectionMatrix;
                    float[] view = { v.m00,v.m01,v.m02,v.m03, v.m10,v.m11,v.m12,v.m13, v.m20,v.m21,v.m22,v.m23, v.m30,v.m31,v.m32,v.m33 };
                    float[] proj = { pr.m00,pr.m01,pr.m02,pr.m03, pr.m10,pr.m11,pr.m12,pr.m13, pr.m20,pr.m21,pr.m22,pr.m23, pr.m30,pr.m31,pr.m32,pr.m33 };
                    Vector3 cp = cam.transform.position, cu = cam.transform.up, cr = cam.transform.right, cf = cam.transform.forward;
                    float[] camv = { cp.x,cp.y,cp.z, cu.x,cu.y,cu.z, cr.x,cr.y,cr.z, cf.x,cf.y,cf.z };
                    Native.Fg_SetFrame(outPtr, depthPtr, mvPtr, -jx, -jy, -renderW, -renderH,
                        cam.nearClipPlane, cam.farClipPlane, cam.fieldOfView * Mathf.Deg2Rad,
                        Time.unscaledDeltaTime * 1000f, reset,
                        (uint)renderW, (uint)renderH, (uint)outW, (uint)outH, (ulong)frames,
                        view, proj, camv);
                    cbEval.IssuePluginEvent(evFn, Native.DLSS_EV_FG_PREPARE);
                }
```

Change the `Native.cs` signature's last three parameters to `float[]` so this compiles without `unsafe` (see Step 10). In `BeginRelease()`, before `MipBias.Reset()`, add `FrameGen.Stop();` — the FG chain references `outRT`/`depthRT`/`mvRT` and must die before they do. In `Step()`'s `Gen.Live` arm, after `KeepCameraState();`, add `FrameGen.Retry();`.

- [x] **Step 10: Managed — the P/Invoke block (`src\Native.cs`)**

Add after the Task 1 block:

```csharp
        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern int Fg_Init(int provider, uint multiplier, [MarshalAs(UnmanagedType.LPWStr)] string dllDir);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Fg_SetEnabled(int on);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Fg_SetFrame(IntPtr hudless, IntPtr depth, IntPtr mv,
            float jitterX, float jitterY, float mvScaleX, float mvScaleY,
            float cameraNear, float cameraFar, float cameraFovY,
            float dtMs, int reset,
            uint renderW, uint renderH, uint outW, uint outH,
            ulong frameId,
            float[] view, float[] proj, float[] cam);

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern uint Fg_Caps();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Fg_Provider();

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl, EntryPoint = "Fg_Status")]
        private static extern IntPtr Fg_StatusPtr();

        public static string Fg_Status() => Marshal.PtrToStringAnsi(Fg_StatusPtr()) ?? "";

        [DllImport("RenderforgeNative", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Fg_Shutdown();
```

- [x] **Step 11: Managed — mod lifecycle and PPCLI entry (`src\RenderforgeMod.cs`)** (`FrameGen.Apply` only in `AttachAndApply`, which `OnConfigChanged` already calls. Also pulled forward from Task 5: `Availability.Reason(Feature.FrameGen)` no longer says "Not implemented yet" on D3D12 — it returns "Turn an upscaler on first" while `Mode == Off`, else null — and `Pickers` opens the FG row from `cfg.FrameGen`, greys per `FrameGen.Caps`, and writes through `SetFrameGen`; without that the tester's `SetFrameGen X2` would have been refused by `Apply`.)

In `OnConfigChanged`, after `AttachAndApply();` add `FrameGen.Apply(Cfg);`.
In `AttachAndApply()`, after `Overlay.Apply(Cfg);` add `FrameGen.Apply(Cfg);`.
In `OnModDisabled`, before `Overlay.Destroy();` add `FrameGen.Stop();`.
Add the PPCLI entry after `SetSharpness`:

```csharp
        /// <summary>PPCLI: {"member":"SetFrameGen","args":["X2"]} - Off / X2 / X3 / X4. Live next frame + saved.</summary>
        public static string SetFrameGen(string mode)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            m.Cfg.FrameGen = (FrameGenMode)Enum.Parse(typeof(FrameGenMode), mode, true);
            FrameGen.Apply(m.Cfg);
            SaveConfig();
            return "frameGen=" + m.Cfg.FrameGen + " " + FrameGen.Status();
        }
```

and extend `GetStatus` so the FG line always ships with the DLSS one:

```csharp
        public static string GetStatus() => (DlssDriver.Instance?.Status ?? ("no driver; available=" + Available + " init=" + InitCode))
                                          + " | fg=" + FrameGen.Status();
```

- [x] **Step 12: Managed — the overlay (`src\Overlay.cs`)**

In `Update()`, right before the `float avg = ...` line, add:

```csharp
            FgFps = FrameGen.Live ? Native.Fg_PresentedFps() : 0;
```

and add the `FG:` line to the text block, between `AA:` and the fps line:

```csharp
                      + "\nFG: " + (FrameGen.Live
                            ? FrameGen.ProviderName(FrameGen.Provider) + " " + (Native.Fg_Caps() != 0 ? RenderforgeMod.Instance.Cfg.FrameGen.ToString().Replace("X", "") + "x" : "?")
                            : "off" + (Availability.Reason(Feature.FrameGen) != null ? " (" + Availability.Reason(Feature.FrameGen) + ")" : ""))
```

- [x] **Step 13: Build** (2026-09-02: `build-native.ps1` → 4x `PROBE OK`, `build-native: OK`, 0 warnings; `dotnet build -c Release` → 0 warnings, 0 errors. Not deployed.)

```powershell
powershell -NoProfile -Command "Set-Location E:\DEV\PhoenixPoint\Renderforge; .\build-native.ps1"
powershell -NoProfile -Command "Set-Location E:\DEV\PhoenixPoint\Renderforge; .\deploy.ps1 -SkipNative"
```
Expected: `build-native: OK`, then `Deployed Renderforge to D:\PP-Instance2\Mods\Renderforge`.

- [x] **Step 14: The null-FG go/no-go run**

```powershell
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods','-force-d3d12'
Start-Sleep -Seconds 60
Set-Location E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["DLAA","None"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"ToggleOverlay"}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf\\fg-off.png"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["X2"]}'
Start-Sleep -Seconds 5
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf\\fg-none.png"}'
```
Expected: `GetStatus` contains `fg=live provider=none enabled=1 multiplier=2 shadow=0x… caps=0x1 lastError=0 presented=<growing> fps=<~60>`. Read both PNGs with the Read tool: **the two images must be indistinguishable** — same scene, same HUD, no black frame, no tearing, no missing overlay.

The two failure modes to watch for and their documented answers:
- **The game hangs on the first FG frame.** Cause: Unity's swapchain was created with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (Task 1 reported `waitable=1`) and Unity waits on a latency object that only the original `Present` signals. Fix: in `FgHostOnPresent`, after the shadow `Present`, also call `FgOriginalPresent(app, 0, DXGI_PRESENT_TEST)` — a test present signals the frame-latency waitable without producing a visible frame — and re-run this step.
- **The window shows Unity's frames, not ours** (the overlay shows FG live but the picture never comes from the shadow chain). Cause: DXGI kept the first swapchain as the HWND's presenter. Fix: switch to fallback 4a from Task 1 Step 9 (`CreateSwapChainForComposition` + a DirectComposition visual on `FgAppHwnd()`); `dcomp` is already linked.

> **2026-09-02 run on `D:\PP-Instance3` (HEAD `55dbf41`, `-mods -force-d3d12`, `ALN_PLT_Nest_48x48_A` seed 12345, 1280x720): FAIL — the shadow swapchain never comes up.**
> `SetMode DLAA/None` + `SetOverlay TopCenter`, then:
> - before: `… present=on broken=False fail= | fg=off provider=- enabled=0 multiplier=0 shadow=0000000000000000 out=0x0 caps=0x0 lastError=0 presented=0 fps=0 frameId=0`
> - `SetFrameGen ["X2"]` → `frameGen=X2 off provider=- enabled=0 multiplier=0 shadow=0000000000000000 out=0x0 caps=0x0 lastError=0 presented=0 fps=0 frameId=0`
> - on, +5 s: `… | fg=off provider=- enabled=0 multiplier=2 shadow=0000000000000000 out=1280x720 caps=0x0 lastError=3 presented=669 fps=119 frameId=0`
> - on, +10 s: `… | fg=off provider=- enabled=0 multiplier=2 shadow=0000000000000000 out=1280x720 caps=0x0 lastError=3 presented=1292 fps=129 frameId=0`
> - `SetFrameGen ["Off"]` → `frameGen=Off off provider=- enabled=0 multiplier=2 shadow=0000000000000000 out=1280x720 caps=0x0 lastError=3 presented=6477 fps=115 frameId=0`; status after: `… fg=off … lastError=3 presented=6867 fps=126 frameId=0`
>
> Root cause, from `D:\PP-Instance3\Mods\Renderforge\renderforge_fg.log` (repeats once per retry tick):
> `host: provider 3 not built yet - pass-through chain` / `none: CreateSwapChainForHwnd 0x80070005`
> `0x80070005` = `E_ACCESSDENIED`: DXGI refuses a **second** swapchain on an HWND that already has Unity's.
> `lastError=3` = `FG_ERR_NO_SWAPCHAIN`, so `FrameGen` retries forever and FG never goes live.
> This is exactly the documented failure mode "the window shows Unity's frames, not ours" — **apply fallback 4a**
> (`CreateSwapChainForComposition` + a DirectComposition visual on `FgAppHwnd()`; `dcomp` is already linked) and re-run.
>
> Everything else is clean: no crash, no `DEVICE_REMOVED`, `Player.log` has only `FG init DLSS 2x -> 3`.
> `docs\shots\fg-null-{before,on,off}.png` are pixel-equivalent in content (same scene, HUD intact, overlay reads `FG: off`
> in all three, DLSS `Mode: DLAA (1280x720)`, fps 137/128/…); no black frame, no flicker. `presented`/`fps` climb because the
> hook counts Unity's own presents, not shadow presents (`frameId=0` throughout). A `start-mission` reload after `Off`
> succeeded (`ok:true`, 58 steps, 19808 ms), so teardown is clean. Not a rendering regression — Step 14 is blocked on 4a.

> **2026-09-02 RE-RUN on `D:\PP-Instance3` (HEAD `e4915dd`, fallback 4a = `CreateSwapChainForComposition` + DComp visual, `-mods -force-d3d12`, `ALN_PLT_Nest_48x48_A` seed 12345, 1280x720): FAIL — the shadow chain now comes up, and the GPU device is removed on the first shadow present.**
> The chain creation half of 4a WORKS. `docs\shots\fg-null2-crash-fg.log`:
> `hook: app swapchain … hwnd 00000000007D099C 1280x720 fmt 28 buffers 3 swapEffect 3 flags 0x802 windowed 1 flip 1 waitable 0`
> `host: provider 3 not built yet - pass-through chain`
> `host: composition chain 000001E78A7F1380 on hwnd 00000000007D099C, flags 0x800 (tearing supported 1)`
> `host: provider=none multiplier=2 shadow=000001E78A7F1380 1280x720 flags=0x800 caps=0x1`
> `host: enabled=1`
> `spike: second CreateSwapChainForHwnd on game hwnd -> 0x80070005`  ← the old 55dbf41 path, kept only as the probe
> `spike: CreateSwapChainForComposition -> 0x00000000`
>
> Sequence: `SetMode ["DLAA","None"]` + `SetOverlay ["TopCenter"]` OK; before-status
> `… present=on broken=False fail= | fg=off provider=- enabled=0 multiplier=0 shadow=0000000000000000 out=0x0 flags=0x0 caps=0x0 lastError=0 presentHr=0x00000000 presented=0 fps=0 frameId=0`;
> `docs\shots\fg-null2-before.png` normal (1280x720, HUD + overlay intact).
> `SetFrameGen ["X2"]` → `frameGen=X2 off provider=- enabled=0 multiplier=0 shadow=0000000000000000 out=0x0 flags=0x0 caps=0x0 lastError=0 presentHr=0x00000000 presented=0 fps=0 frameId=0`
> (that reply is the pre-arm snapshot; the host armed a tick later, see `host: enabled=1`).
> The **very next** `GetStatus` 5 s later never returned: `{"ok":false,"error":"the pipe closed after 0 of 4 bytes"}` — the process was gone.
> No `fg-null2-on.png` / `fg-null2-off.png` exist; the run cannot reach them.
>
> `Player.log`: `d3d12: swapchain present failed (887a0005).` / `d3d12:     DXGI_ERROR_DEVICE_REMOVED reason (887a002b).` (= `DXGI_ERROR_INVALID_CALL`),
> `d3d12 : CreateCommittedResource 'TexturesD3D12::CreateTextureInternal() Texture' (128 x 2) format 10 failed (887a0005).`, then `Crash!!!`
> with the faulting frame in **`nvwgf2umx` `OpenAdapter10`** (NVIDIA UMD) under `UnityPlayer`.
> Crash dumps: `C:\Temp\Snapshot Games Inc\Phoenix Point\Crashes\Crash_2026-09-02_15{2542,2607,2632}*`.
>
> **The crash is deterministic and FG-caused, proven by config bisect.** `SetFrameGen` persists the mode, so
> `…\Steam\76561197996210593\ModConfig.json` → `"com.morgott.Renderforge": {… "FrameGen":1}` made every subsequent cold launch
> die with exit `-1073741819` (`0xC0000005`) ~4 s in, before Unity even flushed `Player.log` — 4 launches, 4 identical exits,
> each writing the same `host: composition chain … / host: enabled=1` pair to `renderforge_fg.log`.
> Flipping that one field to `"FrameGen":0` and relaunching → the game came up and stayed up. Nothing else changed.
>
> Conclusion: fallback 4a fixes swapchain *creation* (`0x80070005` gone, `shadow` non-zero, `caps=0x1`) but the present path
> is invalid — `presentHr` never even got read back, and D3D12 reports `INVALID_CALL` device removal from the shadow present
> while Unity's own D3D12 queue is still using the back buffer. Next suspects, in order: (1) the shadow chain is presented
> from a different queue / without a fence sync against Unity's queue; (2) the DComp visual is never `SetContent` + `Commit`ed
> on the same thread that owns the device, so the chain's buffers are unbound; (3) `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`
> (`flags=0x800`) with a `SyncInterval` that is not 0. Steps 14-16 stay **unticked**; no source edits made in this run.

> **2026-09-02 RCA + FIX on `D:\PP-Instance3` (`-mods -force-d3d12 -force-d3d12-debug`, `RENDERFORGE_D3D12_DEBUG=1`, same scene/seed, 1280x720): PASS.**
> Measured, not guessed. The shadow Present itself was fine: `host: first shadow Present(0, 0x200) -> 0x00000000 removed=0x00000000`.
> First debug-layer error after `host: enabled=1` (tid = Unity's submit thread, one frame later):
> `D3D12 ERROR cat=6 id=907: ID3D12CommandQueue1::ExecuteCommandLists: A command list, which writes to a swapchain back buffer, may only be executed when that back buffer is the back buffer that will be presented during the next call to Present*.`
> then `DEVICE REMOVED at Evaluate: reason 0x887A002B` = `DXGI_ERROR_ACCESS_DENIED` (the plan's `887a002b` was misread as INVALID_CALL).
> **Root cause = hypothesis (b):** the hook returned `true` and never called Unity's Present, so the flip-model chain's
> `GetCurrentBackBufferIndex()` froze (`appIdx=2` every frame) while Unity 2019.4 keeps its own index and renders the next frame
> into buffer `(i+1)%3` — a non-current back buffer, which DXGI punishes with device removal on the 2nd frame. None of (a)/(c)/(d)/(e)
> were involved: the copy list was clean (0 debug-layer errors on `Renderforge ring *` lists in every run), tearing flag `0x800` + `DXGI_PRESENT_ALLOW_TEARING` on the composition chain is accepted, DComp with a NULL device is fine.
> Bisect (env `RENDERFORGE_FG_BISECT`, removed before commit): `''` → dead ≤10 s (id=907 → 0x887A002B); `forward` (shadow Present + Unity's original Present) → alive, id=907 gone, **but** a second defect surfaced:
> `id=838 Command lists must be successfully closed before execution` + `CORRUPTION id=18 Two threads were found to be executing methods associated with the same CommandList` (render thread vs submit thread) → `RemoveDevice DXGI_ERROR_INVALID_CALL` after ~15 s. That one came from `FgHostPrepare` submitting a state-declaring `prep` list for the pass-through provider, which declares hudless/depth/mv as `NON_PIXEL_SHADER_RESOURCE` and makes Unity transition them under the upscaler's own barriers (`id=527` on `Renderforge ring 2/3`). `forward` + no prep for `FG_PROVIDER_NONE` → alive 30 s, 0 errors on our lists.
> **Fix (`native\FgHost.cpp`):** `FgHostOnPresent` now always calls `FgOriginalPresent(app, 0, flags & DXGI_PRESENT_ALLOW_TEARING)` after the shadow Present (sync 0 → no vblank wait; the shadow Present carries the pacing, the topmost DComp visual hides Unity's chain); a failed Unity Present HRESULT wins over the shadow one. `FgHostPrepare` returns before recording when the provider is `FG_PROVIDER_NONE`. One-shot `host: first present …` / `host: first shadow Present …` log lines kept as the cheap diagnostic.
> Gate (debug layer ON, so fps is the debug-layer fps): before `fg=off provider=- enabled=0 … presented=0 fps=0 frameId=0`;
> `SetFrameGen ["X2"]` → `frameGen=X2 off …` (pre-arm snapshot); +5 s `fg=live provider=none enabled=1 multiplier=2 shadow=000001A3C9991AC0 out=1280x720 flags=0x800 caps=0x1 lastError=0 presentHr=0x00000000 presented=519 fps=104 frameId=1083`;
> +10 s `… presentHr=0x00000000 presented=1038 fps=102 frameId=1602`; +60 s alive, `… presented=6209 fps=101 frameId=6773`;
> `SetFrameGen ["Off"]` → `frameGen=Off off provider=- enabled=0 multiplier=2 shadow=0000000000000000 … presented=6358 fps=106 frameId=6921`, process alive; `SetFrameGen ["X2"]` again → `frameGen=X2 live provider=none enabled=1 … shadow=000001A3C9991AC0 …`, +5 s `presented=7217 fps=105 frameId=7781`, +10 s `presented=7751 fps=106 frameId=8315`, alive at end.
> Debug layer over the whole run: 0 errors on `Renderforge ring *` lists, 0 back-buffer / device-removed / corruption messages; the remaining `id=527/538/1315` are Unity's own `Unnamed` lists (present with FG off too).
> Shots: `docs\shots\fg-null3-on.png` (`connect screenshot`, Unity's read-back) and `docs\shots\fg-null3-on-desktop.png` (`PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT)` of the `UnityWndClass` HWND = the DWM-composed output incl. the DComp visual; `CopyFromScreen` was useless because `SetForegroundWindow` is refused from a background shell) — both show the live scene, HUD, overlay `FG: none 2x`, `FPS: 118 / 120`. The TFTV error dialog in both is TFTV's own, raised by the `start-mission` plan, unrelated to Renderforge.
> `renderforge_fg.log` of the run: `host: composition chain … flags 0x800 (tearing supported 1)` / `host: enabled=1` / `host: first present: appIdx=0 shadowIdx=0 … sync=0 flags=0x200` / `host: first shadow Present(0, 0x200) -> 0x00000000 removed=0x00000000` / `host: enabled=0` / `host: teardown (shutdown)` and the same block again for the re-enable. Steps 14-16 ticked; ModConfig `FrameGen` reset to 0.

- [x] **Step 15: Turn it off again and confirm a clean teardown**

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["Off"]}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf\\fg-off-again.png"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
Get-Process PhoenixPointWin64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq 'D:\PP-Instance2\PhoenixPointWin64.exe' } | Stop-Process
```
Expected: `fg=off provider=- enabled=0`, the screenshot identical to `fg-off.png`, the process still alive and responsive.

- [x] **Step 16: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(fg): Fg_* ABI, shadow-swapchain host, managed FrameGen driver, presented-fps overlay"
```

### Task 2b: child-HWND shadow chain (2026-09-02)

Goal: an HWND of our own covering the game's client area, so every vendor FG SDK (FSR proxy, XeSS-FG, Streamline DLSS-G — all of them `CreateSwapChainForHwnd` on an HWND they are handed) has a window to build its swapchain on. Composition (4a) stays as the fallback.

> **2026-09-02 result on `D:\PP-Instance3` (profile `...593`, `-mods -force-d3d12 -force-d3d12-debug`, `RENDERFORGE_D3D12_DEBUG=1`, `ALN_PLT_Nest_48x48_A` seed 12345, 1280x720): PASS.**
> Built: `native\FgWnd.cpp` — `FgWndCreate` subclasses Unity's `UnityWndClass` HWND once for the process lifetime (`SetWindowLongPtrW(GWLP_WNDPROC)`, `FgWnd.cpp:98`; never restored, same policy as the vtable patch — restoring under a later subclass would cut its chain) and sends `WM_APP+0x51` so the child is created ON the window's thread (`ParentProc` `:45`; log `wnd: subclassed … from tid 36960, window tid 36960` = Unity's main thread is the window thread, so the send is a direct call). Child class `RenderforgeFgWnd` (`CS_OWNDC`, no brush, `WS_CHILD|WS_VISIBLE`, `WS_EX_NOPARENTNOTIFY`, parent gets `WS_CLIPCHILDREN`), `ChildProc` `:25`: `WM_NCHITTEST→HTTRANSPARENT`, `WM_ERASEBKGND→1`, `WM_PAINT→ValidateRect`, `WM_SETFOCUS→SetFocus(parent)`, `WM_MOUSEACTIVATE→MA_NOACTIVATE`; parent `WM_SIZE`/`WM_WINDOWPOSCHANGED` refit the child to the client rect (`FitChild` `:38`). Teardown from any thread = `ShowWindowAsync(SW_HIDE)` + posted `WM_APP+0x52` carrying the exact HWND (`FgWndDestroy` `:114`), so a re-enable in the same tick never races the destroy. Chain: `FgHost.cpp` `CreateChildChain` (`:163`) = `IDXGIFactory2::CreateSwapChainForHwnd(queue, child, FLIP_DISCARD, app format/size, ALLOW_TEARING)` + `MakeWindowAssociation(child, NO_ALT_ENTER|NO_WINDOW_CHANGES)`, tried FIRST in `FgHostCreateShadowSwapChain`; `RENDERFORGE_FG_CHAIN=composition` skips it. Status gained `chain=child|comp child=<hwnd> childHr=<HRESULT> hit=<WM_NCHITTEST at client centre> focus=game|child|other|none fg=<foreground==game>`, all sampled on the UI thread through `WM_APP+0x53` (`FgWndProbeNow`).
> Gate: before `fg=off provider=- enabled=0 multiplier=0 chain=- child=0000000000000000 childHr=0x00000000 hit=0 focus=none fg=1 …`; `SetFrameGen ["X2"]` (provider None) +5 s → `fg=live provider=none enabled=1 multiplier=2 chain=child child=0000000000A10B4E childHr=0x00000000 hit=-1 focus=game fg=0 shadow=000001E13D454810 out=1280x720 flags=0x800 caps=0x1 lastError=0 presentHr=0x00000000 presented=479 fps=95 frameId=1105`; +10 s `… hit=-1 focus=game fg=1 … presented=986 fps=100 frameId=1612`; +60 s alive `… presented=5530 fps=89 frameId=6155` (`focus=none fg=0` there = the game had lost foreground to the shell; a background thread's `GetFocus()` is NULL by design — whenever the game WAS foreground, focus=game, never child). `hit=-1` = `HTTRANSPARENT` on every live sample. `renderforge_fg.log`: `host: child chain 000001E13D454810 on child hwnd 0000000000A10B4E (parent 0000000000D008E6), flags 0x800` / `host: first shadow Present(0, 0x200) -> 0x00000000 removed=0x00000000`.
> `SetFgProvider ["Fsr"]` + X2 → `frameGen=X2 live provider=fsr … chain=child child=00000000003F0DF4 childHr=0x00000000 hit=-1`, +8 s `presented=6657 fps=113 frameId=6829`; Off → alive, `fg=off … chain=- child=0000000000000000`; X2 again → live on a fresh child `0000000000351044`. **Resize** via `MoveWindow` on the game HWND (868x517 → 968x577 outer): `host: teardown (resize 1430x810)` → rebuilt on child `0000000000361044`, `… chain=child … out=1430x810 … presentHr=0x00000000 presented=8488 fps=108`, alive; back to 1280x720 → rebuilt again, live. A second `start-mission` plan with FG live: `ok:true`, status live afterwards on child `00000000002D0CE4` (game reloads → new chain, no stall). 0 crashes over the whole run.
> Desktop captures (`PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT)` on the top-level game HWND, 853x480 DPI-scaled): `docs\shots\fg-child-on-desktop.png` shows the live scene + HUD + overlay `FG: none 2x`, `FPS: 76 / 76`; `docs\shots\fg-child-fsr-desktop.png` the same scene with `FG: FSR 2x`, `FPS: 56 / 113`; `docs\shots\fg-child-resized-desktop.png` (953x540) `Mode: DLAA (1430x810)`, `FG: FSR 2x`, `FPS: 55 / 108` — the child chain is what the desktop shows. `docs\shots\fg-child-on.png` = Unity's own read-back (1280x720). The TFTV error dialog is TFTV's, raised by the `start-mission` plan.
> Debug layer, whole run: 0 errors on `Renderforge ring *` lists, 0 back-buffer / `REMOVED` / `CORRUPTION` messages; remaining `id=527 x1167`, `id=538 x1089`, `id=1315 x114` are Unity's own `Unnamed` lists (present with FG off too, see Task 3). Input: no click possible from the harness; `hit=HTTRANSPARENT` from the child's own WndProc on the UI thread + `focus=game` while foreground is the evidence that mouse goes to Unity and keyboard focus never moves. ModConfig `FrameGen` reset to 0, Instance3 stopped.
> Consequence: all three vendor swapchains are now creatable — FSR's proxy `CreateSwapChainForHwnd`/`ForHwnd`, XeSS-FG's `xefgSwapChainD3D12InitFromSwapChainDesc`-style HWND init, and Streamline's `CreateSwapChainForHwnd` hook can all be pointed at `FgWnd`'s child. Not done here (spike only).

---
### Task 3: FSR-FG provider (FidelityFX SDK 2.3, analytical 3.1.6, 2x)

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\native\FgFsr.cpp`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\CMakeLists.txt`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\build-native.ps1`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\deploy.ps1`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\FrameGen.cs`

FSR-FG is the cross-vendor provider and the one that can be fully tested on the RTX 5070 Ti in this machine (through the analytical 3.1.6 model — the ML 4.0.1 model needs RDNA4 and stays untestable here). It ships **2x only**: `numGeneratedFrames = 1`, matching the SDK's own sample.

- [x] **Step 1: Write `native\FgFsr.cpp`** (shipped shape differs from the snippet below — see the Step 7 result: no FSR swapchain context, manual dispatch on the host's composition chain)

```cpp
// FgFsr.cpp - AMD FidelityFX frame generation. Two ffx-api contexts:
//   * the frame-interpolation SWAPCHAIN context, created with
//     ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 - this IS the shadow swapchain the host presents;
//   * the frame-GENERATION context, which owns the interpolation itself.
// Every entry point is resolved at runtime from amd_fidelityfx_loader_dx12.dll in the mod folder, so a player
// without the AMD zip simply gets a greyed picker instead of a failed process start.
//
// Model: we pin the ANALYTICAL 3.1.6 provider. The ML 4.0.1 model in the same DLL requires RDNA4
// (docs/techniques/frame-interpolation-ml.md:75) and would silently be selected by the loader otherwise.
#include "Fg.h"
#include "RenderforgeNative.h"

#include <string.h>
#include <stdio.h>

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_framegeneration.h"
#include "dx12/ffx_api_framegeneration_dx12.h"

namespace {

struct ProviderFsr : IFgProvider
{
    HMODULE          dll;
    ffxFunctions     fn;
    ffxContext       swapCtx;      // FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN
    ffxContext       fgCtx;        // FFX_API_EFFECT_ID_FRAMEGENERATION
    IDXGISwapChain4* sc;
    ID3D12Device*    device;
    unsigned         outW, outH, renderW, renderH;
    unsigned         backBufferFormat;
    int              enabled;
    int              lastRc;
    unsigned long long frameId;

    ProviderFsr() { memset(this, 0, sizeof(*this)); }

    int Id() const { return FG_PROVIDER_FSR; }
    unsigned Caps() const { return FG_CAP_2X; }        // one interpolated frame, per the SDK sample
    const char* Name() const { return "FSR-FG 3.1.6"; }

    // Pick the analytical model: enumerate the FG provider versions and override to the one named "3.1".
    uint64_t PickAnalyticalVersion(ID3D12Device* dev)
    {
        uint64_t count = 0;
        ffxQueryDescGetVersions q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        q.device = dev;
        q.outputCount = &count;
        if (fn.Query(NULL, &q.header) != FFX_API_RETURN_OK || count == 0) { FgLog("fsr: version query failed"); return 0; }
        if (count > 8) count = 8;
        uint64_t ids[8] = {};
        const char* names[8] = {};
        q.versionIds = ids;
        q.versionNames = names;
        if (fn.Query(NULL, &q.header) != FFX_API_RETURN_OK) { FgLog("fsr: version query 2 failed"); return 0; }
        uint64_t pick = 0;
        for (uint64_t i = 0; i < count; ++i) {
            FgLog("fsr: FG version %llu = %s", (unsigned long long)ids[i], names[i] ? names[i] : "?");
            if (names[i] && strstr(names[i], "3.1") && pick == 0) pick = ids[i];
        }
        if (!pick) FgLog("fsr: no 3.1 model found, letting the loader choose");
        return pick;
    }

    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\amd_fidelityfx_loader_dx12.dll", s.dllDir);
        dll = LoadLibraryW(path);
        if (!dll) { FgLog("fsr: amd_fidelityfx_loader_dx12.dll not found in the mod folder"); return FG_ERR_NO_PROVIDER; }
        ffxLoadFunctions(&fn, dll);
        if (!fn.CreateContext || !fn.DestroyContext || !fn.Configure || !fn.Query || !fn.Dispatch) {
            FgLog("fsr: loader DLL is missing an ffx entry point"); return FG_ERR_NO_PROVIDER;
        }

        device = s.device;
        outW = s.desc.Width; outH = s.desc.Height;
        renderW = s.desc.Width; renderH = s.desc.Height;      // replaced by the first Prepare
        backBufferFormat = ffxApiGetSurfaceFormatDX12(s.desc.Format);

        // ---- 1. the frame-interpolation swapchain (this is what the host presents)
        DXGI_SWAP_CHAIN_DESC1 d = s.desc;
        IDXGISwapChain4* chain = NULL;
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 scVer = {};
        scVer.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        scVer.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 scDesc = {};
        scDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        scDesc.header.pNext = &scVer.header;
        scDesc.swapchain = &chain;
        scDesc.hwnd = s.hwnd;
        scDesc.desc = &d;
        scDesc.fullscreenDesc = NULL;
        scDesc.dxgiFactory = s.factory;
        scDesc.gameQueue = s.queue;
        ffxReturnCode_t rc = fn.CreateContext(&swapCtx, &scDesc.header, NULL);
        if (rc != FFX_API_RETURN_OK || !chain) { FgLog("fsr: swapchain context %u", rc); return FG_ERR_PROVIDER_FAILED; }
        sc = chain;
        *out = chain;

        // ---- 2. the frame-generation context
        ffxCreateContextDescFrameGeneration fg = {};
        fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;   // Unity D3D12 uses a reversed-Z buffer
        fg.displaySize.width = outW; fg.displaySize.height = outH;
        fg.maxRenderSize.width = outW; fg.maxRenderSize.height = outH;
        fg.backBufferFormat = backBufferFormat;

        ffxCreateContextDescFrameGenerationVersion fgVer = {};
        fgVer.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        fgVer.version = FFX_FRAMEGENERATION_VERSION;

        ffxCreateBackendDX12Desc backend = {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = s.device;

        ffxOverrideVersion over = {};
        over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        over.versionId = PickAnalyticalVersion(s.device);

        fg.header.pNext = &fgVer.header;
        fgVer.header.pNext = &backend.header;
        backend.header.pNext = over.versionId ? &over.header : NULL;

        rc = fn.CreateContext(&fgCtx, &fg.header, NULL);
        if (rc != FFX_API_RETURN_OK) {
            FgLog("fsr: FG context %u", rc);
            fn.DestroyContext(&swapCtx, NULL);
            return FG_ERR_PROVIDER_FAILED;
        }
        FgLog("fsr: created, display %ux%u fmt %u versionId %llu", outW, outH, backBufferFormat,
              (unsigned long long)over.versionId);
        return FG_OK;
    }

    // Configure + the prepare dispatch, on Unity's own command list inside DLSS_EV_FG_PREPARE.
    void Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f)
    {
        if (!fgCtx) return;
        renderW = f.renderW; renderH = f.renderH;
        frameId = f.frameId;

        ffxConfigureDescFrameGeneration cfg = {};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = sc;
        cfg.frameGenerationEnabled = enabled != 0;
        cfg.allowAsyncWorkloads = false;
        cfg.HUDLessColor = ffxApiGetResourceDX12(f.hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        cfg.flags = 0;
        cfg.onlyPresentGenerated = false;
        cfg.generationRect.left = 0; cfg.generationRect.top = 0;
        cfg.generationRect.width = (int32_t)f.outW; cfg.generationRect.height = (int32_t)f.outH;
        cfg.frameID = f.frameId;
        lastRc = (int)fn.Configure(&fgCtx, &cfg.header);
        if (lastRc != FFX_API_RETURN_OK) { FgLog("fsr: configure %d", lastRc); return; }

        ffxDispatchDescFrameGenerationPrepareV2 pr = {};
        pr.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        pr.frameID = f.frameId;
        pr.flags = 0;
        pr.commandList = list;
        pr.renderSize.width = f.renderW; pr.renderSize.height = f.renderH;
        pr.jitterOffset.x = f.jitterX; pr.jitterOffset.y = f.jitterY;
        pr.motionVectorScale.x = f.mvScaleX; pr.motionVectorScale.y = f.mvScaleY;
        pr.frameTimeDelta = f.dtMs;
        pr.reset = f.reset != 0;
        pr.cameraNear = f.cameraNear;
        pr.cameraFar = f.cameraFar;
        pr.cameraFovAngleVertical = f.cameraFovY;
        pr.viewSpaceToMetersFactor = 1.0f;                 // Unity world units are metres
        pr.depth = ffxApiGetResourceDX12(f.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        pr.motionVectors = ffxApiGetResourceDX12(f.mv, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        memcpy(pr.cameraPosition, f.camPos, sizeof(pr.cameraPosition));
        memcpy(pr.cameraUp, f.camUp, sizeof(pr.cameraUp));
        memcpy(pr.cameraRight, f.camRight, sizeof(pr.cameraRight));
        memcpy(pr.cameraForward, f.camFwd, sizeof(pr.cameraForward));
        lastRc = (int)fn.Dispatch(&fgCtx, &pr.header);
        if (lastRc != FFX_API_RETURN_OK) FgLog("fsr: prepare %d", lastRc);
    }

    // The composed frame is already in the swapchain's current backbuffer (the host copied it there).
    // Ask the swapchain context for its interpolation command list + target and generate the in-between frame.
    void BeforePresent(const FgFrame& f)
    {
        if (!fgCtx || !enabled) return;

        void* interpList = NULL;
        ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12 qList = {};
        qList.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12;
        qList.pOutCommandList = &interpList;
        if (fn.Query(&swapCtx, &qList.header) != FFX_API_RETURN_OK || !interpList) return;

        FfxApiResource interpTex = {};
        ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12 qTex = {};
        qTex.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12;
        qTex.pOutTexture = &interpTex;
        if (fn.Query(&swapCtx, &qTex.header) != FFX_API_RETURN_OK) return;

        ID3D12Resource* back = NULL;
        if (FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&back))) return;

        ffxDispatchDescFrameGeneration dg = {};
        dg.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        dg.commandList = interpList;
        dg.presentColor = ffxApiGetResourceDX12(back, FFX_API_RESOURCE_STATE_PRESENT);
        dg.outputs[0] = interpTex;
        dg.numGeneratedFrames = 1;
        dg.reset = f.reset != 0;
        dg.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dg.minMaxLuminance[0] = 0.0f;
        dg.minMaxLuminance[1] = 300.0f;
        dg.generationRect.left = 0; dg.generationRect.top = 0;
        dg.generationRect.width = (int32_t)outW; dg.generationRect.height = (int32_t)outH;
        dg.frameID = f.frameId;
        lastRc = (int)fn.Dispatch(&fgCtx, &dg.header);
        back->Release();
        if (lastRc != FFX_API_RETURN_OK) FgLog("fsr: dispatch %d", lastRc);
    }

    int AfterPresent(void) { return enabled ? 2 : 1; }   // 1 real + 1 interpolated

    void SetEnabled(bool on)
    {
        enabled = on ? 1 : 0;
        if (!fgCtx) return;
        // The toggle itself is a Configure with the new flag; it drains pending GPU work internally
        // (docs/techniques/frame-interpolation-swap-chain.md:223-226).
        ffxConfigureDescFrameGeneration cfg = {};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = sc;
        cfg.frameGenerationEnabled = on;
        cfg.frameID = frameId;
        fn.Configure(&fgCtx, &cfg.header);
    }

    void Destroy(void)
    {
        if (fgCtx) {
            ffxConfigureDescFrameGeneration off = {};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = sc;
            off.frameGenerationEnabled = false;
            fn.Configure(&fgCtx, &off.header);

            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait = {};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
            fn.Dispatch(&swapCtx, &wait.header);

            fn.DestroyContext(&fgCtx, NULL);
            fgCtx = NULL;
        }
        if (swapCtx) { fn.DestroyContext(&swapCtx, NULL); swapCtx = NULL; }
        sc = NULL;
        enabled = 0;
        if (dll) { FreeLibrary(dll); dll = NULL; }
        FgLog("fsr: destroyed");
    }
};

ProviderFsr g_fsr;

} // namespace

IFgProvider* MakeFgProviderFsr(void) { return &g_fsr; }
```

> Every `FFX_API_..._DX12` macro above is verbatim from `Kits\FidelityFX\framegeneration\include\dx12\ffx_api_framegeneration_dx12.h` (`:33`, `:51`, `:71`, `:78`, `:85`, `:128`), as are `FFX_FRAMEGENERATION_VERSION` (`ffx_framegeneration.h:33`, = 4.0.1, the header/API handshake — the *model* is chosen separately by `ffxOverrideVersion`) and `FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION` (`ffx_api_framegeneration_dx12.h:31`, = 3.1.7).

- [x] **Step 2: `native\CMakeLists.txt` — the FidelityFX headers** (`FFX_FG_INC` added next to the existing `FFX_API_INC`/`FFX_UPSCALE_INC`)

Insert after the `NGX_LIB` check:

```cmake
set(FFX_SDK "${CMAKE_SOURCE_DIR}/../../refs/FidelityFX-SDK/Kits/FidelityFX" CACHE PATH "AMD FidelityFX SDK kit root")
get_filename_component(FFX_SDK "${FFX_SDK}" ABSOLUTE)
if(NOT EXISTS "${FFX_SDK}/api/include/ffx_api.h")
    message(FATAL_ERROR "FidelityFX api headers not found at ${FFX_SDK}/api/include")
endif()
```

add `FgFsr.cpp` to the source list, and extend the include directories:

```cmake
target_include_directories(RenderforgeNative PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${DLSS_SDK}/include" "${CMAKE_CURRENT_BINARY_DIR}"
            "${FFX_SDK}/api/include" "${FFX_SDK}/framegeneration/include")
```

- [x] **Step 3: `build-native.ps1` — verify and stage the AMD DLLs** (`amd_fidelityfx_framegeneration_dx12.dll` joined the existing `$amdDlls` list; loader + upscaler were already there)

Insert after the existing `nvngx_dlss.dll` signature block:

```powershell
# Vendor runtime DLLs: every one is Authenticode-verified before it is staged, so a tampered or repacked
# binary can never reach a release zip. Same rule as nvngx_dlss.dll above.
$ffx = Join-Path $root '..\refs\FidelityFX-SDK\Kits\FidelityFX\signedbin'
$vendorDlls = @(
    @{ Path = (Join-Path $ffx 'amd_fidelityfx_loader_dx12.dll');         Signer = 'Advanced Micro Devices' },
    @{ Path = (Join-Path $ffx 'amd_fidelityfx_framegeneration_dx12.dll'); Signer = 'Advanced Micro Devices' }
)
foreach ($v in $vendorDlls) {
    if (-not (Test-Path $v.Path)) { throw "vendor DLL not found: $($v.Path)" }
    $s = Get-AuthenticodeSignature $v.Path
    if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch $v.Signer) {
        throw "$(Split-Path $v.Path -Leaf) signature invalid (status $($s.Status), signer $($s.SignerCertificate.Subject))"
    }
    Write-Host ("{0} {1} signed by {2}" -f (Split-Path $v.Path -Leaf), (Get-Item $v.Path).VersionInfo.FileVersion, $v.Signer)
}
```

and after the existing `Copy-Item $ngxDll $outDir -Force`:

```powershell
foreach ($v in $vendorDlls) { Copy-Item $v.Path $outDir -Force }
```

- [x] **Step 4: `deploy.ps1` — copy the vendor DLLs into the mod folder** (`$amdFrameGen` added to the explicit list)

Replace the `foreach ($file in ...)` block with:

```powershell
$vendor = Get-ChildItem (Join-Path $root 'build\out') -Filter '*.dll' | Where-Object { $_.Name -ne 'RenderforgeNative.dll' -and $_.Name -ne 'nvngx_dlss.dll' } | ForEach-Object { $_.FullName }
foreach ($file in @((Join-Path $out 'Renderforge.dll'), (Join-Path $root 'meta.json'), $nativeDll, $ngxDll,
                    (Join-Path $root 'LICENSE-NVIDIA.txt'), (Join-Path $root 'LICENSE-NIS.txt'), (Join-Path $root 'LICENSE'), (Join-Path $root 'README.md')) + $vendor) {
    Copy-Item $file $dest -Force
}
```

- [x] **Step 5: `src\FrameGen.cs` — let PPCLI force a provider**

Replace `AutoProvider()` with an override-aware version and add the forcing entry point:

```csharp
        private static int forced = -1;

        /// <summary>PPCLI/test override: -1 = auto, else an FG_PROVIDER_* id.</summary>
        internal static void Force(int provider) { forced = provider; Stop(); }

        /// <summary>Which vendor drives FG on this GPU: NVIDIA -> DLSS-G, everything else -> FSR-FG (cross-vendor),
        /// with XeSS-FG as the second cross-vendor option. Mirrors the spec's Auto order for D3D12.</summary>
        internal static int AutoProvider()
        {
            if (forced >= 0) return forced;
            if (Availability.IsNvidia) return Native.FG_PROVIDER_DLSS;
            return Native.FG_PROVIDER_FSR;
        }
```

and in `src\RenderforgeMod.cs`, after `SetFrameGen`:

```csharp
        /// <summary>PPCLI: {"member":"SetFgProvider","args":["Fsr"]} - Auto / None / Fsr / Xess / Dlss. Test lever:
        /// the RTX in this machine can run all three, and only a forced pick proves the cross-vendor paths.</summary>
        public static string SetFgProvider(string name)
        {
            var m = Instance;
            if (m == null) return "mod not enabled";
            int id;
            switch (name.ToLowerInvariant())
            {
                case "none": id = Native.FG_PROVIDER_NONE; break;
                case "fsr": id = Native.FG_PROVIDER_FSR; break;
                case "xess": id = Native.FG_PROVIDER_XESS; break;
                case "dlss": id = Native.FG_PROVIDER_DLSS; break;
                default: id = -1; break;
            }
            FrameGen.Force(id);
            FrameGen.Apply(m.Cfg);
            return "fgProvider=" + name + " " + FrameGen.Status();
        }
```

- [x] **Step 6: Build and deploy** (2026-09-02: `build-native.ps1` 4x `PROBE OK`, `build-native: OK`, 0 warnings, three AMD lines incl. `amd_fidelityfx_framegeneration_dx12.dll 4.0.1.2740`; `dotnet build -c Release` 0/0; `deploy.ps1 -PPRoot D:\PP-Instance3 -SkipNative` lists the 40,085,776-byte FG DLL)

```powershell
powershell -NoProfile -Command "Set-Location E:\DEV\PhoenixPoint\Renderforge; .\deploy.ps1"
```
Expected: the two AMD lines (`amd_fidelityfx_loader_dx12.dll 2.3.0.2740 signed by Advanced Micro Devices`, `amd_fidelityfx_framegeneration_dx12.dll 4.0.1.2740 signed by ...`), `build-native: OK`, and the deploy listing now includes both AMD DLLs.

- [x] **Step 7: Run FSR-FG in-game**

```powershell
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods','-force-d3d12'
Start-Sleep -Seconds 60
Set-Location E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["Quality","None"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"ToggleOverlay"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFgProvider","args":["Fsr"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["X2"]}'
Start-Sleep -Seconds 8
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf\\fg-fsr-2x.png"}'
Get-Content 'D:\PP-Instance2\Mods\Renderforge\renderforge_fg.log' -Tail 30
```
Expected: `GetStatus` contains `fg=live provider=FSR-FG 3.1.6 enabled=1 multiplier=2 caps=0x1 lastError=0`; the log shows a `fsr: FG version … = …3.1…` line, `fsr: created, display <W>x<H>`, and **no** `fsr: configure`/`fsr: prepare`/`fsr: dispatch` error lines. The screenshot must show the overlay's `FPS: <real> / <presented>` with **presented ≈ 2 × real**, `FG: FSR 2x`, and a HUD that is sharp and not smeared.

> **2026-09-02 result on `D:\PP-Instance3` (profile `...593`, `-mods -force-d3d12 -force-d3d12-debug`, `RENDERFORGE_D3D12_DEBUG=1`, `ALN_PLT_Nest_48x48_A` seed 12345, 1280x720, ModConfig `Upscaler:3`/`Renderer:2` latched so the FSR upscaler runs): PASS.**
> **Deviation from the snippet above — the SDK's frame-interpolation swapchain is NOT used.** All three of its creation shapes end in `IDXGIFactory2::CreateSwapChainForHwnd` on the game window (`FrameInterpolationSwapchainDX12.cpp:1162`, reached from `:477` Wrap, `:520` New, `:523` ForHwnd), which is the `0x80070005` this phase already measured, and `Wrap` also needs `GetHwnd` (`:468`), which a composition chain refuses. `FgFsr.cpp` therefore keeps the host's composition shadow chain and takes the manual-dispatch path (`frame-interpolation-api.md:281`): one FG context, `ffxConfigure` per frame with `FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY` (`ffx_provider_fsr3framegeneration.cpp:240-241` — "allow for run configure without swapchain"), `PrepareV2` in `DLSS_EV_FG_PREPARE`, then in the Present hook `ffxDispatchDescFrameGeneration` with `presentColor` = Unity's back buffer (state PRESENT) and `outputs[0]` = an owned UAV texture that is copied into the shadow chain and presented BEFORE the real frame. `IFgProvider::BeforePresent/AfterPresent` became `Generate(f, unityBackBuffer, shadow, sync, pf) -> frames presented`.
> Inputs are the upscaler's owned twins (`IDevice::Owned12()`, new; `FgOwned12()` in `RenderforgeNative.cpp`): depth/mv/out passed as `FFX_API_RESOURCE_STATE_COMMON` (`ffx_dx12.cpp:625`), so `FgHostPrepare` declares nothing to Unity (`prep.End(0)`); Unity RTs are never handed to ffx. Hud-less = owned `out` (same 1280x720 R8G8B8A8 as the back buffer). The FG DLL lists exactly one provider on this RTX 5070 Ti: `fsr: FG version 17726168133342859270 = 3.1.6` (no 4.0.1 entry) — pinned via `ffxOverrideVersion`, `ffxQueryGetProviderVersion` confirms `3.1.6`. `FfxLoad` pre-loads `amd_fidelityfx_framegeneration_dx12.dll` (optional) beside the upscaler DLL.
> Sequence (`SetMode ["Quality","None"]`, `SetOverlay ["TopCenter"]`, `SetFgProvider ["Fsr"]`, `SetFrameGen ["X2"]`): before `fg=off provider=- enabled=0 multiplier=0 … presented=0 fps=0 frameId=0`; +5 s `fg=live provider=fsr enabled=1 multiplier=2 shadow=00000163A10CAD10 out=1280x720 flags=0x800 caps=0x1 lastError=0 presentHr=0x00000000 presented=618 fps=140 frameId=2028`; +10 s `… presented=1274 fps=133 frameId=2357` (656 presented / 329 rendered in 5 s = 2.0x); +60 s alive `… presented=11336 fps=124 frameId=7390`. Overlay `docs\shots\fg-fsr-on.png`: `Upscaler: FSR 3.1.5`, `FG: FSR 2x`, `FPS: 63 / 124 (15,9 ms)`; desktop `docs\shots\fg-fsr-on-desktop.png` (PrintWindow `PW_CLIENTONLY|PW_RENDERFULLCONTENT` on the game HWND, 853x480 = DPI-scaled client rect) shows the live scene + HUD + `FPS: 50 / 98`, with faint white speckles over the dark ground — the generated frame at capture time, 3.1.6 artefacts in a near-black scene, not a hang.
> `SetFrameGen ["Off"]` → `frameGen=Off off provider=- enabled=0 multiplier=2 shadow=0000000000000000 … presented=25055 fps=126 frameId=14253`, alive 20 s later; `SetFrameGen ["X2"]` again → `frameGen=X2 live provider=fsr … shadow=0000016280A157B0 …`, +8 s `presented=28540 fps=132 frameId=17223`, +13 s `presented=29196 fps=132 frameId=17551`; Off again, process stopped cleanly. `renderforge_fg.log`: no `fsr: configure/prepare/dispatch/generated Present` lines; `host: first present: … generated=1 tid=17532`, `first shadow Present(0, 0x200) -> 0x00000000 removed=0x00000000`, both teardowns `fsr: destroyed (provider '3.1.6')`.
> Debug layer over the whole run (~200 s FG-on): 0 `DEVICE REMOVED`, 0 `CORRUPTION`, **5 x id=527 on `Renderforge ring N`**, every one inside a burst of Unity's own `BackBuffer*` id=527 mismatches on `Unnamed` lists (498 such Unity lines; the tracker already believed RENDER_TARGET when our PRESENT->COPY_SOURCE barrier ran — the same Unity-tracker noise D3D12Owned.h documents, present with FG off too). No fix round spent on it; no `FG interp`/AMD resource is named in any error.
> ponytail ceiling: no frame pacing — the generated and the real frame are presented back to back on the submit thread (sync 0), so the presented counter doubles but the display cadence is not smoothed; upgrade = a present thread that delays the real frame by half the average frame time (what the SDK proxy does). ModConfig reset afterwards (`FrameGen:0`, `Upscaler:1`, `Renderer:0`, `Mode:2`), Instance3 stopped.

> **2026-09-02 (later) — FSR owns the presentation on the child HWND: PASS on `D:\PP-Instance3` (profile `...593`, `-mods -force-d3d12 -force-d3d12-debug`, `RENDERFORGE_D3D12_DEBUG=1`, same scene/seed, 1280x720, ModConfig `Upscaler:3`/`Renderer:2` latched).** The ponytail ceiling above is lifted: with Task 2b's child HWND the SDK's frame-interpolation swapchain is creatable, so `FgFsr.cpp` now creates it with `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12` (`ffx_api_framegeneration_dx12.h:52`, `+ …VersionDX12` `:129` = 3.1.7) on the child from `FgHostChildHwnd(s)` (new host seam, `Fg.h`; `IFgProvider` itself is unchanged — pass-through and the XeSS stub compile untouched) with the host's `s.desc` (flags 0 — the proxy adds `WAITABLE|ALLOW_TEARING` to its real chain itself, `FrameInterpolationSwapchainDX12.cpp:1245-1249`). The proxy IS the host's shadow chain: `FgHostOnPresent` copies Unity's back buffer into `proxy.GetBuffer(current)` (a replacement buffer resting in PRESENT, `:1808`/`:2234`) and calls `proxy->Present(sync, 0)`, inside which the SDK calls our `frameGenerationCallback` (`ProviderFsr::OnGenerate`, `:1934`) to dispatch the FG context into the SDK's own interpolation list, then paces + presents both frames from its present thread. `ffxConfigure` per frame now carries `swapChain=proxy`, `frameID`, the callback, `presentCallback=NULL` (default composition copy), `HUDLessColor` = owned hud-less; `NO_SWAPCHAIN_CONTEXT_NOTIFY` is only set on the manual path, which stays as the fallback for `RENDERFORGE_FG_CHAIN=composition` (or a refused proxy). `Generate()` returns 0 on the proxy path; the presented counter = host's 1 real + `FgPresentedAdd(numGeneratedFrames)` from the callback only when the SDK's dispatch returned OK with `numGeneratedFrames>0` (never faked). Destroy = `Configure(frameGenerationEnabled=false, swapChain=proxy)` (waits for presents, `frame-interpolation-api.md:319-320`) + `WAIT_FOR_PRESENTS` + `DestroyContext(fg)` + `DestroyContext(swapchain)`; the host's `Release` is the proxy's last ref.
> Log: `fsr: proxy swapchain 000001D47A0C0770 on child hwnd 0000000000890372, swapchain provider '3.1.7'` / `fsr: FG version 17726168133342859270 = 3.1.6` / `fsr: created, display 1280x720 fmt 28, provider '3.1.6' (override …), SDK swapchain (paced)` / `host: first shadow Present(0, 0x0) -> 0x00000000 removed=0x00000000`. GetStatus +5 s `fg=live provider=fsr enabled=1 multiplier=2 chain=child child=0000000000890372 childHr=0x00000000 hit=-1 focus=game fg=0 shadow=000001D47A0C0770 out=1280x720 flags=0x0 caps=0x1 lastError=0 presentHr=0x00000000 presented=627 fps=130 frameId=321`; +10 s `… presented=1313 fps=139 frameId=664` (686 presented / 343 rendered = 2.00x). Presented deltas over 5 x ~1.04 s: **+142/+136/+120/+134/+140** (rendered +71/+69/+61/+66/+69, ratio 1.97-2.03), i.e. min 120 / max 142 per sample — the SDK's pacer, not back-to-back presents. +60 s alive `presented=11959 fps=126 frameId=5987`. `SetFrameGen ["Off"]` → `fg=off … chain=- child=0000000000000000`, alive 10 s; `X2` again → live on child `00000000019E0EFC`, `presented=15156 fps=134`. `MoveWindow` +100x60 → `host: teardown (resize 1430x810)` → live `out=1430x810 … presented=18410 fps=129`; back → live `out=1280x720 … fps=129`. Each teardown logs `fsr: destroyed (provider '3.1.6', swapchain '3.1.7', generated N)` (6636 / 1635 / 526 / 1686). Desktop `docs\shots\fg-fsr-swapchain-desktop.png` (PrintWindow, 853x480): live scene + HUD, overlay `FG: FSR 2x`, `FPS: 71 / 143 (14,1 ms)` (the TFTV dialog is TFTV's own, from the `start-mission` plan). Debug layer over the whole run (4522 lines): **0 `REMOVED`, 0 `CORRUPTION`, 0 messages naming `Renderforge ring`/`AMD FSR`/`FG interp`**; the rest is Unity's `Unnamed` id=527 x1939 / id=538 x1851 / id=1315 x100 (present with FG off too). ModConfig restored (`FrameGen:0`, `Upscaler:1`, `Renderer:0`), Instance3 stopped.

- [x] **Step 8: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(fg): FSR frame generation provider (FidelityFX 2.3, analytical 3.1.6, hudless UI mode)"
```

---
### Task 4: XeSS-FG provider (XeSS 3 SDK, XeLL mandatory, 2x off Arc)

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\native\FgXess.cpp`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\CMakeLists.txt`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\build-native.ps1`

XeSS-FG only ever creates its own swapchain for us: `xefgSwapChainD3D12InitFromSwapChain` demands the application's swapchain have refcount 1 and then **destroys** it (`xess_fg_developer_guide_english.md:270-276`), which Unity's live reference makes impossible. So we go through `xefgSwapChainD3D12InitFromSwapChainDesc` with `pApplicationSwapChain = NULL` — the same shadow shape the host already expects. **XeLL is not optional**: without a XeLL context the proxy swapchain refuses to initialise (`:229-231`).

> **2026-09-02 result: SDK-BLOCKED — shipped as a stub, no XeSS-FG provider, XeLL not touched.** The snippet below was written for the plan's `CreateSwapChainForHwnd` shadow chain; Task 1 measured that call as `E_ACCESSDENIED` (`0x80070005`) on the game window (`FgHost.cpp:7-9`) and the host became a `CreateSwapChainForComposition` + DComp chain, which FSR-FG drives by manual `ffxDispatch` (Task 3). XeSS-FG has no equivalent: its ONLY output path is the proxy `IDXGISwapChain` the library creates itself, always on an HWND — `xefgSwapChainD3D12InitFromSwapChainDesc` (`xefg_swapchain_d3d12.h:216`, `:212` "a swap chain will be created according to `hWnd`, `pSwapChainDesc`", no composition overload) and `InitFromSwapChain` (`:190`, guide `:275` "Creates a new swap chain for the underlying window"). The complete API is `xefg_swapchain.h:334-512` + `xefg_swapchain_d3d12.h:120-320` + `xefg_swapchain_debug.h:56` (24 functions, matching `doc\xess_frame_generation_doxygen\globals_func.html:96-119`); `dumpbin /exports bin\libxess_fg.dll` (1.3.1.78) = 31 entries: those 24 + `xefgAIL{GetDecision,GetVersion,SetAppXeFGVersion}` + undocumented `xefgSwapChain{D3D12GetProfilingData,D3D12SetDiagnosticCallbacks,GetParameterP,SetParameterP}`. No dispatch/execute/output-texture/present-callback entry exists; `xefg_swapchain.h:321-324` calls this "utils API" over an "underlying XeSS-FG API" the SDK does not ship. Hacking a second HWND was ruled out by the brief.
> Shipped: `native\FgXess.cpp` (`MakeFgProviderXess` → `NULL`; `FgXessBlockedReason(dllDir)` probes `<modDir>\libxess_fg.dll` once via `LoadLibraryW` + `GetProcAddress("xefgSwapChainGetVersion")`, never linked); `FgHost.cpp` returns `FG_ERR_NO_PROVIDER` for `FG_PROVIDER_XESS` (no pass-through fallback) and appends ` reason=XeSS-FG 1.3.1 needs its own HWND swapchain - unavailable in-process` to `Fg_Status`; `Pickers.FrameGenReason` greys the row with the same EN/RU text whenever the (forced) provider is XeSS. `build-native.ps1` verifies (`libxess_fg.dll 1.3.1.78 signed by Intel Corporation`) + stages, `deploy.ps1` copies; `libxell.dll`/`libxell.lib`/`libxess_fg.lib` are NOT staged or linked. Build: 4x `PROBE OK`, `build-native: OK`, 0 warnings; `dotnet build -c Release` 0/0. Steps 5-6 (in-game) not run — nothing to measure. Revisit when Intel ships a non-swapchain XeSS-FG entry point.

- [x] **Step 1: Write `native\FgXess.cpp`** (superseded: SDK-blocked stub, see the result note above — the snippet below is NOT what shipped)

```cpp
// FgXess.cpp - Intel XeSS Frame Generation. The FG proxy swapchain is created from a DESC (not from Unity's
// swapchain: InitFromSwapChain requires refcount 1 and releases the original, which Unity still holds).
// XeLL is mandatory - the FG context refuses to initialise without one - so we create it first, bind it with
// xefgSwapChainSetLatencyReduction and issue its six required markers around our own frame.
//
// UI: mode 4 (BACKBUFFER_HUDLESS) - interpolate on the hud-less colour, recover the UI by differencing
// against the composed backbuffer. That is exactly our present path and needs no UI texture
// (xess_fg_developer_guide_english.md:623).
#include "Fg.h"
#include "RenderforgeNative.h"

#include <string.h>
#include <stdio.h>

#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"
#include "xell/xell.h"
#include "xell/xell_d3d12.h"

namespace {

void XessLog(const char* msg, xefg_swapchain_logging_level_t lvl, void*)
{
    FgLog("xess[%d]: %s", (int)lvl, msg ? msg : "");
}

struct ProviderXess : IFgProvider
{
    HMODULE                 dllFg;
    HMODULE                 dllLl;
    xefg_swapchain_handle_t fg;
    xell_context_handle_t   ll;
    IDXGISwapChain4*        sc;
    unsigned                caps;
    unsigned                outW, outH;
    unsigned                maxInterp;
    int                     enabled;
    int                     lastRc;
    unsigned                presentId;
    unsigned                lastPresented;

    ProviderXess() { memset(this, 0, sizeof(*this)); }

    int Id() const { return FG_PROVIDER_XESS; }
    unsigned Caps() const { return caps; }
    const char* Name() const { return "XeSS-FG"; }

    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        wchar_t p1[MAX_PATH], p2[MAX_PATH];
        _snwprintf_s(p1, MAX_PATH, _TRUNCATE, L"%s\\libxell.dll", s.dllDir);
        _snwprintf_s(p2, MAX_PATH, _TRUNCATE, L"%s\\libxess_fg.dll", s.dllDir);
        dllLl = LoadLibraryW(p1);
        dllFg = LoadLibraryW(p2);
        if (!dllLl || !dllFg) { FgLog("xess: libxell.dll / libxess_fg.dll missing from the mod folder"); return FG_ERR_NO_PROVIDER; }

        xell_result_t lr = xellD3D12CreateContext(s.device, &ll);
        if (lr != XELL_RESULT_SUCCESS) { FgLog("xess: xellD3D12CreateContext %d", (int)lr); return FG_ERR_PROVIDER_FAILED; }

        xefg_swapchain_result_t r = xefgSwapChainD3D12CreateContext(s.device, &fg);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { FgLog("xess: CreateContext %d", (int)r); return FG_ERR_PROVIDER_FAILED; }
        xefgSwapChainSetLoggingCallback(fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, &XessLog, NULL);

        r = xefgSwapChainSetLatencyReduction(fg, ll);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { FgLog("xess: SetLatencyReduction %d", (int)r); return FG_ERR_PROVIDER_FAILED; }

        outW = s.desc.Width; outH = s.desc.Height;

        xefg_swapchain_d3d12_init_params_t ip = {};
        ip.pApplicationSwapChain = NULL;                  // we create the chain, not wrap Unity's
        ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;   // Unity D3D12 uses reversed Z
        ip.maxInterpolatedFrames = (s.multiplier > 1 ? s.multiplier - 1 : 1);
        ip.creationNodeMask = 1;
        ip.visibleNodeMask = 1;
        ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;

        xefg_swapchain_properties_t props = {};
        r = xefgSwapChainD3D12GetProperties(fg, &ip, outW, outH, s.desc.Format, &props);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { FgLog("xess: GetProperties %d", (int)r); return FG_ERR_PROVIDER_FAILED; }
        maxInterp = props.maxSupportedInterpolations;
        if (maxInterp < 1) maxInterp = 1;
        if (ip.maxInterpolatedFrames > maxInterp) ip.maxInterpolatedFrames = maxInterp;
        caps = FG_CAP_2X | (maxInterp >= 2 ? FG_CAP_3X : 0u) | (maxInterp >= 3 ? FG_CAP_4X : 0u);
        FgLog("xess: maxSupportedInterpolations=%u -> caps 0x%X (1 on every non-Intel GPU)", maxInterp, caps);
        if (s.multiplier - 1 > maxInterp) { FgLog("xess: %ux asked, %ux supported", s.multiplier, maxInterp + 1); return FG_ERR_UNSUPPORTED_MULTIPLIER; }

        DXGI_SWAP_CHAIN_DESC1 d = s.desc;
        r = xefgSwapChainD3D12InitFromSwapChainDesc(fg, s.hwnd, &d, NULL, s.queue, s.factory, &ip);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { FgLog("xess: InitFromSwapChainDesc %d", (int)r); return FG_ERR_PROVIDER_FAILED; }

        r = xefgSwapChainD3D12GetSwapChainPtr(fg, __uuidof(IDXGISwapChain4), (void**)out);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !*out) { FgLog("xess: GetSwapChainPtr %d", (int)r); return FG_ERR_NO_SWAPCHAIN; }
        sc = *out;

        xefgSwapChainSetNumInterpolatedFrames(fg, ip.maxInterpolatedFrames);
        xefgSwapChainSetUiCompositionState(fg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);

        xell_sleep_params_t sp = {};
        sp.minimumIntervalUs = 0;       // no fps cap from XeLL; the mod's own frame-rate limit still applies
        sp.bLowLatencyMode = 1;
        xellSetSleepMode(ll, &sp);

        FgLog("xess: created %ux%u interpolated=%u", outW, outH, ip.maxInterpolatedFrames);
        return FG_OK;
    }

    void Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f)
    {
        if (!fg) return;
        presentId = (unsigned)f.frameId;

        // XeLL sleep + the four pre-present markers. Unity's real simulation boundaries are not reachable from
        // the shim, so SIMULATION_* bracket the render event; the pacing quality that costs is small compared
        // with not issuing the required markers at all (the guide marks all six required).
        xellSleep(ll, presentId);
        xellAddMarkerData(ll, presentId, XELL_SIMULATION_START);
        xellAddMarkerData(ll, presentId, XELL_SIMULATION_END);
        xellAddMarkerData(ll, presentId, XELL_RENDERSUBMIT_START);

        xefg_swapchain_d3d12_resource_data_t rd = {};
        rd.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        rd.resourceBase.x = 0; rd.resourceBase.y = 0;
        rd.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        rd.pResource = f.mv;
        rd.resourceSize.x = f.renderW; rd.resourceSize.y = f.renderH;
        lastRc = (int)xefgSwapChainD3D12TagFrameResource(fg, list, presentId, &rd);
        if (lastRc != XEFG_SWAPCHAIN_RESULT_SUCCESS) FgLog("xess: tag MV %d", lastRc);

        rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
        rd.pResource = f.depth;
        lastRc = (int)xefgSwapChainD3D12TagFrameResource(fg, list, presentId, &rd);
        if (lastRc != XEFG_SWAPCHAIN_RESULT_SUCCESS) FgLog("xess: tag depth %d", lastRc);

        rd.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;   // read again when the interpolated frame is built
        rd.pResource = f.hudless;
        rd.resourceSize.x = f.outW; rd.resourceSize.y = f.outH;
        lastRc = (int)xefgSwapChainD3D12TagFrameResource(fg, list, presentId, &rd);
        if (lastRc != XEFG_SWAPCHAIN_RESULT_SUCCESS) FgLog("xess: tag hudless %d", lastRc);

        xefg_swapchain_frame_constant_data_t c = {};
        memcpy(c.viewMatrix, f.view, sizeof(c.viewMatrix));
        memcpy(c.projectionMatrix, f.proj, sizeof(c.projectionMatrix));
        c.jitterOffsetX = f.jitterX;
        c.jitterOffsetY = f.jitterY;
        c.motionVectorScaleX = f.mvScaleX;
        c.motionVectorScaleY = f.mvScaleY;
        c.resetHistory = (unsigned)f.reset;
        c.frameRenderTime = f.dtMs;
        lastRc = (int)xefgSwapChainTagFrameConstants(fg, presentId, &c);
        if (lastRc != XEFG_SWAPCHAIN_RESULT_SUCCESS) FgLog("xess: tag constants %d", lastRc);
    }

    void BeforePresent(const FgFrame&)
    {
        if (!fg) return;
        xefgSwapChainSetPresentId(fg, presentId);
        xellAddMarkerData(ll, presentId, XELL_RENDERSUBMIT_END);
        xellAddMarkerData(ll, presentId, XELL_PRESENT_START);
    }

    int AfterPresent(void)
    {
        if (!fg) return 1;
        xellAddMarkerData(ll, presentId, XELL_PRESENT_END);
        xefg_swapchain_present_status_t st = {};
        if (xefgSwapChainGetLastPresentStatus(fg, &st) == XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            lastPresented = st.framesPresented;
            if (st.frameGenResult < 0) FgLog("xess: frameGenResult %d", (int)st.frameGenResult);
            return st.framesPresented > 0 ? (int)st.framesPresented : 1;
        }
        return 1;
    }

    void SetEnabled(bool on)
    {
        enabled = on ? 1 : 0;
        if (fg) xefgSwapChainSetEnabled(fg, on ? 1u : 0u);
    }

    void Destroy(void)
    {
        // Order from the guide (:1080): drop the proxy references, destroy FG, then destroy XeLL.
        if (fg) { xefgSwapChainSetEnabled(fg, 0); }
        sc = NULL;
        if (fg) { xefgSwapChainDestroy(fg); fg = NULL; }
        if (ll) { xellDestroyContext(ll); ll = NULL; }
        enabled = 0; caps = 0;
        if (dllFg) { FreeLibrary(dllFg); dllFg = NULL; }
        if (dllLl) { FreeLibrary(dllLl); dllLl = NULL; }
        FgLog("xess: destroyed");
    }
};

ProviderXess g_xess;

} // namespace

IFgProvider* MakeFgProviderXess(void) { return &g_xess; }
```

- [x] **Step 2: `native\CMakeLists.txt` — XeSS headers, import libs, delay load** (only `FgXess.cpp` added; `${XESS_SDK}/inc` was already on the include path; NO `libxess_fg.lib`/`libxell.lib` link and no `/DELAYLOAD` — the stub uses `GetProcAddress`)

Insert after the FidelityFX block:

```cmake
set(XESS_SDK "${CMAKE_SOURCE_DIR}/../../refs/XeSS-sdk" CACHE PATH "Intel XeSS 3 SDK root")
get_filename_component(XESS_SDK "${XESS_SDK}" ABSOLUTE)
if(NOT EXISTS "${XESS_SDK}/inc/xess_fg/xefg_swapchain_d3d12.h")
    message(FATAL_ERROR "XeSS FG headers not found at ${XESS_SDK}/inc/xess_fg")
endif()
```

add `FgXess.cpp` to the source list, add `"${XESS_SDK}/inc"` to `target_include_directories`, and after `target_link_libraries` add:

```cmake
# The Intel libs are import libs; delay-load them so a player without the Intel zip can still start the game.
# FgXess.cpp probes both DLLs with LoadLibraryW before the first call, so the delay-load stub is never hit blind.
target_link_libraries(RenderforgeNative PRIVATE
    "${XESS_SDK}/lib/libxess_fg.lib" "${XESS_SDK}/lib/libxell.lib" delayimp)
target_link_options(RenderforgeNative PRIVATE "/DELAYLOAD:libxess_fg.dll" "/DELAYLOAD:libxell.dll")
```

- [x] **Step 3: `build-native.ps1` — verify and stage the Intel DLLs** (`libxess_fg.dll` only, `$xessFgDll` block after the `libxess.dll` one; `libxell.dll` deliberately not shipped)

Extend the `$vendorDlls` array:

```powershell
$xess = Join-Path $root '..\refs\XeSS-sdk\bin'
$vendorDlls += @(
    @{ Path = (Join-Path $xess 'libxess_fg.dll'); Signer = 'Intel Corporation' },
    @{ Path = (Join-Path $xess 'libxell.dll');    Signer = 'Intel Corporation' }
)
```
(place this immediately after the array literal, before the `foreach` that verifies it).

- [x] **Step 4: Build and deploy** (2026-09-02: `build-native.ps1` prints `libxess_fg.dll 1.3.1.78 from ...\refs\XeSS-sdk\bin\libxess_fg.dll`, 4x `PROBE OK`, `build-native: OK`, 0 warnings; `dotnet build -c Release /p:PPRoot=D:\PP-Instance2` 0 warnings / 0 errors; `deploy.ps1` copies `libxess_fg.dll` — not deployed, nothing to run)

```powershell
powershell -NoProfile -Command "Set-Location E:\DEV\PhoenixPoint\Renderforge; .\deploy.ps1"
```
Expected: `libxess_fg.dll 1.3.1.78 signed by Intel Corporation`, `libxell.dll 1.3.2.10 signed by Intel Corporation`, `build-native: OK`, and both files listed in the deploy output.

- [x] **Step 5: Run XeSS-FG in-game (cross-vendor path on the RTX)** — SKIPPED, SDK-blocked (not measured in-game): by code, `SetFgProvider ["Xess"]` + `SetFrameGen ["X2"]` logs `FG init XeSS 2x -> 4` (`FG_ERR_NO_PROVIDER`) and `Fg_Status` ending in `reason=XeSS-FG 1.3.1 needs its own HWND swapchain - unavailable in-process`; the picker row greys with the same text

```powershell
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods','-force-d3d12'
Start-Sleep -Seconds 60
Set-Location E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["Quality","None"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"ToggleOverlay"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFgProvider","args":["Xess"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["X2"]}'
Start-Sleep -Seconds 8
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf\\fg-xess-2x.png"}'
Get-Content 'D:\PP-Instance2\Mods\Renderforge\renderforge_fg.log' -Tail 40
```
Expected: `fg=live provider=XeSS-FG enabled=1 multiplier=2 caps=0x1 lastError=0`; the log shows `xess: maxSupportedInterpolations=1 -> caps 0x1` (1 is the documented value on every non-Intel GPU) and `xess: created <W>x<H> interpolated=1`, with **no** `xess: tag …` error lines and no `xess: frameGenResult` negatives. The screenshot shows `FG: XeSS 2x` and presented ≈ 2 × real.

- [x] **Step 6: Also try 3x and confirm it is refused cleanly, not crashed** — SKIPPED (same `FG_ERR_NO_PROVIDER` path for every multiplier)

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["X3"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
Get-Process PhoenixPointWin64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq 'D:\PP-Instance2\PhoenixPointWin64.exe' } | Stop-Process
```
Expected: `fg=off …` with the log line `xess: 3x asked, 2x supported` and `FG init XeSS 3x -> 6` (`FG_ERR_UNSUPPORTED_MULTIPLIER`); the process is alive and the picture is back to un-generated frames.

- [x] **Step 7: Commit** (as `feat(fg): XeSS-FG provider stub - SDK requires its own HWND swapchain`)

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(fg): XeSS-FG provider stub - SDK requires its own HWND swapchain"
```

---
### Task 5: DLSS-G / MFG provider (Streamline 2.12.0, manual hooking + Reflex/PCL, 2x-4x)

**Files:**
- Create: `E:\DEV\PhoenixPoint\Renderforge\native\FgStreamline.cpp`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\native\CMakeLists.txt`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\build-native.ps1`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Availability.cs`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\src\Pickers.cs`

**The constraint that shapes this task:** an already-created swapchain **cannot** be upgraded post-hoc. `ProgrammingGuideManualHooking.md:208-215` shows exactly one supported path — `slUpgradeInterface` on the **DXGI factory**, then create the swapchain through that proxy factory — and `sl_hooks.h:48-50` marks `CreateSwapChain` / `CreateSwapChainForHwnd` / `CreateSwapChainForCoreWindow` as mandatory hooks. There is no API for adopting Unity's swapchain. This is precisely why the host builds a shadow swapchain: DLSS-G gets a swapchain it created itself, and Unity keeps its own untouched. Reflex + PCL markers are mandatory for DLSS-G (`ProgrammingGuideDLSS_G.md:676-681`, `DLSSGStatus::eFailReflexNotDetectedAtRuntime`).

- [ ] **Step 1: Write `native\FgStreamline.cpp`**

```cpp
// FgStreamline.cpp - NVIDIA DLSS-G / MFG through Streamline 2.12.0 in MANUAL HOOKING mode.
//
// Streamline normally interposes D3D/DXGI from process start through sl.interposer.dll. A mod loaded after
// Unity built the device cannot do that, and an EXISTING swapchain cannot be adopted: the only documented
// path is slUpgradeInterface on the DXGI FACTORY followed by CreateSwapChainForHwnd on the proxy
// (ProgrammingGuideManualHooking.md:208-215). So DLSS-G owns the host's shadow swapchain, which it creates
// itself; Unity's swapchain is never handed to Streamline.
//
// Every entry point is GetProcAddress'd from sl.interposer.dll in the mod folder, so a player without the
// NVIDIA zip gets a greyed picker rather than a load failure.
#include "Fg.h"
#include "RenderforgeNative.h"

#include <string.h>
#include <stdio.h>

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"
#include "sl_matrix_helpers.h"

namespace {

// Streamline's own project identity for this mod (same GUID family as the NGX project id in
// RenderforgeNative.cpp; NVIDIA only requires it to be stable per application).
const char kSlProjectId[] = "b7a3f2c4-6d1e-4a8b-9c0f-2e5d7a9b1c3d";

struct SlFns
{
    PFun_slInit*                fInit;
    PFun_slShutdown*            fShutdown;
    PFun_slIsFeatureSupported*  fIsSupported;
    PFun_slSetD3DDevice*        fSetDevice;
    PFun_slUpgradeInterface*    fUpgrade;
    PFun_slGetNewFrameToken*    fNewFrame;
    PFun_slSetConstants*        fSetConstants;
    PFun_slSetTagForFrame*      fSetTagForFrame;
    PFun_slGetFeatureFunction*  fGetFeatureFn;
    PFun_slSetFeatureLoaded*    fSetFeatureLoaded;
};

void SlLog(sl::LogType, const char* msg) { FgLog("sl: %s", msg ? msg : ""); }

struct ProviderSl : IFgProvider
{
    HMODULE          dll;
    SlFns            sl;
    PFun_slDLSSGSetOptions* fSetOptions;
    PFun_slDLSSGGetState*   fGetState;
    PFun_slReflexSetOptions* fReflexSetOptions;
    PFun_slReflexSleep*      fReflexSleep;
    PFun_slPCLSetMarker*     fMarker;
    IDXGISwapChain4* sc;
    ID3D12Device*    device;
    unsigned         caps;
    unsigned         outW, outH;
    unsigned         framesToGenerate;   // 1 = 2x, 2 = 3x, 3 = 4x
    int              enabled;
    int              lastRc;
    sl::FrameToken*  token;

    ProviderSl() { memset(this, 0, sizeof(*this)); }

    int Id() const { return FG_PROVIDER_DLSS; }
    unsigned Caps() const { return caps; }
    const char* Name() const { return "DLSS-G"; }

    template <class T> bool Get(const char* name, T& out)
    {
        out = (T)GetProcAddress(dll, name);
        if (!out) FgLog("sl: %s missing from sl.interposer.dll", name);
        return out != NULL;
    }

    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\sl.interposer.dll", s.dllDir);
        dll = LoadLibraryW(path);
        if (!dll) { FgLog("sl: sl.interposer.dll not found in the mod folder"); return FG_ERR_NO_PROVIDER; }
        if (!Get("slInit", sl.fInit) || !Get("slShutdown", sl.fShutdown) ||
            !Get("slIsFeatureSupported", sl.fIsSupported) || !Get("slSetD3DDevice", sl.fSetDevice) ||
            !Get("slUpgradeInterface", sl.fUpgrade) || !Get("slGetNewFrameToken", sl.fNewFrame) ||
            !Get("slSetConstants", sl.fSetConstants) || !Get("slSetTagForFrame", sl.fSetTagForFrame) ||
            !Get("slGetFeatureFunction", sl.fGetFeatureFn) || !Get("slSetFeatureLoaded", sl.fSetFeatureLoaded))
            return FG_ERR_NO_PROVIDER;

        device = s.device;
        outW = s.desc.Width; outH = s.desc.Height;
        framesToGenerate = (s.multiplier > 1 ? s.multiplier - 1 : 1);

        const wchar_t* pluginPaths[1] = { s.dllDir };
        sl::Feature features[3] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eDefault;
        pref.pathsToPlugins = pluginPaths;
        pref.numPathsToPlugins = 1;
        pref.pathToLogsAndData = s.dllDir;
        pref.logMessageCallback = &SlLog;
        // eUseManualHooking: we route creation ourselves; eDisableCLStateTracking: Streamline cannot see
        // Unity's command lists, so every tagged resource carries its own state (sl_core_types.h:506-507).
        pref.flags = sl::PreferenceFlags::eUseManualHooking
                   | sl::PreferenceFlags::eDisableCLStateTracking
                   | sl::PreferenceFlags::eAllowOTA
                   | sl::PreferenceFlags::eLoadDownloadedPlugins;
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = 3;
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "2019.4.31";
        pref.projectId = kSlProjectId;
        pref.renderAPI = sl::RenderAPI::eD3D12;
        sl::Result r = sl.fInit(pref, sl::kSDKVersion);
        if (r != sl::Result::eOk) { FgLog("sl: slInit %d", (int)r); return FG_ERR_PROVIDER_FAILED; }

        LUID luid = device->GetAdapterLuid();
        sl::AdapterInfo ai{};
        ai.deviceLUID = (uint8_t*)&luid;
        ai.deviceLUIDSizeInBytes = sizeof(luid);
        r = sl.fIsSupported(sl::kFeatureDLSS_G, ai);
        if (r != sl::Result::eOk) { FgLog("sl: DLSS-G unsupported on this adapter (%d)", (int)r); sl.fShutdown(); return FG_ERR_PROVIDER_FAILED; }

        r = sl.fSetDevice(device);
        if (r != sl::Result::eOk) { FgLog("sl: slSetD3DDevice %d", (int)r); sl.fShutdown(); return FG_ERR_PROVIDER_FAILED; }

        if (sl.fGetFeatureFn(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)fSetOptions) != sl::Result::eOk ||
            sl.fGetFeatureFn(sl::kFeatureDLSS_G, "slDLSSGGetState",   (void*&)fGetState)   != sl::Result::eOk ||
            sl.fGetFeatureFn(sl::kFeatureReflex, "slReflexSetOptions",(void*&)fReflexSetOptions) != sl::Result::eOk ||
            sl.fGetFeatureFn(sl::kFeatureReflex, "slReflexSleep",     (void*&)fReflexSleep) != sl::Result::eOk ||
            sl.fGetFeatureFn(sl::kFeaturePCL,    "slPCLSetMarker",    (void*&)fMarker)      != sl::Result::eOk)
        {
            FgLog("sl: a feature function is missing");
            sl.fShutdown();
            return FG_ERR_PROVIDER_FAILED;
        }

        // Reflex is MANDATORY for DLSS-G: without it DLSSGState reports eFailReflexNotDetectedAtRuntime.
        sl::ReflexOptions ro{};
        ro.mode = sl::ReflexMode::eLowLatency;
        ro.useMarkersToOptimize = true;
        r = fReflexSetOptions(ro);
        if (r != sl::Result::eOk) FgLog("sl: slReflexSetOptions %d", (int)r);

        // The swapchain MUST be created through the upgraded factory - that is the whole of manual hooking.
        void* proxyFactory = (void*)s.factory;
        r = sl.fUpgrade(&proxyFactory);
        if (r != sl::Result::eOk || !proxyFactory) { FgLog("sl: slUpgradeInterface(factory) %d", (int)r); sl.fShutdown(); return FG_ERR_PROVIDER_FAILED; }

        DXGI_SWAP_CHAIN_DESC1 d = s.desc;
        IDXGISwapChain1* sc1 = NULL;
        HRESULT hr = ((IDXGIFactory2*)proxyFactory)->CreateSwapChainForHwnd(s.queue, s.hwnd, &d, NULL, NULL, &sc1);
        ((IUnknown*)proxyFactory)->Release();
        if (FAILED(hr) || !sc1) { FgLog("sl: proxy CreateSwapChainForHwnd 0x%08X", (unsigned)hr); sl.fShutdown(); return FG_ERR_NO_SWAPCHAIN; }
        hr = sc1->QueryInterface(__uuidof(IDXGISwapChain4), (void**)out);
        sc1->Release();
        if (FAILED(hr)) { sl.fShutdown(); return FG_ERR_NO_SWAPCHAIN; }
        sc = *out;

        // Ask what this GPU can actually do before promising 3x/4x.
        sl::DLSSGState st{};
        sl::DLSSGOptions probe{};
        probe.mode = sl::DLSSGMode::eOff;
        probe.numFramesToGenerate = 1;
        sl::ViewportHandle vp(0u);
        if (fGetState(vp, st, &probe) == sl::Result::eOk) {
            unsigned maxGen = st.numFramesToGenerateMax < 1 ? 1u : st.numFramesToGenerateMax;
            caps = FG_CAP_2X | (maxGen >= 2 ? FG_CAP_3X : 0u) | (maxGen >= 3 ? FG_CAP_4X : 0u);
            FgLog("sl: numFramesToGenerateMax=%u -> caps 0x%X, minWidthOrHeight=%u", maxGen, caps, st.minWidthOrHeight);
            if (framesToGenerate > maxGen) { FgLog("sl: %ux asked, %ux supported", framesToGenerate + 1, maxGen + 1); return FG_ERR_UNSUPPORTED_MULTIPLIER; }
        } else {
            caps = FG_CAP_2X;
            FgLog("sl: slDLSSGGetState failed, assuming 2x only");
            if (framesToGenerate > 1) return FG_ERR_UNSUPPORTED_MULTIPLIER;
        }

        FgLog("sl: created %ux%u framesToGenerate=%u", outW, outH, framesToGenerate);
        return FG_OK;
    }

    // The whole mandatory per-frame sequence except the two present markers, which BeforePresent/AfterPresent
    // own (ProgrammingGuideDLSS_G.md:676-681). The present markers MUST carry the same frame token as
    // slSetConstants, so the token is created here and kept until AfterPresent.
    void Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f)
    {
        if (!fSetOptions || !enabled) return;
        uint32_t idx = (uint32_t)f.frameId;
        if (sl.fNewFrame(token, &idx) != sl::Result::eOk || !token) return;
        sl::ViewportHandle vp(0u);

        fMarker(sl::PCLMarker::eSimulationStart, *token);
        fMarker(sl::PCLMarker::eSimulationEnd, *token);
        fReflexSleep(*token);
        fMarker(sl::PCLMarker::eRenderSubmitStart, *token);

        sl::Constants c{};
        // Row-major, per sl_consts.h:183. Unity's projection is column-vector convention, so the 16 floats
        // handed over by Fg_SetFrame are already in Streamline's row order (m00,m01,m02,m03, m10,...).
        for (int i = 0; i < 4; ++i)
            c.cameraViewToClip.setRow(i, sl::float4(f.proj[i*4+0], f.proj[i*4+1], f.proj[i*4+2], f.proj[i*4+3]));
        c.cameraPos   = sl::float3(f.camPos[0],   f.camPos[1],   f.camPos[2]);
        c.cameraUp    = sl::float3(f.camUp[0],    f.camUp[1],    f.camUp[2]);
        c.cameraRight = sl::float3(f.camRight[0], f.camRight[1], f.camRight[2]);
        c.cameraFwd   = sl::float3(f.camFwd[0],   f.camFwd[1],   f.camFwd[2]);
        c.cameraNear = f.cameraNear;
        c.cameraFar = f.cameraFar;
        c.cameraFOV = f.cameraFovY;
        c.cameraAspectRatio = f.outH ? (float)f.outW / (float)f.outH : 1.0f;
        c.jitterOffset = sl::float2(f.jitterX, f.jitterY);
        c.mvecScale = sl::float2(f.mvScaleX / (float)(f.renderW ? f.renderW : 1), f.mvScaleY / (float)(f.renderH ? f.renderH : 1));
        c.depthInverted = sl::Boolean::eTrue;            // Unity D3D12 reversed Z
        c.cameraMotionIncluded = sl::Boolean::eTrue;     // Unity's motion vectors include camera motion
        c.motionVectors3D = sl::Boolean::eFalse;
        c.motionVectorsDilated = sl::Boolean::eFalse;
        c.motionVectorsJittered = sl::Boolean::eFalse;
        c.orthographicProjection = sl::Boolean::eFalse;
        c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        // Fills clipToCameraView, clipToPrevClip and prevClipToClip from the camera basis
        // (sl_matrix_helpers.h:178). It keeps the previous frame in a file-static, which is correct for our
        // single view and is the only helper the SDK ships.
        sl::recalculateCameraMatrices(c);
        lastRc = (int)sl.fSetConstants(c, *token, vp);
        if (lastRc != (int)sl::Result::eOk) FgLog("sl: slSetConstants %d", lastRc);

        // eDisableCLStateTracking is set, so every tag carries its own resource state.
        const uint32_t kSrv = (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        sl::Resource rDepth(sl::ResourceType::eTex2d, f.depth,   kSrv);
        sl::Resource rMv   (sl::ResourceType::eTex2d, f.mv,      kSrv);
        sl::Resource rHud  (sl::ResourceType::eTex2d, f.hudless, kSrv);
        sl::Extent eRender{ 0, 0, f.renderW, f.renderH };
        sl::Extent eOut{ 0, 0, f.outW, f.outH };
        sl::ResourceTag tags[3] = {
            sl::ResourceTag(&rDepth, sl::kBufferTypeDepth,         sl::ResourceLifecycle::eOnlyValidNow,      &eRender),
            sl::ResourceTag(&rMv,    sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow,      &eRender),
            sl::ResourceTag(&rHud,   sl::kBufferTypeHUDLessColor,  sl::ResourceLifecycle::eValidUntilPresent, &eOut),
        };
        lastRc = (int)sl.fSetTagForFrame(*token, vp, tags, 3, (sl::CommandBuffer*)list);
        if (lastRc != (int)sl::Result::eOk) FgLog("sl: slSetTagForFrame %d", lastRc);

        fMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
    }

    void BeforePresent(const FgFrame&)
    {
        if (!token || !enabled) return;
        fMarker(sl::PCLMarker::ePresentStart, *token);
    }

    int AfterPresent(void)
    {
        if (!token || !enabled) return 1;
        fMarker(sl::PCLMarker::ePresentEnd, *token);
        sl::DLSSGState st{};
        sl::ViewportHandle vp(0u);
        if (fGetState(vp, st, NULL) == sl::Result::eOk) {
            if (st.status != sl::DLSSGStatus::eOk) FgLog("sl: DLSSGStatus 0x%X", (unsigned)st.status);
            return st.numFramesActuallyPresented > 0 ? (int)st.numFramesActuallyPresented : 1;
        }
        return 1 + (int)framesToGenerate;
    }

    void SetEnabled(bool on)
    {
        enabled = on ? 1 : 0;
        if (!fSetOptions) return;
        sl::DLSSGOptions o{};
        o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.numFramesToGenerate = framesToGenerate;
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;   // no stutter when the player toggles FG
        o.enableUserInterfaceRecomposition = sl::Boolean::eTrue;  // we tag hud-less, SL recomposes the HUD
        o.colorWidth = outW; o.colorHeight = outH;
        sl::ViewportHandle vp(0u);
        sl::Result r = fSetOptions(vp, o);
        if (r != sl::Result::eOk) FgLog("sl: slDLSSGSetOptions %d", (int)r);
    }

    void Destroy(void)
    {
        if (fSetOptions) SetEnabled(false);
        sc = NULL;
        if (sl.fShutdown) sl.fShutdown();
        if (dll) { FreeLibrary(dll); dll = NULL; }
        enabled = 0; caps = 0; token = NULL;
        FgLog("sl: destroyed");
    }
};

ProviderSl g_sl;

} // namespace

IFgProvider* MakeFgProviderStreamline(void) { return &g_sl; }
```

- [ ] **Step 2: `native\CMakeLists.txt` — Streamline headers**

Insert after the XeSS block:

```cmake
set(SL_SDK "${CMAKE_SOURCE_DIR}/../../refs/Streamline" CACHE PATH "NVIDIA Streamline SDK root")
get_filename_component(SL_SDK "${SL_SDK}" ABSOLUTE)
if(NOT EXISTS "${SL_SDK}/include/sl_dlss_g.h")
    message(FATAL_ERROR "Streamline headers not found at ${SL_SDK}/include")
endif()
```

add `FgStreamline.cpp` to the source list and `"${SL_SDK}/include"` to `target_include_directories`. No import lib: every Streamline entry point is `GetProcAddress`'d.

- [ ] **Step 3: `build-native.ps1` — verify and stage the NVIDIA FG DLLs**

Extend `$vendorDlls` (after the Intel entries):

```powershell
# nvngx_dlssg.dll: the SDK's own copy under bin\x64 is 310.7.0 = STALE. Ship latest-dll\ (310.7.129), the
# same rule that already applies to nvngx_dlss.dll (docs\DESIGN.md "Vendor SDKs on disk").
$slbin = Join-Path $root '..\refs\Streamline\bin\x64'
$vendorDlls += @(
    @{ Path = (Join-Path $slbin 'sl.interposer.dll'); Signer = 'NVIDIA Corporation' },
    @{ Path = (Join-Path $slbin 'sl.common.dll');     Signer = 'NVIDIA Corporation' },
    @{ Path = (Join-Path $slbin 'sl.dlss_g.dll');     Signer = 'NVIDIA Corporation' },
    @{ Path = (Join-Path $slbin 'sl.reflex.dll');     Signer = 'NVIDIA Corporation' },
    @{ Path = (Join-Path $slbin 'sl.pcl.dll');        Signer = 'NVIDIA Corporation' },
    @{ Path = (Join-Path $root '..\refs\Streamline\latest-dll\nvngx_dlssg.dll'); Signer = 'NVIDIA Corporation' }
)
```

and assert the anti-stale rule right after the verification loop:

```powershell
$dlssg = (Get-Item (Join-Path $root '..\refs\Streamline\latest-dll\nvngx_dlssg.dll')).VersionInfo.FileVersion -replace ',', '.' -replace ' ', ''
if ($dlssg -ne '310.7.129.0') { throw "nvngx_dlssg.dll is $dlssg, expected 310.7.129.0 - check the TechPowerUp DLL database before shipping a different build" }
```

- [ ] **Step 4: `src\Availability.cs` — real reasons for `Feature.FrameGen`**

Replace the shared `case Feature.Fsr: case Feature.Xess: case Feature.FrameGen:` arm with a dedicated FG arm (leave Fsr/Xess as they are for Phases 3/4):

```csharp
                case Feature.Fsr:
                case Feature.Xess:
                    return IsD3D12
                        ? DlssConfig.Loc("Not implemented yet", "Пока не реализовано")
                        : DlssConfig.Loc("Requires DirectX 12 — switch Renderer", "Требуется DirectX 12 — переключите рендерер");
                case Feature.FrameGen:
                    if (!IsD3D12)
                        return DlssConfig.Loc("Requires DirectX 12 — switch Renderer", "Требуется DirectX 12 — переключите рендерер");
                    if (RenderforgeMod.Instance == null || RenderforgeMod.Instance.Cfg.Mode == RenderforgeMode.Off)
                        return DlssConfig.Loc("Turn an upscaler on first", "Сначала включите апскейлер");
                    return FrameGen.MissingDll();
```

and add the DLL check to `src\FrameGen.cs`:

```csharp
        /// <summary>Null when the vendor DLLs the auto-picked provider needs are all present in the mod folder,
        /// else the localized "DLL missing: x" reason the picker tooltip shows.</summary>
        internal static string MissingDll()
        {
            string[] need;
            switch (AutoProvider())
            {
                case Native.FG_PROVIDER_FSR:  need = new[] { "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll" }; break;
                case Native.FG_PROVIDER_XESS: need = new[] { "libxess_fg.dll", "libxell.dll" }; break;
                case Native.FG_PROVIDER_DLSS: need = new[] { "sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll" }; break;
                default: return null;
            }
            foreach (var n in need)
                if (!System.IO.File.Exists(System.IO.Path.Combine(RenderforgeMod.ModDir, n)))
                    return DlssConfig.Loc("DLL missing: " + n, "Отсутствует DLL: " + n);
            return null;
        }
```

- [ ] **Step 5: `src\Pickers.cs` — the FG row actually applies, and 3x/4x grey per capability**

Replace `ShowFrameGen` and `OnFrameGen` with:

```csharp
        private static readonly int[] FrameGenCaps = { 0, Native.FG_CAP_2X, Native.FG_CAP_3X, Native.FG_CAP_4X };

        private static string FrameGenReason(int index)
        {
            if (index == 0) return null;
            string r = Availability.Reason(Feature.FrameGen);
            if (r != null) return r;
            uint caps = FrameGen.Caps;
            // Caps are 0 until the chain has been built once; only grey a multiplier we KNOW is unsupported.
            if (caps != 0 && (caps & (uint)FrameGenCaps[index]) == 0)
                return DlssConfig.Loc("Not supported by this GPU", "Не поддерживается этой видеокартой");
            return null;
        }

        private static void ShowFrameGen()
        {
            string reason = FrameGenReason(pendingFrameGen);
            GraphicsPanel.SetRaw(frameGen.CurrentItem, frameGen.CurrentItemText, FrameGenLabels[pendingFrameGen]);
            GraphicsPanel.Grey(frameGen.CurrentItem.gameObject, reason != null);
            GraphicsPanel.Tip(frameGen.CentralButton.gameObject, reason);
        }

        private static void OnFrameGen(int index)
        {
            try
            {
                pendingFrameGen = index;
                ShowFrameGen();
                var mod = RenderforgeMod.Instance;
                if (mod == null || FrameGenReason(index) != null) return;   // unavailable: show it greyed, write nothing
                RenderforgeMod.SetFrameGen(((FrameGenMode)index).ToString());
            }
            catch (Exception ex) { Log("frame-generation picker change failed", ex); }
        }
```

and set the row's opening value from the config instead of a hard 0 — in `Build`, replace `pendingFrameGen = 0;` with:

```csharp
            pendingFrameGen = (int)cfg.FrameGen;
```

- [ ] **Step 6: Build and deploy**

```powershell
powershell -NoProfile -Command "Set-Location E:\DEV\PhoenixPoint\Renderforge; .\deploy.ps1"
```
Expected: the five `sl.*.dll 2.12.0.0 signed by NVIDIA Corporation` lines plus `nvngx_dlssg.dll 310,7,129,0 signed by NVIDIA Corporation`, `build-native: OK`, and the deploy listing showing all six.

- [ ] **Step 7: Run DLSS-G 2x, then MFG 3x and 4x (this machine's RTX 5070 Ti can do all three)**

```powershell
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods','-force-d3d12'
Start-Sleep -Seconds 60
Set-Location E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["Quality","None"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"ToggleOverlay"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFgProvider","args":["Dlss"]}'
foreach ($m in 'X2','X3','X4') {
  .\ppcli.ps1 connect call ('{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["' + $m + '"]}')
  Start-Sleep -Seconds 8
  .\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
  .\ppcli.ps1 connect screenshot ('{"path":"C:\\Temp\\rf\\fg-dlss-' + $m + '.png"}')
}
Get-Content 'D:\PP-Instance2\Mods\Renderforge\renderforge_fg.log' -Tail 60
```
Expected: the log shows `sl: numFramesToGenerateMax=3 -> caps 0x7` (RTX 50), no `sl: DLSSGStatus 0x…` lines and in particular **not** `0x2` (`eFailReflexNotDetectedAtRuntime`) — that value means the Reflex/PCL marker sequence in `Prepare` is wrong, not that the GPU is unsupported. The three screenshots show `FG: DLSS 2x/3x/4x` with presented ≈ 2×/3×/4× real.

If `slUpgradeInterface(factory)` returns anything but `eOk`, or the proxy `CreateSwapChainForHwnd` fails, DLSS-G is **not available in-process** — record the exact `sl::Result` here, leave the provider in place (it fails cleanly and the picker greys), and note that the `dxgi.dll` shim of Task 8 is the only remaining route for DLSS-G specifically. FSR-FG and XeSS-FG are unaffected.

- [ ] **Step 8: Commit**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(fg): DLSS-G/MFG provider via Streamline manual hooking, Reflex+PCL markers, capability-gated 2x/3x/4x"
```

---
### Task 6: Full in-game verification on `D:\PP-Instance2`

**Files:** none changed (evidence-gathering task).

Never touch `D:\Steam\steamapps\common\Phoenix Point` — that is the user's own game. Everything here runs against the automation install. PPCLI usage is per `E:\DEV\PhoenixPoint\PPCLI\PLAYBOOK.md`.

- [ ] **Step 1: Deploy and take the no-FG baseline**

```powershell
powershell -NoProfile -Command "Set-Location E:\DEV\PhoenixPoint\Renderforge; .\deploy.ps1"
New-Item -ItemType Directory -Force C:\Temp\rf | Out-Null
Start-Process 'D:\PP-Instance2\PhoenixPointWin64.exe' -ArgumentList '-mods','-force-d3d12'
Start-Sleep -Seconds 60
Set-Location E:\DEV\PhoenixPoint\PPCLI
.\ppcli.ps1 connect state
.\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["Quality","None"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"ToggleOverlay"}'
Start-Sleep -Seconds 5
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf\\p5-baseline.png"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
```
Expected: `api=12 feature=1 lastError=0 | fg=off`, overlay reading `FPS: <n> (<ms>)` with no `/`.

- [ ] **Step 2: All three providers, one screenshot each**

```powershell
foreach ($p in 'Fsr','Xess','Dlss') {
  .\ppcli.ps1 connect call ('{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFgProvider","args":["' + $p + '"]}')
  .\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["X2"]}'
  Start-Sleep -Seconds 10
  .\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
  .\ppcli.ps1 connect screenshot ('{"path":"C:\\Temp\\rf\\p5-' + $p + '-2x.png"}')
  .\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["Off"]}'
  Start-Sleep -Seconds 3
}
```
Expected per provider: `fg=live provider=<FSR-FG 3.1.6|XeSS-FG|DLSS-G> enabled=1 multiplier=2 lastError=0` and a screenshot whose overlay reads `FPS: <real> / <presented>` with presented between 1.7× and 2.1× real.

- [ ] **Step 3: Read every screenshot and judge the picture**

Open `C:\Temp\rf\p5-baseline.png`, `p5-Fsr-2x.png`, `p5-Xess-2x.png`, `p5-Dlss-2x.png` with the Read tool. Check, in this order:
1. **HUD not smeared.** Unit cards, action bar, the top status strip and the mod's own overlay text must be as sharp as in the baseline. A ghosted or doubled HUD means the hud-less contract failed — the provider is interpolating the composed backbuffer instead of `outRT`. Fix: `HUDLessColor` (FSR) / `XEFG_SWAPCHAIN_RES_HUDLESS_COLOR` tag (XeSS) / `kBufferTypeHUDLessColor` tag (DLSS) is not reaching the SDK; check `renderforge_fg.log` for a tag/configure error before touching anything else.
2. **Scene identical to the baseline** in colour, brightness and fog of war.
3. **No tear line, no black band, no half-frame.**
4. Overlay `FG:` line names the right provider and multiplier.

- [ ] **Step 4: MFG 3x and 4x (DLSS only)**

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFgProvider","args":["Dlss"]}'
foreach ($m in 'X3','X4') {
  .\ppcli.ps1 connect call ('{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["' + $m + '"]}')
  Start-Sleep -Seconds 10
  .\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
  .\ppcli.ps1 connect screenshot ('{"path":"C:\\Temp\\rf\\p5-Dlss-' + $m + '.png"}')
}
```
Expected: presented ≈ 3× and 4× real, `caps=0x7`.

- [ ] **Step 5: Prove the other two providers refuse 3x cleanly**

```powershell
foreach ($p in 'Fsr','Xess') {
  .\ppcli.ps1 connect call ('{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFgProvider","args":["' + $p + '"]}')
  .\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["X3"]}'
  Start-Sleep -Seconds 4
  .\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
}
```
Expected: `fg=off` with `FG init … 3x -> 6` in `Player.log`, the process alive and the picture un-generated. **A crash here is a bug, not an expected limitation.**

- [ ] **Step 6: 10-minute stability soak with FG on**

```powershell
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFgProvider","args":["Fsr"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["X2"]}'
1..10 | ForEach-Object {
  Start-Sleep -Seconds 60
  .\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
}
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf\\p5-soak-end.png"}'
```
Expected: ten answers, the process alive throughout, `lastError=0` every time, `presented=` growing monotonically, and the final screenshot indistinguishable from the 10-minute-earlier one.

- [ ] **Step 7: Three mission loads with FG live, then a renderer round trip**

```powershell
1..3 | ForEach-Object { .\ppcli.ps1 plan .\plans\start-mission.json '{"scene":"ALN_PLT_Nest_48x48_A","seed":12345}'; Start-Sleep -Seconds 15 }
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"GetStatus"}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetFrameGen","args":["Off"]}'
.\ppcli.ps1 connect call '{"op":"invoke","type":"Renderforge.RenderforgeMod","assembly":"Renderforge","member":"SetMode","args":["Off","None"]}'
.\ppcli.ps1 connect screenshot '{"path":"C:\\Temp\\rf\\p5-all-off.png"}'
```
Expected: the FG chain is torn down and rebuilt across each level change (`FgHostSetEnabled(0)` from `BeginRelease`, then `FrameGen.Retry()` once the new camera is live); `fg=live` again after each load; the final screenshot shows the plain, un-upscaled, un-generated game with a correct HUD.

- [ ] **Step 8: Log diff against the D3D12 baseline**

```powershell
$log = "$env:USERPROFILE\AppData\LocalLow\Snapshot Games Inc\Phoenix Point\Player.log"
Select-String -Path $log -Pattern 'FG |Renderforge|DLSS|D3D12' | Select-Object -Last 60
Select-String -Path $log -Pattern 'Exception|Error|error' | Group-Object Line | Sort-Object Count -Descending | Select-Object -First 15 Count, Name
Get-Content 'D:\PP-Instance2\Mods\Renderforge\renderforge_fg.log' | Select-String -Pattern 'fail|error|-[0-9]|0x8'
Get-Process PhoenixPointWin64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq 'D:\PP-Instance2\PhoenixPointWin64.exe' } | Stop-Process
```
Expected: no `DXGI_ERROR`, no `D3D12 ERROR`, no `RemovedDevice`, and every `Exception` group already present in the Phase 2 D3D12 run. `renderforge_fg.log` may contain informational lines but no failure codes.

- [ ] **Step 9: PPCLI defects only if PPCLI itself misbehaved**

If a PPCLI verb misbehaved (not the mod), append an entry to `E:\DEV\PhoenixPoint\PPCLI\ISSUES.md` (attempted → happened → expected → evidence → severity) and work around it; do not fix PPCLI from this session.

- [ ] **Step 10: Commit the evidence note**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "test(fg): FSR/XeSS/DLSS frame generation verified in-game on Instance2, 10-minute soak clean" --allow-empty
```

---

### Task 7: Documentation

**Files:**
- Create: `E:\DEV\PhoenixPoint\docs\research\framegen-d3d12-contract.md` (outer repo)
- Modify: `E:\DEV\PhoenixPoint\docs\research\README.md` (outer repo index)
- Modify: `E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md`
- Modify: `E:\DEV\PhoenixPoint\Renderforge\README.md`

- [ ] **Step 1: Write `E:\DEV\PhoenixPoint\docs\research\framegen-d3d12-contract.md`**

```markdown
# Frame generation on D3D12 — the cross-vendor contract (2026-09-02, Renderforge Phase 5)

Sources: `refs/FidelityFX-SDK/Kits/FidelityFX/` (SDK 2.3, FG 4.0.1 ML + 3.1.6 analytical, FG-swapchain 3.1.7),
`refs/XeSS-sdk/` (XeSS 3, `libxess_fg.dll` 1.3.1.78, `libxell.dll` 1.3.2.10),
`refs/Streamline/` (2.12.0, `nvngx_dlssg.dll` 310.7.129 from `latest-dll\`).
Line numbers below are from those trees as of this date.

## 1. The one thing all three SDKs agree on

**Frame generation owns presentation.** None of them interpolates into a texture you present yourself; each
one gives you a swapchain and does the work inside `Present`. Any integration therefore starts by answering
"who creates the swapchain", and for a mod loaded into an already-running engine that is the whole problem.

## 2. Unity 2019.4 does not hand out its swapchain

`IUnityGraphicsD3D12v5` (the highest D3D12 plugin interface that exists in 2019.4; v6/v7 are 2023+) exposes
`GetDevice`, `GetFrameFence`, `GetNextFrameFenceValue`, `ExecuteCommandList`,
`SetPhysicalVideoMemoryControlValues`, `GetCommandQueue`, `TextureFromRenderBuffer`. No swapchain getter, in
any version. DXGI has no enumeration API either. The only mechanism left is a vtable patch on
`IDXGISwapChain::Present`, read off a throwaway swapchain of your own — every swapchain from one DXGI runtime
shares one vtable. Indices: `Present` = 8, `ResizeBuffers` = 13, `Present1` = 22.

## 3. In-place wrapping is not available to a mod

| SDK | Entry point | What it does to the app's swapchain |
|---|---|---|
| FidelityFX | `ffxCreateContextDescFrameGenerationSwapChainWrapDX12` (`ffx_api_framegeneration_dx12.h:34`) | `IDXGISwapChain4**` in/out; the original is **released internally**. The SDK's own sample avoids it and uses `...ForHwndDX12` (`:52`) after fully releasing the engine swapchain. |
| XeSS-FG | `xefgSwapChainD3D12InitFromSwapChain` (`xefg_swapchain_d3d12.h:190`) | requires **refcount == 1**, then **destroys it**; the pointer becomes invalid (`doc/xess_fg_developer_guide_english.md:270-276`). |
| Streamline | — | no adoption API at all. `slUpgradeInterface` covers the D3D12 device and the DXGI factory only; the swapchain must be created **through the upgraded factory** (`docs/ProgrammingGuideManualHooking.md:208-215`), and `sl_hooks.h:48-50` makes the three creation calls mandatory hooks. |

The engine keeps its own reference and keeps calling `GetBuffer`/`ResizeBuffers` every frame, so none of these
is usable in place. → the only shape that works is a **shadow swapchain**: create a second, FG-owned swapchain
on the same HWND (`...ForHwndDX12` / `xefgSwapChainD3D12InitFromSwapChainDesc` / proxy-factory
`CreateSwapChainForHwnd`), copy the engine's finished backbuffer into it inside the hooked `Present`, present
the shadow, and never call the engine's original `Present`.

## 4. Hud-less is the cheap UI mode, and all three support it

If the pipeline already produces the scene without UI at output resolution and the backbuffer with UI, no
separate UI render target is needed:

| SDK | How it is expressed |
|---|---|
| FidelityFX | `ffxConfigureDescFrameGeneration.HUDLessColor` (`ffx_framegeneration.h:116`), same resolution as the backbuffer |
| XeSS-FG | `uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS` (4) + a `XEFG_SWAPCHAIN_RES_HUDLESS_COLOR` tag; mode table at `doc/…:623` says hud-less required, UI texture not used |
| Streamline | `kBufferTypeHUDLessColor` tag + `DLSSGOptions::enableUserInterfaceRecomposition` |

## 5. Per-frame contracts

| | FidelityFX | XeSS-FG | Streamline DLSS-G |
|---|---|---|---|
| Needs a recording command list | yes — `ffxDispatchDescFrameGenerationPrepareV2.commandList` | yes — `xefgSwapChainD3D12TagFrameResource(…, ID3D12CommandList*, …)` | yes — `slSetTagForFrame(…, cmdBuffer)` |
| Mandatory inputs | depth + motion vectors (+ camera basis, near/far/fovY, jitter, mvScale, frame time) | motion vectors + depth (+ `xefg_swapchain_frame_constant_data_t`: view/proj row-major, jitter, mvScale, reset, frame time) | `kBufferTypeDepth` + `kBufferTypeMotionVectors` + `sl::Constants` |
| Latency component | none required (async compute optional) | **XeLL mandatory** — the FG context refuses to init without one (`doc/…:229-231`); six markers required | **Reflex + PCL mandatory** — otherwise `DLSSGStatus::eFailReflexNotDetectedAtRuntime` |
| Marker/id per frame | `frameID` on Configure, Prepare and Dispatch | `presentId` on every tag + `xefgSwapChainSetPresentId` | `sl::FrameToken`; the present markers MUST carry the same index as `slSetConstants` (`ProgrammingGuideDLSS_G.md:680-681`) |
| Where interpolation runs | inside the FI swapchain's `Present`, fed by `ffxDispatchDescFrameGeneration` | inside the proxy swapchain's `Present` | inside the proxy swapchain's `Present`; **never** `slEvaluateFeature` |

## 6. How many frames each can generate

| SDK | Multiplier | Gate |
|---|---|---|
| FidelityFX FG | 2x | `numGeneratedFrames = 1` in the SDK sample; `outputs[4]` exists but no doc on disk describes >1 |
| XeSS-FG | 2x, more on Intel Arc | `xefg_swapchain_properties_t.maxSupportedInterpolations`; **1 on every non-Intel GPU** (`doc/…:87`) |
| DLSS-G / MFG | 2x on RTX 40, up to 4x (6x theoretical) on RTX 50 | `DLSSGState::numFramesToGenerateMax`, set with `DLSSGOptions::numFramesToGenerate` (1 = 2x) |

## 7. Model selection inside the AMD DLL

`amd_fidelityfx_framegeneration_dx12.dll` 4.0.1 carries **both** the ML model (4.0.1, needs Windows 11, DX12
Agility SDK 1.4.9, **RDNA4 / RX 9000 or later**, CS_6_6, ≥30 fps input — `docs/techniques/frame-interpolation-ml.md:75`)
and the analytical one (3.1.6, typed UAV load + `R16G16B16A16_UNORM`, ≥60 fps input). Enumerate with
`ffxQueryDescGetVersions` (`ffx_api.h:97`) and pin the one you want by chaining `ffxOverrideVersion`
(`:108`) onto the create desc. On a non-RDNA4 GPU, pin 3.1.6 explicitly rather than letting the loader choose.

## 8. Traps

- **Never ship an SDK's bundled `nvngx_*.dll` blindly.** Streamline 2.12.0's `bin\x64\nvngx_dlssg.dll` is
  310.7.0; `latest-dll\nvngx_dlssg.dll` is 310.7.129. Same trap as `nvngx_dlss.dll` in Phase 2.
- **The engine's backbuffer state at `Present` time is `D3D12_RESOURCE_STATE_PRESENT`.** The copy pass has to
  transition it to `COPY_SOURCE` and back itself; the engine's state tracker is not involved because the
  copy runs directly on the engine's queue after all of its own submissions for that frame.
- **Prepare/tagging must go through the engine's own submission path** (`ExecuteCommandList` with a
  `UnityGraphicsD3D12ResourceState[]`) because only the engine knows the states of its depth/MV/colour
  render textures.
- **Toggle before resize.** All three want FG disabled and (FidelityFX, Streamline) the swapchain recreated
  around any resolution or fullscreen transition.
- **Streamline manual hooking sets `eDisableCLStateTracking`**, so every `sl::Resource` must carry its own
  `state` (`sl_core_types.h:506-507`).
```

- [ ] **Step 2: Add the index entry to `E:\DEV\PhoenixPoint\docs\research\README.md`**

Add, in the same alphabetical/topical place the other graphics notes sit:

```markdown
- `framegen-d3d12-contract.md` — cross-vendor frame-generation contract on D3D12 (FidelityFX FG, XeSS-FG +
  XeLL, Streamline DLSS-G/MFG): who owns the swapchain, why in-place wrapping is unavailable to a mod, the
  shadow-swapchain shape, hud-less UI mode, per-frame contracts, multiplier gates, version traps.
```

- [ ] **Step 3: Add the Frame generation section to `E:\DEV\PhoenixPoint\Renderforge\docs\DESIGN.md`**

Append after the "Renderer switch (Phase 1, 2026-09-02)" section:

```markdown
## Frame generation (Phase 5, 2026-09-02) — D3D12 only

- Three providers behind one seam (`native\Fg.h` `IFgProvider`): `FgFsr.cpp` (FidelityFX FG, analytical
  3.1.6 pinned, 2x), `FgXess.cpp` (XeSS-FG + mandatory XeLL, 2x off Intel Arc), `FgStreamline.cpp`
  (DLSS-G/MFG, 2x on RTX 40, up to 4x on RTX 50). Auto picks DLSS-G on NVIDIA, FSR-FG elsewhere;
  `RenderforgeMod.SetFgProvider` forces one for testing.
- **Swapchain acquisition.** Unity's plugin API exposes no swapchain (`IUnityGraphicsD3D12v5` has
  `GetDevice`/`GetFrameFence`/`GetNextFrameFenceValue`/`ExecuteCommandList`/`GetCommandQueue`/
  `TextureFromRenderBuffer` and nothing else), and none of the three SDKs can adopt a live foreign
  swapchain. So `FgHook.cpp` patches `IDXGISwapChain::Present`/`Present1`/`ResizeBuffers` in the shared
  DXGI vtable — read off an 8x8 throwaway swapchain of ours — and `FgHost.cpp` builds a second, FG-owned
  swapchain on the same HWND. Every Unity `Present` is intercepted: copy Unity's backbuffer into the FG
  swapchain, interpolate, present the FG chain, return `S_OK`. Unity's own swapchain is never presented
  while FG is on. Full reasoning and the SDK line cites: `E:\DEV\PhoenixPoint\docs\research\framegen-d3d12-contract.md`.
- **HUD.** The present path already gives every SDK what it wants for free: `outRT` is the upscaled scene
  WITHOUT the HUD, and by `Present` time the backbuffer is the same frame WITH it. That is FidelityFX's
  `HUDLessColor`, XeSS's UI mode 4 (`BACKBUFFER_HUDLESS`) and DLSS-G's `kBufferTypeHUDLessColor` +
  `enableUserInterfaceRecomposition`. No UI render target is built.
- **Two seams per frame.** Prepare/tagging needs a recording command list with Unity-known resource states,
  so it runs in a new render event `DLSS_EV_FG_PREPARE` at `CameraEvent.AfterEverything`, submitted through
  `IUnityGraphicsD3D12v5::ExecuteCommandList` with a state array (same shape as the DLSS evaluate).
  Interpolation and presentation run in the Present hook.
- **Latency components are not optional.** XeSS-FG refuses to initialise without a XeLL context; DLSS-G
  reports `eFailReflexNotDetectedAtRuntime` without Reflex + the six PCL markers. Both are wired in the
  provider, with the simulation markers approximated around the render event (Unity's real simulation
  boundaries are not reachable from the shim).
- **Overlay.** Real fps is counted in Unity `Update`; presented fps is counted in the Present hook and read
  through `Fg_PresentedFps()` — `FPS: 62 / 118 (16.1 ms)`. A new `FG:` line names the provider and multiplier.
- **Vendor DLLs** are loaded from the mod folder at runtime (`amd_fidelityfx_loader_dx12.dll` via
  `ffxLoadFunctions`, `sl.interposer.dll` via `GetProcAddress`, `libxess_fg.dll`/`libxell.dll` delay-loaded
  behind a `LoadLibraryW` probe), so a player with only the Core zip still starts the game and simply sees
  a greyed FRAME GENERATION row with "DLL missing: <name>".
```

- [ ] **Step 4: `README.md` — the player-facing lines**

Add frame generation to the feature list and a vendor-DLL note:

```markdown
### Frame generation (DirectX 12 only, experimental)

Options → Graphics → **FRAME GENERATION**: `Off / 2x / 3x / 4x`.

| Provider | Used on | Multipliers |
|---|---|---|
| DLSS-G / MFG (NVIDIA Streamline) | NVIDIA RTX | 2x on RTX 40, up to 4x on RTX 50 |
| FSR Frame Generation (AMD FidelityFX) | any DirectX 12 GPU | 2x |
| XeSS-FG (Intel, needs XeLL) | any DirectX 12 GPU | 2x (more on Intel Arc) |

The mod picks the provider automatically: DLSS-G on NVIDIA, FSR-FG elsewhere. Frame generation needs the
DirectX 12 renderer and an upscaler switched on; the row is greyed with the reason when it cannot run.
The benchmark overlay then shows `FPS: 62 / 118` — really rendered frames, then frames actually presented.

Frame generation adds latency and it can show artefacts on fast camera moves. It is off by default.

**Vendor files.** The vendor zips carry redistributable, vendor-signed runtime DLLs
(`amd_fidelityfx_*_dx12.dll`, `libxess_fg.dll` + `libxell.dll`, `sl.*.dll` + `nvngx_dlssg.dll`). Install only
the one for your GPU if you prefer; anything missing simply greys the corresponding option.
```

- [ ] **Step 5: Commit both repos**

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "docs: frame generation - shadow swapchain, three providers, hudless UI, overlay presented fps"
git -C E:\DEV\PhoenixPoint add docs/research/framegen-d3d12-contract.md docs/research/README.md
git -C E:\DEV\PhoenixPoint commit -m "docs(research): cross-vendor D3D12 frame-generation contract"
```

---

### Task 8: CONTINGENCY — the `dxgi.dll` presentation shim (run ONLY if Task 1 Step 9 said NO-GO)

**Files:** none until the user says yes.

Reach this task only when both probes in Task 1 failed: no second swapchain on the game HWND **and** no
composition swapchain. In that case nothing in-process can own presentation, and the only remaining shape is
the one all three SDKs were designed for — owning swapchain creation from process start.

- [ ] **Step 1: STOP and ask the user**

Do not write a file. Report to the user, in these terms:

> The in-process route is closed: DXGI refused a second swapchain on the game's window (`<HRESULT>`) and a
> composition swapchain (`<HRESULT>`). The only remaining way to run frame generation is a small
> `dxgi.dll` next to `PhoenixPointWin64.exe` that forwards every DXGI export to the real
> `C:\Windows\System32\dxgi.dll` and lets Renderforge create the swapchain. That file lives **outside
> `Mods\`**, it loads even when the mod is disabled, it is the classic shape anti-cheat and overlays
> dislike, and uninstalling the mod means deleting it by hand. Do you want it?
> Alternative: frame generation ships unavailable — the FRAME GENERATION row stays greyed with a reason,
> everything else in Renderforge is unaffected.

- [ ] **Step 2: If the user says no — ship it unavailable**

In `src\FrameGen.cs` `MissingDll()`, return the localized string
`"Frame generation needs the Renderforge presentation shim — see README"` /
`"Для генерации кадров нужен модуль вывода Renderforge — смотрите README"` before the per-provider checks,
add the same statement to `README.md` and `docs/DESIGN.md`, keep Tasks 1-2's code (the Present hook and the
presented-fps counter are still correct and still feed the overlay), skip Tasks 3-5, and run Task 7.

```powershell
git -C E:\DEV\PhoenixPoint\Renderforge add -A
git -C E:\DEV\PhoenixPoint\Renderforge commit -m "feat(fg): frame generation reported unavailable - DXGI refuses a second swapchain on the game window"
```

- [ ] **Step 3: If the user says yes — scope the shim as its own phase, do not improvise it here**

Write a new plan `docs/superpowers/plans/<date>-phase5b-dxgi-shim.md` covering, at minimum: the full export
forwarder for `dxgi.dll` (every ordinal the game imports, generated from `dumpbin /exports` of the system
DLL), `DllMain` bootstrapping before Unity's first `CreateDXGIFactory*`, how `RenderforgeNative.dll` finds
the shim's captured factory/swapchain, how Streamline's own `sl.interposer.dll` slots in as the DXGI
provider instead of our forwarder for the DLSS path, an uninstall story, and a Workshop-description warning.
Then execute that plan. Do not extend this file.

---

## Exit criteria for Phase 5

- `build-native.ps1` green, including an Authenticode `Valid` check with the expected signer for all nine
  vendor DLLs (2 AMD, 2 Intel, 5 NVIDIA Streamline) plus `nvngx_dlssg.dll` pinned at 310.7.129.0.
- In-game on `D:\PP-Instance2` under `-force-d3d12`, for each of FSR-FG, XeSS-FG and DLSS-G at 2x: overlay
  screenshot showing `FPS: <real> / <presented>` with presented between 1.7× and 2.1× real, `FG: <provider>
  2x`, a sharp un-smeared HUD, and a scene indistinguishable from the no-FG baseline.
- DLSS-G at 3x and 4x on the RTX 5070 Ti: presented ≈ 3× and 4× real, `caps=0x7`.
- FSR-FG and XeSS-FG refuse 3x with `FG_ERR_UNSUPPORTED_MULTIPLIER` and no crash.
- 10-minute soak with FG live: no crash, `lastError=0` throughout, `presented` monotonic.
- Three mission loads with FG live: the chain tears down and rebuilds each time; FG off restores the plain
  picture exactly.
- `Player.log` free of `DXGI_ERROR`, `D3D12 ERROR`, `RemovedDevice` and of any exception group not already
  present in the Phase 2 D3D12 run; `renderforge_fg.log` free of failure codes.
- `docs/research/framegen-d3d12-contract.md` exists and is indexed; `DESIGN.md` and `README.md` describe
  frame generation; the FRAME GENERATION picker greys unavailable multipliers with a reason tooltip.
- `D:\Steam\steamapps\common\Phoenix Point` untouched throughout.

## Known unknowns carried into execution

These are not guesses papered over — they are the things no file on disk answers, and each has a step that
produces the answer:

1. **Two swapchains on one HWND.** Not documented as legal or illegal by DXGI. → Task 1 Step 8 measures it;
   Task 1 Step 9 branches on it.
2. **Whether Unity waits on a frame-latency object** we never signal once we stop calling its `Present`. →
   Task 1 reports `waitable=`; Task 2 Step 14 names the symptom (hang on the first FG frame) and the fix
   (an extra `DXGI_PRESENT_TEST` present through the original pointer).
3. **Whether `slUpgradeInterface` on a factory created *after* `slInit` in manual-hooking mode is accepted.**
   The guide shows the call but not this ordering. → Task 5 Step 7 records the exact `sl::Result`.
4. **Whether the FidelityFX FI swapchain tolerates a backbuffer written by `CopyResource` rather than by the
   app's own render pass.** Nothing on disk forbids it, nothing confirms it. → Task 3 Step 7's screenshot.
5. **FSR FG 4.0.1 (ML) is untestable on this machine** (RDNA4 only). We pin 3.1.6 and say so in the README.
6. **`sl::Constants::mvecScale` sign/scale convention for Unity's motion vectors.** DLSS SR needed
   `(-renderW, -renderH)` (`DlssDriver.cs:347-352`); the plan divides by the render size for Streamline's
   normalised convention. If DLSS-G ghosts on camera pans, that line is the first suspect.
7. **`recalculateCameraMatrices` keeps the previous frame in a file-static** and the header itself says
   "DO NOT USE THIS IN ANYTHING PROPER" (`sl_matrix_helpers.h:190-192`). It is correct for our single view and
   is the only helper the SDK ships; if DLSS-G's reprojection looks wrong, store the previous
   `cameraViewToWorld`/`cameraViewToClip` in `ProviderSl` and compute `clipToPrevClip` with `matrixMul` +
   `matrixFullInvert` (`sl_matrix_helpers.h:31`, `:58`) instead.

