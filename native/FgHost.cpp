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
#include "D3D12Owned.h"
#include "FgTwins.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

enum { kNone = 0, kLive = 1, kDetached = 2 };
enum { kFrameRing = 8 };   // the main thread is at most a frame or two ahead of the render thread

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
    DXGI_FORMAT           backFmt;      // the app chain's back-buffer format = what the providers want the hud-less in
    FgTwins               twins;        // row-flipped hud-less / depth / mv copies the providers tag (FgTwins.h)
    Chain() : prov(NULL), app(NULL), shadow(NULL), factory(NULL), xfence(NULL), xval(0), scFlags(0), child(NULL), logged(0), backFmt(DXGI_FORMAT_UNKNOWN) {}
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
    // Render thread only. A Present with no frame prepared since the last one (menu screens without a rendering
    // camera, the options screen: Unity still presents, nothing runs the camera path) must not drive the provider:
    // a vendor Present without its per-frame inputs/markers wedges DLSS-G's pacer (measured: "Pacer flush has timed
    // out", "Couldn't lock the mutex on sync present", eFailReflexNotDetectedAtRuntime). The hook forwards Unity's
    // Present untouched, and the child window is hidden so Unity's own chain shows through instead of a stale frame.
    int                   prepared;     // FgHostPrepare recorded AND submitted this frame's inputs since the last OnPresent
    int                   prepFails;    // failed prepares this chain, for the capped FgLog
    long long             idle;         // consecutive presents skipped for lack of a prepared frame
    int                   hidden;       // the child window is hidden because of idle
    FgFrame               ring[kFrameRing]; // main thread writes under g_lock; an entry travels as its event's data
    unsigned              ringIdx;
    FgFrame               paired;       // render thread: the previous event's block (PairFrame)
    int                   havePaired;
    FgFrame               sent;         // render thread: what the previous Prepare was handed (capture notes)
    int                   haveSent;
    int                   twinsOk;      // render thread: this Prepare's list wrote the flipped copies (FgTwins.h)
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
bool ReleaseGpu(Chain& c)
{
    if ((!c.prep.WaitIdle(0) && !c.prep.Removed()) || (!c.copy.WaitIdle(0) && !c.copy.Removed())) return false;
    c.prep.Release();
    c.twins.Release();
    c.copy.Release();
    SafeRelease(c.xfence);
    c.xval = 0;
    SafeRelease(c.shadow);
    SafeRelease(c.factory);
    SafeRelease(c.app);
    if (c.child) { FgWndDestroy(); c.child = NULL; }
    c.scFlags = 0;
    return true;
}

// Main thread, no lock held, chain detached. The host's shadow reference goes first (XeSS-FG's xefgSwapChainDestroy
// refuses while any proxy reference is outstanding; FSR's swapchain context holds its own), then the provider, then
// the remaining GPU objects. False = the provider refused and kept its handles: retried on the next pump.
bool DestroyChain(Chain& c, const char* why, bool force)
{
    ULONGLONG t0 = GetTickCount64();
    if (c.prov) c.prov->SetEnabled(false);
    if ((!c.prep.WaitIdle(0) && !c.prep.Removed()) || (!c.copy.WaitIdle(0) && !c.copy.Removed())) return false;
    SafeRelease(c.shadow);
    if (c.prov) {
        if (!c.prov->Destroy(force)) { FgLog("host: teardown (%s): provider refused Destroy (try %d)", why, H.destroyTries + 1); return false; }
        c.prov = NULL;
    }
    ULONGLONG t1 = GetTickCount64();
    if (!ReleaseGpu(c)) return false;
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
    bool force = false; // A retry count never proves that the vendor/GPU stopped using its handles.
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
    if (H.state != kLive || !H.enabled || H.pinned) return false;
    H.pinned = 1;
    H.cur = H.slot;
    if (H.cur.hudless) H.cur.hudless->AddRef();
    if (H.cur.depth) H.cur.depth->AddRef();
    if (H.cur.mv) H.cur.mv->AddRef();
    return true;
}

