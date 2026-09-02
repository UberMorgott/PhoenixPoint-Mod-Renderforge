// FgHost.cpp - the presentation host. Owns the FG-owned "shadow" swapchain on our child window (FgWnd.cpp),
// copies Unity's finished backbuffer into it every frame, drives one IFgProvider and presents the shadow chain.
// Unity's own swapchain is STILL presented every frame (sync 0, hidden under the child window) so its back-buffer
// index keeps rotating in step with Unity's own tracking - but ONLY by the hook (FgHook.cpp), exactly once per
// hooked call; the host never presents Unity's chain, so no path can present it twice (that was a device removal).
//
// Ownership: the chain lives in H.c and is DESTROYED ON THE MAIN THREAD ONLY (FgHostPump / FgHostShutdown /
// FgHostInit). Every other thread - the render thread inside its Present hook, the UI thread inside WM_NCDESTROY,
// whichever thread Unity calls ResizeBuffers on - only DETACHES it: a flag flip under the lock that stops the render
// thread from pinning it again. Destroying on the render thread was a 5 s stall per teardown (the FSR proxy's
// Destroy waits for presents the render thread must keep feeding) and D3D12Ring::WaitIdle on Unity's frame fence
// from inside the Present hook (4 x 5 s). State: kNone -> (Init) kLive -> (Detach) kDetached -> (Pump) kNone.
// Init refuses while kDetached: the provider singletons (g_fsr / g_xess / g_dlssg) are never Create()d while their
// previous chain is still being destroyed.
//
// One SRWLOCK guards the STATE only - never held across vendor work, a Present, a GPU wait or a window message:
//   - the render thread PINS the chain (Pin/Unpin) around Prepare and OnPresent and works on H.c unlocked;
//   - the main thread's destroy waits (unlocked, bounded) for the render thread to unpin, then works on H.c
//     unlocked too - the render thread cannot pin a detached chain, and Create/SetEnabled/Destroy are all main
//     thread, so a provider is never destroyed under a call in flight.
//
// Two D3D12Ring instances (D3D12Ring.h, the ring every D3D12 backend uses): `prep` submits through
// IUnityGraphicsD3D12v5::ExecuteCommandList with the resource states Unity must know about (its state
// arrays outlive the call, which a stack array would not); `copy` submits straight on Unity's queue inside
// the Present hook, where the frame is already complete and Unity's fence does not apply.
#include "Fg.h"
#include "RenderforgeNative.h"
#include "D3D12Ring.h"

#include <stdio.h>
#include <string.h>

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

enum { kNone = 0, kLive = 1, kDetached = 2 };
const int kDestroyTries = 120;          // pumps (frames) a provider may refuse Destroy before it is forced

struct Chain
{
    IFgProvider*          prov;
    IDXGISwapChain3*      app;          // the Unity chain this was built on, our own reference
    IDXGISwapChain4*      shadow;
    IDXGIFactory2*        factory;
    D3D12Ring             prep;         // DLSS_EV_FG_PREPARE, via Unity
    D3D12Ring             copy;         // backbuffer copy, direct
    ID3D12Fence*          xfence;       // Unity queue -> provider present queue handoff (its own fence: values on
    UINT64                xval;         // one fence from two queues retire out of order and would free a live slot)
    unsigned              scFlags;      // DXGI_SWAP_CHAIN_FLAG_* the shadow chain was created with
    HWND                  child;        // our own WS_CHILD window the chain sits on
    int                   logged;       // 0 nothing, 1 first present, 2 first present result
    Chain() : prov(NULL), app(NULL), shadow(NULL), factory(NULL), xfence(NULL), xval(0), scFlags(0), child(NULL), logged(0) {}
};

