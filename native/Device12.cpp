// Device12.cpp - NGX DLSS on D3D12.
//
// Submission model: we record NGX's work into our own DIRECT command list and hand it to Unity via
// IUnityGraphicsD3D12v5::ExecuteCommandList together with the resource states NGX requires. Unity owns the
// queue and tracks the state of its own RenderTextures, so it inserts the transition barriers for us and
// orders our list against its own rendering of colorRT and its present blit of outRT. A private queue would
// race with both. Slot reuse waits on the fence value ExecuteCommandList returned (Unity's frame fence).
//
// Required states (DLSS Programming Guide p.14 3.4): colour/depth/motion vectors in
// D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, output in D3D12_RESOURCE_STATE_UNORDERED_ACCESS.
// The guide also states DLSS "always transitions buffers back to these known states", so expected == current.
#include "Device.h"
#include "RenderforgeNative.h"

#include <d3d12.h>
#include <string.h>

#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphicsD3D12.h"
#include "D3D12Ring.h"
#include "D3D12Sharpen.h"
#include "D3D12Debug.h"
#include "nvsdk_ngx_helpers.h"

namespace {

struct Device12 : IDevice
{
    ID3D12Device* device;
    NVSDK_NGX_Parameter* params;
    NVSDK_NGX_Handle* feature;
    int ngxInitialized;
    int needsDriver;
    unsigned minDriverMajor, minDriverMinor;
    wchar_t dllDir[MAX_PATH];

    D3D12Ring ring;               // command allocators/lists + Unity fence waits (D3D12Ring.h)
    SharpenPass12 sharpen;        // our own NIS/RCAS pass; NGX writes sharpen.target when it runs (D3D12Sharpen.h)

    ID3D12Resource* logged;        // RENDERFORGE_D3D12_DEBUG: output whose descs were already logged

    Device12() { Zero(); }

    void Zero()
    {
        device = NULL; params = NULL; feature = NULL;
        ngxInitialized = 0; initCode = 0; needsDriver = 0; minDriverMajor = minDriverMinor = 0; dllDir[0] = 0;
        ring.Zero();
        sharpen.Zero();
        lastCreate = (NVSDK_NGX_Result)0; lastEval = (NVSDK_NGX_Result)0; lastError = 0; sharpener = 0; sharpenDead = 0;
        logged = NULL;
    }

    int Api() const override { return 12; }
    bool FeatureAlive() const override { return feature != NULL; }

    // Ring wrappers: Begin() reports its failure through failCode, which is this device's lastError.
    ID3D12GraphicsCommandList* Begin()
    {
        ID3D12GraphicsCommandList* cl = ring.Begin();
        if (!cl) lastError = ring.failCode;
        return cl;
    }
    bool End(int n) { return ring.End(n); }
    void WaitIdle() { ring.WaitIdle(); }

    // Declare the state we need a Unity resource to be in; Unity transitions it for us and records `current`.
    // We never barrier a Unity resource ourselves: `expected = COMMON` is treated by Unity as "nothing to do",
    // so the RT stays in RENDER_TARGET and our own COMMON -> X barrier is then a before-state mismatch
    // (301x D3D12 debug-layer id=527 in one run, 2026-09-02). Transitions only on resources we own (ngxOut).
    static void Declare(UnityGraphicsD3D12ResourceState* st, int& n, ID3D12Resource* res, D3D12_RESOURCE_STATES s)
    {
        st[n].resource = res;
        st[n].expected = st[n].current = s;
        ++n;
    }

    static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                        D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        SharpenPass12::Barrier(cl, res, from, to);
    }

    // ---- IDevice -----------------------------------------------------------

    int Init(void* nativeResource, const wchar_t* inDllDir, const wchar_t* logDir) override
    {
        if (ngxInitialized) return initCode;   // replay the real outcome, not a blanket OK
        ID3D12Resource* res = NULL;
        if (FAILED(((IUnknown*)nativeResource)->QueryInterface(__uuidof(ID3D12Resource), (void**)&res)) || !res)
            return DLSS_ERR_NO_DEVICE;
        HRESULT hr = res->GetDevice(IID_PPV_ARGS(&device));
        res->Release();
        if (FAILED(hr) || !device) return DLSS_ERR_NO_DEVICE;

        if (!g_unityD3D12) { device->Release(); device = NULL; return DLSS_ERR_NO_UNITY_IFACE; }
        ring.Attach(device);
        sharpen.Attach(device, this);
        RfDbg::Attach(device);

        if (inDllDir) wcsncpy_s(dllDir, inDllDir, _TRUNCATE);
        const wchar_t* paths[1] = { dllDir };
        NVSDK_NGX_FeatureCommonInfo common = {};
        common.PathListInfo.Path = paths;
        common.PathListInfo.Length = inDllDir ? 1 : 0;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY, kEngineVersion,
                                                                 logDir ? logDir : L".", device, &common, NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(r)) { lastCreate = r; device->Release(); device = NULL; return DLSS_ERR_INIT_FAILED; }
        ngxInitialized = 1;

