// FgXess.cpp - Intel XeSS Frame Generation (libxess_fg.dll 1.3.1, XeSS SDK 3.0.2) + XeLL as an IFgProvider, D3D12 only.
//
// XeSS-FG generates frames only INSIDE its own proxy IDXGISwapChain, which the library creates on an HWND. That is
// exactly the host's child HWND (FgWnd.cpp, FgHostChildHwnd): xefgSwapChainD3D12InitFromSwapChainDesc
// (xefg_swapchain_d3d12.h:216, guide :288-301 "creates new swap chain for the provided window", no other chain may
// own it, pApplicationSwapChain NULL) builds the proxy there and the proxy IS the shadow chain the host presents.
// The host copies Unity's finished back buffer into proxy.GetBuffer(current) and calls proxy->Present, inside which
// the SDK interpolates and paces (guide :148-153: on non-Intel GPUs a high-priority present thread of its own).
//
// XeLL is mandatory on every GPU (guide :218-231): xellD3D12CreateContext -> xellSetSleepMode(bLowLatencyMode) ->
// xefgSwapChainSetLatencyReduction BEFORE the proxy init, the same frame id for XeLL markers and the XeSS-FG present id
// (guide :240-258), all six markers per frame (xell.h:74-79 "required"), xellSleep first for a new id
// (xell guide :297). Teardown order: proxy refs dropped, xefgSwapChainDestroy, then xellDestroyContext (guide :1070-1094).
// PACING (docs\DESIGN.md "FG pacing"): xellSleep + SIMULATION_START run on the MAIN thread at the start of the frame
// ("at the beginning of each frame, before sampling user input", xell guide :294-298), SIMULATION_END after LateUpdate,
// RENDERSUBMIT_* / PRESENT_* on the render thread with the same id, which travels in FgFrame::paceId. The frame cap is
// xellSetSleepMode.minimumIntervalUs and nothing else (xell guide :93-100, :129).
// SLEEP MODE (docs\DESIGN.md "FG host threading and retirement"): "application must ensure any GPU activity is
// finished before xellSetSleepMode is called" (xell guide :131). EVERY xellSetSleepMode of this file - the first one
// (low latency on) and each cap change - is issued from Quiescent(), which the host calls on the presenting thread
// inside the Present hook only after it has proven Unity's queue retired there (FgHost.cpp ServeQuiescent). Until
// then the wish waits in `wantUs` and XeLL runs in pass-through, which the guide describes as the normal disabled
// state (:112-115). Frame generation itself is switched on right behind the first sleep mode, never before it
// ("XeLL must be enabled before enabling frame generation", xess_fg guide :237, :648).
// THREADS: XeLL is called from three - main (xellSleep, SIMULATION_*, reports), render (RENDERSUBMIT_START) and the
// presenting thread (RENDERSUBMIT_END, PRESENT_*, xellSetSleepMode). The XeLL guide and xell.h say nothing
// about thread safety (only that the logging callback "can be called from other threads", xell.h:114; the XeSS-FG
// guide's "thread-safe" list, :1096-1108, covers xefg* calls only), so every XeLL call except xellSleep goes through
// one lock (XellLock). xellSleep blocks for the pacing interval and is never called under it - the sleep on
// the simulation thread while other threads mark is the shape the guide itself describes. xellSleep and
// xellSetSleepMode exclude each other through SleepGate: a state word, not a lock held across the sleep.
//
// UI: XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS with UI composition enabled (guide :531-535, :602-605, :623): the
// SDK interpolates the hud-less colour and extracts the HUD from the composed back buffer it already holds. Inputs are
// the upscaler's shim-owned twins (D3D12Owned.h, all resting in COMMON): hud-less = owned `out` (same format/size as
// the back buffer, guide :571-572, :860), depth/mv at render res (guide :798 "same resourceSize"). Tagged
// XEFG_SWAPCHAIN_RV_ONLY_NOW with incomingState COMMON (the SDK barriers from/to it, guide :763-764) on the prep
// list: the SDK copies them there (Tag below).
//
// Loading: libxess_fg.lib / libxell.lib are linked but DELAY-LOADED (CMakeLists.txt); Create pins both DLLs from the
// mod folder with LoadLibraryW before the first call, exactly as Xess12.cpp does for libxess.dll.
#include "Fg.h"
#include "RenderforgeNative.h"
#include "D3D12Owned.h"

#include <string.h>
#include <stdio.h>

#include "xess_fg/xefg_swapchain.h"
#include "xess_fg/xefg_swapchain_d3d12.h"
#include "xell/xell.h"
#include "xell/xell_d3d12.h"

