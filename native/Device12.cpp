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
#include <d3dcompiler.h>
#include <string.h>

#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphicsD3D12.h"
#include "D3D12Ring.h"
#include "D3D12Debug.h"
#include "nvsdk_ngx_helpers.h"

namespace {

const int kRing = D3D12Ring::kRing;   // sharpen descriptors/constants are per ring slot

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

    // Sharpen pass. Descriptors are written straight into the shader-visible heap, 2 per ring slot
    // (SRV of ngxOut at +0, UAV of Unity's output at +1). Constants live in one persistently
    // mapped upload buffer, 256 B per ring slot (a root CBV needs 256-byte alignment).
    ID3D12RootSignature* rootSig;
    ID3D12PipelineState* pso;
    ID3D12DescriptorHeap* descHeap;
    UINT descSize;
    ID3D12Resource* cb;
    unsigned char* cbCpu;
    // NGX writes here instead of straight into Unity's RT whenever the sharpen pass runs, and the pass then
    // reads it (SRV) and writes Unity's RT (UAV). Ours, so its state is ours to transition; see Sharpen().
    ID3D12Resource* ngxOut;
    unsigned ngxOutW, ngxOutH;
    DXGI_FORMAT ngxOutFmt;

    ID3D12Resource* logged;        // RENDERFORGE_D3D12_DEBUG: output whose descs were already logged

    Device12() { Zero(); }

    void Zero()
    {
        device = NULL; params = NULL; feature = NULL;
        ngxInitialized = 0; initCode = 0; needsDriver = 0; minDriverMajor = minDriverMinor = 0; dllDir[0] = 0;
        ring.Zero();
        lastCreate = (NVSDK_NGX_Result)0; lastEval = (NVSDK_NGX_Result)0; lastError = 0; sharpener = 0; sharpenDead = 0;
        rootSig = NULL; pso = NULL; descHeap = NULL; descSize = 0; cb = NULL; cbCpu = NULL;
        ngxOut = NULL; ngxOutW = ngxOutH = 0; ngxOutFmt = DXGI_FORMAT_UNKNOWN;
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

    // ---- sharpen ------------------------------------------------------------

    void SharpenFail() { sharpenDead = 1; lastError = DLSS_ERR_SHARPEN; }

    // Unity owns the state of its own RenderTextures and hands them to a plugin in D3D12_RESOURCE_STATE_COMMON;
    // its own D3D12 plugin sample declares exactly that for every Unity-created resource and does the real
    // transitions inside the plugin's list. Declaring anything else desynchronises Unity's state tracking and
    // eventually removes the device with DXGI_ERROR_DEVICE_REMOVED / INVALID_CALL (in-game, 2026-09-02).
    static void Declare(UnityGraphicsD3D12ResourceState* st, int& n, ID3D12Resource* res)
    {
        st[n].resource = res;
        st[n].expected = st[n].current = D3D12_RESOURCE_STATE_COMMON;
        ++n;
    }

    static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                        D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cl->ResourceBarrier(1, &b);
    }

