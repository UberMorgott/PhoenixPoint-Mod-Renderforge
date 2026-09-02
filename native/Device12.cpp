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
#include "Sharpen.h"

#include <d3d12.h>
#include <string.h>

#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphicsD3D12.h"
#include "nvsdk_ngx_helpers.h"

extern IUnityGraphicsD3D12v5* g_unityD3D12;   // set by UnityPluginLoad (RenderforgeNative.cpp) or Dlss_TestSetUnityD3D12

namespace {

const int kRing = 3;              // Unity keeps up to 3 frames in flight
const DWORD kFenceWaitMs = 5000;  // a hung GPU must not deadlock the render thread forever

struct Device12 : IDevice
{
    ID3D12Device* device;
    IUnityGraphicsD3D12v5* unity;
    NVSDK_NGX_Parameter* params;
    NVSDK_NGX_Handle* feature;
    int ngxInitialized;
    int needsDriver;
    unsigned minDriverMajor, minDriverMinor;
    wchar_t dllDir[MAX_PATH];

    ID3D12CommandAllocator* alloc[kRing];
    ID3D12GraphicsCommandList* list[kRing];
    UINT64 submitted[kRing];      // fence value ExecuteCommandList returned for that slot, 0 = never used
    int ringIdx;
    int recording;                // a list is open; End() must be reached on every path
    HANDLE waitEvent;

    Device12() { Zero(); }

    void Zero()
    {
        device = NULL; unity = NULL; params = NULL; feature = NULL;
        ngxInitialized = 0; needsDriver = 0; minDriverMajor = minDriverMinor = 0; dllDir[0] = 0;
        for (int i = 0; i < kRing; ++i) { alloc[i] = NULL; list[i] = NULL; submitted[i] = 0; }
        ringIdx = 0; recording = 0; waitEvent = NULL;
        lastCreate = (NVSDK_NGX_Result)0; lastEval = (NVSDK_NGX_Result)0; lastError = 0; sharpener = 0; sharpenDead = 0;
    }

    int Api() const override { return 12; }
    bool FeatureAlive() const override { return feature != NULL; }

    // ---- command-list ring -------------------------------------------------

    void WaitFence(UINT64 value)
    {
        if (!unity || !value) return;
        ID3D12Fence* fence = unity->GetFrameFence();
        if (!fence || fence->GetCompletedValue() >= value) return;
        if (!waitEvent) waitEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!waitEvent) return;
        if (SUCCEEDED(fence->SetEventOnCompletion(value, waitEvent))) WaitForSingleObject(waitEvent, kFenceWaitMs);
    }

    // Blocks until every list we ever submitted has retired. Needed before releasing an NGX feature:
    // the guide (p.54 5.5) forbids releasing while a command list that used it is still in flight.
    void WaitIdle()
    {
        for (int i = 0; i < kRing; ++i) WaitFence(submitted[i]);
    }

