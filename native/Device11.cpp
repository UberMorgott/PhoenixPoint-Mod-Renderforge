// Device11.cpp - NGX DLSS on D3D11. Behaviour is identical to the pre-seam RenderforgeNative.cpp:
// the immediate context is taken from the device inside the render event, NGX runs on it directly.
#include "Device.h"
#include "RenderforgeNative.h"
#include "Sharpen.h"

#include <d3d11.h>
#include <string.h>

#include "nvsdk_ngx_helpers.h"

namespace {

struct Device11 : IDevice
{
    ID3D11Device* device;
    NVSDK_NGX_Parameter* params;   // capability params, reused for create/eval
    NVSDK_NGX_Handle* feature;
    int ngxInitialized;
    int needsDriver;
    unsigned minDriverMajor, minDriverMinor;
    wchar_t dllDir[MAX_PATH];

    // Sharpen pass (runs after NGX on the output UAV). Scratch = SRV copy of the output,
    // since an in-place read+write of the same texture is a hazard.
    ID3D11ComputeShader* cs;
    ID3D11Buffer* cb;              // 256 B dynamic cbuffer (NISConfig is 256 B; RCAS uses 16 B)
    ID3D11SamplerState* sampler;   // linear clamp, NIS samples uv
    ID3D11Texture2D* scratch;
    ID3D11ShaderResourceView* scratchSrv;
    ID3D11UnorderedAccessView* outUav;
    ID3D11Resource* outUavRes;     // the resource outUav was built for
    unsigned scratchW, scratchH;
    DXGI_FORMAT scratchFmt;
    bool csGrade;

    Device11() { Zero(); }
    void Zero()
    {
        device = NULL; params = NULL; feature = NULL; ngxInitialized = 0; initCode = 0; needsDriver = 0;
        minDriverMajor = minDriverMinor = 0; dllDir[0] = 0;
        cs = NULL; cb = NULL; sampler = NULL; scratch = NULL; scratchSrv = NULL; outUav = NULL; outUavRes = NULL;
        scratchW = scratchH = 0; scratchFmt = DXGI_FORMAT_UNKNOWN; csGrade = false;
        lastCreate = (NVSDK_NGX_Result)0; lastEval = (NVSDK_NGX_Result)0; lastError = 0; sharpener = 0; sharpenDead = 0;
    }

    int Api() const override { return 11; }
    bool FeatureAlive() const override { return feature != NULL; }

    void SharpenFail() { sharpenDead = 1; lastError = DLSS_ERR_SHARPEN; }

    void ReleaseSharpenViews()
    {
        if (outUav) { outUav->Release(); outUav = NULL; } outUavRes = NULL;
        if (scratchSrv) { scratchSrv->Release(); scratchSrv = NULL; }
        if (scratch) { scratch->Release(); scratch = NULL; }
        scratchW = scratchH = 0;
    }

