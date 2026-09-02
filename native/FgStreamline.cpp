// FgStreamline.cpp - NVIDIA DLSS Frame Generation / Multi Frame Generation through Streamline 2.12.0 (sl.dlss_g +
// sl.reflex + sl.pcl) as an IFgProvider, D3D12 only, MANUAL HOOKING mode.
//
// Streamline normally interposes DXGI/D3D from process start. A mod loaded after Unity built its device cannot, and an
// existing swapchain cannot be adopted: the one documented path is slUpgradeInterface on a DXGI FACTORY and creating
// the swapchain through that proxy (ProgrammingGuideManualHooking.md:195-215, sl_hooks.h: CreateSwapChainForHwnd,
// GetBuffer, GetCurrentBackBufferIndex, Present, ResizeBuffers, SetFullscreenState and ID3D12Device::CreateCommandQueue
// are the mandatory hooks). So: the child HWND (FgWnd.cpp, FgHostChildHwnd) gets a proxy swapchain built by the proxy
// factory on a DIRECT queue created through the proxy DEVICE (DLSS-G names it 'nv.sl.dlss_g.cmdQ.game'), and that
// proxy IS the shadow chain the host presents (GetBuffer / GetCurrentBackBufferIndex / Present all land on the proxy,
// which DLSS-G intercepts; the host's GetCurrentBackBufferIndex call is what clears
// DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled). Unity's own swapchain and queue are never handed to Streamline.
//
// The presenting queue is OURS, not Unity's (it must come from the proxy device), so the host copies Unity's finished
// back buffer on it after a fence wait on Unity's queue and makes Unity's queue wait for the copy (FgHost.cpp
// CopyBackBuffer, PresentQueue()). DLSS-G reads its inputs asynchronously on its own queue, so the shim-owned twins
// (D3D12Owned.h, COMMON at rest) are tagged eOnlyValidNow: SL copies them on the prep list (Unity-ordered, after the
// upscaler wrote them) and the next frame's upscaler can rewrite them freely (sl_core_types.h:386-394, DLSS_G.md:288-293).
//
// Reflex is MANDATORY for DLSS-G (DLSS_G.md:674-681, sl_dlss_g.h:131-132 eFailReflexNotDetectedAtRuntime): one
// slReflexSetOptions(eLowLatency) and, per frame with ONE frame token, slReflexSleep + the six PCL markers
// (sl_pcl.h:60-67) around slSetConstants / slSetTagForFrame and the proxy Present. Frame index = our own +1 per
// Prepare, process-wide (SL refuses constants set twice for one index; the host's single slot can repeat or skip
// FgFrame.frameId, same reason as FgXess.cpp).
//
// Loading: sl.interposer.dll is LoadLibraryW'd from the mod folder and every entry point GetProcAddress'd (nothing is
// linked); the interposer loads sl.common/sl.dlss_g/sl.reflex/sl.pcl + nvngx_dlssg.dll from pathsToPlugins = the mod
// folder. slInit runs ONCE per process (it loads plugins and NGX); the chain, queue and factory proxy are per Create.
// OTA flags are deliberately off: nothing may be written outside the mod folder.
//
// STATUS (2026-09-02, RTX 5070 Ti, driver 596.49, Instance3): the chain comes up and presents stably (status eOk,
// numFramesToGenerateMax=5) but DLSS-G presents exactly ONE frame per Present - numFramesActuallyPresented=1, the real
// chain's DXGI PresentCount == our presents - with no warning in sl.log at any log level, in production and
// development builds alike. Tried without effect: tag lifecycle (eOnlyValidNow/eValidUntilPresent), tag set (depth+mv,
// +hudless, +zero UI), frame-based vs legacy slSetTag, explicit vs SL-internal frame indices, EngineType eUnity vs
// eCustom, options every frame, no eRetainResourcesWhenOff. Root cause unknown; see the Task 5 note in
// docs\superpowers\plans\2026-09-02-phase5-framegen.md. RENDERFORGE_SL_VERBOSE=1 makes sl.log verbose.
#include "Fg.h"
#include "RenderforgeNative.h"
#include "D3D12Owned.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "sl_pcl.h"
#include "sl_matrix_helpers.h"

