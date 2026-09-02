// FgHook.cpp - the DXGI vtable patch. All swapchains produced by one DXGI runtime share one vtable, so
// patching the vtable read off a throwaway 8x8 swapchain of our own also patches Unity's. The hook
// recognises the application swapchain as the first `this` whose OutputWindow is not our dummy window.
//
// Vtable indices (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, IDXGISwapChain 8-17,
// IDXGISwapChain1 18-...): Present = 8, SetFullscreenState = 10, ResizeBuffers = 13, Present1 = 22. Verified
// against dxgi.h declaration order; asserted at runtime by comparing the saved original against the vtable slot.
//
// Present contract: the hook is the ONLY place that presents Unity's chain, exactly once per hooked call. The host
// (FgHostOnPresent) only presents the shadow chain and says whether it did; then Unity's chain goes out with sync 0
// (hidden under ours, it just has to keep its back-buffer index rotating). TEST / DO_NOT_SEQUENCE presents are
// not frames: straight through, untouched, uncounted.
#include "Fg.h"

#include <stdio.h>
#include <stdarg.h>

namespace {

typedef HRESULT (STDMETHODCALLTYPE *PfnPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PfnPresent1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PfnResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PfnSetFullscreen)(IDXGISwapChain*, BOOL, IDXGIOutput*);

const int kVtPresent = 8, kVtSetFullscreen = 10, kVtResizeBuffers = 13, kVtPresent1 = 22;
const UINT kPassThrough = DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE;

PfnPresent       g_origPresent = NULL;
PfnPresent1      g_origPresent1 = NULL;
PfnResizeBuffers g_origResize = NULL;
PfnSetFullscreen g_origSetFullscreen = NULL;

// Observed, not owned: only ever compared against `this`, never dereferenced outside NoteApp (where it IS `this`).
// The host takes its own AddRef on it for as long as a chain is built on it (FgHost.cpp Chain::app).
IDXGISwapChain3* g_app = NULL;
HWND             g_appHwnd = NULL;
DXGI_SWAP_CHAIN_DESC1 g_appDesc = {};
int              g_appDescValid = 0;

HWND             g_dummyHwnd = NULL;
IDXGISwapChain1* g_dummy = NULL;
ID3D12CommandQueue* g_queue = NULL;
int              g_installed = 0;

FgSpike          g_spike = {};
int              g_spikeDone = 0;

// Presented-frame counter (0.5 s window).
LARGE_INTEGER    g_freq = {};
LARGE_INTEGER    g_windowStart = {};
long long        g_windowCount = 0;
long long        g_total = 0;
volatile long    g_fps = 0;

FILE*            g_log = NULL;

void CountPresent()
{
    if (g_freq.QuadPart == 0) { QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_windowStart); }
    ++g_total; ++g_windowCount;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - g_windowStart.QuadPart) / double(g_freq.QuadPart);
    if (dt >= 0.5) {
        g_fps = (long)(double(g_windowCount) / dt + 0.5);
        g_windowStart = now; g_windowCount = 0;
    }
}

// Remember the application's swapchain the first time a foreign `this` presents. Our own chains (dummy, child)
// are excluded by their window.
void NoteApp(IDXGISwapChain* sc)
{
    if (g_app) return;
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || d.OutputWindow == NULL || d.OutputWindow == g_dummyHwnd || d.OutputWindow == FgWndChild()) return;
    IDXGISwapChain3* sc3 = NULL;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
        FgLog("hook: app swapchain is not IDXGISwapChain3 - FG needs it, giving up on discovery");
        return;
    }
    sc3->Release();                 // observation only (see g_app); the host AddRefs while it builds on it
    g_app = sc3;
    g_appHwnd = d.OutputWindow;
    IDXGISwapChain1* sc1 = NULL;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
        if (SUCCEEDED(sc1->GetDesc1(&g_appDesc))) g_appDescValid = 1;
        sc1->Release();
    }
    g_spike.sawPresent = 1;
    g_spike.format = (unsigned)d.BufferDesc.Format;
    g_spike.bufferCount = d.BufferCount;
    g_spike.swapEffect = (unsigned)d.SwapEffect;
    g_spike.scFlags = d.Flags;
    g_spike.width = d.BufferDesc.Width;
    g_spike.height = d.BufferDesc.Height;
    g_spike.windowed = d.Windowed ? 1 : 0;
    g_spike.flipModel = (d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) ? 1 : 0;
    g_spike.waitable = (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ? 1 : 0;
    FgLog("hook: app swapchain %p hwnd %p %ux%u fmt %u buffers %u swapEffect %u flags 0x%X windowed %d flip %d waitable %d",
          (void*)sc, (void*)g_appHwnd, g_spike.width, g_spike.height, g_spike.format,
          g_spike.bufferCount, g_spike.swapEffect, g_spike.scFlags, g_spike.windowed,
          g_spike.flipModel, g_spike.waitable);
}