namespace {

void XefgLog(const char* msg, xefg_swapchain_logging_level_t lvl, void*) { FgLog("xefg[%d]: %s", (int)lvl, msg ? msg : ""); }
void XellLog(const char* msg, xell_logging_level_t lvl) { FgLog("xell[%d]: %s", (int)lvl, msg ? msg : ""); }

HMODULE LoadFrom(const wchar_t* dir, const wchar_t* name)
{
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\%s", dir ? dir : L".", name);
    return LoadLibraryW(p);
}

// Both runtimes pinned ONCE per process (the managed driver retries a refused Create 4x/s: a LoadLibrary per try
// would bump the loader refcounts forever). A partial load is rolled back so the next try starts clean.
HMODULE g_xell, g_xefg;
bool PinDlls(const wchar_t* dir)
{
    if (g_xell && g_xefg) return true;
    if (!g_xell) g_xell = LoadFrom(dir, L"libxell.dll");
    if (!g_xefg) g_xefg = LoadFrom(dir, L"libxess_fg.dll");
    if (g_xell && g_xefg) return true;
    if (g_xell) { FreeLibrary(g_xell); g_xell = NULL; }
    if (g_xefg) { FreeLibrary(g_xefg); g_xefg = NULL; }
    return false;
}

SRWLOCK g_xellLock = SRWLOCK_INIT;
struct XellLock
{
    XellLock()  { AcquireSRWLockExclusive(&g_xellLock); }
    ~XellLock() { ReleaseSRWLockExclusive(&g_xellLock); }
};
// Any thread: one XeLL marker, serialised with every other XeLL call of ours (file header THREADS).
void Mark(xell_context_handle_t ll, unsigned id, xell_latency_marker_type_t m)
{
    XellLock lk;
    xellAddMarkerData(ll, id, m);
}

// xellSleep (the sleeping thread: main, or the render thread under the diagnostic render pacing) and xellSetSleepMode
// (the presenting thread) never run inside XeLL together. g_sleepLock guards the STATE WORD only and is never held
// across either SDK call: the sleeper marks itself in and out, the mode setter waits (bounded) for "nobody sleeping",
// and a sleeper arriving while the mode is being set waits for that one call to return (microseconds).
SRWLOCK g_sleepLock = SRWLOCK_INIT;
CONDITION_VARIABLE g_sleepCv = CONDITION_VARIABLE_INIT;
int g_sleepState;           // 0 free, 1 a thread is inside xellSleep, 2 xellSetSleepMode is being issued
struct SleepGate
{
    static void EnterSleep()
    {
        AcquireSRWLockExclusive(&g_sleepLock);
        while (g_sleepState == 2) SleepConditionVariableSRW(&g_sleepCv, &g_sleepLock, INFINITE, 0);
        g_sleepState = 1;
        ReleaseSRWLockExclusive(&g_sleepLock);
    }
    // False = a thread was still inside xellSleep after `waitMs` (one pacing interval at most): nothing entered.
    static bool EnterMode(DWORD waitMs)
    {
        const ULONGLONG until = GetTickCount64() + waitMs;
        AcquireSRWLockExclusive(&g_sleepLock);
        while (g_sleepState != 0) {
            const ULONGLONG now = GetTickCount64();
            if (now >= until) { ReleaseSRWLockExclusive(&g_sleepLock); return false; }
            SleepConditionVariableSRW(&g_sleepCv, &g_sleepLock, (DWORD)(until - now), 0);
        }
        g_sleepState = 2;
        ReleaseSRWLockExclusive(&g_sleepLock);
        return true;
    }
    static void Leave()
    {
        AcquireSRWLockExclusive(&g_sleepLock);
        g_sleepState = 0;
        ReleaseSRWLockExclusive(&g_sleepLock);
        WakeAllConditionVariable(&g_sleepCv);
    }
};

unsigned g_maxInterp;       // maxSupportedInterpolations once measured on this GPU (process lifetime), 0 = not yet
unsigned CapsFor(unsigned maxInterp) { return FG_CAP_2X | (maxInterp >= 2 ? FG_CAP_3X : 0u) | (maxInterp >= 3 ? FG_CAP_4X : 0u); }

struct ProviderXess : IFgProvider
{
    xefg_swapchain_handle_t fg;
    xell_context_handle_t   ll;
    IDXGISwapChain4*        proxy;         // the host's reference; the SDK holds its own
    DXGI_FORMAT             backFmt;
    unsigned                outW, outH;
    unsigned                caps;
    unsigned                maxInterp;
    int                     enabled;
    int                     lastRc;
    // Our own frame counter, +1 per xellSleep: XeLL insists on consecutive ids (Sleep() "expected frame N-1" errors)
    // and the host's single frame slot can hand the render thread the SAME FgFrame.frameId twice or skip one when
    // Unity's main thread runs ahead. The thread that sleeps owns it (main: FrameBegin, render-thread pacing: Prepare).
    // The constants in `f` are the ones of the frame the tagged copies hold (the host pairs them per event, Fg.h FgFrame).
    unsigned                nextId;
    unsigned                presentId;     // render thread: the id of the frame being prepared / presented
    int                     mainPacing;    // FgMainPacing() at Create: 1 = FrameBegin sleeps (main thread), 0 = Prepare does
    int                     marked;        // RENDERSUBMIT_END/PRESENT_START issued for presentId (Generate ran)
    long long               generated;     // interpolated frames the proxy reported presenting
    int                     warned;
    unsigned                modeDeferred;  // sleep-mode changes put off because the sleeping thread did not leave xellSleep (log throttle)
    enum { kSleepWaitMs = 100 };           // the longest Quiescent waits for the sleeping thread to leave xellSleep
    unsigned                epoch;         // the host's chain generation this provider was created for (FgSetup::epoch)
    // XeLL sleep mode: low latency on + minimumIntervalUs for the PRESENTED rate (XeLL halves the rendered rate itself
    // with XeSS-FG, xell guide :210-214). Main thread writes `wantUs` (SetFrameLimit); the presenting thread issues
    // it from Quiescent() and records it in `appliedUs` (-1 = no xellSetSleepMode on this context yet).
    volatile LONG           wantUs;
    volatile LONG           appliedUs;
    int                     fgOn;          // xefgSwapChainSetEnabled(1) is in effect (pinned threads only)
    // Marker bookkeeping (OpenSkipped): the highest id with SIMULATION_END (main thread), the highest id whose
    // prepare event has run (a pinned thread, PrepareDone), the ids of frames nothing was fed in (main thread), the
    // highest id whose PRESENT_START is out, and the ids of skipped frames whose PRESENT_END waits for the Present
    // call their PRESENT_START precedes to return (endFrom..endTo, 0 = none).
    enum { kOpen = 16 };
    volatile LONG           simEnded, settledId;
    volatile LONG           unfedId[kOpen];   // main thread: the id of an unfed frame in that slot (no prepare event will come)
    unsigned                startedId[kOpen]; // render thread: the id that got RENDERSUBMIT_START in that slot
    unsigned                closedId;      // presenting thread (read by Prepare, serialised by the host's pin)
    unsigned                endFrom, endTo; // presenting thread
    unsigned                skippedClosed; // ids closed without a prepared frame, this chain
    char                    version[64];

