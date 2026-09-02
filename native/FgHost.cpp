// FgHost.cpp - the presentation host. Owns the FG-owned "shadow" swapchain on the game's HWND, copies
// Unity's finished backbuffer into it every frame, drives one IFgProvider and presents. Unity's own
// swapchain is never presented again while FG is on; Unity keeps rendering into its buffer 0, which is
// exactly what it does anyway because our interception freezes its back-buffer index.
//
// Threading: Init/Shutdown on the main thread with the render thread idle; SetFrame on the main thread
// into a 4-deep ring; Prepare and OnPresent on the render thread.
//
// Two D3D12Ring instances (D3D12Ring.h, the ring every D3D12 backend uses): `prep` submits through
// IUnityGraphicsD3D12v5::ExecuteCommandList with the resource states Unity must know about (its state
// arrays outlive the call, which a stack array would not); `copy` submits straight on Unity's queue inside
// the Present hook, where the frame is already complete and Unity's fence does not apply.
#include "Fg.h"
#include "RenderforgeNative.h"
#include "D3D12Ring.h"

#include <stdio.h>

namespace {

struct Host
{
    IFgProvider*        prov;
    IDXGISwapChain4*    shadow;
    IDXGIFactory2*      factory;
    ID3D12Device*       device;
    ID3D12CommandQueue* queue;
    D3D12Ring           prep;         // DLSS_EV_FG_PREPARE, via Unity
    D3D12Ring           copy;         // backbuffer copy, direct
    unsigned            multiplier;
    int                 enabled;
    int                 lastError;
    unsigned            outW, outH;
    // per-frame ring, main thread writes, render thread reads
    FgFrame             frames[4];
    volatile long       frameIdx;
    FgFrame             cur;          // render-thread copy
    char                status[512];
};

Host H;

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
    if (H.shadow)  { H.shadow->Release();  H.shadow = NULL; }
    if (H.factory) { H.factory->Release(); H.factory = NULL; }
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
        IDXGISwapChain1* sc1 = NULL;
        DXGI_SWAP_CHAIN_DESC1 d = s.desc;
        HRESULT hr = s.factory->CreateSwapChainForHwnd(s.queue, s.hwnd, &d, NULL, NULL, &sc1);
        if (FAILED(hr) || !sc1) { FgLog("none: CreateSwapChainForHwnd 0x%08X", (unsigned)hr); return FG_ERR_NO_SWAPCHAIN; }
        hr = sc1->QueryInterface(__uuidof(IDXGISwapChain4), (void**)out);
        sc1->Release();
        if (FAILED(hr)) return FG_ERR_NO_SWAPCHAIN;
        sc = *out;
        return FG_OK;
    }
    void Prepare(ID3D12GraphicsCommandList*, const FgFrame&) {}
    void BeforePresent(const FgFrame&) {}
    int  AfterPresent(void) { return 1; }
    void SetEnabled(bool) {}
    void Destroy(void) { sc = NULL; }
};

ProviderNone g_none;

} // namespace

IFgProvider* MakeFgProviderNone(void) { return &g_none; }
// Vendor providers land in Tasks 3-5; until then every id falls back to the pass-through chain.
IFgProvider* MakeFgProviderFsr(void) { return NULL; }
IFgProvider* MakeFgProviderXess(void) { return NULL; }
IFgProvider* MakeFgProviderStreamline(void) { return NULL; }

// ---------------------------------------------------------------- host