    // Returns an open, recording DIRECT command list, or NULL.
    ID3D12GraphicsCommandList* Begin()
    {
        if (!device || !unity || recording) return NULL;
        int i = ringIdx;
        WaitFence(submitted[i]);
        if (!alloc[i]) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i])))) return NULL;
            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], NULL, IID_PPV_ARGS(&list[i])))) return NULL;
            list[i]->Close();
        }
        if (FAILED(alloc[i]->Reset())) return NULL;
        if (FAILED(list[i]->Reset(alloc[i], NULL))) return NULL;
        recording = 1;
        return list[i];
    }

    // Closes the current list and hands it to Unity. `states` may be NULL when stateCount is 0.
    bool End(int stateCount, UnityGraphicsD3D12ResourceState* states)
    {
        if (!recording) return false;
        int i = ringIdx;
        recording = 0;
        ringIdx = (ringIdx + 1) % kRing;
        if (FAILED(list[i]->Close())) return false;
        submitted[i] = unity->ExecuteCommandList(list[i], stateCount, states);
        return true;
    }

    // ---- IDevice -----------------------------------------------------------

    int Init(void* nativeResource, const wchar_t* inDllDir, const wchar_t* logDir) override
    {
        if (ngxInitialized) return DLSS_OK;
        ID3D12Resource* res = NULL;
        if (FAILED(((IUnknown*)nativeResource)->QueryInterface(__uuidof(ID3D12Resource), (void**)&res)) || !res)
            return DLSS_ERR_NO_DEVICE;
        HRESULT hr = res->GetDevice(IID_PPV_ARGS(&device));
        res->Release();
        if (FAILED(hr) || !device) return DLSS_ERR_NO_DEVICE;

        unity = g_unityD3D12;
        if (!unity) { device->Release(); device = NULL; return DLSS_ERR_NO_UNITY_IFACE; }

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
        if (NVSDK_NGX_FAILED(r) || !params) { lastCreate = r; return DLSS_ERR_INIT_FAILED; }

        int available = 0;
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverMajor);
        NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverMinor);

        if (needsDriver) return DLSS_ERR_NEEDS_DRIVER;
        if (!available) return DLSS_ERR_NOT_AVAILABLE;
        return DLSS_OK;
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
        if (!params || !device || !unity) { lastCreate = NVSDK_NGX_Result_FAIL_NotInitialized; return; }
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
        if (!cl) { lastCreate = NVSDK_NGX_Result_FAIL_PlatformError; lastError = DLSS_ERR_NO_CONTEXT; return; }
        // Node masks are "Multi GPU only (default 1)" (DLSS guide p.56 7.1).
        lastCreate = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &feature, params, &dcp);
        End(0, NULL);                     // the list must be closed and submitted even when NGX failed
        if (NVSDK_NGX_FAILED(lastCreate)) { feature = NULL; lastError = (int)lastCreate; }
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
        if (!device || !unity) { lastEval = NVSDK_NGX_Result_FAIL_NotInitialized; lastError = (int)lastEval; return; }
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
        if (!cl) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; lastError = DLSS_ERR_NO_CONTEXT; return; }

        UnityGraphicsD3D12ResourceState st[4];
        int n = 0;
        if (passthrough) {
            st[n].resource = color;  st[n].expected = D3D12_RESOURCE_STATE_COPY_SOURCE; st[n].current = D3D12_RESOURCE_STATE_COPY_SOURCE; ++n;
            st[n].resource = output; st[n].expected = D3D12_RESOURCE_STATE_COPY_DEST;   st[n].current = D3D12_RESOURCE_STATE_COPY_DEST;   ++n;
            cl->CopyResource(output, color);
            lastEval = NVSDK_NGX_Result_Success;
        } else {
            NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
            ep.Feature.pInColor = color;
            ep.Feature.pInOutput = output;
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

            st[n].resource = color;                        st[n].expected = st[n].current = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; ++n;
            st[n].resource = (ID3D12Resource*)fp.depth;    st[n].expected = st[n].current = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; ++n;
            st[n].resource = (ID3D12Resource*)fp.mv;       st[n].expected = st[n].current = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; ++n;
            st[n].resource = output;                       st[n].expected = st[n].current = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;          ++n;

            lastEval = NGX_D3D12_EVALUATE_DLSS_EXT(cl, feature, params, &ep);
            if (NVSDK_NGX_FAILED(lastEval)) lastError = (int)lastEval;
        }
        if (!End(n, st)) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; lastError = DLSS_ERR_NO_CONTEXT; }
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
        WaitIdle();
        for (int i = 0; i < kRing; ++i) {
            if (list[i]) { list[i]->Release(); list[i] = NULL; }
            if (alloc[i]) { alloc[i]->Release(); alloc[i] = NULL; }
            submitted[i] = 0;
        }
        if (waitEvent) { CloseHandle(waitEvent); waitEvent = NULL; }
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
