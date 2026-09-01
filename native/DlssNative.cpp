// DlssNative.cpp - NGX D3D11 DLSS shim. Plain C style, one static state block, no threads, no exceptions.
#define DLSSNATIVE_EXPORTS
#include "DlssNative.h"

#include <d3d11.h>
#include <stdlib.h>
#include <string.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

static const char* kProjectId = "b7a3f2c4-6d1e-4a8b-9c0f-2e5d7a9b1c3d";
static const char* kEngineVersion = "2019.4.31";

static struct {
    int initCode;                 // last Dlss_Init code, DLSS_ERR_NO_DEVICE until init ran
    int ngxInitialized;
    ID3D11Device* device;
    NVSDK_NGX_Parameter* params;  // capability params, reused for create/eval
    NVSDK_NGX_Handle* feature;
    NVSDK_NGX_Result lastCreate, lastEval;
    int needsDriver;
    unsigned minDriverMajor, minDriverMinor;
    wchar_t dllDir[MAX_PATH];

    NVSDK_NGX_DLSS_Create_Params create;
    int quality;
    NVSDK_NGX_D3D11_DLSS_Eval_Params frame;
} S = { DLSS_ERR_NO_DEVICE };

static NVSDK_NGX_PerfQuality_Value ToNgxQuality(int q)
{
    switch (q) {
    case DLSS_Q_DLAA:              return NVSDK_NGX_PerfQuality_Value_DLAA;
    case DLSS_Q_BALANCED:          return NVSDK_NGX_PerfQuality_Value_Balanced;
    case DLSS_Q_PERFORMANCE:       return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case DLSS_Q_ULTRA_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default:                       return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

int __cdecl Dlss_Init(void* anyD3D11Resource, const wchar_t* dllDir, const wchar_t* logDir)
{
    if (S.ngxInitialized) return S.initCode;
    if (!anyD3D11Resource) return S.initCode = DLSS_ERR_NO_DEVICE;

    ID3D11Resource* res = NULL;
    if (FAILED(((IUnknown*)anyD3D11Resource)->QueryInterface(__uuidof(ID3D11Resource), (void**)&res)) || !res)
        return S.initCode = DLSS_ERR_NO_DEVICE;
    res->GetDevice(&S.device);
    res->Release();
    if (!S.device) return S.initCode = DLSS_ERR_NO_DEVICE;

    if (dllDir) wcsncpy_s(S.dllDir, dllDir, _TRUNCATE);
    const wchar_t* paths[1] = { S.dllDir };
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = dllDir ? 1 : 0;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY, kEngineVersion,
                                                             logDir ? logDir : L".", S.device, &common, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) { S.lastCreate = r; S.device->Release(); S.device = NULL; return S.initCode = DLSS_ERR_INIT_FAILED; }
    S.ngxInitialized = 1;

    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&S.params);
    if (NVSDK_NGX_FAILED(r) || !S.params) { S.lastCreate = r; return S.initCode = DLSS_ERR_INIT_FAILED; }

    int available = 0;
    NVSDK_NGX_Parameter_GetI(S.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(S.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &S.needsDriver);
    NVSDK_NGX_Parameter_GetUI(S.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &S.minDriverMajor);
    NVSDK_NGX_Parameter_GetUI(S.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &S.minDriverMinor);

    if (S.needsDriver) return S.initCode = DLSS_ERR_NEEDS_DRIVER;
    if (!available) return S.initCode = DLSS_ERR_NOT_AVAILABLE;
    return S.initCode = DLSS_OK;
}

int __cdecl Dlss_GetOptimal(unsigned outW, unsigned outH, int quality,
                            unsigned* renderW, unsigned* renderH,
                            unsigned* minW, unsigned* minH, unsigned* maxW, unsigned* maxH)
{
    if (!S.params) return NVSDK_NGX_Result_FAIL_NotInitialized;
    unsigned w = 0, h = 0, mnw = 0, mnh = 0, mxw = 0, mxh = 0; float sharp = 0;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(S.params, outW, outH, ToNgxQuality(quality),
                                                       &w, &h, &mxw, &mxh, &mnw, &mnh, &sharp);
    if (renderW) *renderW = w; if (renderH) *renderH = h;
    if (minW) *minW = mnw;     if (minH) *minH = mnh;
    if (maxW) *maxW = mxw;     if (maxH) *maxH = mxh;
    return r;
}

void __cdecl Dlss_SetCreateParams(unsigned w, unsigned h, unsigned outW, unsigned outH, int quality, int flags)
{
    memset(&S.create, 0, sizeof(S.create));
    S.create.Feature.InWidth = w;
    S.create.Feature.InHeight = h;
    S.create.Feature.InTargetWidth = outW;
    S.create.Feature.InTargetHeight = outH;
    S.create.Feature.InPerfQualityValue = ToNgxQuality(quality);
    S.quality = quality;
    int f = 0;
    if (flags & DLSS_F_HDR)            f |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (flags & DLSS_F_DEPTH_INVERTED) f |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (flags & DLSS_F_MV_LOW_RES)     f |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (flags & DLSS_F_MV_JITTERED)    f |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (flags & DLSS_F_AUTO_EXPOSURE)  f |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    S.create.InFeatureCreateFlags = f;
}

void __cdecl Dlss_SetFrame(void* color, void* depth, void* mv, void* output,
                           float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                           int reset, float dtMs, unsigned renderW, unsigned renderH,
                           float preExposure, float sharpness)
{
    NVSDK_NGX_D3D11_DLSS_Eval_Params* p = &S.frame;
    memset(p, 0, sizeof(*p));
    p->Feature.pInColor = (ID3D11Resource*)color;
    p->Feature.pInOutput = (ID3D11Resource*)output;
    p->Feature.InSharpness = sharpness;
    p->pInDepth = (ID3D11Resource*)depth;
    p->pInMotionVectors = (ID3D11Resource*)mv;
    p->InJitterOffsetX = jitterX;
    p->InJitterOffsetY = jitterY;
    p->InRenderSubrectDimensions.Width = renderW;
    p->InRenderSubrectDimensions.Height = renderH;
    p->InReset = reset;
    p->InMVScaleX = mvScaleX;
    p->InMVScaleY = mvScaleY;
    p->InPreExposure = preExposure;
    p->InFrameTimeDeltaInMsec = dtMs;
}

static void DoCreate(void)
{
    if (!S.params || !S.device) { S.lastCreate = NVSDK_NGX_Result_FAIL_NotInitialized; return; }
    if (S.feature) { NVSDK_NGX_D3D11_ReleaseFeature(S.feature); S.feature = NULL; }
    if (!S.create.Feature.InWidth || !S.create.Feature.InTargetWidth) { S.lastCreate = NVSDK_NGX_Result_FAIL_InvalidParameter; return; }

    // Render preset hints: K (transformer) for DLAA/Q/B, M for Perf, L for UltraPerf (header defaults per mode).
    NVSDK_NGX_Parameter_SetUI(S.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(S.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(S.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_Parameter_SetUI(S.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_DLSS_Hint_Render_Preset_M);
    NVSDK_NGX_Parameter_SetUI(S.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_DLSS_Hint_Render_Preset_L);

    ID3D11DeviceContext* ctx = NULL;
    S.device->GetImmediateContext(&ctx);
    if (!ctx) { S.lastCreate = NVSDK_NGX_Result_FAIL_PlatformError; return; }
    S.lastCreate = NGX_D3D11_CREATE_DLSS_EXT(ctx, &S.feature, S.params, &S.create);
    ctx->Release();
    if (NVSDK_NGX_FAILED(S.lastCreate)) S.feature = NULL;
}

static void DoEvaluate(void)
{
    if (!S.feature || !S.params) { S.lastEval = NVSDK_NGX_Result_FAIL_FeatureNotFound; return; }
    NVSDK_NGX_D3D11_DLSS_Eval_Params* p = &S.frame;
    if (!p->Feature.pInColor || !p->Feature.pInOutput || !p->pInDepth || !p->pInMotionVectors) { S.lastEval = NVSDK_NGX_Result_FAIL_MissingInput; return; }
    ID3D11DeviceContext* ctx = NULL;
    S.device->GetImmediateContext(&ctx);
    if (!ctx) { S.lastEval = NVSDK_NGX_Result_FAIL_PlatformError; return; }
    S.lastEval = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, S.feature, S.params, p);
    ctx->Release();
}

static void DoRelease(void)
{
    if (!S.feature) return;
    NVSDK_NGX_D3D11_ReleaseFeature(S.feature);
    S.feature = NULL;
}

static void __stdcall OnRenderEvent(int eventId)
{
    switch (eventId) {
    case DLSS_EV_CREATE:   DoCreate();   break;
    case DLSS_EV_EVALUATE: DoEvaluate(); break;
    case DLSS_EV_RELEASE:  DoRelease();  break;
    default: break;
    }
}

static void __stdcall OnRenderEventAndData(int eventId, void* /*data*/) { OnRenderEvent(eventId); }

void* __cdecl Dlss_GetRenderEventFunc(void)        { return (void*)&OnRenderEvent; }
void* __cdecl Dlss_GetRenderEventAndDataFunc(void) { return (void*)&OnRenderEventAndData; }

int __cdecl Dlss_Status(int* lastCreateResult, int* lastEvalResult, int* featureAlive)
{
    if (lastCreateResult) *lastCreateResult = (int)S.lastCreate;
    if (lastEvalResult) *lastEvalResult = (int)S.lastEval;
    if (featureAlive) *featureAlive = S.feature ? 1 : 0;
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

void __cdecl Dlss_ReleaseNow(void) { DoRelease(); }

void __cdecl Dlss_Shutdown(void)
{
    DoRelease();
    if (S.params) { NVSDK_NGX_D3D11_DestroyParameters(S.params); S.params = NULL; }
    if (S.ngxInitialized) { NVSDK_NGX_D3D11_Shutdown1(S.device); S.ngxInitialized = 0; }
    if (S.device) { S.device->Release(); S.device = NULL; }
    S.initCode = DLSS_ERR_NO_DEVICE;
}
