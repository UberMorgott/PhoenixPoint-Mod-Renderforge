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
// Reflex is MANDATORY for DLSS-G (DLSS_G.md:674-681, sl_dlss_g.h:131-132 eFailReflexNotDetectedAtRuntime):
// slReflexSetOptions(eLowLatency [+ frameLimitUs]) while frames FLOW through the chain and, per frame with ONE frame
// index, slReflexSleep + the six PCL markers (sl_pcl.h:60-67) around slSetConstants / slSetTagForFrame and the proxy
// Present. While the chain is live but nothing is fed to it (menu without a camera, loading, HoldPrepare) nothing is
// presented through the proxy and Unity's limiter paces: Reflex goes eOff with no frameLimitUs (FrameBegin(false)), so
// low-latency mode is never left on without its sleep, and slReflexSleep + the simulation markers keep going on every
// frame as Reflex.md:171,247 asks ("must always be called even when Reflex Low Latency mode is Off").
// PACING (docs\DESIGN.md "FG pacing"): the sleep and SIMULATION_START run on the MAIN thread at the start of the frame
// (FrameBegin, before Unity samples input - Reflex wants the sleep there), SIMULATION_END after LateUpdate (SimEnd),
// RENDERSUBMIT_* / PRESENT_* on the render thread with the same index, which travels in FgFrame::paceId. The frame
// rate cap is ReflexOptions::frameLimitUs (DLSS_G.md:769), nothing else. Frame index = our own process-wide counter,
// +1 per paced frame (SL refuses constants set twice for one index). A token is only a view of one of SL's
// MAX_FRAMES_IN_FLIGHT (6) handles and a request for another index rotates that ring (sl.cpp slGetNewFrameToken), so
// no token pointer is ever kept: both threads carry the INDEX and ask for its token right where they use it.
//
// Loading: sl.interposer.dll is LoadLibraryW'd from the mod folder and every entry point GetProcAddress'd (nothing is
// linked); the interposer loads sl.common/sl.dlss_g/sl.reflex/sl.pcl + nvngx_dlssg.dll from pathsToPlugins = the mod
// folder. STREAMLINE IS PINNED FOR THE PROCESS LIFETIME: slInit runs once (it loads plugins and NGX), slSetD3DDevice
// and the device proxy bind it to the FIRST D3D12 device, and there is no slShutdown - the chain, queue and factory
// proxy are per Create and every later Create reuses that device. A Create on a different device (Unity rebuilt its
// device: kUnityGfxDeviceEventShutdown + Initialize) is refused with FG_ERR_PROVIDER_FAILED rather than fed to a
// Streamline that still points at the old one. OTA flags are deliberately off: nothing may be written outside the
// mod folder.
//
// FOCUS GATE (measured 2026-09-02, RTX 5070 Ti, driver 596.49, Instance3): DLSS-G interpolates ONLY while the game
// process owns the foreground window (sl.extra extra.cpp:97-124 hasFocus compares GetForegroundWindow's PID with the
// process / parent PID; no log line at any level, status stays eOk, numFramesActuallyPresented=1). An automated run
// launched from a terminal never has focus, which is why every unattended measurement read ratio 1.00. The development
// plugin honours sl.dlss_g.json {"runWhenNoFocus":true} (dlfgConfig.cpp:85) and then interpolates 2x/3x/4x exactly;
// the production plugin needs the window in the foreground - the normal state when a player plays.
// RENDERFORGE_SL_VERBOSE=1 makes sl.log verbose; sl.interposer.json beside the exe overrides the level (Debugging.md:80-106).
#include "Fg.h"
#include "RenderforgeNative.h"
#include "D3D12Owned.h"
#include "FgLatch.h"

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
    ID3D12Device*               device;        // the device Streamline is pinned to (compared, never dereferenced)
    int                         state;         // 0 not tried, 1 up, <0 failed (FG_ERR_* negated)
    PFun_slReflexGetState*      reflexGetState;
    unsigned                    maxGen;        // DLSSGState::numFramesToGenerateMax once measured, 0 = not yet
    volatile LONG               frameIdx;      // process-wide: SL refuses constants/options set twice for one index
    // slReflexSetOptions (ReflexApply): what the SDK ACCEPTED, the refused request and its back-off, and `bad` - the
    // latest low-latency request is not in effect, DLSS-G must not be fed (FgLatch.h, with the invariant).
    FgReflexLatch               reflex;
    // ONE slDLSSGSetOptions PER STREAMLINE FRAME (DLSS_G.md:483-491: options take effect at the next Present; a second
    // set before that Present is sl.dlss_g's "Repeated slDLSSGSetOptions() call for the frame N. A redundant call or a
    // race condition with Present()"). Streamline's frame advances with the proxy's Present and only with it - a frame
    // that is not prepared never reaches the proxy. `presents` counts those Presents for the process (AfterPresent),
    // `optStamp` is presents + 1 at the last slDLSSGSetOptions that reached the SDK (0 = none yet): equal = a set is
    // already out for the frame the next Present ends. The eOff of a teardown (main thread, DLSS_G.md:745-748: off
    // before the chain is released) and of Idle() are such sets with no Present behind them; the eOn of the next
    // chain / of the resume then waits one prepared frame (IssueOptions kOptHold).
    volatile LONG               presents, optStamp;
    char                        version[64];
};
SlCore g_sl;

