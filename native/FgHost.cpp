// FgHost.cpp - the presentation host. Owns the FG-owned "shadow" swapchain, copies Unity's finished
// backbuffer into it every frame, drives one IFgProvider and presents the shadow chain. Unity's own swapchain
// is STILL presented every frame (sync 0, hidden under the child window / DComp visual) so its back-buffer index
// keeps rotating in step with Unity's own tracking - but ONLY by the hook (FgHook.cpp), exactly once per hooked
// call; the host never presents Unity's chain, so no path can present it twice (that was a device removal).
//
// The shadow chain is a COMPOSITION swapchain shown through a DirectComposition target on the game's HWND
// (decision 4a). A second CreateSwapChainForHwnd on a window that already owns a flip-model chain fails
// with E_ACCESSDENIED (measured on Instance3: 0x80070005 on every retry); the Task 1 spike's
// `secondSwapChain=0x00000000` was the zero-initialised FgSpike field - the probe only ran inside
// DLSS_EV_FG_PREPARE, which the driver issues only while FrameGen is already live.
//
// Threading: Init/Shutdown/SetEnabled/SetFrame on the main thread, Prepare/OnPresent on the render thread,
// OnResize/TearDown from whichever thread Unity calls ResizeBuffers/SetFullscreenState on (measured: the main
// thread) or the UI thread (WM_NCDESTROY). One SRWLOCK guards the STATE only - it is never held across vendor
// work, a Present, a GPU wait or a window message:
//   - the render thread PINS the chain (Pin/Unpin) around Prepare and OnPresent and works on H.c unlocked;
//   - a teardown waits (unlocked, bounded) for the render thread to unpin, DETACHES the chain under the lock
//     (H.c -> local) and destroys the local outside the lock on the calling thread (RequestTeardown).
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
#include <string.h>

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

// Everything one teardown releases. Detached from H under the lock, destroyed outside it.
struct Chain
{
    IFgProvider*          prov;
    IDXGISwapChain3*      app;          // the Unity chain this was built on, AddRef'd while the chain lives
    IDXGISwapChain4*      shadow;
    IDXGIFactory2*        factory;
    IDCompositionDevice*  dcomp;
    IDCompositionTarget*  target;
    IDCompositionVisual*  visual;
    D3D12Ring             prep;         // DLSS_EV_FG_PREPARE, via Unity
    D3D12Ring             copy;         // backbuffer copy, direct
    ID3D12Fence*          xfence;       // Unity queue -> provider present queue handoff (its own fence: values on
    UINT64                xval;         // one fence from two queues retire out of order and would free a live slot)
    unsigned              scFlags;      // DXGI_SWAP_CHAIN_FLAG_* the shadow chain was created with
    int                   chainKind;    // 0 none, 1 child HWND, 2 composition
    HWND                  child;        // our own WS_CHILD window (chainKind 1)
    Chain() : prov(NULL), app(NULL), shadow(NULL), factory(NULL), dcomp(NULL), target(NULL), visual(NULL),
              xfence(NULL), xval(0), scFlags(0), chainKind(0), child(NULL) {}
};

struct Host
{
    Chain                 c;
    ID3D12Device*         device;
    ID3D12CommandQueue*   queue;
    unsigned              multiplier;
    long                  childHr;      // HRESULT of CreateSwapChainForHwnd on the child (last attempt)
    int                   enabled;
    int                   lastError;
    const char*           reason;       // static text explaining lastError == FG_ERR_NO_PROVIDER, else NULL
    long                  lastPresentHr;
    unsigned              outW, outH;
    unsigned              lastCaps;     // caps of the last provider Create tried, live or not
    FgFrame               slot;         // main thread writes, render thread copies; both under g_lock; resources retained
    FgFrame               cur;          // render-thread copy
    int                   pinned;       // render thread is inside Prepare/OnPresent on H.c
    int                   pending;      // a teardown arrived while pinned; Unpin performs it
    char                  pendingWhy[64];
    char                  status[512];
};

Host H;
SRWLOCK g_lock = SRWLOCK_INIT;

