// Xess12.cpp - Intel XeSS-SR (libxess.dll 2.x, D3D12, cross-vendor DP4a / Intel XMX) as an IDevice provider.
//
// Contract sources, all in E:\DEV\PhoenixPoint\refs\XeSS-sdk (SDK 3.0.2, libxess.dll 2.0.2.68):
//   inc/xess/xess.h, inc/xess/xess_d3d12.h, doc/xess_sr_developer_guide_english.md.
//
// Submission is identical to Device12: xessD3D12Execute RECORDS into our own DIRECT command list (no GPU work of
// its own), which goes to Unity through IUnityGraphicsD3D12v5::ExecuteCommandList. Required states (guide
// "Resource States", xess_d3d12.h:33-47): inputs NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS - the same as
// NGX. The resources are the shim's own (D3D12Owned.h); XeSS does no memory synchronisation and never promises to
// restore states, which is why OwnedSet12::Leave transitions them back to COMMON ourselves.
//
// XeSS has no sharpness of its own, so the shim's NIS/RCAS pass (D3D12Sharpen.h) runs after the execute exactly as
// it does after NGX: XeSS writes sharpen.target, the pass writes Unity's RT.
//
// Loading: libxess.lib is linked with /DELAYLOAD (CMakeLists.txt); Init pins <modDir>\libxess.dll by absolute path
// first, so the delay-load helper's bare-name LoadLibrary binds to that copy and the shim loads fine without it.
#include "Device.h"
#include "RenderforgeNative.h"
#include "D3D12Ring.h"
#include "D3D12Sharpen.h"
#include "D3D12Owned.h"
#include "D3D12Debug.h"

#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "xess/xess.h"
#include "xess/xess_d3d12.h"