// eWarnOutOfVRAM (39, sl_result.h:74) is a WARNING: the call took effect, the DXGI budget is exhausted
// (commonInterface.cpp:556-565). Every DLSS-G result is judged through this - treating 39 as a refusal made the
// teardown's eOff "refused" forever under video-memory overcommit (16781 retries in one session, the chain never
// destroyed, frame generation stuck off) and dropped the generated-frame count (slDLSSGGetState).
bool SlTookEffect(sl::Result r) { return r == sl::Result::eOk || r == sl::Result::eWarnOutOfVRAM; }

bool SlOptionsIssuedThisFrame() { return InterlockedCompareExchange(&g_sl.optStamp, 0, 0) == InterlockedCompareExchange(&g_sl.presents, 0, 0) + 1; }
void SlOptionsIssued() { InterlockedExchange(&g_sl.optStamp, InterlockedCompareExchange(&g_sl.presents, 0, 0) + 1); }

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

// The token of frame `idx`, valid for the calls that follow at once on this thread (header: no pointer is kept).
sl::FrameToken* Tok(unsigned idx)
{
    sl::FrameToken* t = NULL;
    uint32_t i = idx;
    return g_sl.newFrame(t, &i) == sl::Result::eOk ? t : NULL;
}

// Reflex mode + frame cap, only when they change (Reflex.md:176-177; "the value is used each time you call
// slReflexSetOptions", sl_reflex.h:49). Thread safe on SL's side (reflexEntry.cpp slSetData); called on the main thread.
// SDK RESULTS (docs\DESIGN.md "SDK results decide state"): reflexMode / reflexUs record what the SDK ACCEPTED. A
// refusal records nothing, is retried kReflexRetry calls later (FrameBegin calls this every frame: not once per frame
// against a call that keeps failing) and is logged with a throttle. DLSS-G needs Reflex at run time (DLSS_G.md
// eFailReflexNotDetectedAtRuntime; the cap, DLSS_G.md:769): while the last low-latency request stands refused
// (reflex.bad) Prepare feeds DLSS-G nothing, and the managed starvation watchdog drops the provider if that lasts.
// A request for what the SDK already accepted is in effect without a call and CLEARS reflex.bad (FgLatch.h invariant:
// cap 120 working -> 144 refused once -> back to 120 must feed DLSS-G again).
// True = the mode and cap asked for are in effect.
bool ReflexTestRefuse(unsigned frameLimitUs)   // lever: "sl_reflex" = every request, "sl_reflex@<us>" = the ones for that cap
{
    if (FgTestSdkFail("sl_reflex")) return true;
    char name[32];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "sl_reflex@%u", frameLimitUs);
    return FgTestSdkFail(name);
}
bool ReflexApply(sl::ReflexMode mode, unsigned frameLimitUs)
{
    FgReflexLatch& l = g_sl.reflex;
    const bool needed = mode != sl::ReflexMode::eOff;
    sl::Result r = sl::Result::eOk;
    const int step = l.Apply((int)mode, frameLimitUs, needed, [&]() {
        sl::ReflexOptions ro{};
        ro.mode = mode;
        ro.frameLimitUs = frameLimitUs;
        r = ReflexTestRefuse(frameLimitUs) ? sl::Result::eErrorInvalidParameter : g_sl.reflexSetOptions(ro);
        return SlTookEffect(r);                          // 39 = applied under an exhausted DXGI budget, not a refusal
    });
    if (step == FgReflexLatch::kAccepted) FgLog("sl: Reflex mode %d frameLimitUs %u -> %d", (int)mode, frameLimitUs, (int)r);
    else if (step == FgReflexLatch::kRefused && (l.fails <= 8 || l.fails % 64 == 0))
        FgLog("sl: slReflexSetOptions(mode %d, frameLimitUs %u) REFUSED %d (#%u in a row) - in effect: mode %d, %u us; retry in %d frames%s", (int)mode, frameLimitUs, (int)r, l.fails,
              l.mode, l.us, (int)FgReflexLatch::kRetry, needed ? "; DLSS-G is fed nothing meanwhile" : "");
    return step > 0;
}

