// RenderforgeNative.cpp - the flat C ABI, the frame-slot ring and the Unity render-event dispatch.
// All D3D lives behind IDevice (Device11.cpp / Device12.cpp). No threads of our own, no exceptions.
#include "RenderforgeNative.h"    // DLSSNATIVE_EXPORTS comes from CMake (target-wide, every TU sees dllexport)
#include "Device.h"

#include <string.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphics.h"
#include <d3d12.h>
#include "unity/IUnityGraphicsD3D12.h"
#include "D3D12Debug.h"

const char kProjectId[] = "b7a3f2c4-6d1e-4a8b-9c0f-2e5d7a9b1c3d";
const char kEngineVersion[] = "2019.4.31";

// ---------------------------------------------------------------- Unity plugin interface

IUnityGraphicsD3D12v5* g_unityD3D12 = NULL;    // read by Device12.cpp
static IUnityInterfaces* g_unityIfaces = NULL;
static IUnityGraphics* g_unityGfx = NULL;
static int g_unityLoaded = 0;

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
    if (eventType == kUnityGfxDeviceEventInitialize) {
        if (g_unityIfaces && g_unityGfx && g_unityGfx->GetRenderer() == kUnityGfxRendererD3D12)
            g_unityD3D12 = g_unityIfaces->Get<IUnityGraphicsD3D12v5>();
    } else if (eventType == kUnityGfxDeviceEventShutdown) {
        g_unityD3D12 = NULL;
    }
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    RfDbg::EarlyEnable();
    g_unityIfaces = unityInterfaces;
    g_unityLoaded = 1;
    if (!unityInterfaces) return;
    g_unityGfx = unityInterfaces->Get<IUnityGraphics>();
    if (g_unityGfx) g_unityGfx->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
    // The device already exists when a plugin is loaded late, so the Initialize event is never delivered.
    OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
    // Ordering proof: a non-NULL device here means Unity created it before we were loaded, i.e. EarlyEnable
    // above was too late and the debug layer must come from Unity's own -force-d3d12-debug switch.
    RfDbg::Log("UnityPluginLoad: renderer=%d unityD3D12=%p device=%p",
               g_unityGfx ? (int)g_unityGfx->GetRenderer() : -1, (void*)g_unityD3D12,
               (void*)(g_unityD3D12 ? g_unityD3D12->GetDevice() : NULL));
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload(void)
{
    if (g_unityGfx) g_unityGfx->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    g_unityGfx = NULL; g_unityIfaces = NULL; g_unityD3D12 = NULL; g_unityLoaded = 0;
}

// ---------------------------------------------------------------- shared state

static struct {
    int initCode;
    IDevice* dev;
    CreateParams create;
    // Ring of per-frame blocks: the main thread fills slot N while the render thread may still read N-1..N-3.
    FrameParams slots[4];
    unsigned slotIdx;
    FrameParams* lastSlot;
    int passthrough;
    int provider;                       // DLSS_PROVIDER_*, latched by Dlss_Init
    int wantProvider;                   // what Dlss_SetProvider asked for
    float nearZ, farZ, fovY;            // Dlss_SetCamera cache, copied into every slot
} S = { DLSS_ERR_NO_DEVICE, NULL, {}, {}, 0u, NULL, 0, DLSS_PROVIDER_DLSS, DLSS_PROVIDER_DLSS, 0.1f, 1000.0f, 1.0471976f };

NVSDK_NGX_PerfQuality_Value ToNgxQuality(int q)
{
    switch (q) {
    case DLSS_Q_DLAA:              return NVSDK_NGX_PerfQuality_Value_DLAA;
    case DLSS_Q_BALANCED:          return NVSDK_NGX_PerfQuality_Value_Balanced;
    case DLSS_Q_PERFORMANCE:       return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case DLSS_Q_ULTRA_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default:                       return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

// Render preset hints: K (transformer) for DLAA/Q/B, M for Perf, L for UltraPerf (header defaults per mode).
void SetPresetHints(NVSDK_NGX_Parameter* params)
{
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_DLSS_Hint_Render_Preset_M);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_DLSS_Hint_Render_Preset_L);
}