int FgHostInit(int provider, unsigned multiplier, const wchar_t* dllDir)
{
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
    s.desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    s.desc.Flags = 0;                                      // no waitable object: we drive the pacing
    if (s.desc.BufferCount < 3) s.desc.BufferCount = 3;
    H.outW = s.desc.Width; H.outH = s.desc.Height;
    H.multiplier = s.multiplier;

    IDXGISwapChain4* shadow = NULL;
    int rc = p->Create(s, &shadow);
    if (rc != FG_OK || !shadow) { H.factory->Release(); H.factory = NULL; H.lastError = rc; return rc; }
    H.shadow = shadow;
    H.prov = p;
    H.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    H.prep.Attach(H.device);
    H.copy.Attach(H.device);
    H.lastError = FG_OK;
    FgLog("host: provider=%s multiplier=%u shadow=%p %ux%u caps=0x%X",
          p->Name(), H.multiplier, (void*)H.shadow, H.outW, H.outH, p->Caps());
    return FG_OK;
}

void FgHostSetEnabled(int on)
{
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
    if (!H.prov || !H.enabled) return;
    H.cur = H.frames[H.frameIdx & 3];
    if (!H.cur.hudless || !H.cur.depth || !H.cur.mv) return;

    ID3D12GraphicsCommandList* l = H.prep.Begin();
    if (!l) return;
    H.prov->Prepare(l, H.cur);

    // Unity owns the states of hudless/depth/mv. We ask for the state every FG SDK reads them in and
    // hand them back unchanged, exactly like Phase 2's DLSS evaluate.
    UnityGraphicsD3D12ResourceState* st = H.prep.StateSlot();
    st[0].resource = H.cur.hudless; st[0].expected = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; st[0].current = st[0].expected;
    st[1].resource = H.cur.depth;   st[1].expected = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; st[1].current = st[1].expected;
    st[2].resource = H.cur.mv;      st[2].expected = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; st[2].current = st[2].expected;
    H.prep.End(3);
}

bool FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags, HRESULT* outHr)
{
    if (!H.prov || !H.enabled || !H.shadow) return false;
    IDXGISwapChain3* app3 = FgAppSwapChain();
    if (app != (IDXGISwapChain*)app3) return false;

    ID3D12Resource* src = NULL;
    ID3D12Resource* dst = NULL;
    if (FAILED(app3->GetBuffer(app3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&src)) ||
        FAILED(H.shadow->GetBuffer(H.shadow->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&dst)))
    {
        if (src) src->Release();
        if (dst) dst->Release();
        return false;
    }
    bool copied = CopyBackBuffer(src, dst);
    src->Release(); dst->Release();
    if (!copied) return false;

    H.prov->BeforePresent(H.cur);
    HRESULT hr = H.shadow->Present(syncInterval, flags);
    int presented = H.prov->AfterPresent();
    FgPresentedAdd(presented < 1 ? 1 : presented);
    *outHr = hr;
    return true;
}

void FgHostOnResize(unsigned w, unsigned h)
{
    if (!H.prov) return;
    FgLog("host: resize %ux%u - tearing the FG chain down, the driver rebuilds it", w, h);
    IFgProvider* p = H.prov;
    H.enabled = 0;
    p->SetEnabled(false);
    ReleaseGpu();
    p->Destroy();
    H.prov = NULL;
}

void FgHostShutdown(void)
{
    if (H.prov) { H.prov->SetEnabled(false); }
    ReleaseGpu();
    if (H.prov) { H.prov->Destroy(); H.prov = NULL; }
    H.enabled = 0;
    FgLog("host: shutdown");
}

unsigned FgHostCaps(void) { return H.prov ? H.prov->Caps() : 0u; }
int FgHostProvider(void) { return H.prov ? H.prov->Id() : FG_PROVIDER_NONE; }

const char* FgHostStatus(void)
{
    _snprintf_s(H.status, sizeof(H.status), _TRUNCATE,
        "provider=%s enabled=%d multiplier=%u shadow=%p out=%ux%u caps=0x%X lastError=%d presented=%lld fps=%d frameId=%llu",
        H.prov ? H.prov->Name() : "-", H.enabled, H.multiplier, (void*)H.shadow, H.outW, H.outH,
        FgHostCaps(), H.lastError, FgPresentCount(), FgPresentedFps(),
        (unsigned long long)H.frames[H.frameIdx & 3].frameId);
    return H.status;
}
