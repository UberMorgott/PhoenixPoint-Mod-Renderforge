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
// Bracket OUR OWN swapchain creations (the child / vendor proxy chains in FgHostInit): the slots hold dxgi's
// originals while a factory call runs, so a third party that hooks "whatever the new chain's vtable points at"
// (the Steam overlay re-hooks on every CreateSwapChain*) finds dxgi, never our function - a detour on our hook
// whose stored original is our hook again is the loop that ended user crash #2. Main thread; presents in the
// window go straight to dxgi (no chain is live yet). No-ops while not installed.
void FgHookHide(void);
void FgHookShow(void);

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
//
// CONVENTIONS (docs\DESIGN.md "FG input contract"): Direct3D, for a TOP-DOWN image - a frame generator warps the
// swapchain's back buffer with these inputs, so every one of them must index rows the way the back buffer does.
//   resources  the providers tag FgDepth12 / FgMv12 / FgHudless12: the host's row-FLIPPED copies of the owned twins
//              (Unity render textures are bottom-up in memory, the back buffer is top-down; FgTwins.h).
//   view/proj  row-vector matrices (clip = v * view * proj), row-major. View space left-handed: x right, y up,
//              z = camFwd - the basis camRight / camUp / camFwd span (sl_matrix_helpers.h:185-190). Clip space y up,
//              w = +z, reversed Z (near = 1): `depthInverted` / INVERTED_DEPTH / DEPTH_INVERTED. No jitter.
//   jitter     pixels, the NGX offsets, y mirrored with the rows: (-jx, +jy) of the Unity projection shear.
//   mvScale    multiplies the stored UV-space motion into render pixels, current -> previous: (-renderW, +renderH).
// PAIRING: on D3D12 the twins a list of frame N's event reads hold what Unity rendered in frame N-1 (RenderforgeNative.cpp
// PairD3D12), and so does the back buffer of frame N's Present. FgHostPrepare therefore hands the provider the block of
// the PREVIOUS event (view, proj, jitter, camera, reset, dt) next to this event's resources and frameId.
struct FgFrame
{
    ID3D12Resource* hudless;      // outRT: upscaled scene WITHOUT the HUD, output resolution (never read: presence only)
    ID3D12Resource* depth;        // depthRT, render resolution
    ID3D12Resource* mv;           // mvRT, render resolution
    float    jitterX, jitterY;
    float    mvScaleX, mvScaleY;
    float    cameraNear, cameraFar, cameraFovY;   // fov in radians
    float    dtMs;
    int      reset;
    unsigned renderW, renderH, outW, outH;
    unsigned long long frameId;
    float    view[16];            // world -> view
    float    proj[16];            // view -> clip, non-jittered
    float    camPos[3], camUp[3], camRight[3], camFwd[3];   // world space, orthonormal
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
    // False = nothing usable was recorded for this frame (no context/twins, a failed SDK prepare): the host then leaves
    // the frame unprepared and the next Present takes the idle path instead of generating from stale inputs.
    virtual bool     Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f) = 0;
    // Render thread: no Present will consume the frames prepared so far - the host starts a new frame (every earlier
    // unpresented one is superseded) or could not submit this frame's list. Undo ALL per-frame bookkeeping that expects
    // a Present (DLSS-G's token FIFO). Default no-op.
    virtual void     DropPrepared() {}
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
    // Main thread. The frame-rate ceiling in PRESENTED frames per second, 0 = none. True = this provider's own latency
    // SDK enforces it (XeLL: "FPS Cap is controlled via xellSetSleepMode", other limiters "should be disabled",
    // xell_developer_guide_english.md:93-100,129) and the caller must switch its own limiter off; false = the caller
    // keeps limiting rendered frames itself.
    virtual bool     SetFrameLimit(unsigned) { return false; }
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
// Render thread, inside Prepare only: the inputs to tag - the host's row-flipped (top-down) copies of the owned twins,
// written on the prep list right before Prepare (FgTwins.h), all resting in COMMON. The hud-less colour is ALWAYS in
// the app back buffer's format (an FP16 out is sRGB-encoded into 8 bits on the way); NULL when no such copy exists
// this frame (*fmt then says what the out twin is). Depth is R32_FLOAT, motion vectors R16G16_FLOAT, render size;
// NULL = no usable inputs this frame (Prepare returns false).
ID3D12Resource* FgHudless12(DXGI_FORMAT* fmt);
ID3D12Resource* FgDepth12(void);
ID3D12Resource* FgMv12(void);

// Diagnostic capture (Fg_Capture): ONE prepared frame exactly as the provider was fed it - the constants it sent
// (noted by the provider itself inside Prepare) and the raw memory of every tagged resource plus Unity's back buffer
// of the Present that carries the frame. Render thread; a note outside an armed Prepare is dropped.
bool FgCaptureArmed(void);
void FgCaptureNote(const char* fmt, ...);                 // one "key=value" line
void FgCaptureFloats(const char* key, const float* v, int n);
int  FgHostCapture(const wchar_t* dir);                   // main thread: arm (0 = no live chain or one still pending)
int  FgHostCaptureStatus(void);                           // 0 idle, 1 armed, 2 constants noted, 3 written, -1 failed

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
// Main thread. Stores the block in a ring and returns its address: the driver passes it as the `data` of that frame's
// DLSS_EV_FG_PREPARE, so the render thread knows WHICH frame an event belongs to (the main thread runs ahead of it).
// The Unity RTs of the latest block are retained until overwritten.
const FgFrame* FgHostSetFrame(const FgFrame& f);
void FgHostPrepare(const FgFrame* ev);       // render thread, DLSS_EV_FG_PREPARE; ev = that event's block (NULL = latest)
// Main thread, every frame (Fg_Pump): destroys a chain that was detached by the render/UI thread. Returns 1 when
// no chain is alive or detached afterwards.
int  FgHostPump(void);
// Main thread. Detaches AND destroys the chain before returning; 1 = destroyed (or none), 0 = the render thread
// or GPU/provider retirement is still pending. Ownership is retained; the next pump retries.
int  FgHostShutdown(void);
void FgHostTearDown(const char* why);        // any thread: DETACH only (Fg_Alive -> 0, the driver's pump destroys, then rebuilds)
int  FgHostRetired(void);                    // 1 only after detached teardown completed
int  FgHostAlive(void);                      // 1 while a live chain exists
unsigned FgHostCaps(void);
// Ordinary CreateSwapChainForHwnd on the host's child window (the pass-through provider's chain).
int  FgHostCreateChildSwapChain(const FgSetup& s, IDXGISwapChain4** out);
// Our child HWND (FgWnd.cpp) for a provider that builds the swapchain ITSELF (every vendor proxy chain). NULL when
// the child cannot be made.
HWND FgHostChildHwnd(const FgSetup& s);
int  FgHostProvider(void);
int  FgHostSetFrameLimit(unsigned presentedFps);   // main thread; 1 = the live provider paces to it (IFgProvider::SetFrameLimit)
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