    ProviderXess() { Zero(); }
    void Zero()
    {
        simEnded = 0; settledId = 0; closedId = 0; endFrom = endTo = 0; skippedClosed = 0; memset(startedId, 0, sizeof(startedId));
        for (int i = 0; i < kOpen; ++i) unfedId[i] = 0;
        fg = NULL; ll = NULL; proxy = NULL; backFmt = DXGI_FORMAT_UNKNOWN; outW = outH = 0; caps = 0; maxInterp = 0;
        enabled = 0; lastRc = 0; nextId = 0; presentId = 0; mainPacing = 1; marked = 1; generated = 0; warned = 0; version[0] = 0; wantUs = 0; appliedUs = -1; fgOn = 0; modeDeferred = 0;
        epoch = 0;
    }

    int Id() const { return FG_PROVIDER_XESS; }
    unsigned Caps() const { return caps; }
    const char* Name() const { return "xess"; }

    // Create failed: the host never took the proxy reference, so drop it here (Destroy needs refcount 0). Caps survive
    // so the host can grey the refused multiplier.
    int Fail(int rc) { unsigned c = caps; if (proxy) { proxy->Release(); proxy = NULL; } Destroy(true); caps = c; return rc; }

    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        // Pin BEFORE the first xell*/xefg* call: that is what the delay-load helper binds to.
        if (!PinDlls(s.dllDir)) {
            FgLog("xess: libxell.dll / libxess_fg.dll not loadable from the mod folder");
            return FG_ERR_NO_PROVIDER;
        }
        outW = s.desc.Width; outH = s.desc.Height; backFmt = s.desc.Format;
        mainPacing = FgMainPacing() ? 1 : 0;
        epoch = s.epoch;
        // The managed driver retries a refused Init 4x/s: once the GPU's maximum is known, refuse before any context.
        if (g_maxInterp) {
            caps = CapsFor(g_maxInterp);
            if (s.multiplier - 1 > g_maxInterp) return FG_ERR_UNSUPPORTED_MULTIPLIER;
        }
        xefg_swapchain_version_t fv = {};
        xell_version_t lv = {};
        xefgSwapChainGetVersion(&fv);
        xellGetVersion(&lv);
        _snprintf_s(version, sizeof(version), _TRUNCATE, "%u.%u.%u", fv.major, fv.minor, fv.patch);
        FgLog("xess: XeSS-FG %s, XeLL %u.%u.%u", version, lv.major, lv.minor, lv.patch);

