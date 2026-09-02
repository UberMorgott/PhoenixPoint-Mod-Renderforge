// FgHook.cpp - the DXGI vtable patch. All swapchains produced by one DXGI runtime share one vtable, so
// patching the vtable read off a throwaway 8x8 swapchain of our own also patches Unity's. The hook
// recognises the application swapchain as the first `this` that presents on a top-level window of this process
// that is not one of ours (dummy / child class) on Unity's own device - overlays, other devices and the vendor
// pacers' presents of OUR proxy chains never qualify.
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
#include <share.h>

namespace {

typedef HRESULT (STDMETHODCALLTYPE *PfnPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PfnPresent1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *PfnResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PfnSetFullscreen)(IDXGISwapChain*, BOOL, IDXGIOutput*);

const int kVtPresent = 8, kVtSetFullscreen = 10, kVtResizeBuffers = 13, kVtPresent1 = 22;
const UINT kPassThrough = DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE;
const wchar_t kDummyClass[] = L"RenderforgeFgDummy";

PfnPresent       g_origPresent = NULL;
PfnPresent1      g_origPresent1 = NULL;
PfnResizeBuffers g_origResize = NULL;
PfnSetFullscreen g_origSetFullscreen = NULL;

// The app chain: OWNED (AddRef'd) while latched, published under g_appLock. Holding the reference is what makes
// the pointer compare in the hooks safe: the object cannot die and its address cannot be reused while latched.
SRWLOCK          g_appLock = SRWLOCK_INIT;
IDXGISwapChain3* g_app = NULL;
HWND             g_appHwnd = NULL;
DXGI_SWAP_CHAIN_DESC1 g_appDesc = {};
int              g_appGaveUp = 0;      // the first candidate was not IDXGISwapChain3: stop trying

HWND             g_dummyHwnd = NULL;
IDXGISwapChain1* g_dummy = NULL;
ID3D12CommandQueue* g_queue = NULL;
ID3D12Device*    g_device = NULL;      // g_queue's device, for the identity check (not owned)
int              g_installed = 0;
volatile LONG    g_inHook = 0;         // hooks in flight; FgHookRemove drains it

// Presented-frame counter (0.5 s window).
LARGE_INTEGER    g_freq = {};
LARGE_INTEGER    g_windowStart = {};
long long        g_windowCount = 0;
long long        g_total = 0;
volatile long    g_fps = 0;

FILE*            g_log = NULL;

struct AppLocked
{
    AppLocked()  { AcquireSRWLockExclusive(&g_appLock); }
    ~AppLocked() { ReleaseSRWLockExclusive(&g_appLock); }
};

__declspec(thread) int t_depth = 0;    // per-thread nesting of our Present hooks (evidence + re-entrancy guard)
volatile LONG    g_reentryLogged = 0;

struct Entered
{
    Entered()  { InterlockedIncrement(&g_inHook); ++t_depth; }
    ~Entered() { --t_depth; InterlockedDecrement(&g_inHook); }
};

// True when this thread is already inside one of our Present hooks (the vendor pacer / proxy presenting the real child
// chain from within the shadow Present the host issued): logged the first few times, then straight passthrough.
bool Reentered(IDXGISwapChain* self)
{
    if (t_depth <= 1) return false;
    if (InterlockedIncrement(&g_reentryLogged) <= 16)
        FgLog("hook: re-entered Present depth %d on chain %p tid %u - passthrough", t_depth, (void*)self, (unsigned)GetCurrentThreadId());
    return true;
}

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

bool IsOurWindow(HWND h)
{
    wchar_t cls[64] = {};
    if (!GetClassNameW(h, cls, 64)) return false;
    return wcscmp(cls, kDummyClass) == 0 || FgWndIsOurs(h);
}

// The chain a foreign `this` must be before it is latched as the app chain.
bool Validate(IDXGISwapChain* sc, DXGI_SWAP_CHAIN_DESC* d)
{
    if (FAILED(sc->GetDesc(d)) || !d->OutputWindow || !IsWindow(d->OutputWindow)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(d->OutputWindow, &pid);
    if (pid != GetCurrentProcessId() || GetAncestor(d->OutputWindow, GA_ROOT) != d->OutputWindow) return false;
    if (IsOurWindow(d->OutputWindow)) return false;
    if (!d->BufferDesc.Width || !d->BufferDesc.Height || d->BufferDesc.Format == DXGI_FORMAT_UNKNOWN) return false;
    ID3D12Device* dev = NULL;
    if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return false;
    bool same = dev == g_device;
    dev->Release();
    return same;
}

// Remember the application's swapchain the first time a valid foreign `this` presents.
void NoteApp(IDXGISwapChain* sc)
{
    { AppLocked lk; if (g_app || g_appGaveUp) return; }
    DXGI_SWAP_CHAIN_DESC d = {};
    if (!Validate(sc, &d)) return;
    IDXGISwapChain3* sc3 = NULL;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) || !sc3) {
        FgLog("hook: app swapchain is not IDXGISwapChain3 - FG needs it, giving up on discovery");
        g_appGaveUp = 1;
        return;
    }
    DXGI_SWAP_CHAIN_DESC1 d1 = {};
    if (FAILED(sc3->GetDesc1(&d1))) {
        d1.Width = d.BufferDesc.Width; d1.Height = d.BufferDesc.Height; d1.Format = d.BufferDesc.Format;
        d1.BufferCount = d.BufferCount; d1.SampleDesc = d.SampleDesc; d1.BufferUsage = d.BufferUsage;
        d1.SwapEffect = d.SwapEffect; d1.Flags = d.Flags;
    }
    {
        AppLocked lk;
        if (g_app) { sc3->Release(); return; }         // another thread won
        g_app = sc3;                                   // keeps the reference
        g_appHwnd = d.OutputWindow;
        g_appDesc = d1;
    }
    FgLog("hook: app swapchain %p hwnd %p %ux%u fmt %u buffers %u swapEffect %u flags 0x%X windowed %d",
          (void*)sc, (void*)d.OutputWindow, d.BufferDesc.Width, d.BufferDesc.Height, (unsigned)d.BufferDesc.Format,
          d.BufferCount, (unsigned)d.SwapEffect, d.Flags, d.Windowed ? 1 : 0);
}

