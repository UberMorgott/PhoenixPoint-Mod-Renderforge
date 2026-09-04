#pragma once

#include <windows.h>
#include <d3d12.h>
#include "nvsdk_ngx.h"

struct CreateParams;
struct FrameParams;

struct DlssNrConfig
{
    int enabled;
    int style;
    float intensity;
    float localTone;
    float localStructure;
    float skinStructure;
    int autoMask;
};

class DlssNr12
{
public:
    DlssNr12();
    void Configure(const DlssNrConfig& value);
    bool Create(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                NVSDK_NGX_Parameter* params, const wchar_t* dllDir, const CreateParams& create);
    bool Evaluate(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* params,
                  const FrameParams& frame, ID3D12Resource* color, ID3D12Resource* depth,
                  ID3D12Resource* motionVectors, ID3D12Resource* output);
    void ReleaseFeature();
    void Shutdown(ID3D12Device* device);
    bool Wanted() const { return config_.enabled != 0; }
    bool Active() const { return active_ && feature_ != nullptr; }
    bool FeatureAlive() const { return feature_ != nullptr; }
    void Status(int* initResult, int* createResult, int* evalResult, int* alive) const;

private:
    bool EnsureInitialized(ID3D12Device* device, NVSDK_NGX_Parameter* params, const wchar_t* dllDir);
    void Unload();

    DlssNrConfig config_;
    HMODULE runtime_;
    HMODULE bridge_;
    NVSDK_NGX_Handle* feature_;
    bool initialized_;
    bool active_;
    int depthInverted_;
    NVSDK_NGX_Result lastInit_;
    NVSDK_NGX_Result lastCreate_;
    NVSDK_NGX_Result lastEval_;

    void* snippetInit_;
    void* snippetPopulate_;
    void* snippetCreate_;
    void* snippetEvaluate_;
    void* snippetRelease_;
    void* snippetShutdown_;
    void* bridgeInit_;
    void* bridgePopulate_;
    void* bridgeCreate_;
    void* bridgeEvaluate_;
    void* bridgeRelease_;
    void* bridgeShutdown_;
};

