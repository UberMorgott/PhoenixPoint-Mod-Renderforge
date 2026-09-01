// DlssNative.h - flat C ABI of the NGX D3D11 shim consumed by the managed mod via P/Invoke.
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
enum { DLSS_OK = 0, DLSS_ERR_NO_DEVICE = 1, DLSS_ERR_INIT_FAILED = 2, DLSS_ERR_NOT_AVAILABLE = 3, DLSS_ERR_NEEDS_DRIVER = 4 };

// Quality enum (ours), mapped to NVSDK_NGX_PerfQuality_Value inside.
enum { DLSS_Q_DLAA = 0, DLSS_Q_QUALITY = 1, DLSS_Q_BALANCED = 2, DLSS_Q_PERFORMANCE = 3, DLSS_Q_ULTRA_PERFORMANCE = 4 };

// Dlss_SetCreateParams flags bitmask (ours).
enum { DLSS_F_HDR = 1, DLSS_F_DEPTH_INVERTED = 2, DLSS_F_MV_LOW_RES = 4, DLSS_F_MV_JITTERED = 8, DLSS_F_AUTO_EXPOSURE = 16 };

// Render event ids for the callbacks returned by Dlss_GetRenderEventFunc / Dlss_GetRenderEventAndDataFunc.
enum { DLSS_EV_CREATE = 1, DLSS_EV_EVALUATE = 2, DLSS_EV_RELEASE = 3 };

// Dlss_LastError codes that are not NVSDK_NGX_Result values.
enum { DLSS_ERR_PASSTHROUGH_SIZE = -1, DLSS_ERR_NO_CONTEXT = -2 };

// Main thread. anyD3D11Resource = ID3D11Resource* (Unity GetNativeTexturePtr). Idempotent.
DLSS_API int __cdecl Dlss_Init(void* anyD3D11Resource, const wchar_t* dllDir, const wchar_t* logDir);
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
// Returns Dlss_Init code; fills last NGX results (as NVSDK_NGX_Result ints) and feature liveness.
DLSS_API int __cdecl Dlss_Status(int* lastCreateResult, int* lastEvalResult, int* featureAlive);
// NVSDK_NGX_Result -> narrow string (static buffer, not thread-safe).
DLSS_API const char* __cdecl Dlss_ResultString(int ngxResult);
// Releases the feature immediately. ONLY when the render thread is idle (prefer DLSS_EV_RELEASE).
DLSS_API void __cdecl Dlss_ReleaseNow(void);
// Releases feature (if alive), params, NGX for this device, device ref. Main thread, render idle.
DLSS_API void __cdecl Dlss_Shutdown(void);

#ifdef __cplusplus
}
#endif