struct Host
{
    Chain                 c;
    int                   state;        // kNone / kLive / kDetached, under g_lock
    char                  why[64];      // why it was detached
    int                   destroyTries; // failed Destroy() attempts on the detached chain
    ID3D12Device*         device;
    ID3D12CommandQueue*   queue;
    unsigned              multiplier;
    long                  childHr;      // HRESULT of CreateSwapChainForHwnd on the child (last attempt)
    int                   enabled;
    int                   lastError;
    const char*           reason;       // static text explaining the last Init failure, else NULL
    long                  lastPresentHr;
    unsigned              outW, outH;
    unsigned              lastCaps;     // caps of the last provider Create tried, live or not
    FgFrame               slot;         // main thread writes, render thread copies; both under g_lock; resources retained
    FgFrame               cur;          // render-thread copy
    int                   pinned;       // render thread is inside Prepare/OnPresent on H.c
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
// False = a queue/fence call failed (device removed): the caller detaches without presenting.
bool CopyBackBuffer(Chain& c, ID3D12Resource* src, ID3D12Resource* dst)
{
    ID3D12CommandQueue* q = c.prov->PresentQueue();
    if (!q || !c.xfence) q = H.queue;
    if (q != H.queue) {
        if (FAILED(H.queue->Signal(c.xfence, ++c.xval)) || FAILED(q->Wait(c.xfence, c.xval))) return false;
    }
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

    if (!c.copy.EndDirect(q)) return false;
    if (q != H.queue && c.copy.fence && FAILED(H.queue->Wait(c.copy.fence, c.copy.fenceVal))) return false;
    return true;
}

// Main thread, no lock held. GPU objects of a chain whose provider is already gone (or never came up).
void ReleaseGpu(Chain& c)
{
    c.prep.Release();
    c.copy.Release();
    SafeRelease(c.xfence);
    c.xval = 0;
    SafeRelease(c.shadow);
    SafeRelease(c.factory);
    SafeRelease(c.app);
    if (c.child) { FgWndDestroy(); c.child = NULL; }
    c.scFlags = 0;
}

// Main thread, no lock held, chain detached. The host's shadow reference goes first (XeSS-FG's xefgSwapChainDestroy
// refuses while any proxy reference is outstanding; FSR's swapchain context holds its own), then the provider, then
// the remaining GPU objects. False = the provider refused and kept its handles: retried on the next pump.
bool DestroyChain(Chain& c, const char* why, bool force)
{
    ULONGLONG t0 = GetTickCount64();
    if (c.prov) c.prov->SetEnabled(false);
    SafeRelease(c.shadow);
    if (c.prov) {
        if (!c.prov->Destroy(force)) { FgLog("host: teardown (%s): provider refused Destroy (try %d)", why, H.destroyTries + 1); return false; }
        c.prov = NULL;
    }
    ULONGLONG t1 = GetTickCount64();
    ReleaseGpu(c);
    FgLog("host: teardown (%s) tid %u: provider %llu ms, gpu %llu ms%s", why, (unsigned)GetCurrentThreadId(),
          (unsigned long long)(t1 - t0), (unsigned long long)(GetTickCount64() - t1), force ? " (forced)" : "");
    return true;
}

// Any thread, lock only. A live chain stops being pinnable; the main thread destroys it on its next pump.
void Detach(const char* why)
{
    Locked lk;
    if (H.state != kLive) return;
    H.state = kDetached;
    H.enabled = 0;
    H.destroyTries = 0;
    strncpy_s(H.why, sizeof(H.why), why, _TRUNCATE);
}

// Main thread. Destroys the detached chain once the render thread has left it (it leaves every frame; `waitMs`
// bounds a hung one). Returns true when nothing is left detached (a live chain is not "left").
bool DestroyDetached(DWORD waitMs)
{
    {
        Locked lk;
        if (H.state != kDetached) return true;
    }
    ULONGLONG until = GetTickCount64() + waitMs;
    for (;;) {
        { Locked lk; if (!H.pinned) break; }
        if (GetTickCount64() >= until) { FgLog("host: teardown (%s): render thread still pinned after %lu ms - deferred", H.why, waitMs); return false; }
        Sleep(1);
    }
    bool force = H.destroyTries >= kDestroyTries;
    if (!DestroyChain(H.c, H.why, force)) { ++H.destroyTries; return false; }
    Locked lk;
    H.c = Chain();
    H.state = kNone;
    return true;
}

// Render thread. True with H.c pinned (stable until Unpin) and H.cur refreshed from the slot.
bool Pin()
{
    Locked lk;
    if (H.state != kLive || !H.enabled) return false;
    H.pinned = 1;
    H.cur = H.slot;
    return true;
}

// Render thread. `tearWhy` != NULL = this thread found the chain dead: detach (the main thread destroys).
void Unpin(const char* tearWhy)
{
    { Locked lk; H.pinned = 0; }
    if (tearWhy) Detach(tearWhy);
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
    int Id() const { return FG_PROVIDER_NONE; }
    unsigned Caps() const { return FG_CAP_2X; }        // reported, never used: NONE generates nothing
    const char* Name() const { return "none"; }
    int Create(const FgSetup& s, IDXGISwapChain4** out) { return FgHostCreateChildSwapChain(s, out); }
    void Prepare(ID3D12GraphicsCommandList*, const FgFrame&) {}
    int  Generate(const FgFrame&, ID3D12Resource*, IDXGISwapChain4*, UINT, UINT) { return 0; }
    void SetEnabled(bool) {}
    bool Destroy(bool) { return true; }
};

ProviderNone g_none;

} // namespace