        xell_result_t lr = xellD3D12CreateContext(s.device, &ll);
        if (lr != XELL_RESULT_SUCCESS || !ll) { FgLog("xess: xellD3D12CreateContext %d", (int)lr); ll = NULL; return Fail(FG_ERR_PROVIDER_FAILED); }
        xellSetLoggingCallback(ll, XELL_LOGGING_LEVEL_WARNING, &XellLog);

        xefg_swapchain_result_t r = xefgSwapChainD3D12CreateContext(s.device, &fg);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !fg) { FgLog("xess: xefgSwapChainD3D12CreateContext %d", (int)r); fg = NULL; return Fail(FG_ERR_PROVIDER_FAILED); }
        xefgSwapChainSetLoggingCallback(fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, &XefgLog, NULL);
        r = xefgSwapChainSetLatencyReduction(fg, ll);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { FgLog("xess: SetLatencyReduction %d", (int)r); return Fail(FG_ERR_PROVIDER_FAILED); }

        // Before init only maxSupportedInterpolations is reported (xefg_swapchain.h:339-340); 1 on non-Intel GPUs.
        xefg_swapchain_properties_t props = {};
        r = xefgSwapChainGetProperties(fg, &props);
        maxInterp = (r == XEFG_SWAPCHAIN_RESULT_SUCCESS && props.maxSupportedInterpolations) ? props.maxSupportedInterpolations : 1;
        g_maxInterp = maxInterp;
        caps = CapsFor(maxInterp);
        FgLog("xess: maxSupportedInterpolations=%u (rc %d) -> caps 0x%X", maxInterp, (int)r, caps);
        if (s.multiplier - 1 > maxInterp) { FgLog("xess: %ux asked, %ux supported", s.multiplier, maxInterp + 1); return Fail(FG_ERR_UNSUPPORTED_MULTIPLIER); }

        HWND child = FgHostChildHwnd(s);
        if (!child) { FgLog("xess: no child hwnd - XeSS-FG needs its own HWND swapchain"); return Fail(FG_ERR_NO_SWAPCHAIN); }

