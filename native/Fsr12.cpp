// Fsr12.cpp - AMD FSR Super Resolution (FidelityFX SDK 2.3 ffx-api) as an IDevice provider, D3D12 only.
//
// Submission is identical to Device12: ffxDispatch records into our own DIRECT command list, which goes to
// Unity through IUnityGraphicsD3D12v5::ExecuteCommandList. The resources are the shim's own (D3D12Owned.h):
// we barrier them from their rest state to what FSR requires (inputs NON_PIXEL_SHADER_RESOURCE, output
// UNORDERED_ACCESS - super-resolution-ml.md:104) and back; the ffx DX12 backend restores every app-provided
// resource to the state named in FfxApiResource at the end of the dispatch (ffx_dx12.cpp:2413-2426).
//
// Version: no ffxOverrideVersion is used. The upscaler DLL picks the newest provider the GPU supports
// (4.1.1 needs an AMD RX 7000/9000-class GPU, super-resolution-ml.md:325; everything else lands on 3.1.5),
// and ffxQueryGetProviderVersion tells us which one we actually got.
#include "Device.h"
#include "RenderforgeNative.h"
#include "D3D12Ring.h"
#include "D3D12Owned.h"
#include "D3D12Sharpen.h"
#include "FfxLoader.h"
#include "D3D12Debug.h"

#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffx_api_types.h"
#include "ffx_api_dx12.h"
#include "ffx_upscale.h"

namespace {

uint32_t ToFfxQuality(int quality)
{
    switch (quality) {
    case DLSS_Q_DLAA:              return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;          // 1.0x - "Native AA"
    case DLSS_Q_BALANCED:          return FFX_UPSCALE_QUALITY_MODE_BALANCED;          // 1.7x
    case DLSS_Q_PERFORMANCE:       return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;       // 2.0x
    case DLSS_Q_ULTRA_PERFORMANCE: return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE; // 3.0x
    default:                       return FFX_UPSCALE_QUALITY_MODE_QUALITY;           // 1.5x
    }
}

// DLSS_F_* -> FfxApiCreateContextUpscaleFlags. DEPTH_INFINITE is deliberately never set: Unity's camera has a
// finite far plane and the FSR debug checker warns when INFINITE is combined with a small cameraFar.
// AUTO_EXPOSURE is always on regardless of DLSS_F_AUTO_EXPOSURE: the driver never hands us an exposure texture,
// and ffx only allows omitting it when that flag is set (super-resolution-ml.md:111,167).
// HIGH_DYNAMIC_RANGE ("the input color data provided is using a high-dynamic range", ffx_upscale.h:49) is never set:
// the FP16 colour (D3D12HalfColor) holds display-referred linear 0..1 - LDR, like NGX IsHDR=0. DLSS_F_HDR is ignored.
uint32_t ToFfxCreateFlags(int rawFlags)
{
    uint32_t f = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (rawFlags & DLSS_F_DEPTH_INVERTED) f |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;       // Unity reversed-Z
    if (rawFlags & DLSS_F_MV_JITTERED)    f |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    // DLSS_F_MV_LOW_RES means "MVs are at render resolution", which is the ffx default; its ABSENCE is the flag.
    if (!(rawFlags & DLSS_F_MV_LOW_RES))  f |= FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    return f;
}

void FfxMessage(uint32_t type, const wchar_t* message)
{
    OutputDebugStringW(type == FFX_API_MESSAGE_TYPE_ERROR ? L"[Renderforge][FFX][error] " : L"[Renderforge][FFX][warn] ");
    OutputDebugStringW(message ? message : L"(null)");
    OutputDebugStringW(L"\n");
}

// Static-scene A/B knob: env "sx,sy" with each in {-1,1} overrides the jitter axis signs; anything else keeps the defaults.
void ReadJitterSign(const char* env, float* sx, float* sy)
{
    char buf[32]; size_t n = 0; int x = 0, y = 0;
    if (getenv_s(&n, buf, sizeof(buf), env) == 0 && n && sscanf_s(buf, "%d,%d", &x, &y) == 2
        && (x == 1 || x == -1) && (y == 1 || y == -1)) { *sx = (float)x; *sy = (float)y; }
}

struct Fsr12 : IDevice
{
    ID3D12Device* device;
    D3D12Ring ring;
    OwnedSet12 owned;                          // the resources ffx touches; Unity's RTs are only copied (D3D12Owned.h)
    SharpenPass12 post;                        // analytic color grade after FSR's built-in RCAS
    const ffxFunctions* ffx;
    ffxContext context;
    unsigned outW, outH;                       // upscaleSize of the live context
    bool srgbViews;                            // DLSS_F_SRGB_VIEWS of the live context -> owned.Ensure (D3D12Owned.h Typed)
    float jitterSignX, jitterSignY;            // applied to fp.jitter in Evaluate; RENDERFORGE_FSR_JITTER_SIGN overrides
    char version[64];                         // provider version name, copied out of ffx global memory at once
    wchar_t dllDir[MAX_PATH];