// ---------------------------------------------------------------- exports

int __cdecl Dlss_Init(void* anyNativeResource, const wchar_t* dllDir, const wchar_t* logDir)
{
    if (S.dev && S.initCode == DLSS_OK) return S.initCode;
    if (!anyNativeResource) return S.initCode = DLSS_ERR_NO_DEVICE;

    IDevice* d = NULL;
    if (S.wantProvider == DLSS_PROVIDER_FSR) {
        d = MakeFsr12(anyNativeResource);                  // D3D12 only; NULL on a D3D11 resource
        if (!d) return S.initCode = DLSS_ERR_PROVIDER_UNSUPPORTED;
        S.provider = DLSS_PROVIDER_FSR;
    } else {
        if (S.wantProvider == DLSS_PROVIDER_XESS) return S.initCode = DLSS_ERR_PROVIDER_UNSUPPORTED;  // Phase 4
        d = MakeDevice11(anyNativeResource);
        if (!d) d = MakeDevice12(anyNativeResource);
        if (!d) return S.initCode = DLSS_ERR_NO_DEVICE;
        S.provider = DLSS_PROVIDER_DLSS;
    }

    S.dev = d;                       // kept even on failure so Dlss_Api()/Dlss_Status() still answer
    return S.initCode = d->Init(anyNativeResource, dllDir, logDir);
}

int __cdecl Dlss_GetOptimal(unsigned outW, unsigned outH, int quality,
                            unsigned* renderW, unsigned* renderH,
                            unsigned* minW, unsigned* minH, unsigned* maxW, unsigned* maxH)
{
    if (!S.dev) return NVSDK_NGX_Result_FAIL_NotInitialized;
    return S.dev->GetOptimal(outW, outH, quality, renderW, renderH, minW, minH, maxW, maxH);
}

void __cdecl Dlss_SetCreateParams(unsigned w, unsigned h, unsigned outW, unsigned outH, int quality, int flags)
{
    memset(&S.create, 0, sizeof(S.create));
    S.create.w = w; S.create.h = h; S.create.outW = outW; S.create.outH = outH;
    S.create.quality = quality;
    S.create.rawFlags = flags;
    int f = 0;
    if (flags & DLSS_F_HDR)            f |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (flags & DLSS_F_DEPTH_INVERTED) f |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (flags & DLSS_F_MV_LOW_RES)     f |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (flags & DLSS_F_MV_JITTERED)    f |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (flags & DLSS_F_AUTO_EXPOSURE)  f |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    S.create.ngxFlags = f;
}

void* __cdecl Dlss_GetFrameSlot(void)
{
    return &S.slots[S.slotIdx++ & 3u];
}

void __cdecl Dlss_SetFrame(void* slot, void* color, void* depth, void* mv, void* output,
                           float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                           int reset, float dtMs, unsigned renderW, unsigned renderH,
                           float preExposure, float sharpness)
{
    FrameParams* p = (FrameParams*)slot;
    if (!p) return;
    S.lastSlot = p;
    memset(p, 0, sizeof(*p));
    p->color = color; p->depth = depth; p->mv = mv; p->output = output;
    p->jitterX = jitterX; p->jitterY = jitterY;
    p->mvScaleX = mvScaleX; p->mvScaleY = mvScaleY;
    p->reset = reset;
    p->dtMs = dtMs;
    p->renderW = renderW; p->renderH = renderH;
    p->preExposure = preExposure;
    p->sharpness = sharpness < 0 ? 0 : sharpness > 1 ? 1 : sharpness;
    p->nearZ = S.nearZ; p->farZ = S.farZ; p->fovY = S.fovY;
}