namespace {

// Jitter axis signs handed to xessD3D12Execute, applied to the driver's (-jx, -jy) NGX-convention offsets.
// Derived, NOT yet measured: the guide composites "ProjectionMatrix[2][0] += Jx*2/W; [2][1] -= Jy*2/H" (row-vector
// form), our driver applies proj[0,2] += 2*jx/W and proj[1,2] += 2*jy/H, so Jx = jx = -fp.jitterX and
// Jy = -jy = +fp.jitterY - the same (-X, +Y) flip FSR needed (Fsr12.cpp). The guide's own advice is to settle
// the sign with a static-scene A/B ("Debugging Tips": try +1/-1 per axis); flip these constants if Ultra
// Performance doubles thin edges in-game instead of resolving them.
const float kJitterSignX = -1.0f;
const float kJitterSignY =  1.0f;

// Static-scene A/B knob: env "sx,sy" with each in {-1,1} overrides the jitter axis signs; anything else keeps the defaults.
void ReadJitterSign(const char* env, float* sx, float* sy)
{
    char buf[32]; size_t n = 0; int x = 0, y = 0;
    if (getenv_s(&n, buf, sizeof(buf), env) == 0 && n && sscanf_s(buf, "%d,%d", &x, &y) == 2
        && (x == 1 || x == -1) && (y == 1 || y == -1)) { *sx = (float)x; *sy = (float)y; }
}

xess_quality_settings_t ToXessQuality(int quality)
{
    switch (quality) {
    case DLSS_Q_DLAA:               return XESS_QUALITY_SETTING_AA;                  // 1.0x - "Native Anti-Aliasing"
    case DLSS_Q_ULTRA_QUALITY_PLUS: return XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;  // 1.3x
    case DLSS_Q_ULTRA_QUALITY:      return XESS_QUALITY_SETTING_ULTRA_QUALITY;       // 1.5x
    case DLSS_Q_BALANCED:           return XESS_QUALITY_SETTING_BALANCED;            // 2.0x
    case DLSS_Q_PERFORMANCE:        return XESS_QUALITY_SETTING_PERFORMANCE;         // 2.3x
    case DLSS_Q_ULTRA_PERFORMANCE:  return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;   // 3.0x
    default:                        return XESS_QUALITY_SETTING_QUALITY;             // 1.7x
    }
}

// DLSS_F_* -> xess_init_flags_t. Never set: USE_NDC_VELOCITY (Unity MVs are UV-space, scaled to pixels by the
// driver's mvScale), EXPOSURE_SCALE_TEXTURE / RESPONSIVE_PIXEL_MASK (no such textures), EXTERNAL_DESCRIPTOR_HEAP
// (XeSS manages its own heap). LDR colour = "disable tonemapping for input and output" (xess.h:121) and the
// guide's LDR rule: exposure 1.0, no auto-exposure.
uint32_t ToXessInitFlags(int rawFlags)
{
    uint32_t f = XESS_INIT_FLAG_NONE;
    if (!(rawFlags & DLSS_F_HDR))         f |= XESS_INIT_FLAG_LDR_INPUT_COLOR;
    if (rawFlags & DLSS_F_DEPTH_INVERTED) f |= XESS_INIT_FLAG_INVERTED_DEPTH;        // Unity reversed-Z
    if (rawFlags & DLSS_F_MV_JITTERED)    f |= XESS_INIT_FLAG_JITTERED_MV;
    // DLSS_F_MV_LOW_RES means "MVs are at render resolution", which is the XeSS default; its ABSENCE is the flag.
    if (!(rawFlags & DLSS_F_MV_LOW_RES))  f |= XESS_INIT_FLAG_HIGH_RES_MV;
    if (rawFlags & DLSS_F_AUTO_EXPOSURE)  f |= XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
    return f;
}

// xess.h:196 - may be called from other threads and the message dies at return, so no shared buffers here.
void XessLog(const char* message, xess_logging_level_t level)
{
    OutputDebugStringA(level >= XESS_LOGGING_LEVEL_ERROR ? "[Renderforge][XeSS][error] " : "[Renderforge][XeSS] ");
    OutputDebugStringA(message ? message : "(null)");
    OutputDebugStringA("\n");
}

HMODULE LoadFrom(const wchar_t* dir, const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    if (!dir || !dir[0]) return NULL;
    if (wcslen(dir) + wcslen(name) + 2 >= MAX_PATH) return NULL;
    wcscpy_s(path, dir);
    size_t n = wcslen(path);
    if (n && path[n - 1] != L'\\' && path[n - 1] != L'/') wcscat_s(path, L"\\");
    wcscat_s(path, name);
    return LoadLibraryW(path);
}

struct Xess12 : IDevice
{
    ID3D12Device* device;
    D3D12Ring ring;
    OwnedSet12 owned;               // the resources XeSS touches; Unity's RTs are only copied (D3D12Owned.h)
    SharpenPass12 sharpen;
    HMODULE lib;                    // libxess.dll pinned from the mod folder; stays resident (delay-load bound to it)
    xess_context_handle_t ctx;      // NULL until needed; destroyed by ReleaseFeature/Shutdown
    int initialised;                // xessD3D12Init succeeded on `ctx` for the current CreateParams
    unsigned outW, outH;
    float velScaleX, velScaleY;     // last values pushed with xessSetVelocityScale
    float jitterSignX, jitterSignY; // kJitterSign* unless RENDERFORGE_XESS_JITTER_SIGN overrides
    char version[64];              // "2.0.2 DP4a" / "2.0.2 XMX"
    wchar_t dllDir[MAX_PATH];
    ID3D12Resource* logged;

    Xess12() { Zero(); }

    void Zero()
    {
        device = NULL; lib = NULL; ctx = NULL; initialised = 0;
        outW = outH = 0; velScaleX = velScaleY = 0.0f; version[0] = 0; dllDir[0] = 0; logged = NULL;
        jitterSignX = kJitterSignX; jitterSignY = kJitterSignY;
        ring.Zero();
        owned.Zero();
        sharpen.Zero();
        lastCreate = (NVSDK_NGX_Result)0; lastEval = (NVSDK_NGX_Result)0; lastError = 0; initCode = 0;
        sharpener = 0; sharpenDead = 0;
    }