    // The create-descriptor chain MUST stay alive until ffxDestroyContext (ffx_api.h:140), so it lives here.
    struct ffxCreateContextDescUpscale        descUpscale;
    struct ffxCreateContextDescUpscaleVersion descVersion;
    struct ffxCreateBackendDX12Desc           descBackend;

    Fsr12() { Zero(); }

    void Zero()
    {
        device = NULL; ffx = NULL; context = NULL;
        outW = outH = 0; version[0] = 0; dllDir[0] = 0; srgbViews = false;
        jitterSignX = 1.0f; jitterSignY = -1.0f;    // see Evaluate: FSR wants J = (+fp.jitterX, -fp.jitterY)
        memset(&descUpscale, 0, sizeof(descUpscale));
        memset(&descVersion, 0, sizeof(descVersion));
        memset(&descBackend, 0, sizeof(descBackend));
        ring.Zero();
        owned.Zero();
        post.Zero();
        lastCreate = (NVSDK_NGX_Result)0; lastEval = (NVSDK_NGX_Result)0; lastError = 0; initCode = 0;
        sharpener = DLSS_SHARPEN_NONE;   // becomes RCAS per Evaluate when sharpness > 0; post may also grade
        sharpenDead = 0;
    }

    int Api() const override { return 12; }
    bool FeatureAlive() const override { return context != NULL; }
    const OwnedSet12* Owned12() const override { return &owned; }
    const D3D12Ring* Ring12() const override { return &ring; }

    int ProviderVersion(char* buf, int cap) override
    {
        if (!buf || cap <= 0) return 0;
        strncpy_s(buf, (size_t)cap, version, _TRUNCATE);
        return (int)strlen(buf);
    }

    // ffx return code -> the NVSDK_NGX_Result the ABI already speaks.
    static NVSDK_NGX_Result Map(ffxReturnCode_t rc)
    {
        switch (rc) {
        case FFX_API_RETURN_OK:                 return NVSDK_NGX_Result_Success;
        case FFX_API_RETURN_ERROR_PARAMETER:    return NVSDK_NGX_Result_FAIL_InvalidParameter;
        case FFX_API_RETURN_ERROR_MEMORY:       return NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
        case FFX_API_RETURN_NO_PROVIDER:        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        default:                                return NVSDK_NGX_Result_FAIL_PlatformError;
        }
    }

    // ---- IDevice -----------------------------------------------------------