extern const char kProjectId[];        // RenderforgeNative.cpp - the NGX identity DLSS SR already registered with
extern const char kEngineVersion[];

namespace {

void SlLog(sl::LogType t, const char* msg)   // warnings/errors only: the full log is <modDir>\sl.log
{
    if (t == sl::LogType::eInfo) return;
    size_t n = msg ? strlen(msg) : 0;
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) --n;
    FgLog("sl[%d]: %.*s", (int)t, (int)n, msg ? msg : "");
}

// Process-lifetime Streamline state: slInit once, the proxy device once (Unity's device lives as long as the process).
struct SlCore
{
    HMODULE                     dll;
    PFun_slInit*                init;
    PFun_slSetD3DDevice*        setDevice;
    PFun_slIsFeatureSupported*  supported;
    PFun_slUpgradeInterface*    upgrade;
    PFun_slGetNativeInterface*  native;
    PFun_slGetFeatureFunction*  featureFn;
    PFun_slGetFeatureVersion*   featureVersion;
    PFun_slGetNewFrameToken*    newFrame;
    PFun_slSetConstants*        setConstants;
    PFun_slSetTagForFrame*      setTag;
    PFun_slDLSSGSetOptions*     fgSetOptions;
    PFun_slDLSSGGetState*       fgGetState;
    PFun_slReflexSetOptions*    reflexSetOptions;
    PFun_slReflexSleep*         reflexSleep;
    PFun_slPCLSetMarker*        marker;
    ID3D12Device*               proxyDevice;   // slUpgradeInterface(device): the only legal CreateCommandQueue
    int                         state;         // 0 not tried, 1 up, <0 failed (FG_ERR_* negated)
    unsigned                    maxGen;        // DLSSGState::numFramesToGenerateMax once measured, 0 = not yet
    unsigned                    frameIdx;      // process-wide: SL refuses constants/options set twice for one index
    char                        version[64];
};
SlCore g_sl;

template <class T> bool Proc(const char* name, T*& out)
{
    out = (T*)GetProcAddress(g_sl.dll, name);
    if (!out) FgLog("sl: %s missing from sl.interposer.dll", name);
    return out != NULL;
}

template <class T> bool Feature(sl::Feature f, const char* name, T*& out)
{
    void* p = NULL;
    sl::Result r = g_sl.featureFn(f, name, p);
    out = (T*)p;
    if (r != sl::Result::eOk || !out) { FgLog("sl: slGetFeatureFunction(%u, %s) %d", f, name, (int)r); return false; }
    return true;
}

unsigned CapsFor(unsigned maxGen) { return FG_CAP_2X | (maxGen >= 2 ? FG_CAP_3X : 0u) | (maxGen >= 3 ? FG_CAP_4X : 0u); }