// Render thread. `tearWhy` != NULL = this thread found the chain dead: detach (the main thread destroys).
void Unpin(const char* tearWhy)
{
    { Locked lk;
      SafeRelease(H.cur.hudless); SafeRelease(H.cur.depth); SafeRelease(H.cur.mv);
      H.pinned = 0;
    }
    if (tearWhy) Detach(tearWhy);
}

// ---------------------------------------------------------------- capture (diagnostic, Fg_Capture)

struct Capture
{
    volatile LONG state;        // 0 idle, 1 armed (main thread), 2 constants noted (Prepare), 3 written, -1 failed
    wchar_t dir[MAX_PATH];
    char    notes[16384];
    int     len;
};
Capture g_cap;

struct CapItem { const char* name; ID3D12Resource* res; ID3D12Resource* buf; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowBytes; };

// Render thread, Present hook, Unity's frame fully submitted. Every resource rests in COMMON (= PRESENT for the back
// buffer); one throwaway list on Unity's queue copies them into readback buffers, waits, and writes tight rows in
// MEMORY order (row 0 first) - the order every FG SDK indexes them in. Rare and slow on purpose (a diagnostic).
bool CaptureWrite(ID3D12Resource* backBuffer)
{
    const OwnedSet12* o = FgOwned12();
    DXGI_FORMAT hudFmt = DXGI_FORMAT_UNKNOWN;
    CapItem items[4] = { { "depth", FgDepth12(), NULL, {}, 0, 0 }, { "mv", FgMv12(), NULL, {}, 0, 0 },
                         { "hud", FgHudless12(&hudFmt), NULL, {}, 0, 0 }, { "bb", backBuffer, NULL, {}, 0, 0 } };
    if (!o || !H.device || !H.queue) return false;
    ID3D12CommandAllocator* alloc = NULL; ID3D12GraphicsCommandList* cl = NULL; ID3D12Fence* fence = NULL;
    bool ok = SUCCEEDED(H.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(H.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, IID_PPV_ARGS(&cl))) &&
              SUCCEEDED(H.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    for (int i = 0; ok && i < 4; ++i) {
        CapItem& it = items[i];
        if (!it.res) continue;
        D3D12_RESOURCE_DESC d = it.res->GetDesc();
        UINT64 total = 0;
        H.device->GetCopyableFootprints(&d, 0, 1, 0, &it.fp, &it.rows, &it.rowBytes, &total);
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(H.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&it.buf)))) { ok = false; break; }
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = it.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        dst.pResource = it.buf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = it.fp;
        OwnedSet12::Barrier(cl, it.res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
        OwnedSet12::Barrier(cl, it.res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    if (ok) {
        HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        ID3D12CommandList* lists[1] = { cl };
        ok = ev && SUCCEEDED(cl->Close());
        if (ok) {
            H.queue->ExecuteCommandLists(1, lists);
            ok = SUCCEEDED(H.queue->Signal(fence, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, ev)) && WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0;
        }
        if (ev) CloseHandle(ev);
    }
    for (int i = 0; ok && i < 4; ++i) {
        CapItem& it = items[i];
        if (!it.buf) { FgCaptureNote("%s=none", it.name); continue; }
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\fgcap.%S.bin", g_cap.dir, it.name);
        void* p = NULL;
        FILE* f = NULL;
        if (FAILED(it.buf->Map(0, NULL, &p)) || !p || _wfopen_s(&f, path, L"wb") || !f) { ok = false; if (p) it.buf->Unmap(0, NULL); break; }
        for (UINT y = 0; y < it.rows; ++y) fwrite((const char*)p + it.fp.Offset + (UINT64)y * it.fp.Footprint.RowPitch, 1, (size_t)it.rowBytes, f);
        fclose(f);
        it.buf->Unmap(0, NULL);
        FgCaptureNote("%s=%ux%u fmt %u rowBytes %llu", it.name, it.fp.Footprint.Width, it.fp.Footprint.Height, (unsigned)it.fp.Footprint.Format, (unsigned long long)it.rowBytes);
    }
    for (int i = 0; i < 4; ++i) SafeRelease(items[i].buf);
    SafeRelease(fence); SafeRelease(cl); SafeRelease(alloc);
    if (ok) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\fgcap.txt", g_cap.dir);
        FILE* f = NULL;
        if (_wfopen_s(&f, path, L"wb") || !f) ok = false;
        else { fwrite(g_cap.notes, 1, (size_t)g_cap.len, f); fclose(f); }
    }
    return ok;
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
    bool Prepare(ID3D12GraphicsCommandList*, const FgFrame&) { return true; }
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
    c.backFmt = s.desc.Format;

    IDXGISwapChain4* shadow = NULL;
    FgHookHide();                                          // the new chain's vtable shows dxgi, not us (Fg.h)
    int rc = p->Create(s, &shadow);
    FgHookShow();
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
    H.prepared = 0; H.idle = 0; H.hidden = 0; H.prepFails = 0; H.havePaired = 0; H.haveSent = 0; H.twinsOk = 0;              // a fresh child starts shown (FgWnd WM_RF_CREATE)
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

// The latest block also lands in the single slot (status line, presence checks; the Unity RTs are retained there until
// overwritten or Shutdown). The ring copy is plain data: nothing dereferences its resource pointers.
const FgFrame* FgHostSetFrame(const FgFrame& f)
{
    if (f.hudless) f.hudless->AddRef();
    if (f.depth)   f.depth->AddRef();
    if (f.mv)      f.mv->AddRef();
    ID3D12Resource *oh, *od, *om;
    FgFrame* r;
    {
        Locked lk;
        oh = H.slot.hudless; od = H.slot.depth; om = H.slot.mv;
        H.slot = f;
        r = &H.ring[H.ringIdx++ % kFrameRing];
        *r = f;
    }
    if (oh) oh->Release();
    if (od) od->Release();
    if (om) om->Release();
    return r;
}

// Render thread. D3D12 frame pairing (Fg.h FgFrame): the twins this event's list reads, and the back buffer of this
// frame's Present, hold what Unity rendered one frame EARLIER, so everything that describes the rendered frame comes
// from the previous event's block; the resources, sizes and the frame id stay this event's. No previous block (first
// frame of a chain, a skipped frame, a resized generation) = nothing describes the twins: reset.
FgFrame PairFrame(const FgFrame& ev, bool* paired)
{
    FgFrame d = ev;
    const FgFrame& p = H.paired;
    if (H.havePaired && p.frameId + 1 == ev.frameId && p.renderW == ev.renderW && p.renderH == ev.renderH && p.outW == ev.outW && p.outH == ev.outH) {
        d.jitterX = p.jitterX; d.jitterY = p.jitterY;
        d.cameraNear = p.cameraNear; d.cameraFar = p.cameraFar; d.cameraFovY = p.cameraFovY;
        d.dtMs = p.dtMs; d.reset = p.reset;
        memcpy(d.view, p.view, sizeof(d.view)); memcpy(d.proj, p.proj, sizeof(d.proj));
        memcpy(d.camPos, p.camPos, sizeof(d.camPos)); memcpy(d.camUp, p.camUp, sizeof(d.camUp));
        memcpy(d.camRight, p.camRight, sizeof(d.camRight)); memcpy(d.camFwd, p.camFwd, sizeof(d.camFwd));
        *paired = true;
    } else {
        d.reset = 1;
        *paired = false;
    }
    H.paired = ev; H.havePaired = 1;
    return d;
}

void FgHostPrepare(const FgFrame* ev)
{
    if (!Pin()) return;
    // A new frame supersedes whatever is still unpresented: an earlier success nobody presented, or a token a skipped
    // Present (GetBuffer/copy failed) left behind. Reset BEFORE the input checks, so a frame with a missing input or a
    // failing Prepare leaves prepared = 0 and no stale DLSS-G token that a later Present would pop for the wrong frame.
    H.prepared = 0;
    H.twinsOk = 0;
    H.c.prov->DropPrepared();
    // The pass-through provider has nothing to prepare. Nothing is declared to Unity: providers read the
    // shim-owned twins (COMMON at rest), never the Unity RTs - declaring those as NON_PIXEL_SHADER_RESOURCE made
    // Unity transition them under the upscaler's own barriers (debug layer id=527 on every frame).
    if (H.cur.hudless && H.cur.depth && H.cur.mv) {
        // The frame counts as prepared only once the provider's tags/constants are recorded AND the list reached the
        // queue: a failed Begin/Prepare/End leaves prepared = 0, so the next Present takes the idle path (Unity presents
        // its own frame) instead of feeding the generator the previous frame's data.
        int ok = 1;
        if (H.c.prov->Id() != FG_PROVIDER_NONE) {
            ok = 0;
            ID3D12GraphicsCommandList* l = H.c.prep.Begin();
            if (l) {
                // The row-flipped copies FIRST, on this list, so the provider's tags reference inputs in the back
                // buffer's orientation and format (FgTwins.h). Same list = same queue order, after the upscaler's.
                const OwnedSet12* o = FgOwned12();
                if (o && H.c.twins.Ensure(H.device, H.c.prep, *o, H.c.backFmt)) { H.c.twins.Run(l, *o, H.c.prep.ringIdx); H.twinsOk = 1; }
                bool paired = false;
                const FgFrame f = PairFrame(ev ? *ev : H.cur, &paired);
                if (FgCaptureArmed()) {
                    FgCaptureNote("provider=%s", H.c.prov->Name());
                    FgCaptureNote("frameId=%llu", f.frameId);
                    FgCaptureNote("pair=event %llu carries the constants of block %llu (paired %d, reset %d)", f.frameId, paired ? f.frameId - 1 : f.frameId, paired ? 1 : 0, f.reset);
                    FgCaptureNote("tagged=row-flipped copies (FgTwins.h) twins %d", H.twinsOk);
                    FgCaptureNote("render=%ux%u out=%ux%u", f.renderW, f.renderH, f.outW, f.outH);
                    FgCaptureNote("backFmt=%u", (unsigned)H.c.backFmt);
                    FgCaptureFloats("frame.view", f.view, 16);
                    FgCaptureFloats("frame.proj", f.proj, 16);
                    FgCaptureFloats("frame.jitter", &f.jitterX, 2);
                    FgCaptureFloats("frame.mvScale", &f.mvScaleX, 2);
                    FgCaptureFloats("frame.cam", f.camPos, 12);
                    if (H.haveSent && paired) {
                        FgCaptureFloats("prev.view", H.sent.view, 16);
                        FgCaptureFloats("prev.proj", H.sent.proj, 16);
                        FgCaptureFloats("prev.cam", H.sent.camPos, 12);
                    }
                }
                bool prepared = H.c.prov->Prepare(l, f);
                H.sent = f; H.haveSent = 1;
                bool submitted = H.c.prep.End(0);   // always End: an open list would wedge the ring's next Begin
                ok = prepared && submitted;
                if (FgCaptureArmed()) InterlockedExchange(&g_cap.state, ok ? 2 : -1);
                if (prepared && !submitted) H.c.prov->DropPrepared();   // no Present will consume it (DLSS-G token)
                if (!ok && H.prepFails++ < 8)
                    FgLog("host: prepare failed (provider=%d submit=%d) - frame not prepared", prepared ? 1 : 0, submitted ? 1 : 0);
            } else if (H.prepFails++ < 8) FgLog("host: prep list Begin failed (code %d) - frame not prepared", H.c.prep.failCode);
        }
        H.prepared = ok;
    }
    Unpin(NULL);
}

// Render thread, inside Prepare / the capture (chain pinned): the flipped copies FgHostPrepare just wrote on this list.
ID3D12Resource* FgHudless12(DXGI_FORMAT* fmt)
{
    const OwnedSet12* o = FgOwned12();
    const FgTwins& t = H.c.twins;
    if (H.twinsOk && t.tex[FgTwins::kHud]) { if (fmt) *fmt = t.hudFmt; return t.tex[FgTwins::kHud]; }
    if (fmt) *fmt = o ? o->outFmt : DXGI_FORMAT_UNKNOWN;
    return NULL;
}
ID3D12Resource* FgDepth12(void) { return H.twinsOk ? H.c.twins.tex[FgTwins::kDepth] : NULL; }
ID3D12Resource* FgMv12(void)    { return H.twinsOk ? H.c.twins.tex[FgTwins::kMv] : NULL; }

bool FgCaptureArmed(void) { return InterlockedCompareExchange(&g_cap.state, 1, 1) == 1; }

void FgCaptureNote(const char* fmt, ...)
{
    LONG s = InterlockedCompareExchange(&g_cap.state, 0, 0);
    if (s != 1 && s != 2) return;
    int room = (int)sizeof(g_cap.notes) - g_cap.len - 2;
    if (room <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(g_cap.notes + g_cap.len, (size_t)room, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(g_cap.notes + g_cap.len);
    g_cap.len += n;
    g_cap.notes[g_cap.len++] = '\n';
}

void FgCaptureFloats(const char* key, const float* v, int n)
{
    char line[512];
    int at = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s=", key);
    for (int i = 0; i < n && at > 0 && at < (int)sizeof(line) - 20; ++i)
        at += _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, i ? " %.9g" : "%.9g", v[i]);
    FgCaptureNote("%s", line);
}

int FgHostCapture(const wchar_t* dir)
{
    if (!dir || !FgHostAlive()) return 0;
    LONG s = InterlockedCompareExchange(&g_cap.state, 0, 0);
    if (s == 1 || s == 2) return 0;
    wcsncpy_s(g_cap.dir, MAX_PATH, dir, _TRUNCATE);
    g_cap.len = 0;
    InterlockedExchange(&g_cap.state, 1);
    return 1;
}

int FgHostCaptureStatus(void) { return (int)InterlockedCompareExchange(&g_cap.state, 0, 0); }

bool FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags)
{
    if (!Pin()) return false;
    Chain& c = H.c;
    if (app != (IDXGISwapChain*)c.app || !c.shadow) { Unpin(NULL); return false; }

    // No frame prepared since the last present: the provider is not driven at all this frame (no copy, no shadow
    // Present, no vendor call), the hook forwards Unity's Present unchanged, and the child hides on the first such frame.
    if (!H.prepared) {
        if (++H.idle == 1) { H.hidden = 1; FgWndShow(false); FgLog("host: idle: no prepared frame - child hidden, provider untouched, Unity presents on its own"); }
        Unpin(NULL);
        return false;
    }
    H.prepared = 0;
    if (H.hidden) { H.hidden = 0; FgWndShow(true); FgLog("host: frames resumed after %lld idle presents - child shown", H.idle); }
    H.idle = 0;

    // Only the present flags the shadow chain opted into: tearing iff created with ALLOW_TEARING (and
    // Unity asked for it this frame); never TEST / DO_NOT_SEQUENCE / RESTART, which belong to Unity's chain.
    UINT pf = (c.scFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? (flags & DXGI_PRESENT_ALLOW_TEARING) : 0;
    if (pf && syncInterval) pf = 0;                    // tearing needs sync interval 0

    ID3D12Resource* src = NULL;
    if (FAILED(c.app->GetBuffer(c.app->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&src)) || !src) { Unpin(NULL); return false; }

    if (InterlockedCompareExchange(&g_cap.state, 2, 2) == 2) InterlockedExchange(&g_cap.state, CaptureWrite(src) ? 3 : -1);

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
    return DestroyDetached(0) ? 1 : 0;
}

int FgHostShutdown(void)
{
    Detach("shutdown");
    int ok = DestroyDetached(0) ? 1 : 0;
    Locked lk;
    H.enabled = 0;
    if (ok) ReleaseSlotLocked();
    return ok;
}

int FgHostRetired(void) { Locked lk; return H.state == kNone && !H.pinned ? 1 : 0; }
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
        "provider=%s enabled=%d multiplier=%u chain=%s child=%p childHr=0x%08X hit=%d focus=%s fg=%d shadow=%p out=%ux%u flags=0x%X caps=0x%X lastError=%d presentHr=0x%08X presented=%lld fps=%d frameId=%llu idle=%lld%s%s",
        prov, H.enabled, H.multiplier,
        state == kNone ? "-" : "child", child, (unsigned)H.childHr, w->hit, focus,
        w->foreground == game ? 1 : 0,
        shadow, H.outW, H.outH, scFlags,
        caps, H.lastError, (unsigned)H.lastPresentHr, FgPresentCount(), FgPresentedFps(),
        frameId, state == kLive ? H.idle : 0LL, H.reason ? " reason=" : "", H.reason ? H.reason : "");
    return H.status;
}