    int Init(void* nativeResource, const wchar_t* inDllDir, const wchar_t* logDir) override
    {
        (void)logDir;
        if (ffx) return initCode;              // replay the real outcome, not a blanket OK
        ID3D12Resource* res = NULL;
        if (FAILED(((IUnknown*)nativeResource)->QueryInterface(__uuidof(ID3D12Resource), (void**)&res)) || !res)
            return DLSS_ERR_NO_DEVICE;
        HRESULT hr = res->GetDevice(IID_PPV_ARGS(&device));
        res->Release();
        if (FAILED(hr) || !device) return DLSS_ERR_NO_DEVICE;

        if (!g_unityD3D12) { device->Release(); device = NULL; return DLSS_ERR_NO_UNITY_IFACE; }
        ring.Attach(device);
        post.Attach(device, this);
        RfDbg::Attach(device);
        ReadJitterSign("RENDERFORGE_FSR_JITTER_SIGN", &jitterSignX, &jitterSignY);
        RfDbg::Log("FSR: jitterSign=%d,%d", (int)jitterSignX, (int)jitterSignY);

        if (inDllDir) wcsncpy_s(dllDir, inDllDir, _TRUNCATE);
        ffx = FfxLoad(dllDir);
        if (!ffx) { device->Release(); device = NULL; ring.Zero(); return DLSS_ERR_NO_PROVIDER_DLL; }

        // Prove a provider exists for this device before claiming availability: count the upscale versions.
        uint64_t count = 0;
        struct ffxQueryDescGetVersions q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = device;
        q.outputCount = &count;
        ffxReturnCode_t rc = ffx->Query(NULL, &q.header);
        if (rc != FFX_API_RETURN_OK || count == 0) {
            lastCreate = Map(rc); lastError = DLSS_ERR_FFX;
            return initCode = DLSS_ERR_NOT_AVAILABLE;
        }
        return initCode = DLSS_OK;
    }