        r = NVSDK_NGX_D3D12_GetCapabilityParameters(&params);
        if (NVSDK_NGX_FAILED(r) || !params) { lastCreate = r; return initCode = DLSS_ERR_INIT_FAILED; }

        int available = 0;
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverMajor);
        NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverMinor);

        if (needsDriver) return initCode = DLSS_ERR_NEEDS_DRIVER;
        if (!available) return initCode = DLSS_ERR_NOT_AVAILABLE;
        return initCode = DLSS_OK;
    }

    NVSDK_NGX_Result GetOptimal(unsigned outW, unsigned outH, int quality,
                                unsigned* renderW, unsigned* renderH,
                                unsigned* minW, unsigned* minH,
                                unsigned* maxW, unsigned* maxH) override
    {
        if (!params) return NVSDK_NGX_Result_FAIL_NotInitialized;
        unsigned w = 0, h = 0, mnw = 0, mnh = 0, mxw = 0, mxh = 0; float sharp = 0;
        NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(params, outW, outH, ToNgxQuality(quality),
                                                           &w, &h, &mxw, &mxh, &mnw, &mnh, &sharp);
        if (renderW) *renderW = w; if (renderH) *renderH = h;
        if (minW) *minW = mnw;     if (minH) *minH = mnh;
        if (maxW) *maxW = mxw;     if (maxH) *maxH = mxh;
        return r;
    }

    void Create(const CreateParams& cp) override
    {
        if (!params || !device || !g_unityD3D12) { lastCreate = NVSDK_NGX_Result_FAIL_NotInitialized; return; }
        if (feature) { WaitIdle(); NVSDK_NGX_D3D12_ReleaseFeature(feature); feature = NULL; }
        if (!cp.w || !cp.outW) { lastCreate = NVSDK_NGX_Result_FAIL_InvalidParameter; return; }
        SetPresetHints(params);

        NVSDK_NGX_DLSS_Create_Params dcp = {};
        dcp.Feature.InWidth = cp.w;
        dcp.Feature.InHeight = cp.h;
        dcp.Feature.InTargetWidth = cp.outW;
        dcp.Feature.InTargetHeight = cp.outH;
        dcp.Feature.InPerfQualityValue = ToNgxQuality(cp.quality);
        dcp.InFeatureCreateFlags = cp.ngxFlags;

        ID3D12GraphicsCommandList* cl = Begin();
        if (!cl) { lastCreate = NVSDK_NGX_Result_FAIL_PlatformError; return; }   // Begin() set lastError
        // Node masks are "Multi GPU only (default 1)" (DLSS guide p.56 7.1).
        RfDbg::Log("Create: %ux%u -> %ux%u q=%d flags=0x%X", cp.w, cp.h, cp.outW, cp.outH, cp.quality, cp.ngxFlags);
        lastCreate = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &feature, params, &dcp);
        End(0);                     // the list must be closed and submitted even when NGX failed
        if (NVSDK_NGX_FAILED(lastCreate)) { feature = NULL; lastError = (int)lastCreate; }
        RfDbg::Log("Create: result=0x%X feature=%p", (unsigned)lastCreate, (void*)feature);
        RfDbg::Drain();
        RfDbg::Removed(device, "Create");
        logged = NULL;                    // re-log the resource descs for the new generation
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
        if (!device || !g_unityD3D12) { lastEval = NVSDK_NGX_Result_FAIL_NotInitialized; lastError = (int)lastEval; return; }
        if (!color || !output || (!passthrough && (!fp.depth || !fp.mv))) {
            lastEval = NVSDK_NGX_Result_FAIL_MissingInput; lastError = (int)lastEval; return;
        }
        if (!passthrough && (!feature || !params)) {
            lastEval = NVSDK_NGX_Result_FAIL_FeatureNotFound; lastError = (int)lastEval; return;
        }
        if (passthrough && !SameSize(color, output)) {
            lastEval = NVSDK_NGX_Result_FAIL_InvalidParameter; lastError = DLSS_ERR_PASSTHROUGH_SIZE; return;
        }

        ID3D12GraphicsCommandList* cl = Begin();
        if (!cl) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; return; }   // Begin() set lastError

        UnityGraphicsD3D12ResourceState* st = ring.StateSlot();
        int n = 0;
        if (passthrough) {
            Declare(st, n, color,  D3D12_RESOURCE_STATE_COPY_SOURCE);
            Declare(st, n, output, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(output, color);
            lastEval = NVSDK_NGX_Result_Success;
        } else {
            // With sharpening on, NGX writes our own target and the sharpen pass produces Unity's RT; that keeps
            // every resource-state transition on a resource we own (see D3D12Sharpen.h).
            bool doSharpen = fp.sharpness > 0.0f && sharpen.TargetEnsure(output, ring);

            NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
            ep.Feature.pInColor = color;
            ep.Feature.pInOutput = doSharpen ? sharpen.target : output;
            ep.Feature.InSharpness = 0;   // deprecated in SDK 310; our own pass uses fp.sharpness
            ep.pInDepth = (ID3D12Resource*)fp.depth;
            ep.pInMotionVectors = (ID3D12Resource*)fp.mv;
            ep.InJitterOffsetX = fp.jitterX;
            ep.InJitterOffsetY = fp.jitterY;
            ep.InRenderSubrectDimensions.Width = fp.renderW;
            ep.InRenderSubrectDimensions.Height = fp.renderH;
            ep.InReset = fp.reset;
            ep.InMVScaleX = fp.mvScaleX;
            ep.InMVScaleY = fp.mvScaleY;
            ep.InPreExposure = fp.preExposure;
            ep.InFrameTimeDeltaInMsec = fp.dtMs;

            ID3D12Resource* depth = (ID3D12Resource*)fp.depth;
            ID3D12Resource* mv    = (ID3D12Resource*)fp.mv;
            const D3D12_RESOURCE_STATES kIn = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Declare(st, n, color, kIn); Declare(st, n, depth, kIn); Declare(st, n, mv, kIn);
            Declare(st, n, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            lastEval = NGX_D3D12_EVALUATE_DLSS_EXT(cl, feature, params, &ep);
            if (NVSDK_NGX_FAILED(lastEval)) lastError = (int)lastEval;
            else if (doSharpen) sharpen.Run(cl, output, fp.sharpness, ring.ringIdx);
            // No barrier back: NGX restores the incoming states, so `current` == `expected` and Unity's tracker
            // stays right. Transitioning a Unity resource ourselves is what produced the id=527 mismatches.
        }
        if (RfDbg::On() && logged != output) {
            logged = output;
            RfDbg::Log("Evaluate: passthrough=%d render=%ux%u jitter=%.3f,%.3f mvScale=%.1f,%.1f reset=%d sharp=%.2f",
                       (int)passthrough, fp.renderW, fp.renderH, fp.jitterX, fp.jitterY, fp.mvScaleX, fp.mvScaleY, fp.reset, fp.sharpness);
            RfDbg::Resource("color", color);
            RfDbg::Resource("depth", (ID3D12Resource*)fp.depth);
            RfDbg::Resource("mv", (ID3D12Resource*)fp.mv);
            RfDbg::Resource("output", output);
            RfDbg::States(n, st);
        }
        if (!End(n)) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; lastError = DLSS_ERR_NO_CONTEXT; }
        RfDbg::Drain();
        RfDbg::Removed(device, "Evaluate");
    }

    void ReleaseFeature() override
    {
        if (!feature) return;
        WaitIdle();                       // guide p.54 5.5: no command list using the feature may still be in flight
        NVSDK_NGX_D3D12_ReleaseFeature(feature);
        feature = NULL;
    }

    void Shutdown() override
    {
        ReleaseFeature();
        ring.Release();
        sharpen.Release();
        if (params) { NVSDK_NGX_D3D12_DestroyParameters(params); params = NULL; }
        if (ngxInitialized) { NVSDK_NGX_D3D12_Shutdown1(device); ngxInitialized = 0; }
        if (device) { device->Release(); device = NULL; }
        Zero();
    }
};

Device12 g_device12;

} // namespace

IDevice* MakeDevice12(void* nativeResource)
{
    if (!nativeResource) return NULL;
    ID3D12Resource* res = NULL;
    if (FAILED(((IUnknown*)nativeResource)->QueryInterface(__uuidof(ID3D12Resource), (void**)&res)) || !res) return NULL;
    res->Release();
    return &g_device12;
}
