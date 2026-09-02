// Fg.h - the frame-generation seam. FgHook.cpp owns the DXGI vtable patch and the present counter;
// FgHost.cpp owns the shadow swapchain and drives one IFgProvider; FgFsr/FgXess/FgStreamline implement it.
// Everything here runs on the render thread except FgHost::Init/Shutdown (main thread, render idle).
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>

// One line into <modDir>\renderforge_fg.log. Never throws, never allocates after the first call.
void FgLog(const char* fmt, ...);
void FgLogInit(const wchar_t* logDir);

// ---------------------------------------------------------------- hook (FgHook.cpp)

// Patch IDXGISwapChain::Present/Present1/ResizeBuffers/SetFullscreenState in the shared DXGI vtable. Idempotent;
// a partial patch is rolled back. `queue` = Unity's command queue (needed to create the throwaway swapchain the
// vtable is read from). Returns true when every slot is patched.
bool FgHookInstall(ID3D12CommandQueue* queue);
void FgHookRemove(void);

// The application's swapchain, discovered on the first hooked Present. NULL until then. Observed, not owned:
// compare it, never dereference it without a reference of your own (FgHostInit AddRefs it for the chain's life).
IDXGISwapChain3* FgAppSwapChain(void);
HWND             FgAppHwnd(void);
const DXGI_SWAP_CHAIN_DESC1* FgAppDesc(void);   // NULL until discovered
void FgHookForgetApp(void);                     // the app HWND died (WM_NCDESTROY): rediscover on the next Present

// Presented frames per second over a 0.5 s window, counted in the hook (includes generated frames). 0 = no data.
int  FgPresentedFps(void);
long long FgPresentCount(void);

// Spike diagnostics, filled by FgHookSpike(). Read by Fg_SpikeStatus.
struct FgSpike
{
    int   installed;          // vtable patched
    int   sawPresent;         // the hook ran at least once
    int   flipModel;          // app swap effect is FLIP_DISCARD or FLIP_SEQUENTIAL
    int   waitable;           // app flags have DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
    unsigned format;          // DXGI_FORMAT of the app backbuffer
    unsigned bufferCount;
    unsigned swapEffect;
    unsigned scFlags;
    unsigned width, height;
    int   windowed;
    long   secondSwapChainHr; // HRESULT of a second CreateSwapChainForHwnd on the game HWND
    long   compositionHr;     // HRESULT of CreateSwapChainForComposition as the 4a fallback
    long   forwardedPresentHr;// HRESULT the saved original Present returned
};
const FgSpike* FgSpikeResult(void);
void FgHookSpike(void);       // runs the two creation probes once, on the render thread

// ---------------------------------------------------------------- provider seam

// Everything a provider needs to build its own swapchain on the game's window.
struct FgSetup
{
    ID3D12Device*         device;
    ID3D12CommandQueue*   queue;
    IDXGIFactory2*        factory;
    HWND                  hwnd;
    DXGI_SWAP_CHAIN_DESC1 desc;        // FLIP_DISCARD, app format, app size, >= 3 buffers
    unsigned              multiplier;  // 2, 3 or 4 (total frames presented per rendered frame)
    const wchar_t*        dllDir;      // mod folder: where the vendor DLLs live
};

// One rendered frame's inputs. Filled on the main thread by Fg_SetFrame, read on the render thread.
struct FgFrame
{
    ID3D12Resource* hudless;      // outRT: upscaled scene WITHOUT the HUD, output resolution
    ID3D12Resource* depth;        // depthRT, render resolution
    ID3D12Resource* mv;           // mvRT, render resolution, current->previous, pixels
    float    jitterX, jitterY;
    float    mvScaleX, mvScaleY;
    float    cameraNear, cameraFar, cameraFovY;   // fov in radians
    float    dtMs;
    int      reset;
    unsigned renderW, renderH, outW, outH;
    unsigned long long frameId;
    float    view[16];            // worldToCamera, row-major
    float    proj[16];            // non-jittered projection, row-major
    float    camPos[3], camUp[3], camRight[3], camFwd[3];
};

