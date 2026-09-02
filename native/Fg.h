// Fg.h - the frame-generation seam. FgHook.cpp owns the DXGI vtable patch and the present counter;
// FgHost.cpp owns the shadow swapchain and drives one IFgProvider; FgFsr/FgXess/FgStreamline implement it.
// Threads: Prepare/Generate/BeforePresent/AfterPresent on the render thread; Create/SetEnabled/Destroy on the
// MAIN thread only (FgHost.cpp: the render thread and the UI thread only DETACH a chain, never destroy it).
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
// Restores every slot that still holds our function (compare-exchange, a later hook on top of ours is left
// alone), then waits (bounded) for the hooks in flight to leave. Plugin unload only.
void FgHookRemove(void);

// The application's swapchain, discovered on the first hooked Present of a chain that passes validation (same
// device as `queue`, a top-level window of this process that is not one of ours, sane size/format). The hook
// OWNS a reference; FgAppAcquire hands out another one (caller releases), FgAppIs compares without dereferencing.
IDXGISwapChain3* FgAppAcquire(HWND* hwnd, DXGI_SWAP_CHAIN_DESC1* desc);   // NULL until discovered
bool FgAppIs(IDXGISwapChain* sc);
void FgHookForgetApp(void);                     // the app HWND died (WM_NCDESTROY): rediscover on the next Present

// Presented frames per second over a 0.5 s window, counted in the hook (includes generated frames). 0 = no data.
int  FgPresentedFps(void);
long long FgPresentCount(void);

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
    // Main thread. Build the FG-owned swapchain on the host's child HWND (FgHostChildHwnd). Returns FG_OK or FG_ERR_*.
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
    // Render thread, after the host SUBMITTED the back-buffer copy into the shadow chain's current buffer and right
    // before it presents `shadow`: the RENDERSUBMIT_END / PRESENT_START markers of DLSS-G and XeLL belong here
    // (after the last write into the proxy's back buffer). Default no-op.
    virtual void     BeforePresent() {}
    // Render thread, right after the host presented `shadow` (hr = that Present's result). Default no-op.
    virtual void     AfterPresent(HRESULT) {}
    // The queue the provider's chain presents on when it is NOT Unity's (DLSS-G: a queue from the Streamline proxy
    // device). The host then runs its back-buffer copy on it after a fence wait on Unity's queue. NULL = Unity's queue.
    virtual ID3D12CommandQueue* PresentQueue() { return NULL; }
    // Main thread.
    virtual void     SetEnabled(bool on) = 0;
    // Main thread, chain detached, the host's shadow reference already released. True = gone, the singleton is
    // reusable. False = the SDK refused (DLSS-G still on, proxy references outstanding): handles are RETAINED and
    // the host retries on its next pump; `force` = give up, leak what the SDK holds and reset anyway.
    virtual bool     Destroy(bool force) = 0;
};

// The live upscaler backend's owned twins (RenderforgeNative.cpp); NULL when no D3D12 backend is up.
struct OwnedSet12;
const OwnedSet12* FgOwned12(void);

IFgProvider* MakeFgProviderNone(void);
IFgProvider* MakeFgProviderFsr(void);
IFgProvider* MakeFgProviderXess(void);      // XeSS-FG + XeLL on the child HWND (FgXess.cpp)
IFgProvider* MakeFgProviderStreamline(void); // DLSS-G / MFG via Streamline 2.12 manual hooking on the child HWND (FgStreamline.cpp)

// ---------------------------------------------------------------- our own HWND (FgWnd.cpp)

// Subclass `parent` and create a WS_CHILD window covering its client area on the parent's thread. Returns NULL on
// failure. Idempotent while the child exists. A different parent restores the previous one's WndProc first.
HWND FgWndCreate(HWND parent);
void FgWndDestroy(void);                     // any thread: hides now, destroys on the UI thread
HWND FgWndChild(void);                       // the live child, NULL when none
void FgWndShow(bool show);                   // any thread: hide the child (Unity's own chain shows through) / show it again
bool FgWndIsOurs(HWND h);                    // a window of our child class (the hook never adopts a chain on it)
void FgWndUnload(void);                      // plugin unload: destroy the child, restore the subclass, unregister the class
struct FgWndProbe
{
    HWND  parent, child, focus, foreground;  // focus/foreground as seen from the UI thread
    int   hit;                               // WM_NCHITTEST at the client centre (HTTRANSPARENT = -1)
    unsigned long createErr;                 // GetLastError of the failed CreateWindowExW / subclass, else 0
};
const FgWndProbe* FgWndProbeNow(void);       // samples on the UI thread (bounded wait), returns the cache

// ---------------------------------------------------------------- host (FgHost.cpp)

// Main thread. Refuses (FG_ERR_NO_SWAPCHAIN, caller retries) while a detached chain still awaits destruction.
int  FgHostInit(int provider, unsigned multiplier, const wchar_t* dllDir);
void FgHostSetEnabled(int on);               // main thread
void FgHostSetFrame(const FgFrame& f);       // main thread; single slot, the Unity RTs are retained until overwritten
void FgHostPrepare(void);                    // render thread, DLSS_EV_FG_PREPARE
// Main thread, every frame (Fg_Pump): destroys a chain that was detached by the render/UI thread. Returns 1 when
// no chain is alive or detached afterwards.
int  FgHostPump(void);
// Main thread. Detaches AND destroys the chain before returning; 1 = destroyed (or none), 0 = the render thread
// never unpinned within the bound and the chain is still detached (leaked until the next pump).
int  FgHostShutdown(void);
void FgHostTearDown(const char* why);        // any thread: DETACH only (Fg_Alive -> 0, the driver's pump destroys, then rebuilds)
int  FgHostAlive(void);                      // 1 while a live chain exists
unsigned FgHostCaps(void);
// Ordinary CreateSwapChainForHwnd on the host's child window (the pass-through provider's chain).
int  FgHostCreateChildSwapChain(const FgSetup& s, IDXGISwapChain4** out);
// Our child HWND (FgWnd.cpp) for a provider that builds the swapchain ITSELF (every vendor proxy chain). NULL when
// the child cannot be made.
HWND FgHostChildHwnd(const FgSetup& s);
int  FgHostProvider(void);
const char* FgHostStatus(void);
const char* FgHostReason(void);              // why the last Init failed, static text or ""

// Called from the Present hook BEFORE it forwards Unity's Present (the hook is the only place that presents
// Unity's chain, exactly once per call, on every path). Returns true when the shadow chain carried this frame:
// the hook then presents Unity's chain with sync 0 and only ALLOW_TEARING/RESTART kept, and reports the result
// to FgHostAfterUnityPresent. False = the hook forwards the call unchanged. Never presents Unity's chain itself.
bool FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags);
void FgHostAfterUnityPresent(HRESULT hr);    // hook, after the Unity Present of a handled frame (failure/occlusion detaches)
// Called from the ResizeBuffers hook before the original runs.
void FgHostOnResize(unsigned w, unsigned h);
// Add n presented frames to the counter (providers report their generated frames through this).
void FgPresentedAdd(int n);