    int Api() const override { return 12; }
    bool FeatureAlive() const override { return initialised != 0; }
    const OwnedSet12* Owned12() const override { return &owned; }
    const D3D12Ring* Ring12() const override { return &ring; }

    int ProviderVersion(char* buf, int cap) override
    {
        if (!buf || cap <= 0) return 0;
        strncpy_s(buf, (size_t)cap, version, _TRUNCATE);
        return (int)strlen(buf);
    }

    // xess_result_t -> the NVSDK_NGX_Result the ABI already speaks. XESS_RESULT_SUCCESS is 0 (NGX's is 1);
    // the two warnings are advisory and count as success.
    static NVSDK_NGX_Result Map(xess_result_t r)
    {
        switch (r) {
        case XESS_RESULT_SUCCESS:
        case XESS_RESULT_WARNING_NONEXISTING_FOLDER:
        case XESS_RESULT_WARNING_OLD_DRIVER:          return NVSDK_NGX_Result_Success;
        case XESS_RESULT_ERROR_UNINITIALIZED:
        case XESS_RESULT_ERROR_WRONG_CALL_ORDER:      return NVSDK_NGX_Result_FAIL_NotInitialized;
        case XESS_RESULT_ERROR_INVALID_ARGUMENT:      return NVSDK_NGX_Result_FAIL_InvalidParameter;
        case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY:  return NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
        case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
        case XESS_RESULT_ERROR_UNSUPPORTED:
        case XESS_RESULT_ERROR_NOT_IMPLEMENTED:       return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:    return NVSDK_NGX_Result_FAIL_OutOfDate;
        case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:     return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        default:                                      return NVSDK_NGX_Result_FAIL_PlatformError;
        }
    }

    // Lazily creates the live context. XeSS is not thread safe (guide "Thread safety": every call on the thread
    // that initialised it), and `ctx` is xessD3D12Init'ed / executed on the render thread, so nothing on the main
    // thread may touch it after Init: GetOptimal uses its own throwaway context instead.
    xess_result_t EnsureContext()
    {
        if (ctx) return XESS_RESULT_SUCCESS;
        xess_result_t r = xessD3D12CreateContext(device, &ctx);
        if (r != XESS_RESULT_SUCCESS) { ctx = NULL; RfDbg::Log("XeSS: CreateContext failed %d", (int)r); return r; }
        xessSetLoggingCallback(ctx, XESS_LOGGING_LEVEL_WARNING, &XessLog);
        return r;
    }

    void DestroyContext()
    {
        if (!ctx || !BeginDestroy()) return;
        ring.WaitIdle();                       // xess.h:293 - no pending command list may still use the context
        xessDestroyContext(ctx);
        ctx = NULL;
        initialised = 0;
        owned.Release();                       // idle above; the next execute re-creates the set at its own size
        EndDestroy();
    }

    // ---- IDevice -----------------------------------------------------------