struct IFgProvider
{
    virtual ~IFgProvider() {}
    virtual int      Id() const = 0;                    // FG_PROVIDER_*
    virtual unsigned Caps() const = 0;                  // FG_CAP_* mask, valid after Create
    virtual const char* Name() const = 0;
    // Build the FG-owned swapchain on s.hwnd. Returns FG_OK or an FG_ERR_*.
    virtual int      Create(const FgSetup& s, IDXGISwapChain4** outSwapChain) = 0;
    // Render thread, inside the DLSS_EV_FG_PREPARE render event, on a recording DIRECT command list that goes
    // through Unity's ExecuteCommandList with NO state declarations: providers read the shim-owned twins
    // (FgOwned12(), all resting in COMMON), never the Unity RTs in `f`.
    virtual void     Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f) = 0;
    // Render thread, inside the Present hook, BEFORE the host copies the real frame into `shadow`.
    // `unityBackBuffer` is Unity's finished frame (state PRESENT). The provider generates its in-between
    // frame(s), presents them on `shadow` itself (same sync/flags the host will use) and returns how many it
    // presented; 0 = nothing generated this frame (the host still presents the real frame).
    virtual int      Generate(const FgFrame& f, ID3D12Resource* unityBackBuffer, IDXGISwapChain4* shadow, UINT sync, UINT pf) = 0;
    // Render thread, right after the host presented `shadow` (hr = that Present's result). XeSS-FG's present markers
    // and present status live here; the default is a no-op.
    virtual void     AfterPresent(HRESULT) {}
    virtual void     SetEnabled(bool on) = 0;
    virtual void     Destroy(void) = 0;
};

// The live upscaler backend's owned twins (RenderforgeNative.cpp); NULL when no D3D12 backend is up.
struct OwnedSet12;
const OwnedSet12* FgOwned12(void);

IFgProvider* MakeFgProviderNone(void);
IFgProvider* MakeFgProviderFsr(void);
IFgProvider* MakeFgProviderXess(void);      // XeSS-FG + XeLL on the child HWND (FgXess.cpp)
IFgProvider* MakeFgProviderStreamline(void);

// ---------------------------------------------------------------- our own HWND (FgWnd.cpp)

// Subclass `parent` (once, for the process lifetime) and create a WS_CHILD window covering its client area on
// the parent's thread. Returns NULL on failure. Idempotent while the child exists.
HWND FgWndCreate(HWND parent);
void FgWndDestroy(void);                     // any thread: hides now, destroys on the UI thread
HWND FgWndChild(void);                       // the live child, NULL when none (the hook excludes its chain from discovery)
struct FgWndProbe
{
    HWND  parent, child, focus, foreground;  // focus/foreground as seen from the UI thread
    int   hit;                               // WM_NCHITTEST at the client centre (HTTRANSPARENT = -1)
    unsigned long createErr;                 // GetLastError of the failed CreateWindowExW / subclass, else 0
};
const FgWndProbe* FgWndProbeNow(void);       // samples on the UI thread (bounded wait), returns the cache

// ---------------------------------------------------------------- host (FgHost.cpp)

int  FgHostInit(int provider, unsigned multiplier, const wchar_t* dllDir);  // main thread
void FgHostSetEnabled(int on);
void FgHostSetFrame(const FgFrame& f);       // main thread; single slot, the Unity RTs are retained until overwritten
void FgHostPrepare(void);                    // render thread, DLSS_EV_FG_PREPARE
void FgHostShutdown(void);                   // main thread
void FgHostTearDown(const char* why);        // any thread: fullscreen, WM_NCDESTROY, display change (Fg_Alive -> 0, driver rebuilds)
int  FgHostAlive(void);                      // 1 while a chain exists; 0 after a resize/Present-failure/fullscreen teardown
unsigned FgHostCaps(void);
// Composition swapchain on s.hwnd shown through a DirectComposition target (the host owns the DComp objects).
// Every provider that does not bring its own swapchain builds it from this.
int  FgHostCreateShadowSwapChain(const FgSetup& s, IDXGISwapChain4** out);
// Our child HWND (FgWnd.cpp) for a provider that builds the swapchain ITSELF (FSR's proxy chain): the host then
// reports chain=child and presents whatever the provider returned from Create. NULL when the child cannot be
// made or RENDERFORGE_FG_CHAIN=composition - fall back to FgHostCreateShadowSwapChain.
HWND FgHostChildHwnd(const FgSetup& s);
int  FgHostProvider(void);
const char* FgHostStatus(void);

// Called from the Present hook BEFORE it forwards Unity's Present (the hook is the only place that presents
// Unity's chain, exactly once per call, on every path). Returns true when the shadow chain carried this frame:
// the hook then presents Unity's chain with sync 0 and only ALLOW_TEARING/RESTART kept, and reports the result
// to FgHostAfterUnityPresent. False = the hook forwards the call unchanged. Never presents Unity's chain itself.
bool FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags);
void FgHostAfterUnityPresent(HRESULT hr);    // hook, after the Unity Present of a handled frame (failure/occlusion tears down)
// Called from the ResizeBuffers hook before the original runs.
void FgHostOnResize(unsigned w, unsigned h);
// Add n presented frames to the counter (providers report their generated frames through this).
void FgPresentedAdd(int n);