IFgProvider* MakeFgProviderNone(void) { return &g_none; }

// The child-HWND chain: a window of our own over the game's client area (FgWnd.cpp) with an ordinary
// CreateSwapChainForHwnd on it - the shape every vendor FG SDK needs (a second chain on Unity's own window is
// E_ACCESSDENIED). Both run inside Init (main thread, H.state == kNone, so the render thread ignores H.c).
HWND FgHostChildHwnd(const FgSetup& s)
{
    if (!H.c.child) H.c.child = FgWndCreate(s.hwnd);
    if (!H.c.child) { H.childHr = HRESULT_FROM_WIN32(FgWndProbeNow()->createErr); return NULL; }
    H.c.scFlags = 0;                                   // a provider-built chain gets plain Present(sync, 0)
    return H.c.child;
}

int FgHostCreateChildSwapChain(const FgSetup& s, IDXGISwapChain4** out)
{
    *out = NULL;
    if (!FgHostChildHwnd(s)) return FG_ERR_NO_SWAPCHAIN;
    DXGI_SWAP_CHAIN_DESC1 d = s.desc;
    d.Scaling = DXGI_SCALING_STRETCH;                  // child rect vs buffer size may disagree for a frame (DPI, resize)
    d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    BOOL tearing = FALSE;
    IDXGIFactory5* f5 = NULL;
    if (SUCCEEDED(s.factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5))) {
        if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) tearing = FALSE;
        f5->Release();
    }
    d.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = s.factory->CreateSwapChainForHwnd(s.queue, H.c.child, &d, NULL, NULL, &sc1);
    if (FAILED(hr) && d.Flags) {
        FgLog("host: child CreateSwapChainForHwnd with ALLOW_TEARING 0x%08X - retrying without", (unsigned)hr);
        d.Flags = 0;
        hr = s.factory->CreateSwapChainForHwnd(s.queue, H.c.child, &d, NULL, NULL, &sc1);
    }
    H.childHr = (long)hr;
    if (FAILED(hr) || !sc1) { FgLog("host: child CreateSwapChainForHwnd 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }
    IDXGISwapChain4* sc4 = NULL;
    hr = sc1->QueryInterface(__uuidof(IDXGISwapChain4), (void**)&sc4);
    sc1->Release();
    if (FAILED(hr) || !sc4) { FgLog("host: child chain is not IDXGISwapChain4 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }
    s.factory->MakeWindowAssociation(H.c.child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    H.c.scFlags = d.Flags;
    FgLog("host: child chain %p on child hwnd %p (parent %p), flags 0x%X", (void*)sc4, (void*)H.c.child, (void*)s.hwnd, d.Flags);
    *out = sc4;
    return FG_OK;
}

// ---------------------------------------------------------------- host

int FgHostInit(int provider, unsigned multiplier, const wchar_t* dllDir)
{
    if (!DestroyDetached(2000)) return FG_ERR_NO_SWAPCHAIN;   // the previous chain is still going: caller retries
    {
        Locked lk;
        if (H.state == kLive) return FG_OK;
    }
    if (!g_unityD3D12) return FG_ERR_NOT_D3D12;

    HWND hwnd = NULL;
    DXGI_SWAP_CHAIN_DESC1 ad1 = {};
    IDXGISwapChain3* app = FgAppAcquire(&hwnd, &ad1);
    if (!app || !hwnd) { SafeRelease(app); return FG_ERR_NO_SWAPCHAIN; }   // hook has not seen a Present yet: caller retries
    BOOL fullscreen = FALSE;
    if (SUCCEEDED(app->GetFullscreenState(&fullscreen, NULL)) && fullscreen) { app->Release(); return FG_ERR_NO_SWAPCHAIN; }  // never over exclusive fullscreen

    IFgProvider* p = NULL;
    switch (provider) {
    case FG_PROVIDER_FSR:  p = MakeFgProviderFsr();        break;
    case FG_PROVIDER_XESS: p = MakeFgProviderXess();       break;
    case FG_PROVIDER_DLSS: p = MakeFgProviderStreamline(); break;
    default:               p = MakeFgProviderNone();       break;
    }

    H.device = g_unityD3D12->GetDevice();
    H.queue  = g_unityD3D12->GetCommandQueue();
    if (!H.device || !H.queue) { app->Release(); return FG_ERR_NOT_D3D12; }

    // From here on H.c is being built on the main thread in state kNone: the render thread never pins it.
    Chain& c = H.c;
    c.app = app;                                       // the reference FgAppAcquire handed us
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&c.factory);
    if (FAILED(hr)) { FgLog("host: CreateDXGIFactory2 0x%08X", (unsigned)hr); ReleaseGpu(c); return FG_ERR_NO_SWAPCHAIN; }

    FgSetup s = {};
    s.device = H.device; s.queue = H.queue; s.factory = c.factory; s.hwnd = hwnd;
    s.multiplier = multiplier < 2 ? 2 : (multiplier > 4 ? 4 : multiplier);
    s.dllDir = dllDir;
    s.desc = ad1;
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
                 : rc == FG_ERR_NO_PROVIDER ? "vendor DLLs missing from the mod folder"
                 : rc == FG_ERR_PROVIDER_FAILED ? "vendor SDK init failed - see renderforge_fg.log" : NULL;
        return rc;
    }
    c.shadow = shadow;
    c.prep.Attach(H.device);
    c.copy.Attach(H.device);
    if (p->PresentQueue()) H.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.xfence));
    H.lastError = FG_OK;
    H.reason = NULL;
    H.lastPresentHr = 0;
    {
        Locked lk;
        c.prov = p;                                        // publish: the render thread may pin from here on
        H.state = kLive;
    }
    FgLog("host: provider=%s multiplier=%u shadow=%p %ux%u flags=0x%X caps=0x%X",
          p->Name(), H.multiplier, (void*)c.shadow, H.outW, H.outH, c.scFlags, p->Caps());
    return FG_OK;
}