// Once per process. Returns FG_OK or the FG_ERR_* to hand back from Create.
int SlUp(const FgSetup& s)
{
    if (g_sl.state > 0) return FG_OK;
    if (g_sl.state < 0) return -g_sl.state;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\sl.interposer.dll", s.dllDir ? s.dllDir : L".");
    g_sl.dll = LoadLibraryW(path);
    if (!g_sl.dll) { FgLog("sl: sl.interposer.dll not loadable from the mod folder (%lu)", GetLastError()); g_sl.state = -FG_ERR_NO_PROVIDER; return FG_ERR_NO_PROVIDER; }
    if (!Proc("slInit", g_sl.init) || !Proc("slSetD3DDevice", g_sl.setDevice) || !Proc("slIsFeatureSupported", g_sl.supported) ||
        !Proc("slUpgradeInterface", g_sl.upgrade) || !Proc("slGetNativeInterface", g_sl.native) || !Proc("slGetFeatureFunction", g_sl.featureFn) ||
        !Proc("slGetFeatureVersion", g_sl.featureVersion) || !Proc("slGetNewFrameToken", g_sl.newFrame) ||
        !Proc("slSetConstants", g_sl.setConstants) || !Proc("slSetTagForFrame", g_sl.setTag)) {
        g_sl.state = -FG_ERR_NO_PROVIDER; return FG_ERR_NO_PROVIDER;
    }

    // ManualHooking.md:90-107 (eUseManualHooking), :19 (dynamic load). eUseDXGIFactoryProxy: a proxy OBJECT instead of
    // patching the DXGI factory v-table every factory in the process shares (sl_core_types.h:528-531). Frame-based
    // tagging is what slSetTagForFrame needs (sl_core_api.h:136). No OTA: nothing outside the mod folder.
    const wchar_t* paths[1] = { s.dllDir };
    sl::Feature features[3] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    sl::Preferences pref{};
    char env[8] = {};
    pref.showConsole = false;
    pref.logLevel = GetEnvironmentVariableA("RENDERFORGE_SL_VERBOSE", env, sizeof(env)) && env[0] == '1' ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = s.dllDir;             // <modDir>\sl.log
    pref.logMessageCallback = &SlLog;
    pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseDXGIFactoryProxy |
                 sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eUnity;          // the same identity Device12.cpp gave NGX for DLSS SR
    pref.engineVersion = kEngineVersion;
    pref.projectId = kProjectId;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = g_sl.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) { FgLog("sl: slInit %d", (int)r); g_sl.state = -FG_ERR_PROVIDER_FAILED; return FG_ERR_PROVIDER_FAILED; }

    LUID luid = s.device->GetAdapterLuid();
    sl::AdapterInfo ai{};
    ai.deviceLUID = (uint8_t*)&luid;
    ai.deviceLUIDSizeInBytes = sizeof(luid);
    for (int i = 0; i < 3; ++i) {
        r = g_sl.supported(features[i], ai);
        if (r != sl::Result::eOk) { FgLog("sl: feature %u unsupported on this adapter (%d; 4=HWS off, 2=driver, 6/7=adapter)", features[i], (int)r); g_sl.state = -FG_ERR_PROVIDER_FAILED; return FG_ERR_PROVIDER_FAILED; }
    }
    r = g_sl.setDevice(s.device);                  // ManualHooking.md:409-423
    if (r != sl::Result::eOk) { FgLog("sl: slSetD3DDevice %d", (int)r); g_sl.state = -FG_ERR_PROVIDER_FAILED; return FG_ERR_PROVIDER_FAILED; }
    if (!Feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_sl.fgSetOptions) || !Feature(sl::kFeatureDLSS_G, "slDLSSGGetState", g_sl.fgGetState) ||
        !Feature(sl::kFeatureReflex, "slReflexSetOptions", g_sl.reflexSetOptions) || !Feature(sl::kFeatureReflex, "slReflexSleep", g_sl.reflexSleep) ||
        !Feature(sl::kFeaturePCL, "slPCLSetMarker", g_sl.marker)) {
        g_sl.state = -FG_ERR_PROVIDER_FAILED; return FG_ERR_PROVIDER_FAILED;
    }
    sl::FeatureVersion fv{};
    if (g_sl.featureVersion(sl::kFeatureDLSS_G, fv) == sl::Result::eOk)
        _snprintf_s(g_sl.version, sizeof(g_sl.version), _TRUNCATE, "SL %u.%u.%u NGX %u.%u.%u", fv.versionSL.major, fv.versionSL.minor, fv.versionSL.build, fv.versionNGX.major, fv.versionNGX.minor, fv.versionNGX.build);
    FgLog("sl: DLSS-G %s", g_sl.version);

    // Reflex on, once (Reflex.md:153-177: required at least once; DLSS-G needs eLowLatency at runtime).
    sl::ReflexOptions ro{};
    ro.mode = sl::ReflexMode::eLowLatency;
    r = g_sl.reflexSetOptions(ro);
    if (r != sl::Result::eOk) FgLog("sl: slReflexSetOptions %d", (int)r);

    void* dev = s.device;
    r = g_sl.upgrade(&dev);                        // ManualHooking.md:195-203
    if (r != sl::Result::eOk || !dev) { FgLog("sl: slUpgradeInterface(device) %d", (int)r); g_sl.state = -FG_ERR_PROVIDER_FAILED; return FG_ERR_PROVIDER_FAILED; }
    g_sl.proxyDevice = (ID3D12Device*)dev;
    g_sl.state = 1;
    return FG_OK;
}