    int Init(void* nativeResource, const wchar_t* inDllDir, const wchar_t* logDir) override
    {
        (void)logDir;
        if (lib) return initCode;              // replay the real outcome, not a blanket OK
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
        ReadJitterSign("RENDERFORGE_XESS_JITTER_SIGN", &jitterSignX, &jitterSignY);
        RfDbg::Log("XeSS: jitterSign=%d,%d", (int)jitterSignX, (int)jitterSignY);

        if (inDllDir) wcsncpy_s(dllDir, inDllDir, _TRUNCATE);
        // Pin BEFORE the first xess* call: that is what the delay-load helper binds to.
        lib = LoadFrom(dllDir, L"libxess.dll");
        if (!lib) { device->Release(); device = NULL; ring.Zero(); return DLSS_ERR_NO_PROVIDER_DLL; }

        // xess.h:377 - CreateContext itself reports a GPU without SM 6.4 + DP4a or a driver that cannot run XeSS.
        xess_result_t r = EnsureContext();
        if (r != XESS_RESULT_SUCCESS) {
            lastCreate = Map(r); lastError = DLSS_ERR_XESS;
            if (r == XESS_RESULT_ERROR_UNSUPPORTED_DEVICE) return initCode = DLSS_ERR_NOT_AVAILABLE;
            if (r == XESS_RESULT_ERROR_UNSUPPORTED_DRIVER) return initCode = DLSS_ERR_NEEDS_DRIVER;
            return initCode = DLSS_ERR_INIT_FAILED;
        }

        // Execution path. xess.h:216-218: xessGetIntelXeFXVersion returns the loaded Intel XeFX library version on
        // Intel platforms and 0.0.0 everywhere else - that zero IS the cross-vendor DP4a path, and it is the only
        // API that reports it (xessGetProperties returns heap/descriptor sizes only).
        xess_version_t v = {}, xefx = {};
        xessGetVersion(&v);
        bool xmx = xessGetIntelXeFXVersion(ctx, &xefx) == XESS_RESULT_SUCCESS && (xefx.major || xefx.minor || xefx.patch);
        sprintf_s(version, "%u.%u.%u %s", (unsigned)v.major, (unsigned)v.minor, (unsigned)v.patch, xmx ? "XMX" : "DP4a");
        xess_result_t drv = xessIsOptimalDriver(ctx);   // WARNING_OLD_DRIVER (2) is advisory, not fatal
        RfDbg::Log("XeSS: version %s xefx=%u.%u.%u optimalDriver=%d", version,
                   (unsigned)xefx.major, (unsigned)xefx.minor, (unsigned)xefx.patch, (int)drv);
        return initCode = DLSS_OK;
    }

    NVSDK_NGX_Result GetOptimal(unsigned oW, unsigned oH, int quality,
                                unsigned* renderW, unsigned* renderH,
                                unsigned* minW, unsigned* minH,
                                unsigned* maxW, unsigned* maxH) override
    {
        if (!lib || !device) return NVSDK_NGX_Result_FAIL_NotInitialized;
        // Main thread, while `ctx` may be mid-Create/Execute on the render thread (see EnsureContext): query a
        // throwaway context. The guide's own order is CreateContext -> xessGetOptimalInputResolution -> xess*Init
        // ("Fixed Input Resolution" sample), so a never-initialised context answers this; the probe relies on it too.
        xess_context_handle_t tmp = NULL;
        xess_result_t r = xessD3D12CreateContext(device, &tmp);
        if (r != XESS_RESULT_SUCCESS) { lastError = DLSS_ERR_XESS; RfDbg::Log("XeSS: CreateContext failed %d", (int)r); return Map(r); }
        // xessGetInputResolution is deprecated since XeSS 1.2; this one also yields the dynamic range.
        xess_2d_t out = { oW, oH }, opt = {}, mn = {}, mx = {};
        r = xessGetOptimalInputResolution(tmp, &out, ToXessQuality(quality), &opt, &mn, &mx);
        xessDestroyContext(tmp);
        if (r != XESS_RESULT_SUCCESS || !opt.x || !opt.y) { lastError = DLSS_ERR_XESS; return Map(r); }
        if (renderW) *renderW = opt.x; if (renderH) *renderH = opt.y;
        if (minW) *minW = mn.x;        if (minH) *minH = mn.y;
        if (maxW) *maxW = mx.x;        if (maxH) *maxH = mx.y;
        return NVSDK_NGX_Result_Success;
    }

