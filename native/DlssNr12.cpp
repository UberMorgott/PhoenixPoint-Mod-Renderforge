#include "DlssNr12.h"

#include "Device.h"
#include "D3D12Debug.h"
#include "nvsdk_ngx_params.h"

#include <algorithm>

namespace {

const unsigned long long kNrApplicationId = 0x0876232CULL;
const NVSDK_NGX_Feature kNrFeature = static_cast<NVSDK_NGX_Feature>(0x12);

using NgxInit = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t*, ID3D12Device*, unsigned, const NVSDK_NGX_FeatureCommonInfo*);
using NgxPopulate = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter*);
using NgxCreate = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using NgxEvaluate = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using NgxRelease = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle*);
using NgxShutdown = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12Device*);

using BridgeInit = NVSDK_NGX_Result (__cdecl *)(NgxInit, unsigned long long, const wchar_t*, ID3D12Device*, unsigned, const NVSDK_NGX_FeatureCommonInfo*);
using BridgePopulate = NVSDK_NGX_Result (__cdecl *)(NgxPopulate, NVSDK_NGX_Parameter*);
using BridgeCreate = NVSDK_NGX_Result (__cdecl *)(NgxCreate, ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using BridgeEvaluate = NVSDK_NGX_Result (__cdecl *)(NgxEvaluate, ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using BridgeRelease = NVSDK_NGX_Result (__cdecl *)(NgxRelease, NVSDK_NGX_Handle*);
using BridgeShutdown = NVSDK_NGX_Result (__cdecl *)(NgxShutdown, ID3D12Device*);

template <typename T> T Proc(HMODULE module, const char* name)
{
    return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}

float Clamp(float value, float lo, float hi)
{
    return (std::max)(lo, (std::min)(hi, value));
}

void SetSubrect(NVSDK_NGX_Parameter* params, const char* stem, unsigned width, unsigned height)
{
    char key[64];
    sprintf_s(key, "DLSSNR.%sSubrectBaseX", stem); params->Set(key, 0u);
    sprintf_s(key, "DLSSNR.%sSubrectBaseY", stem); params->Set(key, 0u);
    sprintf_s(key, "DLSSNR.%sSubrectWidth", stem); params->Set(key, width);
    sprintf_s(key, "DLSSNR.%sSubrectHeight", stem); params->Set(key, height);
}

} // namespace

DlssNr12::DlssNr12()
    : runtime_(nullptr), bridge_(nullptr), feature_(nullptr), initialized_(false), active_(false), depthInverted_(0),
      lastInit_((NVSDK_NGX_Result)0), lastCreate_((NVSDK_NGX_Result)0), lastEval_((NVSDK_NGX_Result)0),
      snippetInit_(nullptr), snippetPopulate_(nullptr), snippetCreate_(nullptr), snippetEvaluate_(nullptr),
      snippetRelease_(nullptr), snippetShutdown_(nullptr), bridgeInit_(nullptr), bridgePopulate_(nullptr),
      bridgeCreate_(nullptr), bridgeEvaluate_(nullptr), bridgeRelease_(nullptr), bridgeShutdown_(nullptr)
{
    config_ = { 0, 0, 1.0f, 1.0f, 1.0f, -1.0f, 1 };
}

void DlssNr12::Configure(const DlssNrConfig& value)
{
    config_ = value;
    config_.enabled = value.enabled ? 1 : 0;
    config_.style = (std::max)(0, (std::min)(2, value.style));
    config_.intensity = Clamp(value.intensity, 0.0f, 2.0f);
    config_.localTone = Clamp(value.localTone, 0.0f, 2.0f);
    config_.localStructure = Clamp(value.localStructure, 0.0f, 2.0f);
    config_.skinStructure = Clamp(value.skinStructure, -1.0f, 1.0f);
    config_.autoMask = value.autoMask ? 1 : 0;
    if (!config_.enabled) active_ = false;
}

bool DlssNr12::EnsureInitialized(ID3D12Device* device, NVSDK_NGX_Parameter*, const wchar_t* dllDir)
{
    if (!Wanted() || !device || !dllDir || !dllDir[0]) return false;
    if (initialized_) return true;

    wchar_t runtimePath[MAX_PATH] = {};
    wchar_t bridgePath[MAX_PATH] = {};
    if (swprintf_s(runtimePath, L"%s\\RenderforgeNR\\nvngx_dlssnr.dll", dllDir) < 0 ||
        swprintf_s(bridgePath, L"%s\\RenderforgeNR\\nvngx.dll", dllDir) < 0) {
        lastInit_ = NVSDK_NGX_Result_FAIL_InvalidParameter;
        return false;
    }

    runtime_ = LoadLibraryExW(runtimePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    bridge_ = LoadLibraryExW(bridgePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    snippetInit_ = Proc<void*>(runtime_, "NVSDK_NGX_D3D12_Init_Ext");
    snippetPopulate_ = Proc<void*>(runtime_, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    snippetCreate_ = Proc<void*>(runtime_, "NVSDK_NGX_D3D12_CreateFeature");
    snippetEvaluate_ = Proc<void*>(runtime_, "NVSDK_NGX_D3D12_EvaluateFeature");
    snippetRelease_ = Proc<void*>(runtime_, "NVSDK_NGX_D3D12_ReleaseFeature");
    snippetShutdown_ = Proc<void*>(runtime_, "NVSDK_NGX_D3D12_Shutdown1");
    bridgeInit_ = Proc<void*>(bridge_, "NVNGXBridge_D3D12_InitExt");
    bridgePopulate_ = Proc<void*>(bridge_, "NVNGXBridge_D3D12_PopulateParameters");
    bridgeCreate_ = Proc<void*>(bridge_, "NVNGXBridge_D3D12_CreateFeature");
    bridgeEvaluate_ = Proc<void*>(bridge_, "NVNGXBridge_D3D12_EvaluateFeature");
    bridgeRelease_ = Proc<void*>(bridge_, "NVNGXBridge_D3D12_ReleaseFeature");
    bridgeShutdown_ = Proc<void*>(bridge_, "NVNGXBridge_D3D12_Shutdown1");
    if (!snippetInit_ || !snippetPopulate_ || !snippetCreate_ || !snippetEvaluate_ || !snippetRelease_ || !snippetShutdown_ ||
        !bridgeInit_ || !bridgePopulate_ || !bridgeCreate_ || !bridgeEvaluate_ || !bridgeRelease_ || !bridgeShutdown_) {
        lastInit_ = NVSDK_NGX_Result_FAIL_FeatureNotFound;
        Unload();
        return false;
    }

    lastInit_ = reinterpret_cast<BridgeInit>(bridgeInit_)(reinterpret_cast<NgxInit>(snippetInit_), kNrApplicationId,
        runtimePath, device, NVSDK_NGX_Version_API, nullptr);
    initialized_ = NVSDK_NGX_SUCCEED(lastInit_);
    RfDbg::Log("DLSS-NR Init_Ext: result=0x%X private=%ls", (unsigned)lastInit_, runtimePath);
    if (!initialized_) Unload();
    return initialized_;
}

bool DlssNr12::Create(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                      NVSDK_NGX_Parameter* params, const wchar_t* dllDir, const CreateParams& create)
{
    active_ = false;
    if (!EnsureInitialized(device, params, dllDir)) return false;
    params->Reset();
    NVSDK_NGX_Result populated = reinterpret_cast<BridgePopulate>(bridgePopulate_)(
        reinterpret_cast<NgxPopulate>(snippetPopulate_), params);
    if (NVSDK_NGX_FAILED(populated)) { lastCreate_ = populated; return false; }

    const float ratio = 1.0f;
    params->Set("CreationNodeMask", 1u); params->Set("VisibilityNodeMask", 1u);
    params->Set("Width", create.w); params->Set("Height", create.h);
    params->Set("OutWidth", create.w); params->Set("OutHeight", create.h);
    params->Set("ResourceWidth", create.w); params->Set("ResourceHeight", create.h);
    params->Set("ResourceOutWidth", create.w); params->Set("ResourceOutHeight", create.h);
    params->Set("PerfQualityValue", (int)ToNgxQuality(create.quality));
    params->Set("DLSS.Feature.Create.Flags", create.ngxFlags);
    params->Set("DLSS.Enable.Output.Subrects", 0);
    params->Set("DLSS.Denoise.Mode", 1); params->Set("DLSS.Roughness.Mode", 0u); params->Set("DLSS.Use.HW.Depth", 1u);
    params->Set("DLSSNR.Enabled", 1u);
    params->Set("DLSSNR.InputWidth", create.w); params->Set("DLSSNR.InputHeight", create.h);
    params->Set("DLSSNR.Width", create.w); params->Set("DLSSNR.Height", create.h);
    params->Set("DLSSNR.OutputWidth", create.w); params->Set("DLSSNR.OutputHeight", create.h);
    params->Set("Output.Width", create.w); params->Set("Output.Height", create.h);
    params->Set("DLSSNR.Upscaling", 1u); params->Set("DLSSNR.ScalingRatio", ratio); params->Set("DLSSNR.Scale", ratio);
    params->Set("DLSSNR.Hint.Render.Preset", 1); params->Set("DLSSNR.Style", (unsigned)config_.style);
    params->Set("DLSSNR.Intensity", config_.intensity); params->Set("DLSSNR.LocalToneStrength", config_.localTone);
    params->Set("DLSSNR.LocalStructureStrength", config_.localStructure);
    params->Set("DLSSNR.SkinStructureStrength", config_.skinStructure);
    params->Set("DLSSNR.UseAutoMask", (unsigned)config_.autoMask); params->Set("DLSSNR.UICorrection", 0u);
    depthInverted_ = (create.rawFlags & DLSS_F_DEPTH_INVERTED) ? 1 : 0;
    lastCreate_ = reinterpret_cast<BridgeCreate>(bridgeCreate_)(reinterpret_cast<NgxCreate>(snippetCreate_),
        commandList, kNrFeature, params, &feature_);
    active_ = NVSDK_NGX_SUCCEED(lastCreate_) && feature_ != nullptr;
    RfDbg::Log("DLSS-NR CreateFeature(18): result=0x%X feature=%p style=%d", (unsigned)lastCreate_, feature_, config_.style);
    if (!active_) feature_ = nullptr;
    return active_;
}

bool DlssNr12::Evaluate(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* params,
                        const FrameParams& frame, ID3D12Resource* color, ID3D12Resource* depth,
                        ID3D12Resource* motionVectors, ID3D12Resource* output)
{
    if (!Active()) return false;
    params->Reset();
    params->Set("DLSSNR.Color", color); params->Set("DLSSNR.Output", output);
    params->Set("DLSSNR.Depth", depth); params->Set("DLSSNR.MVec", motionVectors);
    params->Set("DLSSNR.Reset", frame.reset); params->Set("DLSSNR.JitterOffsetX", frame.jitterX);
    params->Set("DLSSNR.JitterOffsetY", frame.jitterY); params->Set("DLSSNR.MVecScaleX", frame.mvScaleX);
    params->Set("DLSSNR.MVecScaleY", frame.mvScaleY); params->Set("DLSSNR.DepthInverted", depthInverted_);
    params->Set("DLSSNR.Enabled", 1); params->Set("DLSSNR.UICorrection", 0);
    params->Set("DLSSNR.Style", (unsigned)config_.style); params->Set("DLSSNR.Intensity", config_.intensity);
    params->Set("DLSSNR.LocalToneStrength", config_.localTone);
    params->Set("DLSSNR.LocalStructureStrength", config_.localStructure);
    params->Set("DLSSNR.SkinStructureStrength", config_.skinStructure);
    params->Set("DLSSNR.UseAutoMask", (unsigned)config_.autoMask);
    SetSubrect(params, "Color", frame.renderW, frame.renderH);
    SetSubrect(params, "Depth", frame.renderW, frame.renderH);
    SetSubrect(params, "MVec", frame.renderW, frame.renderH);
    SetSubrect(params, "Output", frame.renderW, frame.renderH);
    lastEval_ = reinterpret_cast<BridgeEvaluate>(bridgeEvaluate_)(reinterpret_cast<NgxEvaluate>(snippetEvaluate_),
        commandList, feature_, params, nullptr);
    if (NVSDK_NGX_FAILED(lastEval_)) active_ = false;
    return active_;
}

void DlssNr12::ReleaseFeature()
{
    if (feature_) {
        reinterpret_cast<BridgeRelease>(bridgeRelease_)(reinterpret_cast<NgxRelease>(snippetRelease_), feature_);
        feature_ = nullptr;
    }
    active_ = false;
}

void DlssNr12::Shutdown(ID3D12Device* device)
{
    ReleaseFeature();
    if (initialized_ && device)
        reinterpret_cast<BridgeShutdown>(bridgeShutdown_)(reinterpret_cast<NgxShutdown>(snippetShutdown_), device);
    initialized_ = false;
    Unload();
}

void DlssNr12::Unload()
{
    if (runtime_) FreeLibrary(runtime_);
    if (bridge_) FreeLibrary(bridge_);
    runtime_ = bridge_ = nullptr;
    snippetInit_ = snippetPopulate_ = snippetCreate_ = snippetEvaluate_ = snippetRelease_ = snippetShutdown_ = nullptr;
    bridgeInit_ = bridgePopulate_ = bridgeCreate_ = bridgeEvaluate_ = bridgeRelease_ = bridgeShutdown_ = nullptr;
}

void DlssNr12::Status(int* initResult, int* createResult, int* evalResult, int* alive) const
{
    if (initResult) *initResult = (int)lastInit_;
    if (createResult) *createResult = (int)lastCreate_;
    if (evalResult) *evalResult = (int)lastEval_;
    if (alive) *alive = Active() ? 1 : 0;
}
