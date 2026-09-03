// RenderforgeNative.h - flat C ABI of the NGX D3D11 shim consumed by the managed mod via P/Invoke.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DLSSNATIVE_EXPORTS
#define DLSS_API __declspec(dllexport)
#else
#define DLSS_API __declspec(dllimport)
#endif

// Dlss_Init return codes.
enum { DLSS_OK = 0, DLSS_ERR_NO_DEVICE = 1, DLSS_ERR_INIT_FAILED = 2, DLSS_ERR_NOT_AVAILABLE = 3, DLSS_ERR_NEEDS_DRIVER = 4,
       DLSS_ERR_NO_UNITY_IFACE = 5, DLSS_ERR_NO_PROVIDER_DLL = 6, DLSS_ERR_PROVIDER_UNSUPPORTED = 7 };

// Upscaler provider chosen by Dlss_SetProvider BEFORE Dlss_Init. 0 = the default (NVIDIA NGX).
enum { DLSS_PROVIDER_DLSS = 0, DLSS_PROVIDER_FSR = 1, DLSS_PROVIDER_XESS = 2 };

// Quality enum (ours), mapped to NVSDK_NGX_PerfQuality_Value inside. 5/6 exist only on XeSS
// (XESS_QUALITY_SETTING_ULTRA_QUALITY 1.5x / _PLUS 1.3x); DLSS and FSR treat them as Quality.
enum { DLSS_Q_DLAA = 0, DLSS_Q_QUALITY = 1, DLSS_Q_BALANCED = 2, DLSS_Q_PERFORMANCE = 3, DLSS_Q_ULTRA_PERFORMANCE = 4,
       DLSS_Q_ULTRA_QUALITY = 5, DLSS_Q_ULTRA_QUALITY_PLUS = 6 };

// Dlss_SetCreateParams flags bitmask (ours).
// DLSS_F_SRGB_VIEWS (D3D12 only, diagnostic): the colour input is sRGB-encoded - the owned colour twin is created
// as *_UNORM_SRGB so every SDK's SRV decodes it to linear (D3D12Owned.h). Output stays UNORM (no sRGB UAV in D3D12).
enum { DLSS_F_HDR = 1, DLSS_F_DEPTH_INVERTED = 2, DLSS_F_MV_LOW_RES = 4, DLSS_F_MV_JITTERED = 8, DLSS_F_AUTO_EXPOSURE = 16, DLSS_F_SRGB_VIEWS = 32 };

// Render event ids for the callbacks returned by Dlss_GetRenderEventFunc / Dlss_GetRenderEventAndDataFunc.
enum { DLSS_EV_CREATE = 1, DLSS_EV_EVALUATE = 2, DLSS_EV_RELEASE = 3 };

// Dlss_LastError codes that are not NVSDK_NGX_Result values.
enum { DLSS_ERR_PASSTHROUGH_SIZE = -1, DLSS_ERR_NO_CONTEXT = -2, DLSS_ERR_SHARPEN = -3,
       DLSS_ERR_FENCE_TIMEOUT = -4,     // D3D12: a ring slot's fence never retired (hung GPU / dead Unity fence); frame skipped
       DLSS_ERR_FFX = -5,               // FSR: an ffx-api call failed (Dlss_Status's create/eval result carries the mapped code)
       DLSS_ERR_XESS = -6 };            // XeSS: a xess* call failed (same convention; the raw xess_result_t goes to the debug log)

// Dlss_Sharpener: which sharpen shader is active. 0 = not compiled yet (first non-zero sharpness compiles it).
enum { DLSS_SHARPEN_NONE = 0, DLSS_SHARPEN_NIS = 1, DLSS_SHARPEN_RCAS = 2, DLSS_SHARPEN_FAILED = -1 };

// Main thread. anyNativeResource = ID3D11Resource* or ID3D12Resource* (Unity GetNativeTexturePtr); the API is
// chosen by QueryInterface. Idempotent once it returned DLSS_OK.
DLSS_API int __cdecl Dlss_Init(void* anyNativeResource, const wchar_t* dllDir, const wchar_t* logDir);
// Main thread. Optimal render res + dynamic range for the mode. Returns NVSDK_NGX_Result (0x1 = success).
DLSS_API int __cdecl Dlss_GetOptimal(unsigned outW, unsigned outH, int quality,
                                     unsigned* renderW, unsigned* renderH,
                                     unsigned* minW, unsigned* minH, unsigned* maxW, unsigned* maxH);
// Main thread. Stored, consumed by DLSS_EV_CREATE.
DLSS_API void __cdecl Dlss_SetCreateParams(unsigned w, unsigned h, unsigned outW, unsigned outH, int quality, int flags);
// Main thread. Next slot of a 4-deep ring of per-frame parameter blocks (round-robin). Fill it with
// Dlss_SetFrame and pass the same pointer as the `data` of DLSS_EV_EVALUATE (AndData callback).
DLSS_API void* __cdecl Dlss_GetFrameSlot(void);
// Main thread. Fills `slot` (from Dlss_GetFrameSlot). All resources = ID3D11Resource*.
// sharpness 0..1 = our sharpen compute pass on `output` after NGX (NIS sharpen-only, RCAS if NIS fails to compile; 0 = skipped);
// NGX's own InSharpness is deprecated and stays 0. A failed setup sets Dlss_LastError() = DLSS_ERR_SHARPEN and disables the
// pass; the DLSS frame is unaffected.
DLSS_API void __cdecl Dlss_SetFrame(void* slot, void* color, void* depth, void* mv, void* output,
                                    float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                                    int reset, float dtMs, unsigned renderW, unsigned renderH,
                                    float preExposure, float sharpness);