struct Locked
{
    Locked()  { AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
};

// Barrier src PRESENT->COPY_SOURCE and dst PRESENT->COPY_DEST, CopyResource, barrier back, execute on the queue.
// A provider with its own presenting queue (DLSS-G) gets the copy on THAT queue: it first waits for everything Unity's
// queue submitted for this frame (xfence), and Unity's queue then waits for the copy (ring fence) before it may render
// into that back buffer again - DLSS-G holds its presenting queue while it paces, so the copy can run frames later
// (measured: debug layer id=1047 races on Unity's back buffers, then id=541 + DEVICE_REMOVED, without the second wait).
bool CopyBackBuffer(Chain& c, ID3D12Resource* src, ID3D12Resource* dst)
{
    ID3D12CommandQueue* q = c.prov->PresentQueue();
    if (!q || !c.xfence) q = H.queue;
    ID3D12GraphicsCommandList* l = c.copy.Begin();
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

    if (q != H.queue) {
        H.queue->Signal(c.xfence, ++c.xval);
        q->Wait(c.xfence, c.xval);
    }
    bool ok = c.copy.EndDirect(q);
    if (ok && q != H.queue && c.copy.fence) H.queue->Wait(c.copy.fence, c.copy.fenceVal);
    return ok;
}

// No lock held. GPU objects of a chain whose provider is already gone (or never came up).
void ReleaseGpu(Chain& c)
{
    c.prep.Release();
    c.copy.Release();
    SafeRelease(c.xfence);
    c.xval = 0;
    if (c.visual) c.visual->SetContent(NULL);
    if (c.dcomp)  c.dcomp->Commit();
    SafeRelease(c.visual);
    SafeRelease(c.target);
    SafeRelease(c.dcomp);
    SafeRelease(c.shadow);
    SafeRelease(c.factory);
    SafeRelease(c.app);
    if (c.child) { FgWndDestroy(); c.child = NULL; }
    c.scFlags = 0;
    c.chainKind = 0;
}

// No lock held. The host's shadow reference goes first (XeSS-FG's xefgSwapChainDestroy refuses while any proxy
// reference is outstanding; FSR's swapchain context and the DComp visual hold their own), then the provider, then
// the remaining GPU objects.
void DestroyChain(Chain& c, const char* why)
{
    ULONGLONG t0 = GetTickCount64();
    if (c.prov) c.prov->SetEnabled(false);
    SafeRelease(c.shadow);
    if (c.prov) { c.prov->Destroy(); c.prov = NULL; }
    ULONGLONG t1 = GetTickCount64();
    ReleaseGpu(c);
    FgLog("host: teardown (%s) tid %u: provider %llu ms, gpu %llu ms", why, (unsigned)GetCurrentThreadId(),
          (unsigned long long)(t1 - t0), (unsigned long long)(GetTickCount64() - t1));
}

// Lock held. Moves the live chain out of H so no thread starts new work on it. False when there is none.
bool DetachLocked(Chain* out)
{
    if (!H.c.prov) return false;
    *out = H.c;
    H.c = Chain();
    H.enabled = 0;
    H.pending = 0;
    return true;
}

// Any thread but the render thread's own Prepare/OnPresent (those use Unpin(why)). Waits - lock NOT held -
// for the render thread to leave the chain (what Unity's main thread does every frame anyway), then detaches
// under the lock and destroys on the CALLER. Destroying on the render thread inside its Present hook is not an
// option: the FSR proxy's Destroy waits for queued presents the render thread must keep feeding (measured: a
// 5 s stall per teardown). If the render thread stays pinned for 2 s the request is parked and its Unpin runs it.
void RequestTeardown(const char* why)
{
    Chain c;
    bool now = false;
    for (int i = 0; i < 200; ++i) {
        {
            Locked lk;
            if (!H.c.prov) return;
            if (!H.pinned) { now = DetachLocked(&c); break; }
            H.enabled = 0;                             // no new Pin while we wait
        }
        Sleep(10);
    }
    if (now) { DestroyChain(c, why); return; }
    Locked lk;
    if (H.c.prov) { H.pending = 1; strncpy_s(H.pendingWhy, sizeof(H.pendingWhy), why, _TRUNCATE); }
}

// Render thread. True with H.c pinned (stable until Unpin) and H.cur refreshed from the slot.
bool Pin()
{
    Locked lk;
    if (!H.c.prov || !H.enabled || H.pending) return false;
    H.pinned = 1;
    H.cur = H.slot;
    return true;
}

// Render thread. `tearWhy` != NULL = this thread found the chain dead; either way a parked teardown runs now.
void Unpin(const char* tearWhy)
{
    Chain c;
    bool now = false;
    {
        Locked lk;
        H.pinned = 0;
        if (tearWhy && H.c.prov) { H.pending = 1; strncpy_s(H.pendingWhy, sizeof(H.pendingWhy), tearWhy, _TRUNCATE); }
        if (H.pending) now = DetachLocked(&c);
    }
    if (now) DestroyChain(c, H.pendingWhy);
}

// Lock held. Drop the retained Unity RTs of the frame slot.
void ReleaseSlotLocked()
{
    SafeRelease(H.slot.hudless);
    SafeRelease(H.slot.depth);
    SafeRelease(H.slot.mv);
    memset(&H.slot, 0, sizeof(H.slot));
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

// Composition swapchain + DirectComposition target on s.hwnd (topmost, above Unity's own chain).
// Tearing: asked for when the factory reports DXGI_FEATURE_PRESENT_ALLOW_TEARING, dropped if the
// composition chain refuses the flag; the flags actually granted are kept in H.c.scFlags for Present.
// The child-HWND chain: a window of our own over the game's client area (FgWnd.cpp) with an ordinary
// CreateSwapChainForHwnd on it - the shape every vendor FG SDK needs. Composition is the fallback.
// RENDERFORGE_FG_CHAIN=composition skips the child. Both run inside Init (main thread, H.c.prov still
// NULL, so the render thread ignores H.c) - no lock needed.
HWND FgHostChildHwnd(const FgSetup& s)
{
    char env[32] = {};
    if (GetEnvironmentVariableA("RENDERFORGE_FG_CHAIN", env, sizeof(env)) && _stricmp(env, "composition") == 0) return NULL;
    if (!H.c.child) H.c.child = FgWndCreate(s.hwnd);
    if (!H.c.child) { H.childHr = HRESULT_FROM_WIN32(FgWndProbeNow()->createErr); return NULL; }
    H.c.scFlags = 0;                                   // a provider-built chain gets plain Present(sync, 0)
    H.c.chainKind = 1;
    return H.c.child;
}

static int CreateChildChain(const FgSetup& s, DXGI_SWAP_CHAIN_DESC1 d, IDXGISwapChain4** out)
{
    if (!FgHostChildHwnd(s)) return FG_ERR_NO_SWAPCHAIN;
    d.Scaling = DXGI_SCALING_STRETCH;                  // child rect vs buffer size may disagree for a frame (DPI, resize)
    d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = s.factory->CreateSwapChainForHwnd(s.queue, H.c.child, &d, NULL, NULL, &sc1);
    if (FAILED(hr) && d.Flags) {
        FgLog("host: child CreateSwapChainForHwnd with ALLOW_TEARING 0x%08X - retrying without", (unsigned)hr);
        d.Flags = 0;
        hr = s.factory->CreateSwapChainForHwnd(s.queue, H.c.child, &d, NULL, NULL, &sc1);
    }
    H.childHr = (long)hr;
    if (FAILED(hr) || !sc1) { FgLog("host: child CreateSwapChainForHwnd 0x%08X", (unsigned)hr); FgWndDestroy(); H.c.child = NULL; return FG_ERR_NO_SWAPCHAIN; }
    IDXGISwapChain4* sc4 = NULL;
    hr = sc1->QueryInterface(__uuidof(IDXGISwapChain4), (void**)&sc4);
    sc1->Release();
    if (FAILED(hr) || !sc4) { FgLog("host: child chain is not IDXGISwapChain4 0x%08X", (unsigned)hr); FgWndDestroy(); H.c.child = NULL; return FG_ERR_NO_SWAPCHAIN; }
    s.factory->MakeWindowAssociation(H.c.child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    H.c.scFlags = d.Flags;
    H.c.chainKind = 1;
    FgLog("host: child chain %p on child hwnd %p (parent %p), flags 0x%X", (void*)sc4, (void*)H.c.child, (void*)s.hwnd, d.Flags);
    *out = sc4;
    return FG_OK;
}

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

    if (CreateChildChain(s, d, out) == FG_OK) return FG_OK;

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

    Chain& c = H.c;
    hr = DCompositionCreateDevice2(NULL, __uuidof(IDCompositionDevice), (void**)&c.dcomp);
    if (SUCCEEDED(hr)) hr = c.dcomp->CreateTargetForHwnd(s.hwnd, TRUE, &c.target);
    if (SUCCEEDED(hr)) hr = c.dcomp->CreateVisual(&c.visual);
    if (SUCCEEDED(hr)) hr = c.visual->SetContent(sc4);
    if (SUCCEEDED(hr)) hr = c.target->SetRoot(c.visual);
    if (SUCCEEDED(hr)) hr = c.dcomp->Commit();
    if (FAILED(hr)) {
        FgLog("host: DirectComposition 0x%08X (device %p target %p visual %p)", (unsigned)hr, (void*)c.dcomp, (void*)c.target, (void*)c.visual);
        SafeRelease(c.visual); SafeRelease(c.target); SafeRelease(c.dcomp);
        sc4->Release();
        return FG_ERR_NO_SWAPCHAIN;
    }
    c.scFlags = d.Flags;
    c.chainKind = 2;
    FgLog("host: composition chain %p on hwnd %p, flags 0x%X (tearing supported %d)", (void*)sc4, (void*)s.hwnd, d.Flags, (int)tearing);
    *out = sc4;
    return FG_OK;
}

// ---------------------------------------------------------------- host

int FgHostInit(int provider, unsigned multiplier, const wchar_t* dllDir)
{
    {
        Locked lk;
        if (H.c.prov) return FG_OK;
        if (H.pending) return FG_ERR_NO_SWAPCHAIN;     // the render thread still owes a teardown: caller retries
    }
    if (!g_unityD3D12) return FG_ERR_NOT_D3D12;

    IDXGISwapChain3* app = FgAppSwapChain();
    HWND hwnd = FgAppHwnd();
    if (!app || !hwnd) return FG_ERR_NO_SWAPCHAIN;    // hook has not seen a Present yet: caller retries
    BOOL fullscreen = FALSE;
    if (SUCCEEDED(app->GetFullscreenState(&fullscreen, NULL)) && fullscreen) return FG_ERR_NO_SWAPCHAIN;  // never over exclusive fullscreen

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

    // From here on H.c is being built on the main thread with H.c.prov == NULL: the render thread never pins it.
    Chain& c = H.c;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&c.factory);
    if (FAILED(hr)) { FgLog("host: CreateDXGIFactory2 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }

    DXGI_SWAP_CHAIN_DESC ad = {};
    app->GetDesc(&ad);
    FgSetup s = {};
    s.device = H.device; s.queue = H.queue; s.factory = c.factory; s.hwnd = hwnd;
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
    // The provider's caps survive a refused multiplier (XeSS-FG: 1 interpolated frame on non-Intel GPUs), so the
    // picker can grey 3x/4x from Fg_Caps even while no chain is up.
    H.lastCaps = p->Caps();
    if (rc != FG_OK || !shadow) {
        ReleaseGpu(c);
        H.lastError = rc;
        H.reason = rc == FG_ERR_UNSUPPORTED_MULTIPLIER ? "multiplier above what this provider supports on this GPU"
                 : rc == FG_ERR_NO_PROVIDER ? "vendor DLLs missing from the mod folder" : NULL;
        return rc;
    }
    c.shadow = shadow;
    c.app = app; app->AddRef();
    c.prep.Attach(H.device);
    c.copy.Attach(H.device);
    if (p->PresentQueue()) H.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.xfence));
    H.lastError = FG_OK;
    H.reason = NULL;
    H.lastPresentHr = 0;
    {
        Locked lk;
        c.prov = p;                                        // publish: the render thread may pin from here on
    }
    FgLog("host: provider=%s multiplier=%u shadow=%p %ux%u flags=0x%X caps=0x%X",
          p->Name(), H.multiplier, (void*)c.shadow, H.outW, H.outH, c.scFlags, p->Caps());
    return FG_OK;
}

void FgHostSetEnabled(int on)
{
    Locked lk;
    H.enabled = on ? 1 : 0;
    if (H.c.prov) H.c.prov->SetEnabled(on != 0);         // a flag write in every provider, fine under the lock
    FgLog("host: enabled=%d", H.enabled);
}

// Single slot: the render thread copies it under the lock in Pin (a few hundred bytes), so a 4-deep ring and
// its racy index are not needed. The Unity RTs are retained until overwritten or Shutdown.
void FgHostSetFrame(const FgFrame& f)
{
    if (f.hudless) f.hudless->AddRef();
    if (f.depth)   f.depth->AddRef();
    if (f.mv)      f.mv->AddRef();
    ID3D12Resource *oh, *od, *om;
    {
        Locked lk;
        oh = H.slot.hudless; od = H.slot.depth; om = H.slot.mv;
        H.slot = f;
    }
    if (oh) oh->Release();
    if (od) od->Release();
    if (om) om->Release();
}

void FgHostPrepare(void)
{
    FgHookSpike();                       // Task 1 probes; no-op after the first call
    if (!Pin()) return;
    // The pass-through provider has nothing to prepare. Nothing is declared to Unity: providers read the
    // shim-owned twins (COMMON at rest), never the Unity RTs - declaring those as NON_PIXEL_SHADER_RESOURCE made
    // Unity transition them under the upscaler's own barriers (debug layer id=527 on every frame).
    if (H.c.prov->Id() != FG_PROVIDER_NONE && H.cur.hudless && H.cur.depth && H.cur.mv) {
        ID3D12GraphicsCommandList* l = H.c.prep.Begin();
        if (l) { H.c.prov->Prepare(l, H.cur); H.c.prep.End(0); }
    }
    Unpin(NULL);
}

bool FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags)
{
    if (!Pin()) return false;
    Chain& c = H.c;
    if (app != (IDXGISwapChain*)c.app || !c.shadow) { Unpin(NULL); return false; }

    // Only the present flags the shadow chain opted into: tearing iff created with ALLOW_TEARING (and
    // Unity asked for it this frame); never TEST / DO_NOT_SEQUENCE / RESTART, which belong to Unity's chain.
    UINT pf = (c.scFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? (flags & DXGI_PRESENT_ALLOW_TEARING) : 0;
    if (pf && syncInterval) pf = 0;                    // tearing needs sync interval 0

    ID3D12Resource* src = NULL;
    if (FAILED(c.app->GetBuffer(c.app->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&src)) || !src) { Unpin(NULL); return false; }

    // Generated frame(s) first - they sit between the previous real frame and this one - then the real one.
    // The provider presents its own frames on the shadow chain, so the current back buffer is fetched after.
    int generated = c.prov->Generate(H.cur, src, c.shadow, syncInterval, pf);

    ID3D12Resource* dst = NULL;
    if (FAILED(c.shadow->GetBuffer(c.shadow->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&dst)) || !dst) {
        src->Release();
        Unpin(NULL);
        return false;
    }
    static int firstLogged = 0;
    if (!firstLogged) {
        firstLogged = 1;
        FgLog("host: first present: appIdx=%u shadowIdx=%u src=%p dst=%p sync=%u flags=0x%X generated=%d tid=%u",
              c.app->GetCurrentBackBufferIndex(), c.shadow->GetCurrentBackBufferIndex(), (void*)src, (void*)dst,
              syncInterval, flags, generated, (unsigned)GetCurrentThreadId());
    }
    bool copied = CopyBackBuffer(c, src, dst);
    src->Release(); dst->Release();
    if (!copied) { Unpin(NULL); return false; }

    HRESULT hr = c.shadow->Present(syncInterval, pf);
    H.lastPresentHr = (long)hr;
    c.prov->AfterPresent(hr);
    if (firstLogged == 1) {
        firstLogged = 2;
        FgLog("host: first shadow Present(%u, 0x%X) -> 0x%08X removed=0x%08X", syncInterval, pf, (unsigned)hr, (unsigned)H.device->GetDeviceRemovedReason());
        RfDbg::Removed(H.device, "FG first shadow Present");
    }
    if (FAILED(hr) || hr == DXGI_STATUS_OCCLUDED) {
        char why[64];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "shadow Present 0x%08X", (unsigned)hr);
        H.lastError = FG_ERR_NO_SWAPCHAIN;
        Unpin(why);                                    // torn down; the hook presents Unity's frame as usual
        return false;
    }
    // Unity's chain MUST still be presented (by the hook, once): Unity 2019.4 tracks its own back-buffer index
    // and renders the next frame into buffer (i+1)%n, while a flip-model chain only advances its index on
    // Present. Skipping it made Unity write to a non-current back buffer (debug layer id=907) and DXGI removed
    // the device with DXGI_ERROR_ACCESS_DENIED (0x887A002B) on the second frame.
    FgPresentedAdd(1 + (generated < 0 ? 0 : generated));
    Unpin(NULL);
    return true;
}

