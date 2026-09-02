// Device.h - the graphics-API seam. One IDevice implementation per backend (D3D11, D3D12);
// RenderforgeNative.cpp owns the ABI, the frame-slot ring and the render-event dispatch and knows
// nothing about D3D. All members are touched from the render thread except Init/GetOptimal/Shutdown.
#pragma once

#include <windows.h>
#include "nvsdk_ngx.h"

// NGX project identity, defined in RenderforgeNative.cpp, used by both backends.
extern const char kProjectId[];
extern const char kEngineVersion[];

// One per-frame parameter block. API-neutral: resources are opaque here and cast by the backend.
// Filled by Dlss_SetFrame on the main thread; the same address is handed to Unity as the event data.
struct FrameParams
{
    void* color;
    void* depth;
    void* mv;
    void* output;
    float jitterX, jitterY;
    float mvScaleX, mvScaleY;
    int   reset;
    float dtMs;
    unsigned renderW, renderH;
    float preExposure;
    float sharpness;      // 0..1, our own post pass; NGX InSharpness stays 0 (deprecated in SDK 310)
    float nearZ, farZ, fovY;   // camera near/far/vertical FOV (radians) from Dlss_SetCamera; FSR needs them, NGX does not
};

// Feature-creation parameters, stored by Dlss_SetCreateParams and consumed by DLSS_EV_CREATE.
struct CreateParams
{
    unsigned w, h, outW, outH;
    int quality;      // DLSS_Q_*
    int ngxFlags;     // already translated to NVSDK_NGX_DLSS_Feature_Flags
    int rawFlags;     // the untranslated DLSS_F_* bitmask, for providers that map them differently (FSR, XeSS)
};

struct OwnedSet12;   // D3D12Owned.h: the shim-owned twins a D3D12 backend fills every Evaluate

struct IDevice
{
    NVSDK_NGX_Result lastCreate;
    NVSDK_NGX_Result lastEval;
    int lastError;      // NVSDK_NGX_Result, or one of the DLSS_ERR_* negatives
    int sharpener;      // DLSS_SHARPEN_*
    int sharpenDead;    // sharpen setup failed once -> pass skipped for good
    int initCode;       // DLSS_OK / DLSS_ERR_* of the first NGX init; Init() replays it once NGX is up

    IDevice() : lastCreate((NVSDK_NGX_Result)0), lastEval((NVSDK_NGX_Result)0), lastError(0), sharpener(0), sharpenDead(0), initCode(0) {}
    virtual ~IDevice() {}

    virtual int  Api() const = 0;                       // 11 or 12
    virtual int  Init(void* nativeResource, const wchar_t* dllDir, const wchar_t* logDir) = 0;   // DLSS_OK / DLSS_ERR_*
    virtual NVSDK_NGX_Result GetOptimal(unsigned outW, unsigned outH, int quality,
                                        unsigned* renderW, unsigned* renderH,
                                        unsigned* minW, unsigned* minH,
                                        unsigned* maxW, unsigned* maxH) = 0;
    virtual void Create(const CreateParams& cp) = 0;                    // render thread
    virtual void Evaluate(const FrameParams& fp, bool passthrough) = 0; // render thread
    virtual void ReleaseFeature() = 0;                                  // render thread
    virtual void Shutdown() = 0;                                        // main thread, render idle
    virtual bool FeatureAlive() const = 0;
    // Writes the provider's version string into buf (NUL-terminated, at most cap bytes). Returns bytes written.
    // Default: nothing - the NGX backends report their runtime version on the managed side from nvngx_dlss.dll.
    virtual int ProviderVersion(char* buf, int cap) { (void)buf; (void)cap; return 0; }
    // D3D12 backends: the owned twins (depth/mv at render res, out = hud-less at output res) the last Evaluate
    // filled, all resting in COMMON. The FG providers read these instead of Unity's RTs (D3D12Owned.h contract).
    virtual const OwnedSet12* Owned12() const { return NULL; }
};

// Return the singleton backend if `nativeResource` belongs to that API, else NULL. No allocation.
IDevice* MakeDevice11(void* nativeResource);
IDevice* MakeDevice12(void* nativeResource);
// FSR (FidelityFX ffx-api, D3D12 only). NULL when the resource is not a D3D12 one.
IDevice* MakeFsr12(void* nativeResource);
// XeSS (Intel libxess.dll, D3D12 only, cross-vendor DP4a / Intel XMX). NULL when the resource is not a D3D12 one.
IDevice* MakeXess12(void* nativeResource);

// Shared translation helpers (defined in RenderforgeNative.cpp).
NVSDK_NGX_PerfQuality_Value ToNgxQuality(int quality);
void SetPresetHints(NVSDK_NGX_Parameter* params);