// Main thread. The flag under the lock, the provider's SDK call outside it (SetEnabled and Destroy are both
// main-thread only, so the provider cannot go away underneath).
void FgHostSetEnabled(int on)
{
    IFgProvider* p = NULL;
    {
        Locked lk;
        H.enabled = on ? 1 : 0;
        if (H.state == kLive) p = H.c.prov;
    }
    if (p) p->SetEnabled(on != 0);
    FgLog("host: enabled=%d", on ? 1 : 0);
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
    if (!c.logged) {
        c.logged = 1;
        FgLog("host: first present: appIdx=%u shadowIdx=%u src=%p dst=%p sync=%u flags=0x%X generated=%d tid=%u",
              c.app->GetCurrentBackBufferIndex(), c.shadow->GetCurrentBackBufferIndex(), (void*)src, (void*)dst,
              syncInterval, flags, generated, (unsigned)GetCurrentThreadId());
    }
    bool copied = CopyBackBuffer(c, src, dst);
    src->Release(); dst->Release();
    // A queue/fence failure is the device going away: detach without presenting, the hook forwards Unity's frame.
    if (!copied) { H.lastError = FG_ERR_NO_SWAPCHAIN; Unpin("back-buffer copy failed"); return false; }
    // A provider presenting on its own queue (DLSS-G) reads the back buffer we just wrote on ITS queues right after
    // Present, and nothing fences that read against our copy (measured with the debug layer on, DLSS-G 2x/3x/4x:
    // ~35-48/s id=1047 "fake-swapchain-buffer still referenced by cmdQ.dlssg / cmdQ.game" without this wait, 0 with
    // it, fps unchanged). So the copy retires on the CPU before Present - the same one-frame-in-flight cadence Reflex
    // low-latency imposes anyway. ponytail: a GPU-side handoff would need DLSS-G's own fence, which it does not expose.
    if (c.prov->PresentQueue() && c.copy.fence && c.copy.WaitOn(c.copy.fence, c.copy.fenceVal) != WAIT_OBJECT_0) {
        H.lastError = FG_ERR_NO_SWAPCHAIN;
        Unpin("copy fence wait failed");
        return false;
    }
    c.prov->BeforePresent();                           // the copy is the last write into the shadow back buffer

    HRESULT hr = c.shadow->Present(syncInterval, pf);
    H.lastPresentHr = (long)hr;
    c.prov->AfterPresent(hr);
    if (c.logged == 1) {
        c.logged = 2;
        FgLog("host: first shadow Present(%u, 0x%X) -> 0x%08X removed=0x%08X", syncInterval, pf, (unsigned)hr, (unsigned)H.device->GetDeviceRemovedReason());
        RfDbg::Removed(H.device, "FG first shadow Present");
    }
    if (FAILED(hr) || hr == DXGI_STATUS_OCCLUDED) {
        char why[64];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "shadow Present 0x%08X", (unsigned)hr);
        H.lastError = FG_ERR_NO_SWAPCHAIN;
        Unpin(why);                                    // detached; the hook presents Unity's frame as usual
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
        Detach(why);
    }
}