// Unity UnityRenderingEvent: void (__stdcall*)(int eventId). Event 2 here evaluates the LAST filled slot.
DLSS_API void* __cdecl Dlss_GetRenderEventFunc(void);
// Unity UnityRenderingEventAndData: void (__stdcall*)(int eventId, void* data). Event 2 reads the slot in `data`; 1/3 ignore it.
DLSS_API void* __cdecl Dlss_GetRenderEventAndDataFunc(void);
// When on, DLSS_EV_EVALUATE does CopyResource(output, color) (sizes must match) instead of NGX. Returns previous value.
DLSS_API int __cdecl Dlss_Passthrough(int on);
// Last failure: an NVSDK_NGX_Result, or one of the DLSS_ERR_* negatives above. 0 = none.
DLSS_API int __cdecl Dlss_LastError(void);
// Active sharpen shader: DLSS_SHARPEN_* (NIS / RCAS fallback / FAILED / NONE = not needed yet).
DLSS_API int __cdecl Dlss_Sharpener(void);
// Returns Dlss_Init code; fills last NGX results (as NVSDK_NGX_Result ints) and feature liveness.
DLSS_API int __cdecl Dlss_Status(int* lastCreateResult, int* lastEvalResult, int* featureAlive);
// D3D12 only, ~60-frame averages of the evaluate list: GPU ms of copy-in (Unity RTs -> twins), the
// upscale (+ sharpen), copy-out (owned out -> Unity outRT), and the CPU ms the render thread waited for a ring slot's
// fences. All 0 under D3D11 / before the first frame.
DLSS_API void __cdecl Dlss_Timings(float* copyInMs, float* evalMs, float* copyOutMs, float* ringWaitMs);
// NVSDK_NGX_Result -> narrow string (static buffer, not thread-safe).
DLSS_API const char* __cdecl Dlss_ResultString(int ngxResult);
// Releases the feature immediately. ONLY when the render thread is idle (prefer DLSS_EV_RELEASE).
DLSS_API void __cdecl Dlss_ReleaseNow(void);
// Releases feature (if alive), params, NGX for this device, device ref. Main thread, render idle.
DLSS_API void __cdecl Dlss_Shutdown(void);

// Graphics API the shim bound to: 0 = none/not initialised, 11 = D3D11, 12 = D3D12. Main thread, always safe.
DLSS_API int __cdecl Dlss_Api(void);
// Diagnostics: bit0 = UnityPluginLoad ran, bit1 = IUnityGraphicsD3D12v5 acquired. 0 under D3D11 is expected.
DLSS_API int __cdecl Dlss_UnityIface(void);
// TEST ONLY (dlss_probe): injects a stand-in IUnityGraphicsD3D12v5 so the D3D12 backend can run without Unity.
// Never called by the mod.
DLSS_API void __cdecl Dlss_TestSetUnityD3D12(void* iface);

// Main thread, BEFORE Dlss_Init (ignored afterwards). DLSS_PROVIDER_*; unknown values fall back to DLSS.
DLSS_API void __cdecl Dlss_SetProvider(int provider);
// Provider actually in use: DLSS_PROVIDER_*. -1 before Dlss_Init.
DLSS_API int __cdecl Dlss_Provider(void);
// Human-readable version of the live provider ("4.1.1" / "3.1.5" for FSR; "2.0.2 DP4a" / "2.0.2 XMX" for XeSS, the
// suffix = execution path; empty for NGX, whose runtime version the managed side reads off nvngx_dlss.dll),
// NUL-terminated into `buf` (at most `cap` bytes incl. NUL). Returns the number of bytes written, 0 when unknown.
DLSS_API int __cdecl Dlss_ProviderVersion(char* buf, int cap);
// Main thread. Camera parameters FSR/XeSS need and NGX does not; cached and copied into every later frame slot.
// fovYRadians = vertical field of view in RADIANS. Defaults: 0.1 / 1000 / 60 degrees.
DLSS_API void __cdecl Dlss_SetCamera(float nearZ, float farZ, float fovYRadians);

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

// Presented frames per second counted in the Present hook (includes generated frames). 0 = no data yet.
DLSS_API int __cdecl Fg_PresentedFps(void);

// Main thread. Builds the FG chain: hook (if not yet), provider, shadow swapchain. Retry-safe: returns
// FG_ERR_NO_SWAPCHAIN until the hook has seen at least one Present and while a previous chain is still being
// destroyed, so the caller may call it per frame.
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
// Why the last Fg_Init failed, static text ("" when unknown / none): the picker's tooltip.
DLSS_API const char* __cdecl Fg_Reason(void);
// Main thread. Destroys the chain SYNCHRONOUSLY (waits for the render thread to leave it) and returns 1; 0 = the
// render thread never left within the bound and the chain stays parked (Fg_Pump retries). The hook stays inert.
DLSS_API int __cdecl Fg_Shutdown(void);
// Main thread, every frame. The render/UI threads only DETACH a chain (resize, Present failure, window recreated);
// this destroys it. Returns 1 when nothing is left to destroy.
DLSS_API int __cdecl Fg_Pump(void);
// 1 while the chain exists. 0 once the shim detached it itself (ResizeBuffers, shadow Present failure):
// the managed side must drop `live`, keep pumping and call Fg_Init again.
DLSS_API int __cdecl Fg_Alive(void);

#ifdef __cplusplus
}
#endif