    void Create(const CreateParams& cp) override
    {
        if (!lib || !device || !g_unityD3D12) { lastCreate = NVSDK_NGX_Result_FAIL_NotInitialized; return; }
        if (!cp.w || !cp.outW) { lastCreate = NVSDK_NGX_Result_FAIL_InvalidParameter; return; }
        // Re-init on a live context is allowed once no command list is pending (xess_d3d12.h:149-150).
        if (initialised) ring.WaitIdle();
        xess_result_t r = EnsureContext();
        if (r != XESS_RESULT_SUCCESS) { lastCreate = Map(r); lastError = DLSS_ERR_XESS; return; }

        // xess_d3d12_init_params_t (xess_d3d12.h:84-112): outputResolution is an xess_2d_t; the heaps and the
        // pipeline library stay NULL (internal allocation, no pre-built pipelines - xessD3D12BuildPipelines only
        // moves the JIT earlier and this create already runs off the hot path).
        xess_d3d12_init_params_t ip = {};
        ip.outputResolution.x = cp.outW;
        ip.outputResolution.y = cp.outH;
        ip.qualitySetting = ToXessQuality(cp.quality);
        ip.initFlags = ToXessInitFlags(cp.rawFlags);
        ip.creationNodeMask = 1;    // single adapter; 1 = default node, like NGX's node masks
        ip.visibleNodeMask = 1;

        RfDbg::Log("XeSS Create: %ux%u -> %ux%u q=%d flags=0x%X", cp.w, cp.h, cp.outW, cp.outH, cp.quality, ip.initFlags);
        r = xessD3D12Init(ctx, &ip);
        lastCreate = Map(r);
        if (r != XESS_RESULT_SUCCESS) { initialised = 0; lastError = DLSS_ERR_XESS; RfDbg::Log("XeSS Create: init failed %d", (int)r); return; }
        initialised = 1;
        outW = cp.outW; outH = cp.outH;
        // Jitter arrives in render-resolution pixels in [-0.5, 0.5] already (only the axis signs are applied, per
        // frame); LDR colour means exposure 1.0 (guide "Color"). Velocity scale is pushed by the first Evaluate.
        xessSetJitterScale(ctx, 1.0f, 1.0f);
        xessSetExposureMultiplier(ctx, 1.0f);
        velScaleX = velScaleY = 0.0f;
        logged = NULL;
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
        if (!lib || !device || !g_unityD3D12) { lastEval = NVSDK_NGX_Result_FAIL_NotInitialized; lastError = (int)lastEval; return; }
        if (!color || !output || (!passthrough && (!fp.depth || !fp.mv))) {
            lastEval = NVSDK_NGX_Result_FAIL_MissingInput; lastError = (int)lastEval; return;
        }
        if (!passthrough && !initialised) {
            lastEval = NVSDK_NGX_Result_FAIL_FeatureNotFound; lastError = (int)lastEval; return;
        }
        if (passthrough && !SameSize(color, output)) {
            lastEval = NVSDK_NGX_Result_FAIL_InvalidParameter; lastError = DLSS_ERR_PASSTHROUGH_SIZE; return;
        }

        ID3D12GraphicsCommandList* cl = ring.Begin();
        if (!cl) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; lastError = ring.failCode; return; }