void FgHostOnResize(unsigned w, unsigned h)
{
    char why[64];
    _snprintf_s(why, sizeof(why), _TRUNCATE, "resize %ux%u", w, h);
    Detach(why);                                       // the managed driver rebuilds it (Fg_Alive -> 0)
}

void FgHostTearDown(const char* why) { Detach(why); }

int FgHostPump(void)
{
    {
        Locked lk;
        // The hook forgot / rediscovered the app chain (WM_NCDESTROY): a chain built on the old one is dead.
        if (H.state == kLive && !FgAppIs((IDXGISwapChain*)H.c.app)) { H.state = kDetached; H.enabled = 0; H.destroyTries = 0; strncpy_s(H.why, sizeof(H.why), "app chain changed", _TRUNCATE); }
    }
    return DestroyDetached(2000) ? 1 : 0;
}

int FgHostShutdown(void)
{
    Detach("shutdown");
    int ok = DestroyDetached(10000) ? 1 : 0;
    Locked lk;
    H.enabled = 0;
    ReleaseSlotLocked();
    return ok;
}

int FgHostAlive(void) { Locked lk; return H.state == kLive ? 1 : 0; }
unsigned FgHostCaps(void) { Locked lk; return H.state == kLive ? H.c.prov->Caps() : H.lastCaps; }
int FgHostProvider(void) { Locked lk; return H.state == kLive ? H.c.prov->Id() : FG_PROVIDER_NONE; }
const char* FgHostReason(void) { return H.reason ? H.reason : ""; }

const char* FgHostStatus(void)
{
    const FgWndProbe* w = FgWndProbeNow();
    HWND game = w->parent;
    const char* focus = !w->focus ? "none" : w->focus == game ? "game" : w->focus == w->child ? "child" : "other";
    unsigned long long frameId;
    const char* prov; int state; void* shadow; void* child; unsigned scFlags; unsigned caps;
    {
        Locked lk;
        frameId = H.slot.frameId;
        prov = H.state == kLive ? H.c.prov->Name() : H.state == kDetached ? "detached" : "-";
        state = H.state;
        shadow = H.c.shadow; child = H.c.child; scFlags = H.c.scFlags;
        caps = H.state == kLive ? H.c.prov->Caps() : H.lastCaps;
    }
    _snprintf_s(H.status, sizeof(H.status), _TRUNCATE,
        "provider=%s enabled=%d multiplier=%u chain=%s child=%p childHr=0x%08X hit=%d focus=%s fg=%d shadow=%p out=%ux%u flags=0x%X caps=0x%X lastError=%d presentHr=0x%08X presented=%lld fps=%d frameId=%llu%s%s",
        prov, H.enabled, H.multiplier,
        state == kNone ? "-" : "child", child, (unsigned)H.childHr, w->hit, focus,
        w->foreground == game ? 1 : 0,
        shadow, H.outW, H.outH, scFlags,
        caps, H.lastError, (unsigned)H.lastPresentHr, FgPresentCount(), FgPresentedFps(),
        frameId, H.reason ? " reason=" : "", H.reason ? H.reason : "");
    return H.status;
}