// The app chain, by pointer AND by window: a mismatch means the latch is stale - forget it, rediscover.
bool IsApp(IDXGISwapChain* self)
{
    if (!FgAppIs(self)) return false;
    DXGI_SWAP_CHAIN_DESC d = {};
    HWND hwnd; { AppLocked lk; hwnd = g_appHwnd; }
    if (SUCCEEDED(self->GetDesc(&d)) && d.OutputWindow == hwnd) return true;
    FgLog("hook: app swapchain %p now on hwnd %p (latched %p) - forgetting", (void*)self, (void*)d.OutputWindow, (void*)hwnd);
    FgHookForgetApp();
    return false;
}

// Unity's Present flags once the shadow chain carried the frame: sync 0, only ALLOW_TEARING / RESTART kept.
UINT UnityFlags(UINT flags) { return flags & (DXGI_PRESENT_ALLOW_TEARING | DXGI_PRESENT_RESTART); }

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync, UINT flags)
{
    Entered e;
    if ((flags & kPassThrough) || Reentered(self)) return g_origPresent(self, sync, flags);
    NoteApp(self);
    if (!IsApp(self)) return g_origPresent(self, sync, flags);
    bool fg = FgHostOnPresent(self, sync, flags);
    HRESULT hr = fg ? g_origPresent(self, 0, UnityFlags(flags)) : g_origPresent(self, sync, flags);
    if (fg) FgHostAfterUnityPresent(hr); else CountPresent();
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    Entered e;
    if ((flags & kPassThrough) || Reentered((IDXGISwapChain*)self)) return g_origPresent1(self, sync, flags, pp);
    NoteApp(self);
    if (!IsApp(self)) return g_origPresent1(self, sync, flags, pp);
    bool fg = FgHostOnPresent((IDXGISwapChain*)self, sync, flags);
    HRESULT hr = fg ? g_origPresent1(self, 0, UnityFlags(flags), pp) : g_origPresent1(self, sync, flags, pp);
    if (fg) FgHostAfterUnityPresent(hr); else CountPresent();
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    Entered e;
    if (FgAppIs(self)) {
        FgLog("hook: app ResizeBuffers %ux%u count %u fmt %d flags 0x%X", w, h, count, (int)fmt, flags);
        FgHostOnResize(w, h);
        AppLocked lk;
        if (w) g_appDesc.Width = w;
        if (h) g_appDesc.Height = h;
        if (fmt != DXGI_FORMAT_UNKNOWN) g_appDesc.Format = fmt;
        if (count) g_appDesc.BufferCount = count;
        g_appDesc.Flags = flags;
    }
    return g_origResize(self, count, w, h, fmt, flags);
}