    bool SharpenEnsure()
    {
        if (pso) return true;
        if (sharpenDead) return false;

        int kind = 0;
        ID3DBlob* blob = CompileSharpenBlob(&kind);
        if (!blob) { SharpenFail(); return false; }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[3] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &ranges[0];
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &ranges[1];
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC ss = {};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderRegister = 0; ss.RegisterSpace = 0;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3; rsd.pParameters = rp;
        rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (err) err->Release();
        if (FAILED(hr) || !sig) { blob->Release(); SharpenFail(); return false; }
        hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSig));
        sig->Release();
        if (FAILED(hr)) { blob->Release(); SharpenFail(); return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig;
        pd.CS.pShaderBytecode = blob->GetBufferPointer();
        pd.CS.BytecodeLength = blob->GetBufferSize();
        hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        blob->Release();
        if (FAILED(hr)) { SharpenFail(); return false; }

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2 * kRing;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descHeap)))) { SharpenFail(); return false; }
        descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256 * kRing; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&cb)))) { SharpenFail(); return false; }
        D3D12_RANGE noRead = { 0, 0 };
        if (FAILED(cb->Map(0, &noRead, (void**)&cbCpu)) || !cbCpu) { SharpenFail(); return false; }

        sharpener = kind;
        return true;
    }

    // Our own NGX target, one per output size/format. Returns false (and disables the pass) when it cannot be
    // made, in which case the caller lets NGX write Unity's RT directly and simply skips sharpening.
    bool NgxOutEnsure(ID3D12Resource* output)
    {
        if (sharpenDead || !output || !device) return false;
        if (!SharpenEnsure()) return false;

        D3D12_RESOURCE_DESC od = output->GetDesc();
        // CreateUnorderedAccessView is void: a UAV on a resource without this flag is a debug-layer error
        // and garbage at runtime, so this is the D3D12 twin of the D3D11 CreateUnorderedAccessView guard.
        if (!(od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) { SharpenFail(); return false; }
        unsigned w = (unsigned)od.Width, h = od.Height;
        if (ngxOut && ngxOutW == w && ngxOutH == h && ngxOutFmt == od.Format) return true;

        if (ngxOut) { WaitIdle(); ngxOut->Release(); ngxOut = NULL; }
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = od;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;   // NGX writes it, the sharpen pass reads it
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, IID_PPV_ARGS(&ngxOut)))) { SharpenFail(); return false; }
        ngxOutW = w; ngxOutH = h; ngxOutFmt = od.Format;
        RfDbg::Log("NgxOut: %ux%u fmt=%d created", w, h, (int)od.Format);
        return true;
    }

    // Runs on the SAME command list as the NGX evaluate, immediately after it: reads ngxOut (which NGX has just
    // written) and writes Unity's RT. NGX clobbers the command-list state (guide p.52), so everything is re-bound.
    //
    // NO BARRIER IS EVER ISSUED ON A UNITY RESOURCE. Unity owns the state of its own RenderTextures and puts them
    // into the `expected` state itself; a transition of our own with a guessed StateBefore is what removed the
    // device with DXGI_ERROR_DEVICE_REMOVED / INVALID_CALL (2026-09-02, bisected in-game: a bare
    // UNORDERED_ACCESS -> COPY_SOURCE -> UNORDERED_ACCESS pair on outRT was enough, before or after the evaluate).
    // Unity's own D3D12 plugin sample barriers only resources IT created and declares Unity's as COMMON.
    // ngxOut is ours, so transitioning it is safe. `slot` is the ring index being recorded into.
    void Sharpen(ID3D12GraphicsCommandList* cl, ID3D12Resource* output, float sharpness, int slot)
    {
        unsigned w = ngxOutW, h = ngxOutH;
        DXGI_FORMAT viewFmt = SharpenViewFormat(ngxOutFmt);
        if (logged != output)
            RfDbg::Log("Sharpen: kind=%d sharpness=%.2f fmt=%d viewFmt=%d %ux%u slot=%d", sharpener, sharpness, (int)ngxOutFmt, (int)viewFmt, w, h, slot);

        FillSharpenConstants(cbCpu + 256 * (size_t)slot, sharpener, sharpness, w, h);

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = descHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = descHeap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T)(2 * slot) * descSize;
        gpu.ptr += (UINT64)(2 * slot) * descSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = viewFmt;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(ngxOut, &sv, cpu);

        D3D12_CPU_DESCRIPTOR_HANDLE cpuUav = cpu; cpuUav.ptr += descSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += descSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
        uv.Format = viewFmt;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output, NULL, &uv, cpuUav);

        Barrier(cl, ngxOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        ID3D12DescriptorHeap* heaps[1] = { descHeap };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(rootSig);
        cl->SetPipelineState(pso);
        cl->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress() + 256 * (UINT64)slot);
        cl->SetComputeRootDescriptorTable(1, gpu);
        cl->SetComputeRootDescriptorTable(2, gpuUav);
        unsigned g = SharpenGroupSize(sharpener);
        cl->Dispatch((w + g - 1) / g, (h + g - 1) / g, 1);

        // ngxOut goes back to the state it was created in, so every list starts from the same known state.
        Barrier(cl, ngxOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
            Declare(st, n, color);
            Declare(st, n, output);
            Barrier(cl, color,  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(output, color);
            Barrier(cl, color,  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, output, D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_COMMON);
            lastEval = NVSDK_NGX_Result_Success;
        } else {
            // With sharpening on, NGX writes our own ngxOut and the sharpen pass produces Unity's RT; that keeps
            // every resource-state transition on a resource we own (see Sharpen()).
            bool sharpen = fp.sharpness > 0.0f && NgxOutEnsure(output);

            NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
            ep.Feature.pInColor = color;
            ep.Feature.pInOutput = sharpen ? ngxOut : output;
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
            Declare(st, n, color); Declare(st, n, depth); Declare(st, n, mv); Declare(st, n, output);

            const D3D12_RESOURCE_STATES kIn = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Barrier(cl, color, D3D12_RESOURCE_STATE_COMMON, kIn);
            Barrier(cl, depth, D3D12_RESOURCE_STATE_COMMON, kIn);
            Barrier(cl, mv,    D3D12_RESOURCE_STATE_COMMON, kIn);
            Barrier(cl, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            lastEval = NGX_D3D12_EVALUATE_DLSS_EXT(cl, feature, params, &ep);
            if (NVSDK_NGX_FAILED(lastEval)) lastError = (int)lastEval;
            else if (sharpen) Sharpen(cl, output, fp.sharpness, ring.ringIdx);

            Barrier(cl, color, kIn, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, depth, kIn, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, mv,    kIn, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
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
        if (cb && cbCpu) { D3D12_RANGE noWrite = { 0, 0 }; cb->Unmap(0, &noWrite); cbCpu = NULL; }
        if (cb) { cb->Release(); cb = NULL; }
        if (ngxOut) { ngxOut->Release(); ngxOut = NULL; }
        if (descHeap) { descHeap->Release(); descHeap = NULL; }
        if (pso) { pso->Release(); pso = NULL; }
        if (rootSig) { rootSig->Release(); rootSig = NULL; }
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