void FgHostAfterUnityPresent(HRESULT hr)
{
    if (FAILED(hr) || hr == DXGI_STATUS_OCCLUDED) {
        char why[64];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "unity Present 0x%08X", (unsigned)hr);
        H.lastError = FG_ERR_NO_SWAPCHAIN;
        RequestTeardown(why);
    }
}

void FgHostOnResize(unsigned w, unsigned h)
{
    char why[64];
    _snprintf_s(why, sizeof(why), _TRUNCATE, "resize %ux%u", w, h);
    RequestTeardown(why);                              // the managed driver rebuilds it (Fg_Alive -> 0)
}

void FgHostTearDown(const char* why) { RequestTeardown(why); }

void FgHostShutdown(void)
{
    RequestTeardown("shutdown");
    Locked lk;
    H.enabled = 0;
    ReleaseSlotLocked();
}

int FgHostAlive(void) { return H.c.prov ? 1 : 0; }
unsigned FgHostCaps(void) { return H.c.prov ? H.c.prov->Caps() : H.lastCaps; }
int FgHostProvider(void) { return H.c.prov ? H.c.prov->Id() : FG_PROVIDER_NONE; }

const char* FgHostStatus(void)
{
    const FgWndProbe* w = FgWndProbeNow();
    HWND game = FgAppHwnd();
    const char* focus = !w->focus ? "none" : w->focus == game ? "game" : w->focus == w->child ? "child" : "other";
    unsigned long long frameId;
    { Locked lk; frameId = H.slot.frameId; }
    _snprintf_s(H.status, sizeof(H.status), _TRUNCATE,
        "provider=%s enabled=%d multiplier=%u chain=%s child=%p childHr=0x%08X hit=%d focus=%s fg=%d shadow=%p out=%ux%u flags=0x%X caps=0x%X lastError=%d presentHr=0x%08X presented=%lld fps=%d frameId=%llu%s%s",
        H.c.prov ? H.c.prov->Name() : "-", H.enabled, H.multiplier,
        H.c.chainKind == 1 ? "child" : H.c.chainKind == 2 ? "comp" : "-", (void*)H.c.child, (unsigned)H.childHr, w->hit, focus,
        w->foreground == game ? 1 : 0,
        (void*)H.c.shadow, H.outW, H.outH, H.c.scFlags,
        FgHostCaps(), H.lastError, (unsigned)H.lastPresentHr, FgPresentCount(), FgPresentedFps(),
        frameId, H.reason ? " reason=" : "", H.reason ? H.reason : "");
    return H.status;
}
