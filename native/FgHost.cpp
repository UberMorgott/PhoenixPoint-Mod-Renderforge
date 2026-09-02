// FgHost.cpp - the presentation host. Owns the FG-owned "shadow" swapchain, copies Unity's finished
// backbuffer into it every frame, drives one IFgProvider and presents. Unity's own swapchain is STILL
// presented every frame (sync 0, hidden under the DComp visual) so its back-buffer index keeps rotating in
// step with Unity's own tracking - see FgHostOnPresent.
//
// The shadow chain is a COMPOSITION swapchain shown through a DirectComposition target on the game's HWND
// (decision 4a). A second CreateSwapChainForHwnd on a window that already owns a flip-model chain fails
// with E_ACCESSDENIED (measured on Instance3: 0x80070005 on every retry); the Task 1 spike's
// `secondSwapChain=0x00000000` was the zero-initialised FgSpike field - the probe only ran inside
// DLSS_EV_FG_PREPARE, which the driver issues only while FrameGen is already live.
//
// Threading: Init/Shutdown/SetEnabled on the main thread, SetFrame on the main thread into a 4-deep ring,
// Prepare/OnPresent/OnResize on the render thread. One SRWLOCK serialises everything that touches
// H.prov/H.shadow; the render thread may tear the chain down itself (shadow Present failed).
//
// Two D3D12Ring instances (D3D12Ring.h, the ring every D3D12 backend uses): `prep` submits through
// IUnityGraphicsD3D12v5::ExecuteCommandList with the resource states Unity must know about (its state
// arrays outlive the call, which a stack array would not); `copy` submits straight on Unity's queue inside
// the Present hook, where the frame is already complete and Unity's fence does not apply.
#include "Fg.h"
#include "RenderforgeNative.h"
#include "D3D12Ring.h"

#include <dcomp.h>
#include <stdio.h>

namespace {

struct Host
{
    IFgProvider*          prov;
    IDXGISwapChain4*      shadow;
    IDXGIFactory2*        factory;
    IDCompositionDevice*  dcomp;
    IDCompositionTarget*  target;
    IDCompositionVisual*  visual;
    ID3D12Device*         device;
    ID3D12CommandQueue*   queue;
    D3D12Ring             prep;         // DLSS_EV_FG_PREPARE, via Unity
    D3D12Ring             copy;         // backbuffer copy, direct
    unsigned              multiplier;
    unsigned              scFlags;      // DXGI_SWAP_CHAIN_FLAG_* the shadow chain was created with
    int                   enabled;
    int                   lastError;
    long                  lastPresentHr;
    unsigned              outW, outH;
    // per-frame ring, main thread writes, render thread reads
    FgFrame               frames[4];
    volatile long         frameIdx;
    FgFrame               cur;          // render-thread copy
    char                  status[512];
};

Host H;
SRWLOCK g_lock = SRWLOCK_INIT;

struct Locked
{
    Locked()  { AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
};

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

// Barrier src PRESENT->COPY_SOURCE and dst PRESENT->COPY_DEST, CopyResource, barrier back, execute on the queue.
bool CopyBackBuffer(ID3D12Resource* src, ID3D12Resource* dst)
{
    ID3D12GraphicsCommandList* l = H.copy.Begin();
    if (!l) return false;

    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = src;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource = dst;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    l->ResourceBarrier(2, b);

    l->CopyResource(dst, src);

    for (int i = 0; i < 2; ++i) {
        D3D12_RESOURCE_STATES t = b[i].Transition.StateAfter;
        b[i].Transition.StateAfter = b[i].Transition.StateBefore;
        b[i].Transition.StateBefore = t;
    }
    l->ResourceBarrier(2, b);

    return H.copy.EndDirect(H.queue);
}

void ReleaseGpu()
{
    H.prep.Release();
    H.copy.Release();
    if (H.visual) H.visual->SetContent(NULL);
    if (H.dcomp)  H.dcomp->Commit();
    SafeRelease(H.visual);
    SafeRelease(H.target);
    SafeRelease(H.dcomp);
    SafeRelease(H.shadow);
    SafeRelease(H.factory);
    H.scFlags = 0;
}

// Lock held. Provider first (it may still hold the chain), then the GPU objects.
void TearDownLocked(const char* why)
{
    if (!H.prov) return;
    FgLog("host: teardown (%s)", why);
    IFgProvider* p = H.prov;
    H.enabled = 0;
    p->SetEnabled(false);
    p->Destroy();
    H.prov = NULL;
    ReleaseGpu();
}

// ---------------------------------------------------------------- the pass-through provider

struct ProviderNone : IFgProvider
{
    IDXGISwapChain4* sc;
    ProviderNone() : sc(NULL) {}
    int Id() const { return FG_PROVIDER_NONE; }
    unsigned Caps() const { return FG_CAP_2X; }        // reported, never used: NONE generates nothing
    const char* Name() const { return "none"; }
    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        int rc = FgHostCreateShadowSwapChain(s, out);
        if (rc == FG_OK) sc = *out;
        return rc;
    }
    void Prepare(ID3D12GraphicsCommandList*, const FgFrame&) {}
    int  Generate(const FgFrame&, ID3D12Resource*, IDXGISwapChain4*, UINT, UINT) { return 0; }
    void SetEnabled(bool) {}
    void Destroy(void) { sc = NULL; }
};

ProviderNone g_none;

} // namespace