    NVSDK_NGX_Result GetOptimal(unsigned oW, unsigned oH, int quality,
                                unsigned* renderW, unsigned* renderH,
                                unsigned* minW, unsigned* minH,
                                unsigned* maxW, unsigned* maxH) override
    {
        if (!ffx || !device) return NVSDK_NGX_Result_FAIL_NotInitialized;
        uint32_t rw = 0, rh = 0;
        struct ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        q.displayWidth = oW;
        q.displayHeight = oH;
        q.qualityMode = ToFfxQuality(quality);
        q.pOutRenderWidth = &rw;
        q.pOutRenderHeight = &rh;
        // A null-context query without an embedded device must chain the backend desc, or it returns
        // FFX_API_RETURN_NO_PROVIDER (ffx-api.md:133).
        struct ffxCreateBackendDX12Desc backend = {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        q.header.pNext = &backend.header;

        ffxReturnCode_t rc = ffx->Query(NULL, &q.header);
        if (rc != FFX_API_RETURN_OK || !rw || !rh) { lastError = DLSS_ERR_FFX; return Map(rc); }
        // FSR has no dynamic-resolution range unless FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION is set: one size.
        if (renderW) *renderW = rw; if (renderH) *renderH = rh;
        if (minW) *minW = rw;       if (minH) *minH = rh;
        if (maxW) *maxW = rw;       if (maxH) *maxH = rh;
        return NVSDK_NGX_Result_Success;
    }

    bool DestroyContext()
    {
        if (!BeginDestroy()) return false;
        if (!ring.WaitIdle(0) && !ring.Removed()) { EndDestroy(); return false; }
        if (context && ffx->DestroyContext(&context, NULL) != FFX_API_RETURN_OK) { EndDestroy(); return false; }
        context = NULL;
        version[0] = 0;
        owned.Release();                       // idle above; the next Create's first dispatch re-creates the set
        EndDestroy();
        return true;
    }

    void Create(const CreateParams& cp) override
    {
        if (!ffx || !device || !g_unityD3D12) { lastCreate = NVSDK_NGX_Result_FAIL_NotInitialized; return; }
        if (!DestroyContext()) { lastCreate = NVSDK_NGX_Result_FAIL_PlatformError; return; }
        if (!cp.w || !cp.outW) { lastCreate = NVSDK_NGX_Result_FAIL_InvalidParameter; return; }

        memset(&descUpscale, 0, sizeof(descUpscale));
        memset(&descVersion, 0, sizeof(descVersion));
        memset(&descBackend, 0, sizeof(descBackend));

        descUpscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        descUpscale.flags = ToFfxCreateFlags(cp.rawFlags);
        descUpscale.maxRenderSize.width = cp.w;
        descUpscale.maxRenderSize.height = cp.h;
        descUpscale.maxUpscaleSize.width = cp.outW;
        descUpscale.maxUpscaleSize.height = cp.outH;
        descUpscale.fpMessage = &FfxMessage;

        // Required since SDK 2.1 (super-resolution-ml.md:99): declare the API version we were built against.
        descVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        descVersion.version = FFX_UPSCALER_VERSION;

        descBackend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        descBackend.device = device;

        descUpscale.header.pNext = &descVersion.header;
        descVersion.header.pNext = &descBackend.header;

        ffxReturnCode_t rc = ffx->CreateContext(&context, &descUpscale.header, NULL);
        lastCreate = Map(rc);
        if (rc != FFX_API_RETURN_OK) { context = NULL; lastError = DLSS_ERR_FFX; return; }
        outW = cp.outW; outH = cp.outH;
        srgbViews = cp.SrgbViews();

        // Which provider did we actually get? The name lives in ffx global memory and a later query may
        // overwrite it (ffx-api.md:243), so copy it now.
        struct ffxQueryGetProviderVersion pv = {};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (ffx->Query(&context, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
            strncpy_s(version, pv.versionName, _TRUNCATE);
        else
            version[0] = 0;
    }

    static bool SameSize(ID3D12Resource* a, ID3D12Resource* b)
    {
        D3D12_RESOURCE_DESC da = a->GetDesc(), db = b->GetDesc();
        return da.Width == db.Width && da.Height == db.Height;
    }

    void Evaluate(const FrameParams& fp, bool passthrough) override
    {
        ID3D12Resource* color  = (ID3D12Resource*)fp.color;
        ID3D12Resource* output = (ID3D12Resource*)fp.output;
        if (!ffx || !device || !g_unityD3D12) { lastEval = NVSDK_NGX_Result_FAIL_NotInitialized; lastError = (int)lastEval; return; }
        if (!color || !output || (!passthrough && (!fp.depth || !fp.mv))) {
            lastEval = NVSDK_NGX_Result_FAIL_MissingInput; lastError = (int)lastEval; return;
        }
        if (!passthrough && !context) {
            lastEval = NVSDK_NGX_Result_FAIL_FeatureNotFound; lastError = (int)lastEval; return;
        }
        if (passthrough && !SameSize(color, output)) {
            lastEval = NVSDK_NGX_Result_FAIL_InvalidParameter; lastError = DLSS_ERR_PASSTHROUGH_SIZE; return;
        }

        ID3D12GraphicsCommandList* cl = ring.Begin();
        if (!cl) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; lastError = ring.failCode; return; }

        // Unity's RTs are only copy sources / the copy destination of this list (declared so); ffx sees the owned set.
        ID3D12Resource* depth = (ID3D12Resource*)fp.depth;
        ID3D12Resource* mv    = (ID3D12Resource*)fp.mv;
        UnityGraphicsD3D12ResourceState* st = ring.StateSlot();
        int n = OwnedSet12::Declare(st, color, passthrough ? NULL : depth, passthrough ? NULL : mv, output);
        if (passthrough) {
            bool wantPost = fp.sharpness > 0.0f || (ColorGradeEnabled(fp.lutPreset, fp.lutStrength) || SceneStyleEnabled(fp.style));
            if (!wantPost || !post.RunPassthrough(cl, color, output, owned, ring, srgbViews,
                                                   fp.sharpness, fp.lutPreset, fp.lutStrength, ring.ringIdx, fp.style))
                OwnedSet12::Passthrough(cl, color, output);
            lastEval = NVSDK_NGX_Result_Success;
        } else if (!owned.Ensure(device, ring, color, output, srgbViews)) {
            lastEval = NVSDK_NGX_Result_FAIL_OutOfGPUMemory; lastError = (int)lastEval;
        } else {
            bool grade = (ColorGradeEnabled(fp.lutPreset, fp.lutStrength) || SceneStyleEnabled(fp.style));
            bool doPost = grade && post.TargetEnsure(owned.out, ring, true);
            struct ffxDispatchDescUpscale d = {};
            d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            d.commandList = cl;
            d.color         = ffxApiGetResourceDX12(owned.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.depth         = ffxApiGetResourceDX12(owned.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.motionVectors = ffxApiGetResourceDX12(owned.mv,    FFX_API_RESOURCE_STATE_COMPUTE_READ);
            d.output        = ffxApiGetResourceDX12(doPost ? post.target : owned.out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            // exposure / reactive / transparencyAndComposition stay empty: AUTO_EXPOSURE is on and both masks
            // are optional for FSR 3.1 and FSR 4 (super-resolution-ml.md:154).

            // Jitter. The driver applies proj[0,2] += 2*jx/w, proj[1,2] += 2*jy/h and hands us fp = (-jx,-jy) (NGX).
            // super-resolution-ml.md:233-236: "jitterX = 2*Jx/W; jitterY = -2*Jy/H; jittered = translate(jitterX,jitterY) * P";
            // with P[3][2] = -1 that is proj[0,2] += -2*Jx/W, proj[1,2] += +2*Jy/H, so Jx = -jx = fp.jitterX, Jy = jy = -fp.jitterY.
            // ffx_upscale.h:98 only says "The subpixel jitter offset applied to the camera" (no sign). Measured in-game
            // 2026-09-03 (FSR Quality, static scene): the old (-X,+Y) shook 0.405, this (+X,-Y) 0.248.
            d.jitterOffset.x = jitterSignX * fp.jitterX;
            d.jitterOffset.y = jitterSignY * fp.jitterY;

            // Motion vectors: Unity's texture is (current - previous) in UV space; the driver's negative scale
            // turns it into current->previous pixels, which is exactly FSR's convention and range
            // ([<-w,-h>..<w,h>], super-resolution-ml.md:129). Pass the scale straight through.
            d.motionVectorScale.x = fp.mvScaleX;
            d.motionVectorScale.y = fp.mvScaleY;

            d.renderSize.width   = fp.renderW;
            d.renderSize.height  = fp.renderH;
            d.upscaleSize.width  = outW;
            d.upscaleSize.height = outH;
            d.enableSharpening = fp.sharpness > 0.0f;      // FSR's built-in RCAS; the shim's own pass is skipped
            d.sharpness        = fp.sharpness;
            sharpener = d.enableSharpening ? DLSS_SHARPEN_RCAS : DLSS_SHARPEN_NONE;   // report what this frame runs
            d.frameTimeDelta   = fp.dtMs;                  // milliseconds (super-resolution-ml.md:280)
            d.preExposure      = fp.preExposure > 0.0f ? fp.preExposure : 1.0f;   // must be > 0
            d.reset            = fp.reset != 0;
            d.cameraNear       = fp.nearZ;
            d.cameraFar        = fp.farZ;
            d.cameraFovAngleVertical = fp.fovY;
            d.viewSpaceToMetersFactor = 1.0f;
            d.flags = 0;

            // The ffx DX12 backend restores each resource to the state named in FfxApiResource after the dispatch
            // (ffx_dx12.cpp UnregisterResourcesDX12), so Leave starts from the SDK states we entered.
            owned.Enter(cl, color, depth, mv);
            ring.Stamp(1);
            ffxReturnCode_t rc = ffx->Dispatch(&context, &d.header);
            lastEval = Map(rc);
            if (rc != FFX_API_RETURN_OK) lastError = DLSS_ERR_FFX;
            else if (doPost) post.Run(cl, owned.out, 0.0f, fp.lutPreset, fp.lutStrength, ring.ringIdx, fp.style);
            ring.Stamp(2);
            owned.Leave(cl, output);
        }
        if (!ring.End(n)) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; lastError = DLSS_ERR_NO_CONTEXT; }
    }

    bool ReleaseFeature() override { return DestroyContext(); }

    bool Shutdown() override
    {
        if (!DestroyContext() || !ring.Release()) return false;
        owned.Release();
        post.Release();
        if (device) { device->Release(); device = NULL; }
        Zero();                 // the loaded AMD modules stay resident; FfxLoad is idempotent by design
        return true;
    }
};

Fsr12 g_fsr12;

} // namespace

IDevice* MakeFsr12(void* nativeResource)
{
    if (!nativeResource) return NULL;
    ID3D12Resource* res = NULL;
    if (FAILED(((IUnknown*)nativeResource)->QueryInterface(__uuidof(ID3D12Resource), (void**)&res)) || !res) return NULL;
    res->Release();
    return &g_fsr12;
}