// Exclusive fullscreen on Unity's chain: our child window / shadow chain cannot live over it, tear down first
// (the managed driver retries Init, which refuses while GetFullscreenState says fullscreen). Never our own chains.
HRESULT STDMETHODCALLTYPE HookSetFullscreenState(IDXGISwapChain* self, BOOL fullscreen, IDXGIOutput* target)
{
    Entered e;
    if (FgAppIs(self)) {
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

// Restore the first `n` patched slots - only those that still hold OUR function (someone hooking on top of us
// keeps their chain; they forward to our saved original through their own copy of the slot).
void UnpatchSlots(void** vt, int n)
{
    for (int i = n - 1; i >= 0; --i) {
        void* orig = *g_slots[i].orig;
        if (orig) {
            DWORD old = 0;
            if (VirtualProtect(&vt[g_slots[i].idx], sizeof(void*), PAGE_READWRITE, &old)) {
                void* was = InterlockedCompareExchangePointer(&vt[g_slots[i].idx], orig, g_slots[i].fn);
                if (was != g_slots[i].fn) FgLog("hook: slot %d is %p, not ours - left alone", g_slots[i].idx, was);
                DWORD tmp = 0;
                VirtualProtect(&vt[g_slots[i].idx], sizeof(void*), old, &tmp);
            }
        }
        *g_slots[i].orig = NULL;
    }
}

// An 8x8 off-screen window that never becomes visible; the throwaway swapchain hangs off it.
HWND MakeDummyWindow()
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kDummyClass;
    RegisterClassExW(&wc);          // duplicate registration is harmless, we ignore the result
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kDummyClass, kDummyClass, WS_POPUP,
                           0, 0, 8, 8, NULL, NULL, inst, NULL);
}

} // namespace

// ---------------------------------------------------------------- log

void FgLogInit(const wchar_t* logDir)
{
    if (g_log || !logDir) return;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\renderforge_fg.log", logDir);
    g_log = _wfsopen(path, L"w", _SH_DENYNO);   // readable while the game holds it open (same as D3D12Debug.cpp)
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
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), (void**)&g_device)) || !g_device) { FgLog("hook: queue has no device"); return false; }
    g_device->Release();            // Unity's device outlives the hook; compared, never dereferenced

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
    FgLog("hook: installed (vtable %p, Present %p, Present1 %p, ResizeBuffers %p, SetFullscreenState %p)",
          (void*)vt, (void*)g_origPresent, (void*)g_origPresent1, (void*)g_origResize, (void*)g_origSetFullscreen);
    return true;
}

void FgHookRemove(void)
{
    if (!g_installed) return;
    void** vt = *(void***)g_dummy;
    UnpatchSlots(vt, kSlots);
    // A hook that entered before the slot was restored may still be running: let it leave before anything it
    // uses goes away (bounded: a Present blocked on vsync returns within a frame).
    for (int i = 0; i < 2000 && g_inHook > 0; ++i) Sleep(1);
    if (g_inHook > 0) FgLog("hook: %ld hook(s) still in flight after 2 s", (long)g_inHook);
    FgHookForgetApp();
    g_dummy->Release(); g_dummy = NULL;
    DestroyWindow(g_dummyHwnd); g_dummyHwnd = NULL;
    UnregisterClassW(kDummyClass, GetModuleHandleW(NULL));
    g_installed = 0;
    FgLog("hook: removed");
}

void FgHookForgetApp(void)
{
    IDXGISwapChain3* old;
    HWND hwnd;
    {
        AppLocked lk;
        old = g_app; hwnd = g_appHwnd;
        g_app = NULL; g_appHwnd = NULL;
        memset(&g_appDesc, 0, sizeof(g_appDesc));
    }
    if (!old) return;
    FgLog("hook: app swapchain %p on hwnd %p forgotten", (void*)old, (void*)hwnd);
    old->Release();
}

IDXGISwapChain3* FgAppAcquire(HWND* hwnd, DXGI_SWAP_CHAIN_DESC1* desc)
{
    AppLocked lk;
    if (!g_app) return NULL;
    g_app->AddRef();
    if (hwnd) *hwnd = g_appHwnd;
    if (desc) *desc = g_appDesc;
    return g_app;
}

bool FgAppIs(IDXGISwapChain* sc)
{
    AppLocked lk;
    return sc != NULL && sc == (IDXGISwapChain*)g_app;
}

void FgPresentedAdd(int n)
{
    for (int i = 0; i < n; ++i) CountPresent();
}

int FgPresentedFps(void) { return (int)g_fps; }
long long FgPresentCount(void) { return g_total; }