IFgProvider* MakeFgProviderNone(void) { return &g_none; }
// XeSS / Streamline providers land in Tasks 4-5; until then those ids fall back to the pass-through chain.
IFgProvider* MakeFgProviderXess(void) { return NULL; }
IFgProvider* MakeFgProviderStreamline(void) { return NULL; }

// Composition swapchain + DirectComposition target on s.hwnd (topmost, above Unity's own chain).
// Tearing: asked for when the factory reports DXGI_FEATURE_PRESENT_ALLOW_TEARING, dropped if the
// composition chain refuses the flag; the flags actually granted are kept in H.scFlags for Present.
int FgHostCreateShadowSwapChain(const FgSetup& s, IDXGISwapChain4** out)
{
    *out = NULL;
    DXGI_SWAP_CHAIN_DESC1 d = s.desc;
    d.Scaling = DXGI_SCALING_STRETCH;                  // the only value CreateSwapChainForComposition accepts
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;              // opaque game output

    BOOL tearing = FALSE;
    IDXGIFactory5* f5 = NULL;
    if (SUCCEEDED(s.factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5))) {
        if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) tearing = FALSE;
        f5->Release();
    }
    d.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = s.factory->CreateSwapChainForComposition(s.queue, &d, NULL, &sc1);
    if (FAILED(hr) && d.Flags) {
        FgLog("host: CreateSwapChainForComposition with ALLOW_TEARING 0x%08X - retrying without", (unsigned)hr);
        d.Flags = 0;
        hr = s.factory->CreateSwapChainForComposition(s.queue, &d, NULL, &sc1);
    }
    if (FAILED(hr) || !sc1) { FgLog("host: CreateSwapChainForComposition 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }
    IDXGISwapChain4* sc4 = NULL;
    hr = sc1->QueryInterface(__uuidof(IDXGISwapChain4), (void**)&sc4);
    sc1->Release();
    if (FAILED(hr) || !sc4) { FgLog("host: shadow chain is not IDXGISwapChain4 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }

    hr = DCompositionCreateDevice2(NULL, __uuidof(IDCompositionDevice), (void**)&H.dcomp);
    if (SUCCEEDED(hr)) hr = H.dcomp->CreateTargetForHwnd(s.hwnd, TRUE, &H.target);
    if (SUCCEEDED(hr)) hr = H.dcomp->CreateVisual(&H.visual);
    if (SUCCEEDED(hr)) hr = H.visual->SetContent(sc4);
    if (SUCCEEDED(hr)) hr = H.target->SetRoot(H.visual);
    if (SUCCEEDED(hr)) hr = H.dcomp->Commit();
    if (FAILED(hr)) {
        FgLog("host: DirectComposition 0x%08X (device %p target %p visual %p)", (unsigned)hr, (void*)H.dcomp, (void*)H.target, (void*)H.visual);
        SafeRelease(H.visual); SafeRelease(H.target); SafeRelease(H.dcomp);
        sc4->Release();
        return FG_ERR_NO_SWAPCHAIN;
    }
    H.scFlags = d.Flags;
    FgLog("host: composition chain %p on hwnd %p, flags 0x%X (tearing supported %d)", (void*)sc4, (void*)s.hwnd, d.Flags, (int)tearing);
    *out = sc4;
    return FG_OK;
}

// ---------------------------------------------------------------- host

int FgHostInit(int provider, unsigned multiplier, const wchar_t* dllDir)
{
    Locked lk;
    if (H.prov) return FG_OK;
    if (!g_unityD3D12) return FG_ERR_NOT_D3D12;

    IDXGISwapChain3* app = FgAppSwapChain();
    HWND hwnd = FgAppHwnd();
    if (!app || !hwnd) return FG_ERR_NO_SWAPCHAIN;    // hook has not seen a Present yet: caller retries

    IFgProvider* p = NULL;
    switch (provider) {
    case FG_PROVIDER_FSR:  p = MakeFgProviderFsr();        break;
    case FG_PROVIDER_XESS: p = MakeFgProviderXess();       break;
    case FG_PROVIDER_DLSS: p = MakeFgProviderStreamline(); break;
    default:               p = MakeFgProviderNone();       break;
    }
    if (!p) { FgLog("host: provider %d not built yet - pass-through chain", provider); p = MakeFgProviderNone(); }

    H.device = g_unityD3D12->GetDevice();
    H.queue  = g_unityD3D12->GetCommandQueue();
    if (!H.device || !H.queue) return FG_ERR_NOT_D3D12;

    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&H.factory);
    if (FAILED(hr)) { FgLog("host: CreateDXGIFactory2 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }

    DXGI_SWAP_CHAIN_DESC ad = {};
    app->GetDesc(&ad);
    FgSetup s = {};
    s.device = H.device; s.queue = H.queue; s.factory = H.factory; s.hwnd = hwnd;
    s.multiplier = multiplier < 2 ? 2 : (multiplier > 4 ? 4 : multiplier);
    s.dllDir = dllDir;
    const DXGI_SWAP_CHAIN_DESC1* a1 = FgAppDesc();
    if (a1) s.desc = *a1;
    else {
        s.desc.Width = ad.BufferDesc.Width; s.desc.Height = ad.BufferDesc.Height;
        s.desc.Format = ad.BufferDesc.Format;
        s.desc.BufferCount = ad.BufferCount;
    }
    s.desc.SampleDesc.Count = 1; s.desc.SampleDesc.Quality = 0;
    s.desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    s.desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;    // every FG SDK on disk requires the flip model
    s.desc.Scaling = DXGI_SCALING_STRETCH;
    s.desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    s.desc.Flags = 0;                                      // no waitable object: we drive the pacing; tearing decided in Create
    if (s.desc.BufferCount < 3) s.desc.BufferCount = 3;
    H.outW = s.desc.Width; H.outH = s.desc.Height;
    H.multiplier = s.multiplier;

    IDXGISwapChain4* shadow = NULL;
    int rc = p->Create(s, &shadow);
    if (rc != FG_OK || !shadow) { ReleaseGpu(); H.lastError = rc; return rc; }
    H.shadow = shadow;
    H.prov = p;
    H.prep.Attach(H.device);
    H.copy.Attach(H.device);
    H.lastError = FG_OK;
    H.lastPresentHr = 0;
    FgLog("host: provider=%s multiplier=%u shadow=%p %ux%u flags=0x%X caps=0x%X",
          p->Name(), H.multiplier, (void*)H.shadow, H.outW, H.outH, H.scFlags, p->Caps());
    return FG_OK;
}

void FgHostSetEnabled(int on)
{
    Locked lk;
    H.enabled = on ? 1 : 0;
    if (H.prov) H.prov->SetEnabled(on != 0);
    FgLog("host: enabled=%d", H.enabled);
}

void FgHostSetFrame(const FgFrame& f)
{
    long i = (H.frameIdx + 1) & 3;
    H.frames[i] = f;
    H.frameIdx = i;
}

void FgHostPrepare(void)
{
    FgHookSpike();                       // Task 1 probes; no-op after the first call
    Locked lk;
    if (!H.prov || !H.enabled) return;
    H.cur = H.frames[H.frameIdx & 3];
    // The pass-through provider has nothing to prepare. Nothing is declared to Unity: providers read the
    // shim-owned twins (COMMON at rest), never the Unity RTs - declaring those as NON_PIXEL_SHADER_RESOURCE made
    // Unity transition them under the upscaler's own barriers (debug layer id=527 on every frame).
    if (H.prov->Id() == FG_PROVIDER_NONE) return;
    if (!H.cur.hudless || !H.cur.depth || !H.cur.mv) return;

    ID3D12GraphicsCommandList* l = H.prep.Begin();
    if (!l) return;
    H.prov->Prepare(l, H.cur);
    H.prep.End(0);
}

bool FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags, HRESULT* outHr)
{
    Locked lk;
    if (!H.prov || !H.enabled || !H.shadow) return false;
    IDXGISwapChain3* app3 = FgAppSwapChain();
    if (app != (IDXGISwapChain*)app3) return false;

    // Only the present flags the shadow chain opted into: tearing iff created with ALLOW_TEARING (and
    // Unity asked for it this frame); never TEST / DO_NOT_SEQUENCE / RESTART, which belong to Unity's chain.
    UINT pf = (H.scFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? (flags & DXGI_PRESENT_ALLOW_TEARING) : 0;
    if (pf && syncInterval) pf = 0;                    // tearing needs sync interval 0

    ID3D12Resource* src = NULL;
    if (FAILED(app3->GetBuffer(app3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&src)) || !src) return false;

    // Generated frame(s) first - they sit between the previous real frame and this one - then the real one.
    // The provider presents its own frames on the shadow chain, so the current back buffer is fetched after.
    int generated = H.prov->Generate(H.cur, src, H.shadow, syncInterval, pf);

    ID3D12Resource* dst = NULL;
    if (FAILED(H.shadow->GetBuffer(H.shadow->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&dst)) || !dst) {
        src->Release();
        return false;
    }
    static int firstLogged = 0;
    if (!firstLogged) {
        firstLogged = 1;
        FgLog("host: first present: appIdx=%u shadowIdx=%u src=%p dst=%p sync=%u flags=0x%X generated=%d tid=%u",
              app3->GetCurrentBackBufferIndex(), H.shadow->GetCurrentBackBufferIndex(), (void*)src, (void*)dst,
              syncInterval, flags, generated, (unsigned)GetCurrentThreadId());
    }
    bool copied = CopyBackBuffer(src, dst);
    src->Release(); dst->Release();
    if (!copied) return false;

    HRESULT hr = H.shadow->Present(syncInterval, pf);
    H.lastPresentHr = (long)hr;
    if (firstLogged == 1) {
        firstLogged = 2;
        FgLog("host: first shadow Present(%u, 0x%X) -> 0x%08X removed=0x%08X", syncInterval, pf, (unsigned)hr, (unsigned)H.device->GetDeviceRemovedReason());
        RfDbg::Removed(H.device, "FG first shadow Present");
    }
    // Unity's chain MUST still be presented: Unity 2019.4 tracks its own back-buffer index and renders the next
    // frame into buffer (i+1)%n, while a flip-model chain only advances its index on Present. Skipping it made
    // Unity write to a non-current back buffer (debug layer id=907) and DXGI removed the device with
    // DXGI_ERROR_ACCESS_DENIED (0x887A002B) on the second frame. Sync 0 (+ tearing iff Unity asked for it), so it
    // never waits on vblank - the shadow Present above carries the pacing and the DComp visual hides this one.
    HRESULT uhr = FgOriginalPresent(app, 0, flags & DXGI_PRESENT_ALLOW_TEARING);
    if (FAILED(uhr)) hr = uhr;
    if (FAILED(hr) || hr == DXGI_STATUS_OCCLUDED) {
        char why[64];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "shadow Present 0x%08X", (unsigned)hr);
        H.lastError = FG_ERR_NO_SWAPCHAIN;
        TearDownLocked(why);                           // this frame goes through the original Present
        return false;
    }
    FgPresentedAdd(1 + (generated < 0 ? 0 : generated));
    *outHr = hr;
    return true;
}

void FgHostOnResize(unsigned w, unsigned h)
{
    Locked lk;
    if (!H.prov) return;
    char why[64];
    _snprintf_s(why, sizeof(why), _TRUNCATE, "resize %ux%u", w, h);
    TearDownLocked(why);                               // the managed driver rebuilds it (Fg_Alive -> 0)
}

void FgHostShutdown(void)
{
    Locked lk;
    TearDownLocked("shutdown");
    H.enabled = 0;
}

int FgHostAlive(void) { return H.prov ? 1 : 0; }
unsigned FgHostCaps(void) { return H.prov ? H.prov->Caps() : 0u; }
int FgHostProvider(void) { return H.prov ? H.prov->Id() : FG_PROVIDER_NONE; }

const char* FgHostStatus(void)
{
    _snprintf_s(H.status, sizeof(H.status), _TRUNCATE,
        "provider=%s enabled=%d multiplier=%u shadow=%p out=%ux%u flags=0x%X caps=0x%X lastError=%d presentHr=0x%08X presented=%lld fps=%d frameId=%llu",
        H.prov ? H.prov->Name() : "-", H.enabled, H.multiplier, (void*)H.shadow, H.outW, H.outH, H.scFlags,
        FgHostCaps(), H.lastError, (unsigned)H.lastPresentHr, FgPresentCount(), FgPresentedFps(),
        (unsigned long long)H.frames[H.frameIdx & 3].frameId);
    return H.status;
}