struct ProviderStreamline : IFgProvider
{
    IDXGISwapChain4*    proxy;         // the host's reference
    IDXGIFactory2*      proxyFactory;  // slUpgradeInterface(factory), held while the chain lives
    ID3D12CommandQueue* queue;         // DIRECT queue from the proxy device: the chain's presenting queue
    DXGI_FORMAT         backFmt;
    unsigned            outW, outH, buffers;
    unsigned            framesToGen;   // multiplier - 1
    unsigned            caps;
    int                 wantOn, isOn;  // desired / applied DLSSGMode (render thread applies in Generate)
    unsigned            frames;        // Prepare calls on this chain
    sl::FrameToken*     token;
    int                 marked;        // 1 = RENDERSUBMIT_END/PRESENT_START issued for `token`
    unsigned            lastStatus;
    long long           generated;
    int                 warned;
    sl::float4x4        prevView, prevProj;   // row-vector (transposed Unity) matrices of the previous frame
    int                 havePrev;

    ProviderStreamline() { Zero(); }
    void Zero()
    {
        proxy = NULL; proxyFactory = NULL; queue = NULL; backFmt = DXGI_FORMAT_UNKNOWN; outW = outH = buffers = 0;
        framesToGen = 1; caps = 0; wantOn = isOn = 0; frames = 0; token = NULL; marked = 1; lastStatus = 0;
        generated = 0; warned = 0; havePrev = 0;
    }

    int Id() const { return FG_PROVIDER_DLSS; }
    unsigned Caps() const { return caps; }
    const char* Name() const { return "dlss"; }
    ID3D12CommandQueue* PresentQueue() { return queue; }

    int Fail(int rc) { unsigned c = caps; if (proxy) { proxy->Release(); proxy = NULL; } Destroy(); caps = c; return rc; }

    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        int rc = SlUp(s);
        if (rc != FG_OK) return rc;
        outW = s.desc.Width; outH = s.desc.Height; backFmt = s.desc.Format; buffers = s.desc.BufferCount;
        framesToGen = s.multiplier - 1;
        if (g_sl.maxGen) {                             // known from an earlier chain: refuse before building anything
            caps = CapsFor(g_sl.maxGen);
            if (framesToGen > g_sl.maxGen) return FG_ERR_UNSUPPORTED_MULTIPLIER;
        }

        HWND child = FgHostChildHwnd(s);
        if (!child) { FgLog("sl: no child hwnd - DLSS-G needs its own HWND swapchain"); return FG_ERR_NO_SWAPCHAIN; }