        xefg_swapchain_d3d12_init_params_t ip = {};
        ip.pApplicationSwapChain = NULL;                          // xefg_swapchain_d3d12.h:212 - the SDK creates the chain
        ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;   // reversed Z, as FgFrame.proj maps it (Fg.h)
        ip.maxInterpolatedFrames = s.multiplier - 1;
        ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;
        DXGI_SWAP_CHAIN_DESC1 d = s.desc;                         // FLIP_DISCARD, app format/size, >= 3 buffers, flags 0
        d.Scaling = DXGI_SCALING_STRETCH;
        d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        r = xefgSwapChainD3D12InitFromSwapChainDesc(fg, child, &d, NULL, s.queue, s.factory, &ip);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { FgLog("xess: InitFromSwapChainDesc %d on child %p", (int)r, (void*)child); return Fail(FG_ERR_PROVIDER_FAILED); }
        r = xefgSwapChainD3D12GetSwapChainPtr(fg, __uuidof(IDXGISwapChain4), (void**)&proxy);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !proxy) { FgLog("xess: GetSwapChainPtr %d", (int)r); proxy = NULL; return Fail(FG_ERR_NO_SWAPCHAIN); }
        s.factory->MakeWindowAssociation(child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        xefgSwapChainSetUiCompositionState(fg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);

        // No xellSetSleepMode here: Create runs on the main thread while Unity renders, and nothing this thread can
        // do proves the GPU idle (xell guide :131). appliedUs stays -1 = WantsQuiescent(): the first Present of the
        // chain turns low latency on at the host's quiescent point, and frame generation right behind it.

        *out = proxy;
        FgLog("xess: proxy swapchain %p on child hwnd %p, display %ux%u fmt %u, interpolated=%u, ui=backbuffer_hudless",
              (void*)proxy, (void*)child, outW, outH, (unsigned)backFmt, ip.maxInterpolatedFrames);
        return FG_OK;
    }

    // ONLY_NOW: XeSS-FG records its own copy on the prep list, right behind the host's flipped copies on that list
    // (guide "Resource Tagging": reusable "as soon as the tagging command list has been submitted").
    // UNTIL_NEXT_PRESENT promised the twins untouched until the Present with THIS id (guide: "can only be reused after
    // the Present/Present1 call with the matching frame ID") - the host cannot: a frame whose Present does not come
    // within kPinWaitMs is superseded and the next Prepare rewrites the same three textures, and a quality switch
    // releases them (FgTwins::Ensure).
    void Tag(ID3D12GraphicsCommandList* list, xefg_swapchain_resource_type_t type, ID3D12Resource* res, unsigned w, unsigned h, const char* what)
    {
        xefg_swapchain_d3d12_resource_data_t rd = {};
        rd.type = type;
        rd.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        rd.resourceSize.x = w; rd.resourceSize.y = h;
        rd.pResource = res;
        rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
        xefg_swapchain_result_t r = xefgSwapChainD3D12TagFrameResource(fg, list, presentId, &rd);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { lastRc = (int)r; if (warned < 8) { ++warned; FgLog("xess: tag %s %d", what, (int)r); } }
    }

    // The sleeping thread: a fresh id, xellSleep (first call for a new id), SIMULATION_START. The sleep mode is not
    // this thread's business any more (Quiescent); the gate only keeps the two calls from overlapping.
    unsigned Sleep()
    {
        unsigned id = ++nextId;
        SleepGate::EnterSleep();
        xellSleep(ll, id);                                 // never under a lock: it blocks for the pacing interval
        SleepGate::Leave();
        Mark(ll, id, XELL_SIMULATION_START);
        return id;
    }

    // The host's quiescent point (Fg.h): the presenting thread inside the Present hook, chain pinned, and the host
    // has JUST proven Unity's queue retired on this thread, which is the thread Unity submits from - "any GPU
    // activity is finished" (xell guide :131) holds here and nowhere else we can reach. Out of reach even here: what
    // the XeSS-FG proxy's own present thread still does with the previous frame (the SDK's work, not the
    // application's).
    // INVARIANT: xellSetSleepMode is issued only from here, i.e. only behind a successful drain, and never while a
    // thread is inside xellSleep. INVARIANT: xefgSwapChainSetEnabled(1) is issued only once a sleep mode with low
    // latency has been set on this context (appliedUs >= 0).
    bool WantsQuiescent() { return ll && fg && (wantUs != appliedUs || (enabled && !fgOn)); }
    bool Quiescent()
    {
        if (!ll || !fg) return true;
        const LONG us = InterlockedCompareExchange(&wantUs, 0, 0);
        if (us != appliedUs) {
            if (!SleepGate::EnterMode(kSleepWaitMs)) {
                if (modeDeferred++ % 64 == 0) FgLog("xess: XeLL sleep mode (cap %ld us) deferred - a thread stayed inside xellSleep for %d ms", us, (int)kSleepWaitMs);
                return false;
            }
            xell_sleep_params_t sp = {};
            sp.bLowLatencyMode = 1;
            sp.minimumIntervalUs = (uint32_t)us;
            xell_result_t lr;
            { XellLock lk; lr = xellSetSleepMode(ll, &sp); }
            SleepGate::Leave();
            // An SDK refusal is not a missing synchronisation: recorded as applied, so it is reported once, not every Present.
            InterlockedExchange(&appliedUs, us);
            FgLog("xess: XeLL sleep mode: low latency on, frame cap %ld us (presented %ld fps) -> %d", us, us ? 1000000 / us : 0, (int)lr);
        }
        if (enabled && !fgOn) {
            xefg_swapchain_result_t r = xefgSwapChainSetEnabled(fg, 1u);   // thread-safe (guide :1098-1107)
            fgOn = 1;
            FgLog("xess: frame generation on (behind the XeLL sleep mode) -> %d", (int)r);
        }
        return true;
    }

    // The presenting thread. "All marker types ... must be delivered each frame for XeLL to correctly calculate and
    // execute the xellSleep API call" (xell guide "Markers", xell.h:74-79 "required"). FrameBegin opens an id for every
    // paced frame, and some never reach Prepare: the frames of the grace period after the feed stops (FrameGen.cs
    // IdleAfter), a refused or failed prepare, a superseded one, a prepare event that never got the host's pin. Each of
    // those gets its four missing markers around a Present, in id order, so no id is left open: before a prepared
    // frame's own markers (every id below it is settled - the prepare events come in id order), and on a Present
    // without a prepared frame every id that can no longer be prepared - nothing was fed in its frame (FrameUnfed,
    // main thread) or its prepare event has run (PrepareDone; a Present without a prepared frame means that event left
    // it unprepared). An id whose event is still to come stops the walk. Prepare refuses an id opened this way
    // (cannot happen by the above; a backstop).
    // "XELL_PRESENT_START and XELL_PRESENT_END markers delimit IDXGISwapChain::Present" (xell guide :339), so the
    // walk is split around the real call: OpenSkipped issues RENDERSUBMIT_START (unless Prepare already did),
    // RENDERSUBMIT_END and PRESENT_START before it, EndSkipped the PRESENT_END after it returned - the proxy's
    // Present next to a prepared frame (BeforePresent / AfterPresent), Unity's own forwarded Present otherwise
    // (UnpreparedPresent / AfterUnpreparedPresent; the host keeps its pin across that call).
    // INVARIANT: every id in endFrom..endTo has PRESENT_START out and gets PRESENT_END after exactly one Present call.
    // True = at least one id was opened: EndSkipped must follow that Present.
    bool OpenSkipped(unsigned upTo, bool onlySettled)
    {
        EndSkipped();                                      // backstop: a Present that never reported back
        const unsigned settled = (unsigned)InterlockedCompareExchange(&settledId, 0, 0);
        const unsigned first = closedId + 1;
        while (closedId < upTo) {
            const unsigned id = closedId + 1;
            if (onlySettled && id > settled && (unsigned)unfedId[id % kOpen] != id && upTo - id < kOpen) break;
            if (startedId[id % kOpen] != id) Mark(ll, id, XELL_RENDERSUBMIT_START);   // a superseded frame already has it (Prepare)
            Mark(ll, id, XELL_RENDERSUBMIT_END);
            Mark(ll, id, XELL_PRESENT_START);
            closedId = id;
            ++skippedClosed;
        }
        if (closedId < first) return false;
        endFrom = first; endTo = closedId;
        return true;
    }
    void EndSkipped()
    {
        for (unsigned id = endFrom; endFrom && id <= endTo; ++id) Mark(ll, id, XELL_PRESENT_END);
        endFrom = endTo = 0;
    }

    bool UnpreparedPresent()
    {
        if (!ll || !fg || !mainPacing) return false;
        return OpenSkipped((unsigned)InterlockedCompareExchange(&simEnded, 0, 0), true);
    }
    void AfterUnpreparedPresent() { if (ll && fg) EndSkipped(); }
    void FrameUnfed(unsigned id) { if (id) InterlockedExchange(&unfedId[id % kOpen], (LONG)id); }
    void PrepareDone(unsigned id) { if ((LONG)id > settledId) InterlockedExchange(&settledId, (LONG)id); }

    // Main thread, start of the frame / after LateUpdate. An unpaced frame (nothing fed to the chain) gets no XeLL
    // frame: XeLL ids must be consecutive AND each one must see all its markers through a Present (xell.h:74-79).
    unsigned FrameBegin(bool pace) { return ll && fg && mainPacing && pace ? Sleep() : 0; }
    void SimEnd(unsigned id) { if (ll && fg && mainPacing && id) { Mark(ll, id, XELL_SIMULATION_END); InterlockedExchange(&simEnded, (LONG)id); } }

    // Main thread. XeLL's last 64 frame reports (xell.h xell_frame_report_t, nanoseconds).
    void Stats(char* buf, int cap)
    {
        if (cap <= 0) return;
        buf[0] = 0;
        if (!ll) return;
        static xell_frame_report_t rep[64];
        memset(rep, 0, sizeof(rep));
        xell_result_t r;
        { XellLock lk; r = xellGetFramesReports(ll, rep); }
        if (r != XELL_RESULT_SUCCESS) { _snprintf_s(buf, cap, _TRUNCATE, "xell: xellGetFramesReports %d", (int)r); return; }
        unsigned n = 0;
        double sim = 0, toPresent = 0, submit = 0;
        for (int i = 0; i < 64; ++i) {
            const xell_frame_report_t& e = rep[i];
            if (!e.m_sim_start_ts || e.m_present_end_ts <= e.m_sim_start_ts) continue;
            ++n;
            sim += (double)(e.m_sim_end_ts - e.m_sim_start_ts);
            toPresent += (double)(e.m_present_end_ts - e.m_sim_start_ts);
            submit += (double)(e.m_render_submit_end_ts - e.m_render_submit_start_ts);
        }
        double k = n ? 1.0 / (1000000.0 * n) : 0.0;
        _snprintf_s(buf, cap, _TRUNCATE, "xell: frames=%u simMs=%.2f simToPresentEndMs=%.2f renderSubmitMs=%.2f minimumIntervalUs=%ld pacing=%s",
                    n, sim * k, toPresent * k, submit * k, appliedUs, mainPacing ? "main" : "render");
    }

    // Render thread, DLSS_EV_FG_PREPARE: the pre-present markers, then tag this frame's inputs.
    bool Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f)
    {
        const OwnedSet12* o = FgOwned12();
        ID3D12Resource* depth = FgDepth12();
        ID3D12Resource* mv = FgMv12();
        if (!fg || !o || !depth || !mv) return false;
        if (f.epoch != epoch) return false;                // a block of another chain generation (the host drops these first)
        // Frame generation is not on yet: the chain's first Present switches it on behind the XeLL sleep mode
        // (Quiescent). Nothing is tagged to a disabled chain; the frame goes out the idle way, its id is closed there.
        if (!fgOn) return false;
        // Main-thread pacing: this frame's id was opened by FrameBegin. None = the frame was fed before the pacer started
        // (the first one after an idle stretch): left unprepared, the host presents it the idle way. The same id twice =
        // a second prepare event inside one frame.
        if (mainPacing && (!f.paceId || f.paceId == presentId || f.paceId <= closedId)) return false;
        // FgHudless12: the host's flipped copy in the back buffer's format (FgTwins.h). None = tag depth/mv/constants
        // anyway (XeSS-FG interpolates from the back buffer; hudless is optional).
        DXGI_FORMAT hudFmt = DXGI_FORMAT_UNKNOWN;
        ID3D12Resource* hud = FgHudless12(&hudFmt);
        if (!hud || hudFmt != backFmt || o->outW != outW || o->outH != outH) {
            if (warned < 8) { ++warned; FgLog("xess: hudless skipped: out %ux%u fmt %u vs backbuffer %ux%u fmt %u", o->outW, o->outH, (unsigned)hudFmt, outW, outH, (unsigned)backFmt); }
            hud = NULL;
        }
        FgNoteHudless(hud != NULL);
        if (mainPacing) presentId = f.paceId;
        else {
            // Render-thread pacing (diagnostic): the simulation is not reachable from here, SIMULATION_* bracket this event.
            presentId = Sleep();
            Mark(ll, presentId, XELL_SIMULATION_END);
        }
        marked = 0;
        startedId[presentId % kOpen] = presentId;
        Mark(ll, presentId, XELL_RENDERSUBMIT_START);

        Tag(list, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, mv, o->w, o->h, "mv");
        Tag(list, XEFG_SWAPCHAIN_RES_DEPTH, depth, o->w, o->h, "depth");
        if (hud) Tag(list, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, hud, o->outW, o->outH, "hudless");

        xefg_swapchain_frame_constant_data_t c = {};
        // Row-major, non-jittered (guide :879-880), and ROW-VECTOR like the guide's own example, which memcpy's an
        // XMFLOAT4X4 of XMMatrixTranslation (:903-909) - the layout the SR guide shears for jitter ([2][0], [2][1],
        // xess_sr guide :274-275). FgFrame holds exactly that.
        memcpy(c.viewMatrix, f.view, sizeof(c.viewMatrix));
        memcpy(c.projectionMatrix, f.proj, sizeof(c.projectionMatrix));
        // The NGX offsets unchanged, as XeSS-SR takes them (Xess12.cpp, measured), in top-down rows here (Fg.h).
        c.jitterOffsetX = f.jitterX; c.jitterOffsetY = f.jitterY;
        c.motionVectorScaleX = f.mvScaleX; c.motionVectorScaleY = f.mvScaleY;
        c.resetHistory = f.reset ? 1u : 0u;
        c.frameRenderTime = f.dtMs;
        if (FgCaptureArmed()) {
            FgCaptureFloats("xess.viewMatrix", c.viewMatrix, 16);
            FgCaptureFloats("xess.projectionMatrix", c.projectionMatrix, 16);
            FgCaptureNote("xess.jitter=%.9g %.9g mvScale=%.9g %.9g reset=%u dtMs=%.9g presentId=%u enabled=%d", c.jitterOffsetX, c.jitterOffsetY,
                          c.motionVectorScaleX, c.motionVectorScaleY, c.resetHistory, c.frameRenderTime, presentId, enabled);
        }
        xefg_swapchain_result_t r = xefgSwapChainTagFrameConstants(fg, presentId, &c);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { lastRc = (int)r; if (warned < 8) { ++warned; FgLog("xess: tag constants %d", (int)r); } }
        // Still prepared: presentId advanced and XeLL's SIMULATION/RENDERSUBMIT_START markers are out, so this frame's
        // Present must follow to close them (a constants failure was warn-only before too).
        return true;
    }

    // Presenting thread, just before the host copies the real frame into the proxy and presents it: the present id
    // identifies this frame's tags (guide :1020-1022). The proxy generates inside its Present, so 0 here.
    int Generate(const FgFrame&, ID3D12Resource*, IDXGISwapChain4*, UINT, UINT)
    {
        if (fg) xefgSwapChainSetPresentId(fg, presentId);
        return 0;
    }

    // The host's back-buffer copy is submitted, Present is next: the submit closes and the present opens HERE (after
    // the last write into the proxy's back buffer), the same place DLSS-G's markers live.
    void BeforePresent()
    {
        if (!fg || marked) return;
        marked = 1;
        if (mainPacing && presentId) OpenSkipped(presentId - 1, false);   // their PRESENT_END: AfterPresent, in id order
        Mark(ll, presentId, XELL_RENDERSUBMIT_END);
        Mark(ll, presentId, XELL_PRESENT_START);
    }

    void AfterPresent(HRESULT)
    {
        if (!fg) return;
        EndSkipped();
        if (marked == 1) { marked = 2; Mark(ll, presentId, XELL_PRESENT_END); if (mainPacing && presentId > closedId) closedId = presentId; }
        xefg_swapchain_present_status_t st = {};
        if (xefgSwapChainGetLastPresentStatus(fg, &st) != XEFG_SWAPCHAIN_RESULT_SUCCESS) return;
        lastRc = (int)st.frameGenResult;
        if (st.framesPresented > 1) { generated += st.framesPresented - 1; FgPresentedAdd((int)st.framesPresented - 1); }  // the host counts the real one
        if (st.frameGenResult != XEFG_SWAPCHAIN_RESULT_SUCCESS && warned < 8) { ++warned; FgLog("xess: frameGenResult %d (presented %u, enabled %u)", (int)st.frameGenResult, st.framesPresented, st.isFrameGenEnabled); }
    }

    bool SetFrameLimit(unsigned presentedFps)
    {
        InterlockedExchange(&wantUs, presentedFps ? (LONG)(1000000u / presentedFps) : 0);
        return true;
    }

    // Main thread with the chain pinned (FgHost.cpp ApplyEnabled) or detached and unpinned (DestroyChain). Off goes out
    // at once. On goes out here only when XeLL low latency is already set on this context; on a fresh chain it is
    // Quiescent() that issues it, right behind the first xellSetSleepMode ("When enabling frame generation, the
    // application must ensure that XeLL latency reduction is enabled", xess_fg guide :648) - the proxy presents the
    // application's frames unchanged until then (:652 "passthrough mode").
    void SetEnabled(bool on)
    {
        enabled = on ? 1 : 0;
        if (!fg) return;
        if (!on) { xefgSwapChainSetEnabled(fg, 0u); fgOn = 0; }  // thread-safe (guide :1098-1107)
        else if (appliedUs >= 0 && !fgOn) { xefgSwapChainSetEnabled(fg, 1u); fgOn = 1; }
    }

    // Main thread, chain detached. The host has already released its proxy reference (FgHost.cpp DestroyChain):
    // xefgSwapChainDestroy refuses while any reference to the proxy is outstanding (guide :1076-1077) - then BOTH
    // handles are retained (XeLL after XeSS-FG, guide :1080) and the host retries on its next pump.
    bool Destroy(bool force)
    {
        if (fg) {
            xefg_swapchain_result_t r = xefgSwapChainDestroy(fg);
            if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
                FgLog("xess: xefgSwapChainDestroy %d%s", (int)r, force ? " - forced, handles leaked" : " - retained");
                if (!force) return false;
            }
            fg = NULL;
        }
        if (ll && !force) { xellDestroyContext(ll); }
        ll = NULL;
        FgLog("xess: destroyed (XeSS-FG %s, generated %lld, lastRc %d, XeLL ids %u, closed without a prepared frame %u)", version, generated, lastRc, nextId, skippedClosed);
        Zero();                                                    // the Intel modules stay resident (delay-load bound)
        return true;
    }
};

ProviderXess g_xess;

} // namespace

IFgProvider* MakeFgProviderXess(void) { return &g_xess; }