// Unity's Present flags once the shadow chain carried the frame: sync 0, only ALLOW_TEARING / RESTART kept.
UINT UnityFlags(UINT flags) { return flags & (DXGI_PRESENT_ALLOW_TEARING | DXGI_PRESENT_RESTART); }

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync, UINT flags)
{
    if (flags & kPassThrough) return g_origPresent(self, sync, flags);
    NoteApp(self);
    if (self != (IDXGISwapChain*)g_app) return g_origPresent(self, sync, flags);
    bool fg = FgHostOnPresent(self, sync, flags);
    HRESULT hr = fg ? g_origPresent(self, 0, UnityFlags(flags)) : g_origPresent(self, sync, flags);
    g_spike.forwardedPresentHr = (long)hr;
    if (fg) FgHostAfterUnityPresent(hr); else CountPresent();
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    if (flags & kPassThrough) return g_origPresent1(self, sync, flags, pp);
    NoteApp(self);
    if ((IDXGISwapChain*)self != (IDXGISwapChain*)g_app) return g_origPresent1(self, sync, flags, pp);
    bool fg = FgHostOnPresent((IDXGISwapChain*)self, sync, flags);
    HRESULT hr = fg ? g_origPresent1(self, 0, UnityFlags(flags), pp) : g_origPresent1(self, sync, flags, pp);
    g_spike.forwardedPresentHr = (long)hr;
    if (fg) FgHostAfterUnityPresent(hr); else CountPresent();
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    if (self == (IDXGISwapChain*)g_app) {
        FgLog("hook: app ResizeBuffers %ux%u count %u fmt %d flags 0x%X", w, h, count, (int)fmt, flags);
        FgHostOnResize(w, h);
        g_appDescValid = 0;
    }
    return g_origResize(self, count, w, h, fmt, flags);
}

// Exclusive fullscreen on Unity's chain: our child window / shadow chain cannot live over it, tear down first
// (the managed driver retries Init, which refuses while GetFullscreenState says fullscreen). Never our own chains.
HRESULT STDMETHODCALLTYPE HookSetFullscreenState(IDXGISwapChain* self, BOOL fullscreen, IDXGIOutput* target)
{
    if (self == (IDXGISwapChain*)g_app) {
        FgLog("hook: app SetFullscreenState(%d)", (int)fullscreen);
        if (fullscreen) FgHostTearDown("SetFullscreenState(TRUE)");
    }
    return g_origSetFullscreen(self, fullscreen, target);
}

// Writes `fn` into vt[idx], returning the previous entry in *outOrig. False (slot untouched) on VirtualProtect failure.
bool PatchSlot(void** vt, int idx, void* fn, void** outOrig)
{
    DWORD old = 0;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *outOrig = vt[idx];
    vt[idx] = fn;
    DWORD tmp = 0;
    VirtualProtect(&vt[idx], sizeof(void*), old, &tmp);
    return true;
}

struct Slot { int idx; void* fn; void** orig; };
Slot g_slots[] = {
    { kVtPresent,       (void*)&HookPresent,            (void**)&g_origPresent },
    { kVtResizeBuffers, (void*)&HookResizeBuffers,      (void**)&g_origResize },
    { kVtPresent1,      (void*)&HookPresent1,           (void**)&g_origPresent1 },
    { kVtSetFullscreen, (void*)&HookSetFullscreenState, (void**)&g_origSetFullscreen },
};
const int kSlots = sizeof(g_slots) / sizeof(g_slots[0]);

// Restore the first `n` patched slots (a partial install must never leave the vtable half ours).
void UnpatchSlots(void** vt, int n)
{
    for (int i = n - 1; i >= 0; --i) {
        void* tmp = NULL;
        if (*g_slots[i].orig) PatchSlot(vt, g_slots[i].idx, *g_slots[i].orig, &tmp);
        *g_slots[i].orig = NULL;
    }
}

// An 8x8 off-screen window that never becomes visible; the throwaway swapchain hangs off it.
HWND MakeDummyWindow()
{
    static const wchar_t kClass[] = L"RenderforgeFgDummy";
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);          // duplicate registration is harmless, we ignore the result
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, kClass, WS_POPUP,
                           0, 0, 8, 8, NULL, NULL, inst, NULL);
}

} // namespace

// ---------------------------------------------------------------- log

void FgLogInit(const wchar_t* logDir)
{
    if (g_log || !logDir) return;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\renderforge_fg.log", logDir);
    _wfopen_s(&g_log, path, L"w");
}