// Once per process. Returns FG_OK or the FG_ERR_* to hand back from Create.
int SlUp(const FgSetup& s)
{
    if (g_sl.state > 0) {
        if (g_sl.device == s.device) return FG_OK;
        FgLog("sl: device %p != %p Streamline is pinned to (no slShutdown) - DLSS-G refused on this device", (void*)s.device, (void*)g_sl.device);
        return FG_ERR_PROVIDER_FAILED;
    }
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
    void* gs = NULL;                                // optional: latency statistics only
    if (g_sl.featureFn(sl::kFeatureReflex, "slReflexGetState", gs) == sl::Result::eOk) g_sl.reflexGetState = (PFun_slReflexGetState*)gs;
    sl::FeatureVersion fv{};
    if (g_sl.featureVersion(sl::kFeatureDLSS_G, fv) == sl::Result::eOk)
        _snprintf_s(g_sl.version, sizeof(g_sl.version), _TRUNCATE, "SL %u.%u.%u NGX %u.%u.%u", fv.versionSL.major, fv.versionSL.minor, fv.versionSL.build, fv.versionNGX.major, fv.versionNGX.minor, fv.versionNGX.build);
    FgLog("sl: DLSS-G %s", g_sl.version);

    // Reflex options are issued per chain (Create: eLowLatency, DLSS-G needs it at runtime; Destroy: eOff, no cap).
    g_sl.reflex.mode = -1;

    void* dev = s.device;
    r = g_sl.upgrade(&dev);                        // ManualHooking.md:195-203
    if (r != sl::Result::eOk || !dev) { FgLog("sl: slUpgradeInterface(device) %d", (int)r); g_sl.state = -FG_ERR_PROVIDER_FAILED; return FG_ERR_PROVIDER_FAILED; }
    g_sl.proxyDevice = (ID3D12Device*)dev;
    g_sl.device = s.device;
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
    unsigned            sentW, sentH;  // mvecDepthWidth/Height last issued via slDLSSGSetOptions (re-issued on a quality switch)
    unsigned            failW, failH;  // last refused options (size, mode) + result: logged once, and what optSkip backs off
    int                 failRc, failOn;
    // SDK RESULTS: Prepares to let pass before refused options are issued again, Prepares in a row whose constants /
    // tags the SDK refused, and `dead` - those reached kInputFailMax: the provider prepares nothing more.
    enum { kOptRetry = 30, kInputFailMax = 120 };
    unsigned            optSkip, inputFails;
    int                 dead;
    unsigned            epoch;         // the host's chain generation this provider was created for (FgSetup::epoch)
    unsigned            frames;        // Prepare calls on this chain
    // Frame indices awaiting their proxy Present, in order: Prepare pushes one (constants + tags carry it), BeforePresent
    // pops exactly one per Present so the PRESENT_START/END markers name the frame whose inputs were tagged (DLSS_G.md:933).
    // A Present the host skipped (GetBuffer / copy failed) or a prepared frame superseded before its Present leaves its
    // index behind: the host's next FgHostPrepare drops every unpresented one (DropPrepared), so at most one is queued.
    // The full-ring guard below stays as a backstop.
    static const unsigned kTokens = 4;
    unsigned            fifo[kTokens];
    unsigned            head, tail;
    unsigned            cur;           // the index of the frame being presented (popped in BeforePresent), 0 = none
    int                 marked;        // 1 = RENDERSUBMIT_END/PRESENT_START issued for `cur`, 2 = PRESENT_END too
    int                 mainPacing;    // FgMainPacing() at Create: 1 = FrameBegin sleeps (main thread), 0 = Prepare does
    int                 paced;         // main thread: the last FrameBegin was a paced one (Reflex eLowLatency + the cap)
    unsigned            lastPrepared;  // render thread: the index of the last Prepare (one Prepare per index)
    unsigned            limitFps;      // main thread: the presented-fps ceiling asked for (SetFrameLimit), 0 = none
    unsigned            lastStatus;
    long long           generated;
    int                 warned;
    sl::float4x4        prevView, prevProj;   // row-vector (transposed Unity) matrices of the previous frame
    int                 havePrev;

    ProviderStreamline() { Zero(); }
    void Zero()
    {
        proxy = NULL; proxyFactory = NULL; queue = NULL; backFmt = DXGI_FORMAT_UNKNOWN; outW = outH = buffers = 0;
        framesToGen = 1; caps = 0; wantOn = isOn = 0; sentW = sentH = failW = failH = 0; failRc = 0; failOn = 0; frames = 0; cur = 0; marked = 0; lastStatus = 0;
        for (unsigned i = 0; i < kTokens; ++i) fifo[i] = 0;
        head = tail = 0; mainPacing = 1; paced = 0; lastPrepared = 0; limitFps = 0;
        generated = 0; warned = 0; havePrev = 0;
        optSkip = 0; inputFails = 0; dead = 0; epoch = 0;
    }

    int Id() const { return FG_PROVIDER_DLSS; }
    unsigned Caps() const { return caps; }
    const char* Name() const { return "dlss"; }
    ID3D12CommandQueue* PresentQueue() { return queue; }

    int Fail(int rc) { unsigned c = caps; if (proxy) { proxy->Release(); proxy = NULL; } Destroy(true); caps = c; return rc; }

    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        int rc = SlUp(s);
        if (rc != FG_OK) return rc;
        outW = s.desc.Width; outH = s.desc.Height; backFmt = s.desc.Format; buffers = s.desc.BufferCount;
        framesToGen = s.multiplier - 1;
        mainPacing = FgMainPacing() ? 1 : 0;
        epoch = s.epoch;
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
        queue->SetName(L"Renderforge DLSS-G present queue");

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
        // eWarnOutOfVRAM answers the query all the same (SlTookEffect). A query that failed is not a measurement: the
        // value an earlier chain measured stands (0 = never measured: 2x only), or a 3x/4x rebuild under memory
        // pressure would be refused as unsupported and the picker would lose 3x/4x.
        const bool measured = SlTookEffect(r) && st.numFramesToGenerateMax;
        unsigned maxGen = measured ? st.numFramesToGenerateMax : (g_sl.maxGen ? g_sl.maxGen : 1);
        if (measured) g_sl.maxGen = maxGen;            // a failed query is not a measurement: asked again with the next chain
        caps = CapsFor(maxGen);
        FgLog("sl: numFramesToGenerateMax=%u (rc %d, minWidthOrHeight %u, vsync %d) -> caps 0x%X", maxGen, (int)r, st.minWidthOrHeight, (int)st.bIsVsyncSupportAvailable, caps);
        if (framesToGen > maxGen) { FgLog("sl: %ux asked, %ux supported", s.multiplier, maxGen + 1); return Fail(FG_ERR_UNSUPPORTED_MULTIPLIER); }

        // Main-thread pacing: Reflex stays eOff until the first paced FrameBegin (frames flow); this also is the "at
        // least once" slReflexSetOptions of Reflex.md:177. Render-thread pacing (diagnostic) sleeps in Prepare: on now.
        g_sl.reflex.NewChain();                         // a refusal / back-off of the previous chain does not gate this one
        if (mainPacing) ReflexApply(sl::ReflexMode::eOff, 0);
        else ReflexApply(sl::ReflexMode::eLowLatency, LimitUs());
        *out = proxy;
        void* nat = NULL;
        g_sl.native(proxy, &nat);
        FgLog("sl: proxy swapchain %p (native %p) on child hwnd %p, queue %p, display %ux%u fmt %u x%u buffers flags 0x%X, framesToGenerate=%u",
              (void*)proxy, nat, (void*)child, (void*)queue, outW, outH, (unsigned)backFmt, buffers, d.Flags, framesToGen);
        if (nat) ((IUnknown*)nat)->Release();
        return FG_OK;
    }

    static void Rows(sl::float4x4& m, const float* u)   // FgFrame row-vector, row-major = sl::float4x4 as is
    {
        for (int r = 0; r < 4; ++r) m.setRow(r, sl::float4(u[4 * r], u[4 * r + 1], u[4 * r + 2], u[4 * r + 3]));
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
            o.hudLessBufferFormat = (unsigned)backFmt;   // what FgHudless12 hands Prepare (FgTwins.h)
        }
    }

    // Frame boundary, render thread inside the prepare event (DLSS_G.md:483-491: options take effect at the next Present).
    // The presenting thread is another one (Unity's D3D12 submission thread); the host's Pin serialises this event with
    // the Present of the frame before it, so no Present runs while the options change. Issued before this frame's token,
    // constants and tags. The options carry the mvec/depth
    // size (sl_dlss_g.h:89-91): a quality switch resizes the owned twins while the chain (outW/outH/backFmt) stays - re-issue
    // on a size change, or DLSS-G interpolates with stale dimensions (heat-haze shimmer at Quality/Performance, DLAA fine).
    // eWarnOutOfVRAM (39, sl_result.h:74) = options applied, DXGI budget exhausted (commonInterface.cpp:556-565); treating it
    // as a failure re-issued the options every frame until the device was removed (measured 2026-09-03).
    // SDK RESULTS: isOn / sentW / sentH record what the SDK ACCEPTED. True = the mode and the mvec/depth size DLSS-G
    // has are the ones this frame needs. False = they are not (refused, or waiting out the kOptRetry back-off): the
    // caller prepares NOTHING - no token, no constants, no tags reach a DLSS-G that is off or sized for other inputs,
    // and the frame goes out the idle way. A refusal is logged once per size/result; if it lasts, the starvation
    // watchdog drops the provider.
    // kOptHold = a slDLSSGSetOptions is already out for this Streamline frame (SlCore::optStamp: the eOff of the
    // previous chain's teardown, or of Idle()): this frame's set would be the "Repeated slDLSSGSetOptions() call" and
    // race the Present that applies the first. The frame is prepared all the same - token, constants, tags, markers -
    // and presented through the proxy with the options DLSS-G has (off: passed through, nothing generated), which is
    // what ends the Streamline frame; the set goes out with the next prepare. Refusing the frame instead would
    // deadlock: an unprepared frame bypasses the proxy, the Streamline frame never ends. Cost: the first frame of a
    // chain / after an idle stretch is not interpolated.
    // The hold also covers isOn == wantOn == 1 with only the SIZE changed (review 5 #10): that state is reached when
    // the resize set of this very Streamline frame was REFUSED (the stamp is taken, isOn / sentW / sentH still describe
    // the old size, the refused frame went out unprepared and ended no Streamline frame). One frame is then tagged at
    // the new size against the old-size options - DLSS-G skips or mis-scales that one frame - and its Present is what
    // lets the retry go out. Refusing instead would leave the frame to Idle(), which turns DLSS-G off with a second
    // set inside the same Streamline frame: a repeated call AND an off/on cycle for the same one frame.
    enum { kOptRefused = 0, kOptReady = 1, kOptHold = 2 };
    int IssueOptions(unsigned w, unsigned h)
    {
        if (wantOn == isOn && (!wantOn || (w == sentW && h == sentH))) return kOptReady;
        if (SlOptionsIssuedThisFrame()) return kOptHold;
        // The back-off belongs to the REQUEST that was refused: another one (the size switched back, or on again
        // after Idle) is issued at once instead of waiting out a refusal that is not about it.
        if (optSkip && wantOn == failOn && w == failW && h == failH) { --optSkip; return kOptRefused; }
        sl::DLSSGOptions op{};
        Options(op, wantOn ? (unsigned)sl::DLSSGMode::eOn : (unsigned)sl::DLSSGMode::eOff);
        sl::ViewportHandle vp(0u);
        sl::Result r = sl::Result::eErrorInvalidParameter;
        if (!FgTestSdkFail("sl_options")) { r = g_sl.fgSetOptions(vp, op); SlOptionsIssued(); }
        if (SlTookEffect(r)) {
            isOn = wantOn; sentW = w; sentH = h; failRc = 0; optSkip = 0;
            FgLog("sl: DLSS-G mode %s, numFramesToGenerate=%u, options %ux%u -> %ux%u fmt %u%s", wantOn ? "eOn" : "eOff", framesToGen, w, h, outW, outH, (unsigned)backFmt,
                  r == sl::Result::eWarnOutOfVRAM ? " (eWarnOutOfVRAM: DXGI budget exhausted)" : "");
            return kOptReady;
        }
        optSkip = kOptRetry;
        const bool news = failRc != (int)r || failW != w || failH != h || failOn != wantOn;
        failRc = (int)r; failW = w; failH = h; failOn = wantOn;
        if (news) {
            FgLog("sl: slDLSSGSetOptions(%d) REFUSED %d for %ux%u - DLSS-G stays %s (%ux%u), frames are not prepared, retry every %d prepares", wantOn, (int)r, w, h,
                  isOn ? "eOn" : "eOff", sentW, sentH, (int)kOptRetry);
        }
        return kOptRefused;
    }

    // frameLimitUs is the interval between PRESENTED frames: the driver knows about frame generation and spaces the
    // rendered frames multiplier times wider itself (measured 2026-10-12 with DLSS-G generating, docs\DESIGN.md "FG
    // pacing": 8333 us -> 60 rendered / 120 presented at 2x, 40 / 120 at 3x; with nothing generated - DLSS-G off or the
    // window not in the foreground - rendered = presented = 120). So the ceiling goes in as it is, never divided.
    unsigned LimitUs() const { return limitFps ? 1000000u / limitFps : 0u; }

    // Main thread. Reflex's own limiter is the only cap while frames flow (DLSS_G.md:769); while they do not, the cap
    // is only remembered - FrameBegin(true) applies it together with eLowLatency.
    // False = Reflex refused its last low-latency request: this provider does NOT enforce the cap now, the caller keeps
    // its own limiter (and the refusal is being retried, ReflexApply).
    bool SetFrameLimit(unsigned presentedFps)
    {
        limitFps = presentedFps;
        if (proxy && (paced || !mainPacing)) ReflexApply(sl::ReflexMode::eLowLatency, LimitUs());
        return !g_sl.reflex.bad;
    }

    // Main thread (Fg.h). HELD: the Reflex options the SDK last ACCEPTED are low latency with exactly the cap wanted,
    // and the sleep that enforces them runs (a paced frame). On a fresh chain the first paced FrameBegin has not asked
    // yet, a refused or backed-off request leaves the previous options in effect: neither is held, the caller's
    // limiter stays on. GENERATING: slDLSSGSetOptions(eOn) was accepted and Prepare feeds DLSS-G (not `dead`, Reflex
    // not refused - Prepare's own gate).
    int PacerState()
    {
        if (!proxy) return 0;
        const FgReflexLatch& l = g_sl.reflex;
        const bool held = limitFps && (paced || !mainPacing) && !l.bad && l.mode == (int)sl::ReflexMode::eLowLatency && l.us == LimitUs();
        return (held ? FG_PACER_HELD : 0) | (isOn && !dead && !l.bad ? FG_PACER_GENERATING : 0);
    }

    // Main thread, start of every frame: a fresh index, the Reflex sleep, SIMULATION_START (Reflex guide: sleep at the
    // start of the simulation thread's frame, before input; SIMULATION_START right after it). `pace` decides who paces:
    // true = Reflex (eLowLatency + frameLimitUs), false = Unity's limiter (Reflex eOff, no cap: the sleep returns at
    // once). ReflexApply only talks to SL when the mode or the cap changes.
    unsigned FrameBegin(bool pace)
    {
        if (!proxy || !mainPacing) return 0;
        paced = pace ? 1 : 0;
        if (pace) ReflexApply(sl::ReflexMode::eLowLatency, LimitUs());
        else ReflexApply(sl::ReflexMode::eOff, 0);
        unsigned idx = (unsigned)InterlockedIncrement(&g_sl.frameIdx);
        sl::FrameToken* t = Tok(idx);
        if (!t) return 0;
        g_sl.reflexSleep(*t);
        g_sl.marker(sl::PCLMarker::eSimulationStart, *t);
        return idx;
    }

    void SimEnd(unsigned idx)
    {
        sl::FrameToken* t = proxy && mainPacing && idx ? Tok(idx) : NULL;
        if (t) g_sl.marker(sl::PCLMarker::eSimulationEnd, *t);
    }

    // The presenting thread (Unity's D3D12 submission thread, inside the Present hook, chain pinned - so no Prepare runs
    // on the render thread meanwhile). Nothing reaches the proxy until frames resume (menus without a camera,
    // loading): DLSS-G goes eOff (DLSS_G.md:29 "when the game is paused, loading, in menus"), and the next Prepare's
    // IssueOptions turns it back on; eRetainResourcesWhenOff keeps the toggle free of reallocation.
    void Idle()
    {
        if (!proxy || !isOn) return;
        sl::DLSSGOptions o{};
        Options(o, (unsigned)sl::DLSSGMode::eOff);
        sl::ViewportHandle vp(0u);
        sl::Result r = g_sl.fgSetOptions(vp, o);
        SlOptionsIssued();
        if (SlTookEffect(r)) isOn = 0;
        FgLog("sl: DLSS-G eOff while no frames are prepared -> %d", (int)r);
    }

    // Main thread. Reflex's latency report over the frames it still holds (sl_reflex.h ReflexReport, microseconds).
    void Stats(char* buf, int cap)
    {
        if (cap <= 0) return;
        buf[0] = 0;
        if (!g_sl.reflexGetState) return;
        static sl::ReflexState st;                       // ~10 KB: not on the stack
        st = sl::ReflexState();
        sl::Result r = g_sl.reflexGetState(st);
        if (!SlTookEffect(r)) { _snprintf_s(buf, cap, _TRUNCATE, "reflex: slReflexGetState %d", (int)r); return; }
        unsigned n = 0;
        double sim = 0, toPresent = 0, toGpuEnd = 0, gpu = 0;
        for (int i = 0; i < sl::kReflexFrameReportCount; ++i) {
            const sl::ReflexReport& e = st.frameReport[i];
            if (!e.simStartTime || e.gpuRenderEndTime <= e.simStartTime || e.presentEndTime < e.simStartTime) continue;
            ++n;
            sim += (double)(e.simEndTime - e.simStartTime);
            toPresent += (double)(e.presentEndTime - e.simStartTime);
            toGpuEnd += (double)(e.gpuRenderEndTime - e.simStartTime);
            gpu += (double)e.gpuActiveRenderTimeUs;
        }
        double k = n ? 1.0 / (1000.0 * n) : 0.0;
        _snprintf_s(buf, cap, _TRUNCATE, "reflex: lowLatency=%d report=%d frames=%u simMs=%.2f simToPresentEndMs=%.2f simToGpuEndMs=%.2f gpuActiveMs=%.2f mode=%d frameLimitUs=%u pacing=%s",
                    st.lowLatencyAvailable ? 1 : 0, st.latencyReportAvailable ? 1 : 0, n, sim * k, toPresent * k, toGpuEnd * k, gpu * k,
                    g_sl.reflex.mode, g_sl.reflex.us, mainPacing ? "main" : "render");
    }

    // Render thread, DLSS_EV_FG_PREPARE (after the upscaler list wrote the owned twins, same Unity queue order).
    bool Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f)
    {
        const OwnedSet12* o = FgOwned12();
        ID3D12Resource* depth = FgDepth12();
        ID3D12Resource* mv = FgMv12();
        if (!proxy || !o || !depth || !mv) return false;
        if (f.epoch != epoch) return false;                // a block of another chain generation (the host drops these first)
        // SDK RESULTS: a provider whose inputs the SDK kept refusing, or whose Reflex low-latency mode was refused,
        // prepares nothing - the starvation watchdog sees it.
        if (dead || g_sl.reflex.bad) return false;
        // Main-thread pacing: this frame's index was opened by FrameBegin. None = the frame was fed before the pacer
        // started (the first one after an idle stretch): left unprepared, the host presents it the idle way. The same
        // index twice = a second prepare event inside one frame: one set of constants per index.
        if (mainPacing && (!f.paceId || f.paceId == lastPrepared)) return false;
        if (IssueOptions(o->w, o->h) == kOptRefused) return false;   // kOptHold: prepared, the set follows next frame
        // The hud-less in the back buffer's format and row order (FgHudless12, the host's flipped copy). Without one the
        // frame still gets its token, constants and depth/mv tags - a skipped hudless is a quality loss, a skipped
        // token/constants wedges DLSS-G ("missing common constants", generated=0; measured 2026-09-03).
        DXGI_FORMAT hudFmt = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* hud = FgHudless12(&hudFmt);
        if (!hud || hudFmt != backFmt || o->outW != outW || o->outH != outH) {
            if (warned < 8) { ++warned; FgLog("sl: hudless skipped: out %ux%u fmt %u vs backbuffer %ux%u fmt %u", o->outW, o->outH, (unsigned)hudFmt, outW, outH, (unsigned)backFmt); }
            hud = NULL;
        }
        FgNoteHudless(hud != NULL);
        if (tail - head >= kTokens) { if (warned < 8) { ++warned; FgLog("sl: %u frames prepared without a Present - dropping the oldest token", kTokens); } ++head; }
        ++frames;
        const unsigned idx = mainPacing ? f.paceId : (unsigned)InterlockedIncrement(&g_sl.frameIdx);
        sl::FrameToken* t = Tok(idx);
        if (!t) { if (warned < 8) { ++warned; FgLog("sl: slGetNewFrameToken failed"); } return false; }
        lastPrepared = idx;
        fifo[tail++ % kTokens] = idx;
        if (!mainPacing) {
            // Render-thread pacing (diagnostic): the simulation is not reachable from here, SIMULATION_* bracket this event.
            g_sl.reflexSleep(*t);
            g_sl.marker(sl::PCLMarker::eSimulationStart, *t);
            g_sl.marker(sl::PCLMarker::eSimulationEnd, *t);
        }
        g_sl.marker(sl::PCLMarker::eRenderSubmitStart, *t);

        // sl_consts.h:183-185 row-major, jitter-free, v * M order (sl_matrix_helpers.h:206-214 chains them so). FgFrame is
        // already that: left-handed view space spanned by cameraRight/Up/Fwd, the basis sl_matrix_helpers.h:185-190 puts
        // into cameraViewToWorld, so clipToCameraView * [right up fwd pos] is a valid clip -> world there too.
        sl::Constants c{};
        sl::float4x4 view, invView, clipToWorld, clipToPrevView;
        Rows(view, f.view);
        Rows(c.cameraViewToClip, f.proj);
        sl::matrixFullInvert(c.clipToCameraView, c.cameraViewToClip);
        if (!havePrev || f.reset) { prevView = view; prevProj = c.cameraViewToClip; havePrev = 1; }
        sl::matrixFullInvert(invView, view);
        sl::matrixMul(clipToWorld, c.clipToCameraView, invView);       // clip -> view -> world
        sl::matrixMul(clipToPrevView, clipToWorld, prevView);          // -> previous view
        sl::matrixMul(c.clipToPrevClip, clipToPrevView, prevProj);     // -> previous clip
        sl::matrixFullInvert(c.prevClipToClip, c.clipToPrevClip);
        prevView = view; prevProj = c.cameraViewToClip;
        c.jitterOffset = sl::float2(f.jitterX, f.jitterY);      // pixels (sl_consts.h:201-202), NGX offsets in top-down rows (Fg.h)
        c.mvecScale = sl::float2(f.mvScaleX / (float)(f.renderW ? f.renderW : 1), f.mvScaleY / (float)(f.renderH ? f.renderH : 1));  // pixels -> [-1,1]
        c.cameraPinholeOffset = sl::float2(0.f, 0.f);
        c.cameraPos   = sl::float3(f.camPos[0], f.camPos[1], f.camPos[2]);
        c.cameraUp    = sl::float3(f.camUp[0], f.camUp[1], f.camUp[2]);
        c.cameraRight = sl::float3(f.camRight[0], f.camRight[1], f.camRight[2]);
        c.cameraFwd   = sl::float3(f.camFwd[0], f.camFwd[1], f.camFwd[2]);
        c.cameraNear = f.cameraNear; c.cameraFar = f.cameraFar; c.cameraFOV = f.cameraFovY;
        c.cameraAspectRatio = f.renderH ? (float)f.renderW / (float)f.renderH : 1.f;
        c.depthInverted = sl::Boolean::eTrue;                   // reversed Z: closer = higher (sl_consts.h:228-229), as f.proj maps it
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.orthographicProjection = sl::Boolean::eFalse;
        c.motionVectorsDilated = sl::Boolean::eFalse;
        c.motionVectorsJittered = sl::Boolean::eFalse;
        if (FgCaptureArmed()) {
            FgCaptureFloats("sl.cameraViewToClip", &c.cameraViewToClip[0].x, 16);
            FgCaptureFloats("sl.clipToCameraView", &c.clipToCameraView[0].x, 16);
            FgCaptureFloats("sl.clipToPrevClip", &c.clipToPrevClip[0].x, 16);
            FgCaptureFloats("sl.prevClipToClip", &c.prevClipToClip[0].x, 16);
            FgCaptureFloats("sl.jitterOffset", &c.jitterOffset.x, 2);
            FgCaptureFloats("sl.mvecScale", &c.mvecScale.x, 2);
            FgCaptureFloats("sl.cameraPos", &c.cameraPos.x, 3);
            FgCaptureFloats("sl.cameraUp", &c.cameraUp.x, 3);
            FgCaptureFloats("sl.cameraRight", &c.cameraRight.x, 3);
            FgCaptureFloats("sl.cameraFwd", &c.cameraFwd.x, 3);
            FgCaptureNote("sl.near=%.9g far=%.9g fov=%.9g aspect=%.9g depthInverted=%d reset=%d frameIdx=%u mode=%d", c.cameraNear, c.cameraFar, c.cameraFOV,
                          c.cameraAspectRatio, (int)c.depthInverted, (int)c.reset, idx, isOn);
        }
        sl::ViewportHandle vp(0u);
        sl::Result r = g_sl.setConstants(c, *t, vp);
        bool inputBad = !SlTookEffect(r);                  // 39 (eWarnOutOfVRAM) = taken, not a refused input
        if (r != sl::Result::eOk && warned < 8) { ++warned; FgLog("sl: slSetConstants %d", (int)r); }

        // The host's flipped copies rest in COMMON (FgTwins.h); SL copies them on `list` (eOnlyValidNow, ManualHooking.md:501-517).
        sl::Resource rDepth(sl::ResourceType::eTex2d, depth, (uint32_t)D3D12_RESOURCE_STATE_COMMON);
        sl::Resource rMv(sl::ResourceType::eTex2d, mv, (uint32_t)D3D12_RESOURCE_STATE_COMMON);
        sl::Resource rHud(sl::ResourceType::eTex2d, hud, (uint32_t)D3D12_RESOURCE_STATE_COMMON);
        sl::Extent eRender{}, eOut{};                            // full-size extents, explicit (SL warns on an unset 0x0 extent)
        eRender.width = o->w; eRender.height = o->h;
        eOut.width = o->outW; eOut.height = o->outH;
        sl::ResourceTag tags[3] = {
            sl::ResourceTag(&rDepth, sl::kBufferTypeDepth,         sl::ResourceLifecycle::eOnlyValidNow, &eRender),
            sl::ResourceTag(&rMv,    sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &eRender),
            sl::ResourceTag(&rHud,   sl::kBufferTypeHUDLessColor,  sl::ResourceLifecycle::eOnlyValidNow, &eOut),
        };
        r = g_sl.setTag(*t, vp, tags, hud ? 3 : 2, list);
        if (!SlTookEffect(r)) inputBad = true;
        if (r != sl::Result::eOk && warned < 8) { ++warned; FgLog("sl: slSetTagForFrame %d", (int)r); }
        // One refused constants/tag call stays warn-only: the token is in the FIFO and only a Present pops it
        // (BeforePresent), and DLSS-G skips a frame it has no valid inputs for (DLSSGStatus). Refused kInputFailMax
        // frames in a row it is a provider that generates nothing while every counter says "prepared": dead from the
        // next Prepare on, where the starvation watchdog can see it.
        inputFails = inputBad ? inputFails + 1 : 0;
        if (inputFails >= kInputFailMax && !dead) {
            dead = 1;
            FgLog("sl: slSetConstants / slSetTagForFrame refused %u frames in a row (last %d) - provider FAILED for this chain: no frame is prepared any more", inputFails, (int)r);
        }
        return true;
    }

    // Render thread. No Present will pop the unpresented tokens (the host is starting a new frame, or could not submit
    // this frame's prep list): drop them ALL, so the next Present pops exactly the token of the frame it presents.
    void DropPrepared() { head = tail; }

    // Presenting thread, Present hook: nothing to do here - the options went out in Prepare (IssueOptions), the markers
    // come after the copy, in BeforePresent.
    int Generate(const FgFrame&, ID3D12Resource*, IDXGISwapChain4*, UINT, UINT) { return 0; }

    // Presenting thread, the copy into the proxy's current back buffer is submitted, Present is next: pop THIS frame's token
    // and close its submit / open its present (sl_dlss_g: GetBuffer -> writes -> RENDERSUBMIT_END -> PRESENT_START -> Present).
    void BeforePresent()
    {
        if (!proxy) return;
        cur = 0; marked = 0;
        if (head == tail) { if (warned < 8) { ++warned; FgLog("sl: Present without a prepared frame - no markers"); } return; }
        cur = fifo[head++ % kTokens];
        sl::FrameToken* t = Tok(cur);
        if (!t) { cur = 0; return; }
        marked = 1;
        g_sl.marker(sl::PCLMarker::eRenderSubmitEnd, *t);
        g_sl.marker(sl::PCLMarker::ePresentStart, *t);
    }

    void AfterPresent(HRESULT)
    {
        InterlockedIncrement(&g_sl.presents);              // the proxy's Present ended a Streamline frame (SlCore::optStamp)
        if (!proxy) return;
        if (cur && marked == 1) { marked = 2; sl::FrameToken* t = Tok(cur); if (t) g_sl.marker(sl::PCLMarker::ePresentEnd, *t); }
        sl::DLSSGState st{};
        sl::ViewportHandle vp(0u);
        sl::Result r = g_sl.fgGetState(vp, st, NULL);                    // DLSS_G.md:753-783: frames since the last call
        if (!SlTookEffect(r)) { if (warned < 8) { ++warned; FgLog("sl: slDLSSGGetState %d", (int)r); } return; }
        if (st.numFramesActuallyPresented > 1) { generated += st.numFramesActuallyPresented - 1; FgPresentedAdd((int)st.numFramesActuallyPresented - 1); }
        if ((unsigned)st.status != lastStatus) { lastStatus = (unsigned)st.status; FgLog("sl: DLSSGStatus 0x%X (2=Reflex not detected, 8=constants invalid, 16=GetCurrentBackBufferIndex not called)", lastStatus); }
        // Independent evidence: the real child chain's DXGI PresentCount counts DLSS-G's own presents (real + generated).
        if (frames <= 3 || frames % 300 == 0) {
            DXGI_FRAME_STATISTICS fs = {};
            HRESULT hr = proxy->GetFrameStatistics(&fs);
            FgLog("sl: frame %u (token %u): presented=%u status=0x%X mode=%d dxgiPresentCount=%u (hr 0x%08X) generated=%lld", frames, cur,
                  st.numFramesActuallyPresented, (unsigned)st.status, isOn, fs.PresentCount, (unsigned)hr, generated);
        }
    }

    // Main thread. True once DLSS-G is eOff (or was never on).
    // Deliberately NOT gated on SlOptionsIssuedThisFrame (review 5 #11): Off runs for a disable / teardown, where no
    // Present of this chain follows that could end the Streamline frame, and DLSS-G must be eOff before the chain is
    // released (DLSS_G.md:745-748). If the eOn of the same frame is still out, Streamline logs "Repeated
    // slDLSSGSetOptions() call" once - the line below says so - and the later call (eOff) is the one that stands.
    bool Off()
    {
        if (!proxy || !isOn) return true;
        if (SlOptionsIssuedThisFrame()) FgLog("sl: slDLSSGSetOptions(off) follows a set of the same Streamline frame (teardown before its Present) - a 'Repeated slDLSSGSetOptions' warning from Streamline is expected here");
        sl::DLSSGOptions o{};
        Options(o, (unsigned)sl::DLSSGMode::eOff);
        sl::ViewportHandle vp(0u);
        sl::Result r = g_sl.fgSetOptions(vp, o);
        SlOptionsIssued();
        if (SlTookEffect(r)) { isOn = 0; if (r != sl::Result::eOk) FgLog("sl: slDLSSGSetOptions(off) -> %d (eWarnOutOfVRAM: applied, DXGI budget exhausted)", (int)r); return true; }
        FgLog("sl: slDLSSGSetOptions(off) %d", (int)r);
        return false;
    }

    // ponytail: off is applied HERE (main thread) because DLSS-G must be eOff before the chain is released
    // (DLSS_G.md:745-748); on waits for the presenting thread (Prepare -> IssueOptions).
    void SetEnabled(bool on)
    {
        wantOn = on ? 1 : 0;
        if (!on) Off();
    }

    // Main thread, chain detached; the host already released its proxy reference (FgHost.cpp DestroyChain). The
    // proxy queue and factory go ONLY once DLSS-G is eOff: while it is on, it still presents on that queue. A refused
    // eOff retains everything and the host retries next pump; `force` releases regardless (last resort).
    bool Destroy(bool force)
    {
        if (!Off() && !force) return false;
        if (proxy && g_sl.state > 0) ReflexApply(sl::ReflexMode::eOff, 0);   // FG off: no Reflex sleep runs, no cap of ours stays behind
        proxy = NULL;
        if (queue) { queue->Release(); queue = NULL; }
        if (proxyFactory) { proxyFactory->Release(); proxyFactory = NULL; }
        FgLog("sl: destroyed (%s, generated %lld, lastStatus 0x%X, frames %u)%s", g_sl.version, generated, lastStatus, frames, force && isOn ? " (forced with DLSS-G still on)" : "");
        Zero();                                          // Streamline itself stays up for the process
        return true;
    }
};

ProviderStreamline g_dlssg;

} // namespace

IFgProvider* MakeFgProviderStreamline(void) { return &g_dlssg; }