static void __stdcall OnRenderEventAndData(int eventId, void* data)
{
    if (!S.dev) return;
    if (RfDbg::NoEvents()) return;   // RENDERFORGE_D3D12_NOEVENTS=1: managed side runs, no GPU work of ours
    switch (eventId) {
    case DLSS_EV_CREATE:
        S.dev->Create(S.create);
        break;
    case DLSS_EV_EVALUATE: {
        FrameParams* p = (FrameParams*)data;
        if (!p) p = S.lastSlot;
        if (!p) { S.dev->lastEval = NVSDK_NGX_Result_FAIL_MissingInput; S.dev->lastError = (int)S.dev->lastEval; break; }
        S.dev->Evaluate(*p, S.passthrough != 0);
        break;
    }
    case DLSS_EV_RELEASE:
        S.dev->ReleaseFeature();
        break;
    default:
        break;
    }
}

static void __stdcall OnRenderEvent(int eventId) { OnRenderEventAndData(eventId, NULL); }

void* __cdecl Dlss_GetRenderEventFunc(void)        { return (void*)&OnRenderEvent; }
void* __cdecl Dlss_GetRenderEventAndDataFunc(void) { return (void*)&OnRenderEventAndData; }

int __cdecl Dlss_Passthrough(int on) { int prev = S.passthrough; S.passthrough = on ? 1 : 0; return prev; }
int __cdecl Dlss_LastError(void)     { return S.dev ? S.dev->lastError : 0; }
int __cdecl Dlss_Sharpener(void)     { return S.dev ? (S.dev->sharpenDead ? DLSS_SHARPEN_FAILED : S.dev->sharpener) : DLSS_SHARPEN_NONE; }
int __cdecl Dlss_Api(void)           { return S.dev ? S.dev->Api() : 0; }
int __cdecl Dlss_UnityIface(void)    { return (g_unityLoaded ? 1 : 0) | (g_unityD3D12 ? 2 : 0); }

// Probe-only hook: honoured only while no real UnityPluginLoad has happened, so inside the game it is a no-op.
void __cdecl Dlss_TestSetUnityD3D12(void* iface) { if (!g_unityLoaded) g_unityD3D12 = (IUnityGraphicsD3D12v5*)iface; }

void __cdecl Dlss_SetProvider(int provider)
{
    if (S.dev) return;                                     // latched at Dlss_Init; a later call is a no-op
    S.wantProvider = (provider == DLSS_PROVIDER_FSR || provider == DLSS_PROVIDER_XESS) ? provider : DLSS_PROVIDER_DLSS;
}

int __cdecl Dlss_Provider(void) { return S.dev ? S.provider : -1; }

int __cdecl Dlss_ProviderVersion(char* buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!S.dev) return 0;
    return S.dev->ProviderVersion(buf, cap);
}

void __cdecl Dlss_SetCamera(float nearZ, float farZ, float fovYRadians)
{
    if (nearZ > 0.0f) S.nearZ = nearZ;
    if (farZ > 0.0f) S.farZ = farZ;
    if (fovYRadians > 0.0f) S.fovY = fovYRadians;
}

int __cdecl Dlss_Status(int* lastCreateResult, int* lastEvalResult, int* featureAlive)
{
    if (lastCreateResult) *lastCreateResult = S.dev ? (int)S.dev->lastCreate : 0;
    if (lastEvalResult)   *lastEvalResult   = S.dev ? (int)S.dev->lastEval : 0;
    if (featureAlive)     *featureAlive     = (S.dev && S.dev->FeatureAlive()) ? 1 : 0;
    return S.initCode;
}

const char* __cdecl Dlss_ResultString(int ngxResult)
{
    static char buf[256];
    const wchar_t* w = GetNGXResultAsString((NVSDK_NGX_Result)ngxResult);
    if (!w) return "NVSDK_NGX_Result_Unknown";
    size_t n = 0;
    wcstombs_s(&n, buf, sizeof(buf), w, _TRUNCATE);
    return buf;
}

void __cdecl Dlss_ReleaseNow(void) { if (S.dev) S.dev->ReleaseFeature(); }

void __cdecl Dlss_Shutdown(void)
{
    if (S.dev) S.dev->Shutdown();
    S.dev = NULL;
    S.lastSlot = NULL;
    S.initCode = DLSS_ERR_NO_DEVICE;
}