void FgLog(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- install

bool FgHookInstall(ID3D12CommandQueue* queue)
{
    if (g_installed) return true;
    if (!queue) { FgLog("hook: no command queue"); return false; }
    g_queue = queue;

    g_dummyHwnd = MakeDummyWindow();
    if (!g_dummyHwnd) { FgLog("hook: dummy window failed (%lu)", GetLastError()); return false; }

    IDXGIFactory2* factory = NULL;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&factory);
    if (FAILED(hr)) { FgLog("hook: CreateDXGIFactory2 0x%08X", (unsigned)hr); return false; }

    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = 8; d.Height = 8;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(queue, g_dummyHwnd, &d, NULL, NULL, &g_dummy);
    factory->Release();
    if (FAILED(hr) || !g_dummy) { FgLog("hook: dummy swapchain 0x%08X", (unsigned)hr); return false; }

    void** vt = *(void***)g_dummy;
    for (int i = 0; i < kSlots; ++i) {
        if (PatchSlot(vt, g_slots[i].idx, g_slots[i].fn, g_slots[i].orig)) continue;
        FgLog("hook: VirtualProtect failed on slot %d - rolling back %d patched", g_slots[i].idx, i);
        UnpatchSlots(vt, i);
        g_dummy->Release(); g_dummy = NULL;
        DestroyWindow(g_dummyHwnd); g_dummyHwnd = NULL;
        return false;
    }

    g_installed = 1;
    g_spike.installed = 1;
    FgLog("hook: installed (vtable %p, Present %p, Present1 %p, ResizeBuffers %p, SetFullscreenState %p)",
          (void*)vt, (void*)g_origPresent, (void*)g_origPresent1, (void*)g_origResize, (void*)g_origSetFullscreen);
    return true;
}

void FgHookRemove(void)
{
    if (!g_installed) return;
    void** vt = *(void***)g_dummy;
    UnpatchSlots(vt, kSlots);
    g_dummy->Release(); g_dummy = NULL;
    DestroyWindow(g_dummyHwnd); g_dummyHwnd = NULL;
    g_installed = 0; g_app = NULL; g_appHwnd = NULL; g_appDescValid = 0;
    FgLog("hook: removed");
}

void FgHookForgetApp(void)
{
    if (!g_app) return;
    FgLog("hook: app swapchain %p on hwnd %p forgotten", (void*)g_app, (void*)g_appHwnd);
    g_app = NULL; g_appHwnd = NULL; g_appDescValid = 0;
}

IDXGISwapChain3* FgAppSwapChain(void) { return g_app; }
HWND FgAppHwnd(void) { return g_appHwnd; }
const DXGI_SWAP_CHAIN_DESC1* FgAppDesc(void) { return g_appDescValid ? &g_appDesc : NULL; }

void FgPresentedAdd(int n)
{
    for (int i = 0; i < n; ++i) CountPresent();
}

int FgPresentedFps(void) { return (int)g_fps; }
long long FgPresentCount(void) { return g_total; }
const FgSpike* FgSpikeResult(void) { return &g_spike; }

// The two probes decision 3 and decision 4a hang on. Runs once; safe to call every frame.
void FgHookSpike(void)
{
    if (g_spikeDone || !g_app || !g_queue) return;
    g_spikeDone = 1;

    IDXGIFactory2* factory = NULL;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&factory);
    if (FAILED(hr)) { FgLog("spike: CreateDXGIFactory2 0x%08X", (unsigned)hr); return; }

    DXGI_SWAP_CHAIN_DESC1 d = {};
    const DXGI_SWAP_CHAIN_DESC1* app = FgAppDesc();
    if (app) d = *app; else {
        d.Width = g_spike.width; d.Height = g_spike.height;
        d.Format = (DXGI_FORMAT)g_spike.format; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 3;
    }
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;      // FG needs the flip model regardless of Unity's choice
    d.Flags = 0;
    if (d.BufferCount < 2) d.BufferCount = 3;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    // Probe 1 (decision 3): a second swapchain on the game's own HWND.
    IDXGISwapChain1* second = NULL;
    hr = factory->CreateSwapChainForHwnd(g_queue, g_appHwnd, &d, NULL, NULL, &second);
    g_spike.secondSwapChainHr = (long)hr;
    FgLog("spike: second CreateSwapChainForHwnd on game hwnd -> 0x%08X", (unsigned)hr);
    if (second) { second->Release(); second = NULL; }

    // Probe 2 (decision 4a): a composition swapchain, which never claims the HWND.
    DXGI_SWAP_CHAIN_DESC1 c = d;
    c.Scaling = DXGI_SCALING_STRETCH;
    c.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    IDXGISwapChain1* comp = NULL;
    hr = factory->CreateSwapChainForComposition(g_queue, &c, NULL, &comp);
    g_spike.compositionHr = (long)hr;
    FgLog("spike: CreateSwapChainForComposition -> 0x%08X", (unsigned)hr);
    if (comp) comp->Release();

    factory->Release();
}