        // Unity's RTs are only copy sources / the copy destination of this list (declared so); XeSS sees the owned set.
        ID3D12Resource* depth = (ID3D12Resource*)fp.depth;
        ID3D12Resource* mv    = (ID3D12Resource*)fp.mv;
        UnityGraphicsD3D12ResourceState* st = ring.StateSlot();
        int n = OwnedSet12::Declare(st, color, passthrough ? NULL : depth, passthrough ? NULL : mv, output);
        if (passthrough) {
            OwnedSet12::Passthrough(cl, color, output);
            lastEval = NVSDK_NGX_Result_Success;
        } else if (!owned.Ensure(device, ring, color, output)) {
            lastEval = NVSDK_NGX_Result_FAIL_OutOfGPUMemory; lastError = (int)lastEval;
        } else {
            bool doSharpen = fp.sharpness > 0.0f && sharpen.TargetEnsure(owned.out, ring);

            // Motion vectors: Unity's texture is (current - previous) in UV space; the driver's negative scale
            // turns it into "current -> previous in pixels", which is exactly XeSS's convention (guide "Motion
            // Vectors"). Pass the scale straight through; XeSS keeps it per context, so push only on change.
            if (fp.mvScaleX != velScaleX || fp.mvScaleY != velScaleY) {
                xessSetVelocityScale(ctx, fp.mvScaleX, fp.mvScaleY);
                velScaleX = fp.mvScaleX; velScaleY = fp.mvScaleY;
            }

            xess_d3d12_execute_params_t ep = {};
            ep.pColorTexture    = owned.color;
            ep.pVelocityTexture = owned.mv;
            ep.pDepthTexture    = owned.depth;
            ep.pOutputTexture   = doSharpen ? sharpen.target : owned.out;
            ep.jitterOffsetX    = jitterSignX * fp.jitterX;
            ep.jitterOffsetY    = jitterSignY * fp.jitterY;
            ep.exposureScale    = fp.preExposure > 0.0f ? fp.preExposure : 1.0f;
            ep.resetHistory     = fp.reset ? 1u : 0u;
            ep.inputWidth       = fp.renderW;
            ep.inputHeight      = fp.renderH;
            // Every *Base coordinate stays (0,0): our RTs are exactly the input/output resolution.

            // XeSS neither transitions nor restores (xess_d3d12.h:33-47): the resources are in the SDK states from
            // Enter to Leave, and Leave puts them back at rest.
            owned.Enter(cl, color, depth, mv);
            ring.Stamp(1);
            xess_result_t r = xessD3D12Execute(ctx, cl, &ep);
            lastEval = Map(r);
            if (r != XESS_RESULT_SUCCESS) { lastError = DLSS_ERR_XESS; RfDbg::Log("XeSS Execute: failed %d", (int)r); }
            // XeSS binds its own heap/root signature/PSO on this list; the sharpen pass re-binds all three.
            else if (doSharpen) sharpen.Run(cl, owned.out, fp.sharpness, ring.ringIdx);
            ring.Stamp(2);
            owned.Leave(cl, output);
        }
        if (RfDbg::On() && logged != output) {
            logged = output;
            RfDbg::Log("XeSS Evaluate: passthrough=%d render=%ux%u jitter=%.3f,%.3f mvScale=%.1f,%.1f reset=%d sharp=%.2f",
                       (int)passthrough, fp.renderW, fp.renderH, fp.jitterX, fp.jitterY, fp.mvScaleX, fp.mvScaleY, fp.reset, fp.sharpness);
            RfDbg::Resource("color", color);
            RfDbg::Resource("depth", (ID3D12Resource*)fp.depth);
            RfDbg::Resource("mv", (ID3D12Resource*)fp.mv);
            RfDbg::Resource("output", output);
            RfDbg::States(n, st);
        }
        if (!ring.End(n)) { lastEval = NVSDK_NGX_Result_FAIL_PlatformError; lastError = DLSS_ERR_NO_CONTEXT; }
        RfDbg::Drain();
        RfDbg::Removed(device, "XeSS Evaluate");
    }

    // XeSS has no feature handle apart from the context: a preset/resolution change is a re-init, and dropping the
    // context is the cheapest way to release everything it allocated.
    void ReleaseFeature() override { DestroyContext(); }

    void Shutdown() override
    {
        DestroyContext();
        ring.Release();
        owned.Release();
        sharpen.Release();
        if (device) { device->Release(); device = NULL; }
        Zero();                 // libxess.dll stays resident: the delay-load thunks are bound to it
    }
};

Xess12 g_xess12;

} // namespace

IDevice* MakeXess12(void* nativeResource)
{
    if (!nativeResource) return NULL;
    ID3D12Resource* res = NULL;
    if (FAILED(((IUnknown*)nativeResource)->QueryInterface(__uuidof(ID3D12Resource), (void**)&res)) || !res) return NULL;
    res->Release();
    return &g_xess12;
}
