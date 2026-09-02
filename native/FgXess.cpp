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
//
// UI: XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS with UI composition enabled (guide :531-535, :602-605, :623): the
// SDK interpolates the hud-less colour and extracts the HUD from the composed back buffer it already holds. Inputs are
// the upscaler's shim-owned twins (D3D12Owned.h, all resting in COMMON): hud-less = owned `out` (same format/size as
// the back buffer, guide :571-572, :860), depth/mv at render res (guide :798 "same resourceSize"). Tagged
// XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT with incomingState COMMON (the SDK barriers from/to it, guide :763-764); the
// twins are only rewritten by the next frame's upscaler list, which the render thread submits after this Present.
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
    // Our own frame counter, +1 per Prepare: XeLL insists on consecutive ids (Sleep() "expected frame N-1" errors)
    // and the host's single frame slot can hand the render thread the SAME FgFrame.frameId twice or skip one when
    // Unity's main thread runs ahead. The owned twins are always the latest upscaled frame, so every Prepare tags a
    // fresh id; only the constants in `f` can be one frame stale (host ceiling, shared with FSR).
    unsigned                presentId;
    int                     marked;        // RENDERSUBMIT_END/PRESENT_START issued for presentId (Generate ran)
    long long               generated;     // interpolated frames the proxy reported presenting
    int                     warned;
    char                    version[64];

    ProviderXess() { Zero(); }
    void Zero()
    {
        fg = NULL; ll = NULL; proxy = NULL; backFmt = DXGI_FORMAT_UNKNOWN; outW = outH = 0; caps = 0; maxInterp = 0;
        enabled = 0; lastRc = 0; presentId = 0; marked = 1; generated = 0; warned = 0; version[0] = 0;
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
        ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;   // Unity D3D12 = reversed-Z (DlssDriver.cs:253)
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

        // XeLL on, no XeLL fps cap (the mod's own limiter stays). Render is idle here (Init runs on the main thread).
        xell_sleep_params_t sp = {};
        sp.bLowLatencyMode = 1;
        lr = xellSetSleepMode(ll, &sp);
        if (lr != XELL_RESULT_SUCCESS) FgLog("xess: xellSetSleepMode %d", (int)lr);

        *out = proxy;
        FgLog("xess: proxy swapchain %p on child hwnd %p, display %ux%u fmt %u, interpolated=%u, ui=backbuffer_hudless",
              (void*)proxy, (void*)child, outW, outH, (unsigned)backFmt, ip.maxInterpolatedFrames);
        return FG_OK;
    }

    void Tag(xefg_swapchain_resource_type_t type, ID3D12Resource* res, unsigned w, unsigned h, const char* what)
    {
        xefg_swapchain_d3d12_resource_data_t rd = {};
        rd.type = type;
        rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        rd.resourceSize.x = w; rd.resourceSize.y = h;
        rd.pResource = res;
        rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
        xefg_swapchain_result_t r = xefgSwapChainD3D12TagFrameResource(fg, NULL, presentId, &rd);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { lastRc = (int)r; if (warned < 8) { ++warned; FgLog("xess: tag %s %d", what, (int)r); } }
    }

    // Render thread, DLSS_EV_FG_PREPARE: XeLL sleep + the pre-present markers, then tag this frame's inputs.
    void Prepare(ID3D12GraphicsCommandList*, const FgFrame& f)
    {
        const OwnedSet12* o = FgOwned12();
        if (!fg || !o || !o->depth || !o->mv || !o->out) return;
        if (o->outFmt != backFmt || o->outW != outW || o->outH != outH) {
            if (warned < 8) { ++warned; FgLog("xess: hudless skipped: out %ux%u fmt %u vs backbuffer %ux%u fmt %u", o->outW, o->outH, (unsigned)o->outFmt, outW, outH, (unsigned)backFmt); }
            return;
        }
        ++presentId;
        marked = 0;
        // Unity's simulation boundaries are not reachable from the shim: SIMULATION_* bracket this event.
        xellSleep(ll, presentId);
        xellAddMarkerData(ll, presentId, XELL_SIMULATION_START);
        xellAddMarkerData(ll, presentId, XELL_SIMULATION_END);
        xellAddMarkerData(ll, presentId, XELL_RENDERSUBMIT_START);

        Tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, o->mv, o->w, o->h, "mv");
        Tag(XEFG_SWAPCHAIN_RES_DEPTH, o->depth, o->w, o->h, "depth");
        Tag(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, o->out, o->outW, o->outH, "hudless");

        xefg_swapchain_frame_constant_data_t c = {};
        memcpy(c.viewMatrix, f.view, sizeof(c.viewMatrix));         // row-major, non-jittered (guide :879-880)
        memcpy(c.projectionMatrix, f.proj, sizeof(c.projectionMatrix));
        c.jitterOffsetX = -f.jitterX; c.jitterOffsetY = f.jitterY;  // same signs as XeSS-SR (Xess12.cpp kJitterSign)
        c.motionVectorScaleX = f.mvScaleX; c.motionVectorScaleY = f.mvScaleY;
        c.resetHistory = f.reset ? 1u : 0u;
        c.frameRenderTime = f.dtMs;
        xefg_swapchain_result_t r = xefgSwapChainTagFrameConstants(fg, presentId, &c);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) { lastRc = (int)r; if (warned < 8) { ++warned; FgLog("xess: tag constants %d", (int)r); } }
    }

    // Render thread, just before the host copies the real frame into the proxy and presents it: the present id
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
        xellAddMarkerData(ll, presentId, XELL_RENDERSUBMIT_END);
        xellAddMarkerData(ll, presentId, XELL_PRESENT_START);
    }

    void AfterPresent(HRESULT)
    {
        if (!fg) return;
        if (marked == 1) { marked = 2; xellAddMarkerData(ll, presentId, XELL_PRESENT_END); }
        xefg_swapchain_present_status_t st = {};
        if (xefgSwapChainGetLastPresentStatus(fg, &st) != XEFG_SWAPCHAIN_RESULT_SUCCESS) return;
        lastRc = (int)st.frameGenResult;
        if (st.framesPresented > 1) { generated += st.framesPresented - 1; FgPresentedAdd((int)st.framesPresented - 1); }  // the host counts the real one
        if (st.frameGenResult != XEFG_SWAPCHAIN_RESULT_SUCCESS && warned < 8) { ++warned; FgLog("xess: frameGenResult %d (presented %u, enabled %u)", (int)st.frameGenResult, st.framesPresented, st.isFrameGenEnabled); }
    }

    void SetEnabled(bool on)
    {
        enabled = on ? 1 : 0;
        if (fg) xefgSwapChainSetEnabled(fg, on ? 1u : 0u);       // thread-safe (guide :1098-1107)
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
        FgLog("xess: destroyed (XeSS-FG %s, generated %lld, lastRc %d)", version, generated, lastRc);
        Zero();                                                    // the Intel modules stay resident (delay-load bound)
        return true;
    }
};

ProviderXess g_xess;

} // namespace

IFgProvider* MakeFgProviderXess(void) { return &g_xess; }