        // sl_hooks.h:61-62 eID3D12Device_CreateCommandQueue is mandatory: the presenting queue comes from the proxy device.
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = g_sl.proxyDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr) || !queue) { FgLog("sl: proxy CreateCommandQueue 0x%08X", (unsigned)hr); return Fail(FG_ERR_PROVIDER_FAILED); }

        void* fac = s.factory;
        sl::Result r = g_sl.upgrade(&fac);              // ManualHooking.md:205-215: the swapchain MUST come from the proxy factory
        if (r != sl::Result::eOk || !fac) { FgLog("sl: slUpgradeInterface(factory) %d", (int)r); return Fail(FG_ERR_PROVIDER_FAILED); }
        if (FAILED(((IUnknown*)fac)->QueryInterface(__uuidof(IDXGIFactory2), (void**)&proxyFactory)) || !proxyFactory) {
            FgLog("sl: proxy factory is not IDXGIFactory2"); ((IUnknown*)fac)->Release(); return Fail(FG_ERR_PROVIDER_FAILED);
        }
        ((IUnknown*)fac)->Release();

        DXGI_SWAP_CHAIN_DESC1 d = s.desc;               // FLIP_DISCARD, app format/size, >= 3 buffers
        d.Scaling = DXGI_SCALING_STRETCH;
        d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        // DLSS-G's present path calls SetMaximumFrameLatency on the real chain (sl.log swapChainData.cpp:471
        // throttleFlipQueue) and presents with tearing when it can: both are DXGI_ERROR_INVALID_CALL on a chain
        // created without these flags (measured: the first proxy Present failed with 0x887A0001 on flags 0).
        BOOL tearing = FALSE;
        IDXGIFactory5* f5 = NULL;
        if (SUCCEEDED(s.factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5))) {
            if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) tearing = FALSE;
            f5->Release();
        }
        d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        IDXGISwapChain1* sc1 = NULL;
        hr = proxyFactory->CreateSwapChainForHwnd(queue, child, &d, NULL, NULL, &sc1);
        if (FAILED(hr) || !sc1) { FgLog("sl: proxy CreateSwapChainForHwnd 0x%08X on child %p", (unsigned)hr, (void*)child); return Fail(FG_ERR_NO_SWAPCHAIN); }
        hr = sc1->QueryInterface(__uuidof(IDXGISwapChain4), (void**)&proxy);
        sc1->Release();
        if (FAILED(hr) || !proxy) { FgLog("sl: proxy chain is not IDXGISwapChain4 0x%08X", (unsigned)hr); proxy = NULL; return Fail(FG_ERR_NO_SWAPCHAIN); }
        s.factory->MakeWindowAssociation(child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

        // DLSS_G.md:815-823: numFramesToGenerateMax (RTX 50 + 310.7.129: 5). SL warns that this query belongs on the
        // present thread; the caps are needed before the chain is live, and the warning is harmless.
        sl::DLSSGState st{};
        sl::ViewportHandle vp(0u);
        r = g_sl.fgGetState(vp, st, NULL);
        unsigned maxGen = (r == sl::Result::eOk && st.numFramesToGenerateMax) ? st.numFramesToGenerateMax : 1;
        g_sl.maxGen = maxGen;
        caps = CapsFor(maxGen);
        FgLog("sl: numFramesToGenerateMax=%u (rc %d, minWidthOrHeight %u, vsync %d) -> caps 0x%X", maxGen, (int)r, st.minWidthOrHeight, (int)st.bIsVsyncSupportAvailable, caps);
        if (framesToGen > maxGen) { FgLog("sl: %ux asked, %ux supported", s.multiplier, maxGen + 1); return Fail(FG_ERR_UNSUPPORTED_MULTIPLIER); }

        *out = proxy;
        void* nat = NULL;
        g_sl.native(proxy, &nat);
        FgLog("sl: proxy swapchain %p (native %p) on child hwnd %p, queue %p, display %ux%u fmt %u x%u buffers flags 0x%X, framesToGenerate=%u",
              (void*)proxy, nat, (void*)child, (void*)queue, outW, outH, (unsigned)backFmt, buffers, d.Flags, framesToGen);
        if (nat) ((IUnknown*)nat)->Release();
        return FG_OK;
    }

    static void Transpose(sl::float4x4& m, const float* u)   // Unity column-vector m00..m33 -> SL row-vector (v * M)
    {
        for (int r = 0; r < 4; ++r) m.setRow(r, sl::float4(u[r], u[4 + r], u[8 + r], u[12 + r]));
    }

    void Options(sl::DLSSGOptions& o, unsigned mode)
    {
        const OwnedSet12* owned = FgOwned12();
        o.mode = (sl::DLSSGMode)mode;
        o.numFramesToGenerate = framesToGen;
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;   // DLSS_G.md:575-582: no stutter on a toggle
        o.numBackBuffers = buffers;
        o.colorWidth = outW; o.colorHeight = outH; o.colorBufferFormat = (unsigned)backFmt;
        if (owned) {
            o.mvecDepthWidth = owned->w; o.mvecDepthHeight = owned->h;
            o.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT; o.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
            o.hudLessBufferFormat = owned->outFmt;
        }
    }

    // Render thread, DLSS_EV_FG_PREPARE (after the upscaler list wrote the owned twins, same Unity queue order).
    void Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f)
    {
        const OwnedSet12* o = FgOwned12();
        if (!proxy || !o || !o->depth || !o->mv || !o->out) return;
        if (o->outFmt != backFmt || o->outW != outW || o->outH != outH) {
            if (warned < 8) { ++warned; FgLog("sl: hudless skipped: out %ux%u fmt %u vs backbuffer %ux%u fmt %u", o->outW, o->outH, (unsigned)o->outFmt, outW, outH, (unsigned)backFmt); }
            return;
        }
        ++frames;
        ++g_sl.frameIdx;
        sl::FrameToken* t = NULL;
        if (g_sl.newFrame(t, &g_sl.frameIdx) != sl::Result::eOk || !t) { if (warned < 8) { ++warned; FgLog("sl: slGetNewFrameToken failed"); } return; }
        token = t;
        marked = 0;
        // Unity's simulation is not reachable from the shim: SIMULATION_* bracket this event (Reflex.md:179-207, PCL.md:118-155).
        g_sl.reflexSleep(*token);
        g_sl.marker(sl::PCLMarker::eSimulationStart, *token);
        g_sl.marker(sl::PCLMarker::eSimulationEnd, *token);
        g_sl.marker(sl::PCLMarker::eRenderSubmitStart, *token);

        // sl_consts.h:183-185 row-major, jitter-free, v * M order (sl_matrix_helpers.h:185-213 builds them so).
        sl::Constants c{};
        sl::float4x4 view, invView, clipToWorld, clipToPrevView;
        Transpose(view, f.view);
        Transpose(c.cameraViewToClip, f.proj);
        sl::matrixFullInvert(c.clipToCameraView, c.cameraViewToClip);
        if (!havePrev || f.reset) { prevView = view; prevProj = c.cameraViewToClip; havePrev = 1; }
        sl::matrixFullInvert(invView, view);
        sl::matrixMul(clipToWorld, c.clipToCameraView, invView);       // clip -> view -> world
        sl::matrixMul(clipToPrevView, clipToWorld, prevView);          // -> previous view
        sl::matrixMul(c.clipToPrevClip, clipToPrevView, prevProj);     // -> previous clip
        sl::matrixFullInvert(c.prevClipToClip, c.clipToPrevClip);
        prevView = view; prevProj = c.cameraViewToClip;
        c.jitterOffset = sl::float2(f.jitterX, f.jitterY);      // NGX convention, the signs Device12.cpp hands DLSS SR
        c.mvecScale = sl::float2(f.mvScaleX / (float)(f.renderW ? f.renderW : 1), f.mvScaleY / (float)(f.renderH ? f.renderH : 1));  // pixels -> [-1,1]
        c.cameraPinholeOffset = sl::float2(0.f, 0.f);
        c.cameraPos   = sl::float3(f.camPos[0], f.camPos[1], f.camPos[2]);
        c.cameraUp    = sl::float3(f.camUp[0], f.camUp[1], f.camUp[2]);
        c.cameraRight = sl::float3(f.camRight[0], f.camRight[1], f.camRight[2]);
        c.cameraFwd   = sl::float3(f.camFwd[0], f.camFwd[1], f.camFwd[2]);
        c.cameraNear = f.cameraNear; c.cameraFar = f.cameraFar; c.cameraFOV = f.cameraFovY;
        c.cameraAspectRatio = f.renderH ? (float)f.renderW / (float)f.renderH : 1.f;
        c.depthInverted = sl::Boolean::eTrue;                   // Unity D3D12 = reversed-Z (DlssDriver.cs:253)
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.orthographicProjection = sl::Boolean::eFalse;
        c.motionVectorsDilated = sl::Boolean::eFalse;
        c.motionVectorsJittered = sl::Boolean::eFalse;
        sl::ViewportHandle vp(0u);
        sl::Result r = g_sl.setConstants(c, *token, vp);
        if (r != sl::Result::eOk && warned < 8) { ++warned; FgLog("sl: slSetConstants %d", (int)r); }

        // Owned twins rest in COMMON (D3D12Owned.h); SL copies them on `list` (eOnlyValidNow, ManualHooking.md:501-517).
        sl::Resource rDepth(sl::ResourceType::eTex2d, o->depth, (uint32_t)D3D12_RESOURCE_STATE_COMMON);
        sl::Resource rMv(sl::ResourceType::eTex2d, o->mv, (uint32_t)D3D12_RESOURCE_STATE_COMMON);
        sl::Resource rHud(sl::ResourceType::eTex2d, o->out, (uint32_t)D3D12_RESOURCE_STATE_COMMON);
        sl::ResourceTag tags[3] = {
            sl::ResourceTag(&rDepth, sl::kBufferTypeDepth,         sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(&rMv,    sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(&rHud,   sl::kBufferTypeHUDLessColor,  sl::ResourceLifecycle::eOnlyValidNow),
        };
        r = g_sl.setTag(*token, vp, tags, 3, list);
        if (r != sl::Result::eOk && warned < 8) { ++warned; FgLog("sl: slSetTagForFrame %d", (int)r); }
    }

    // Render thread, Present hook, before the host copies + presents on the proxy: options (DLSS_G.md:483-491 want
    // the presenting thread), then the markers that close the submit and open the present.
    int Generate(const FgFrame&, ID3D12Resource*, IDXGISwapChain4*, UINT, UINT)
    {
        if (!proxy) return 0;
        if (wantOn != isOn) {
            sl::DLSSGOptions o{};
            Options(o, wantOn ? (unsigned)sl::DLSSGMode::eOn : (unsigned)sl::DLSSGMode::eOff);
            sl::ViewportHandle vp(0u);
            sl::Result r = g_sl.fgSetOptions(vp, o);
            if (r != sl::Result::eOk) { if (warned < 8) { ++warned; FgLog("sl: slDLSSGSetOptions(%d) %d", wantOn, (int)r); } }
            else { isOn = wantOn; FgLog("sl: DLSS-G mode %s, numFramesToGenerate=%u", wantOn ? "eOn" : "eOff", framesToGen); }
        }
        if (token && !marked) {
            marked = 1;
            g_sl.marker(sl::PCLMarker::eRenderSubmitEnd, *token);
            g_sl.marker(sl::PCLMarker::ePresentStart, *token);
        }
        return 0;
    }

    void AfterPresent(HRESULT)
    {
        if (!proxy) return;
        if (token && marked == 1) { marked = 2; g_sl.marker(sl::PCLMarker::ePresentEnd, *token); }
        sl::DLSSGState st{};
        sl::ViewportHandle vp(0u);
        sl::Result r = g_sl.fgGetState(vp, st, NULL);                    // DLSS_G.md:753-783: frames since the last call
        if (r != sl::Result::eOk) { if (warned < 8) { ++warned; FgLog("sl: slDLSSGGetState %d", (int)r); } return; }
        if (st.numFramesActuallyPresented > 1) { generated += st.numFramesActuallyPresented - 1; FgPresentedAdd((int)st.numFramesActuallyPresented - 1); }
        if ((unsigned)st.status != lastStatus) { lastStatus = (unsigned)st.status; FgLog("sl: DLSSGStatus 0x%X (2=Reflex not detected, 8=constants invalid, 16=GetCurrentBackBufferIndex not called)", lastStatus); }
        // Independent evidence: the real child chain's DXGI PresentCount counts DLSS-G's own presents (real + generated).
        if (frames <= 3 || frames % 300 == 0) {
            DXGI_FRAME_STATISTICS fs = {};
            HRESULT hr = proxy->GetFrameStatistics(&fs);
            FgLog("sl: frame %u (token %u): presented=%u status=0x%X mode=%d dxgiPresentCount=%u (hr 0x%08X) generated=%lld", frames, token ? (unsigned)*token : 0u,
                  st.numFramesActuallyPresented, (unsigned)st.status, isOn, fs.PresentCount, (unsigned)hr, generated);
        }
    }

    // ponytail: off is applied HERE (main thread, or the host's teardown with the render thread idle) because DLSS-G
    // must be eOff before the chain is released (DLSS_G.md:745-748); on waits for the presenting thread (Generate).
    void SetEnabled(bool on)
    {
        wantOn = on ? 1 : 0;
        if (!on && proxy && isOn) {
            sl::DLSSGOptions o{};
            Options(o, (unsigned)sl::DLSSGMode::eOff);
            sl::ViewportHandle vp(0u);
            sl::Result r = g_sl.fgSetOptions(vp, o);
            if (r == sl::Result::eOk) isOn = 0; else FgLog("sl: slDLSSGSetOptions(off) %d", (int)r);
        }
    }

    // Caller thread, chain detached; the host already released its proxy reference (FgHost.cpp DestroyChain).
    void Destroy(void)
    {
        proxy = NULL;
        if (queue) { queue->Release(); queue = NULL; }
        if (proxyFactory) { proxyFactory->Release(); proxyFactory = NULL; }
        FgLog("sl: destroyed (%s, generated %lld, lastStatus 0x%X, frames %u)", g_sl.version, generated, lastStatus, frames);
        Zero();                                          // Streamline itself stays up for the process
    }
};

ProviderStreamline g_dlssg;

} // namespace

IFgProvider* MakeFgProviderStreamline(void) { return &g_dlssg; }