    int EnsureSharpenShader(bool colorGrade)
    {
        if (cs && csGrade == colorGrade) return 1;
        if (cs) { cs->Release(); cs = NULL; }
        int kind = 0;
        ID3DBlob* blob = CompileSharpenBlob(&kind, false, colorGrade);
        if (!blob) { SharpenFail(); return 0; }
        HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &cs);
        blob->Release();
        if (FAILED(hr) || !cs) { SharpenFail(); return 0; }
        csGrade = colorGrade;
        sharpener = kind;
        if (!cb) {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = 256; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&bd, NULL, &cb))) { SharpenFail(); return 0; }
        }
        if (!sampler) {
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sd, &sampler))) { SharpenFail(); return 0; }
        }
        return 1;
    }

    void Sharpen(ID3D11DeviceContext* ctx, ID3D11Resource* output, float sharpness,
                 int lutPreset, float lutStrength, const SceneStyleParams& style)
    {
        if (sharpenDead || !output || !device) return;
        bool grade = ColorGradeEnabled(lutPreset, lutStrength) || SceneStyleEnabled(style);
        if (!EnsureSharpenShader(grade)) return;

        ID3D11Texture2D* tex = NULL;
        if (FAILED(output->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) { SharpenFail(); return; }
        D3D11_TEXTURE2D_DESC od = {}; tex->GetDesc(&od);
        tex->Release();
        DXGI_FORMAT viewFmt = SharpenViewFormat(od.Format);

        if (!scratch || scratchW != od.Width || scratchH != od.Height || scratchFmt != od.Format) {
            ReleaseSharpenViews();
            D3D11_TEXTURE2D_DESC sd = od;
            sd.BindFlags = D3D11_BIND_SHADER_RESOURCE; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_DEFAULT; sd.CPUAccessFlags = 0;
            if (FAILED(device->CreateTexture2D(&sd, NULL, &scratch))) { SharpenFail(); return; }
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = viewFmt; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateShaderResourceView(scratch, &sv, &scratchSrv))) { SharpenFail(); return; }
            scratchW = od.Width; scratchH = od.Height; scratchFmt = od.Format;
        }
        if (outUavRes != output) {
            if (outUav) { outUav->Release(); outUav = NULL; }
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv = {};
            uv.Format = viewFmt; uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateUnorderedAccessView(output, &uv, &outUav))) { SharpenFail(); return; }
            outUavRes = output;
        }

        ctx->CopyResource(scratch, output);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            FillSharpenConstants(m.pData, sharpener, sharpness, od.Width, od.Height, lutPreset, lutStrength, false, style);
            ctx->Unmap(cb, 0);
        } else { SharpenFail(); return; }

        // NGX and Unity share this immediate context. Preserve every CS slot we touch, then release the Get refs.
        ID3D11ComputeShader* oldCs = NULL; ID3D11ClassInstance* oldClasses[256] = {}; UINT oldClassCount = 256;
        ID3D11Buffer* oldCb = NULL; ID3D11SamplerState* oldSampler = NULL;
        ID3D11ShaderResourceView* oldSrv = NULL; ID3D11UnorderedAccessView* oldUav = NULL;
        ctx->CSGetShader(&oldCs, oldClasses, &oldClassCount);
        ctx->CSGetConstantBuffers(0, 1, &oldCb);
        ctx->CSGetSamplers(0, 1, &oldSampler);
        ctx->CSGetShaderResources(0, 1, &oldSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
        ctx->CSSetShader(cs, NULL, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetSamplers(0, 1, &sampler);
        ctx->CSSetShaderResources(0, 1, &scratchSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &outUav, NULL);
        unsigned g = SharpenGroupSize(sharpener);
        ctx->Dispatch((od.Width + g - 1) / g, (od.Height + g - 1) / g, 1);
        // Unbind our read/write hazard first, then restore the exact caller state.
        ID3D11UnorderedAccessView* nullUav = NULL; ID3D11ShaderResourceView* nullSrv = NULL;
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, NULL);
        ctx->CSSetShaderResources(0, 1, &nullSrv);
        ctx->CSSetShader(oldCs, oldClasses, oldClassCount);
        ctx->CSSetConstantBuffers(0, 1, &oldCb);
        ctx->CSSetSamplers(0, 1, &oldSampler);
        ctx->CSSetShaderResources(0, 1, &oldSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, NULL);
        if (oldUav) oldUav->Release(); if (oldSrv) oldSrv->Release();
        if (oldSampler) oldSampler->Release(); if (oldCb) oldCb->Release(); if (oldCs) oldCs->Release();
        for (UINT i = 0; i < oldClassCount; ++i) if (oldClasses[i]) oldClasses[i]->Release();
    }

    int Init(void* nativeResource, const wchar_t* inDllDir, const wchar_t* logDir) override
    {
        if (ngxInitialized) return initCode;   // replay the real outcome, not a blanket OK
        ID3D11Resource* res = NULL;
        if (FAILED(((IUnknown*)nativeResource)->QueryInterface(__uuidof(ID3D11Resource), (void**)&res)) || !res)
            return DLSS_ERR_NO_DEVICE;
        res->GetDevice(&device);
        res->Release();
        if (!device) return DLSS_ERR_NO_DEVICE;

        if (inDllDir) wcsncpy_s(dllDir, inDllDir, _TRUNCATE);
        const wchar_t* paths[1] = { dllDir };
        NVSDK_NGX_FeatureCommonInfo common = {};
        common.PathListInfo.Path = paths;
        common.PathListInfo.Length = inDllDir ? 1 : 0;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

        NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNITY, kEngineVersion,
                                                                 logDir ? logDir : L".", device, &common, NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(r)) { lastCreate = r; device->Release(); device = NULL; return DLSS_ERR_INIT_FAILED; }
        ngxInitialized = 1;

        r = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
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
        if (!params || !device) { lastCreate = NVSDK_NGX_Result_FAIL_NotInitialized; return; }
        if (feature) { NVSDK_NGX_D3D11_ReleaseFeature(feature); feature = NULL; }
        if (!cp.w || !cp.outW) { lastCreate = NVSDK_NGX_Result_FAIL_InvalidParameter; return; }
        SetPresetHints(params);

        NVSDK_NGX_DLSS_Create_Params dcp = {};
        dcp.Feature.InWidth = cp.w;
        dcp.Feature.InHeight = cp.h;
        dcp.Feature.InTargetWidth = cp.outW;
        dcp.Feature.InTargetHeight = cp.outH;
        dcp.Feature.InPerfQualityValue = ToNgxQuality(cp.quality);
        dcp.InFeatureCreateFlags = cp.ngxFlags;

        ID3D11DeviceContext* ctx = NULL;
        device->GetImmediateContext(&ctx);
        if (!ctx) { lastCreate = NVSDK_NGX_Result_FAIL_PlatformError; return; }
        lastCreate = NGX_D3D11_CREATE_DLSS_EXT(ctx, &feature, params, &dcp);
        ctx->Release();
        if (NVSDK_NGX_FAILED(lastCreate)) { feature = NULL; lastError = (int)lastCreate; }
    }

    static int SameSize(ID3D11Resource* a, ID3D11Resource* b)
    {
        ID3D11Texture2D* ta = NULL; ID3D11Texture2D* tb = NULL;
        D3D11_TEXTURE2D_DESC da = {}, db = {};
        if (FAILED(a->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&ta)) || !ta) return 0;
        if (FAILED(b->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tb)) || !tb) { ta->Release(); return 0; }
        ta->GetDesc(&da); tb->GetDesc(&db);
        ta->Release(); tb->Release();
        return da.Width == db.Width && da.Height == db.Height;
    }

    void Evaluate(const FrameParams& fp, bool passthrough) override
    {
        ID3D11Resource* color = (ID3D11Resource*)fp.color;
        ID3D11Resource* output = (ID3D11Resource*)fp.output;
        if (!device) { lastEval = NVSDK_NGX_Result_FAIL_NotInitialized; lastError = (int)lastEval; return; }
        if (!color || !output || (!passthrough && (!fp.depth || !fp.mv))) {
            lastEval = NVSDK_NGX_Result_FAIL_MissingInput; lastError = (int)lastEval; return;
        }
        ID3D11DeviceContext* ctx = NULL;
        device->GetImmediateContext(&ctx);
        if (!ctx) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; lastError = DLSS_ERR_NO_CONTEXT; return; }
        if (passthrough) {
            if (SameSize(color, output)) {
                ctx->CopyResource(output, color); lastEval = NVSDK_NGX_Result_Success;
                if (fp.sharpness > 0.0f || (ColorGradeEnabled(fp.lutPreset, fp.lutStrength) || SceneStyleEnabled(fp.style)))
                    Sharpen(ctx, output, fp.sharpness, fp.lutPreset, fp.lutStrength, fp.style);
            }
            else { lastEval = NVSDK_NGX_Result_FAIL_InvalidParameter; lastError = DLSS_ERR_PASSTHROUGH_SIZE; }
        } else if (!feature || !params) {
            lastEval = NVSDK_NGX_Result_FAIL_FeatureNotFound; lastError = (int)lastEval;
        } else {
            NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
            ep.Feature.pInColor = color;
            ep.Feature.pInOutput = output;
            ep.Feature.InSharpness = 0;   // deprecated in SDK 310; our own pass uses fp.sharpness
            ep.pInDepth = (ID3D11Resource*)fp.depth;
            ep.pInMotionVectors = (ID3D11Resource*)fp.mv;
            ep.InJitterOffsetX = fp.jitterX;
            ep.InJitterOffsetY = fp.jitterY;
            ep.InRenderSubrectDimensions.Width = fp.renderW;
            ep.InRenderSubrectDimensions.Height = fp.renderH;
            ep.InReset = fp.reset;
            ep.InMVScaleX = fp.mvScaleX;
            ep.InMVScaleY = fp.mvScaleY;
            ep.InPreExposure = fp.preExposure;
            ep.InFrameTimeDeltaInMsec = fp.dtMs;
            lastEval = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, feature, params, &ep);
            if (NVSDK_NGX_FAILED(lastEval)) lastError = (int)lastEval;
            else if (fp.sharpness > 0.0f || (ColorGradeEnabled(fp.lutPreset, fp.lutStrength) || SceneStyleEnabled(fp.style)))
                Sharpen(ctx, output, fp.sharpness, fp.lutPreset, fp.lutStrength, fp.style);
        }
        ctx->Release();
    }

    void ReleaseFeature() override
    {
        ReleaseSharpenViews();   // outUav refs a Unity RT the driver frees two frames after this event
        if (!feature) return;
        NVSDK_NGX_D3D11_ReleaseFeature(feature);
        feature = NULL;
    }

    void Shutdown() override
    {
        ReleaseFeature();
        if (cb) { cb->Release(); cb = NULL; }
        if (cs) { cs->Release(); cs = NULL; }
        if (sampler) { sampler->Release(); sampler = NULL; }
        if (params) { NVSDK_NGX_D3D11_DestroyParameters(params); params = NULL; }
        if (ngxInitialized) { NVSDK_NGX_D3D11_Shutdown1(device); ngxInitialized = 0; }
        if (device) { device->Release(); device = NULL; }
        Zero();
    }
};

Device11 g_device11;

} // namespace

IDevice* MakeDevice11(void* nativeResource)
{
    if (!nativeResource) return NULL;
    ID3D11Resource* res = NULL;
    if (FAILED(((IUnknown*)nativeResource)->QueryInterface(__uuidof(ID3D11Resource), (void**)&res)) || !res) return NULL;
    res->Release();
    return &g_device11;
}
