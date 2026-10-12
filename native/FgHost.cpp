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
// DEFERRED REQUESTS (docs\DESIGN.md "FG host threading and retirement"): a call that needs a synchronisation it did
// not get is never issued "anyway" - the wish is stored and applied at the next proven-safe point of the thread that
// owns it: the provider's on/off on the main thread under PinMain (ApplyEnabled, retried by FgHostPump), an SDK call
// that needs the GPU idle on the presenting thread inside the Present hook (ServeQuiescent).
//
// CHAIN EPOCH (docs\DESIGN.md "FG chain epoch"): H.epoch names the generation of the chain in H.c. Every frame block
// carries the epoch it was fed to (FgFrame::epoch), and whatever crosses a thread boundary with a delay - a queued
// prepare event, an armed capture, the result of Unity's own Present - acts only on the generation it belongs to.
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
#include <stdlib.h>
#include <string.h>

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

enum { kNone = 0, kLive = 1, kDetached = 2 };
enum { kFrameRing = 8 };   // the main thread is at most a frame or two ahead of the render thread
enum { kIdleNotify = 30 }; // consecutive presents without a prepared frame before IFgProvider::Idle

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
    int                   pinned;       // a thread is inside the chain: the render thread (Prepare) or the presenting thread (OnPresent)
    // Pinned threads only (Prepare on the render thread, OnPresent on the presenting thread; Pin serialises the two).
    // A Present with no frame prepared since the last one (menu screens without a rendering
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
    unsigned              lastEvPace;   // render thread: pace id of the last prepare event taken (0 = none / unpaced)
    FgFrame               paired;       // render thread: the previous event's block (PairFrame)
    int                   havePaired;
    FgFrame               sent;         // render thread: what the previous Prepare was handed (capture notes)
    int                   haveSent;
    int                   twinsOk;      // render thread: this Prepare's list wrote the flipped copies (FgTwins.h)
    volatile LONG         hudless;      // the last provider Prepare of this chain: 1 = tagged a hud-less colour, 0 = did not, -1 = none yet
    unsigned              paceId;       // main thread: the pace id of the frame in progress (0 = this frame is not paced)
    unsigned              simId;        // main thread: the id FrameBegin opened this frame, paced or not (SimEnd closes it)
    int                   fed;          // main thread: FgHostSetFrame ran since the last FgHostFrameBegin
    // Totals of the live chain (status line): prepare events, prepared frames, presents seen, presents taken the idle way,
    // prepares that waited for a Present, duplicate prepare events refused, Pin timeouts; the longest prepare wait (ms).
    volatile LONG         nPrepare, nPrepared, nPresent, nIdle, nWaited, nDup, nStall;
    unsigned              maxWaitMs;    // under g_lock
    int                   presentStalled; // a prepared frame's Present did not come within kPinWaitMs: prepares stop waiting until one does
    int                   tidLogged;
    // Any render-thread exit of FgHostPrepare, pinned or not (EventRan): the highest pace id whose prepare event has
    // run. Handed to the provider (PrepareDone) by whichever thread holds the pin next.
    volatile LONG         evDone;
    // Main thread only: the provider switch FgHostSetEnabled asked for and could not issue yet (-1 = none pending), and
    // the last one issued on this chain (-1 = none yet).
    int                   provWant, provOn;
    // Presenting thread: presents to let pass before the quiescent point is tried again (after a failed drain), and the
    // quiescent points served / refused for lack of a retirement proof (status line).
    int                   quiesceSkip;
    volatile LONG         nQuiesce, nQuiesceFail;
    // CHAIN EPOCH (docs\DESIGN.md "FG chain epoch"): the generation of the chain in H.c, under g_lock; the main thread
    // bumps it in the critical section that publishes a new chain (FgHostInit) and nothing else writes it. Never 0
    // once a chain was built. nStale: queued prepare events dropped because their block belongs to another generation.
    unsigned              epoch;
    volatile LONG         nStale;
    // Threading diagnostics (docs\DESIGN.md "Present thread = submission thread"), pinned threads only: the thread
    // of the last hooked Present / prepare event of this chain and how often it CHANGED (0 = one thread throughout);
    // hooked Presents during which Unity's next-frame-fence value moved (FenceWatch); served quiescent points whose
    // queue was no longer empty right behind the provider's SDK call (ServeQuiescent).
    DWORD                 presentTid, prepareTid;
    volatile LONG         nPresentTidSw, nPrepareTidSw, nFenceMoved, nQuiesceBusy;
    char                  status[1024];
};

Host H;
SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_unpinned = CONDITION_VARIABLE_INIT;   // Unpin: the other thread left the chain
enum { kPinWaitMs = 100 };
bool g_mainPacing = true;   // FgMainPacing(): main thread writes, the providers read it at Create
volatile LONG g_testFailPrepare = 0;   // Fg_TestFailPrepare (test lever): every provider Prepare is treated as failed

struct Locked
{
    Locked()  { AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
};

// FgTestSdkFail (Fg.h). `on` is written once, on the main thread, before any chain exists (FgHostInit).
struct TestSdkFail { int on; char what[32]; volatile LONG left; volatile LONG hits; volatile LONG skip; };
TestSdkFail g_testSdk;
void TestSdkFailInit()
{
    static bool done = false;
    if (done) return;
    done = true;
    char v[64] = {};
    if (!GetEnvironmentVariableA("RENDERFORGE_FG_TEST_SDK_FAIL", v, sizeof(v)) || !v[0]) return;
    int count = -1, skip = 0;
    char* comma = strchr(v, ',');
    if (comma) {
        *comma = 0; count = atoi(comma + 1);
        const char* sk = strstr(comma + 1, "skip=");
        if (sk) skip = atoi(sk + 5);
    }
    strncpy_s(g_testSdk.what, sizeof(g_testSdk.what), v, _TRUNCATE);
    g_testSdk.left = count > 0 ? count : -1;           // -1 = every call
    g_testSdk.skip = skip > 0 ? skip : 0;
    g_testSdk.on = 1;
    FgLog("host: TEST RENDERFORGE_FG_TEST_SDK_FAIL=%s,%d,skip=%d - the named SDK call is skipped and reported as failed (%s, after %d calls that go through)",
          g_testSdk.what, count, skip, count > 0 ? "that many times" : "every time", skip > 0 ? skip : 0);
}

// --- VIDEO MEMORY + PRESENT WATCHDOG (diagnostics; docs\DESIGN.md "A vendor Present that does not return") ---------
// 2026-10-12: FSR-FG, 7th fast off/on rebuild, video memory overcommitted by a second game instance. The second
// Present of a fresh proxy never returned: AMD's proxy spun in waitForFenceValue (no timeout) on its composition fence,
// its interpolation thread on the interpolation fence it had signalled on Unity's queue in Present #1 - a Signal that
// never completed - and Unity's render thread hit its own 30 s fence timeout and broke into the debugger. No lock or
// pin of ours was involved, the device was not removed, and the minidump cannot say why the queue stood still. So the
// next occurrence must be decidable from the log alone: a thread of our own watches the time the presenting thread
// spends inside the provider's Present and, past kPresentStuckMs, writes down what the queue and the memory are doing.
// It only reads and logs - a vendor spin without a timeout cannot be interrupted from outside.
enum { kPresentStuckMs = 2000, kWatchPeriodMs = 500 };
volatile LONGLONG g_presentSince;       // GetTickCount64() at the entry of the shadow Present, 0 = not inside one
volatile LONG     g_presentNo;          // counts shadow Presents (the watchdog reports each stuck one once)
volatile LONG     g_presentFlagged;     // the Present in flight was reported stuck: its return is logged too
HANDLE            g_watchThread, g_watchStop;

// Any thread. "vram local <usage>/<budget> MB, shared <usage>/<budget> MB" of the adapter the device runs on.
void VramLine(ID3D12Device* dev, char* buf, int cap)
{
    _snprintf_s(buf, cap, _TRUNCATE, "vram unknown");
    IDXGIFactory4* dxgi = NULL;
    IDXGIAdapter3* adapter = NULL;
    if (dev && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi))) && dxgi && SUCCEEDED(dxgi->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO loc = {}, non = {};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &loc)) && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non)))
            _snprintf_s(buf, cap, _TRUNCATE, "vram local %llu/%llu MB%s, shared %llu/%llu MB", (unsigned long long)(loc.CurrentUsage >> 20), (unsigned long long)(loc.Budget >> 20),
                        loc.CurrentUsage > loc.Budget ? " OVER BUDGET" : "", (unsigned long long)(non.CurrentUsage >> 20), (unsigned long long)(non.Budget >> 20));
    }
    SafeRelease(adapter);
    SafeRelease(dxgi);
}

DWORD WINAPI PresentWatchdog(void*)
{
    LONG reported = -1;
    while (WaitForSingleObject(g_watchStop, kWatchPeriodMs) == WAIT_TIMEOUT) {
        const LONGLONG since = InterlockedCompareExchange64(&g_presentSince, 0, 0);
        const LONG no = InterlockedCompareExchange(&g_presentNo, 0, 0);
        if (!since || no == reported || (LONGLONG)GetTickCount64() - since < kPresentStuckMs) continue;
        reported = no;
        // The presenting thread is inside Present with the chain pinned: device, queue and provider cannot go away.
        ID3D12Device* dev = H.device;
        ID3D12CommandQueue* queue = H.queue;
        if (!dev || !queue) continue;
        InterlockedExchange(&g_presentFlagged, 1);
        char vram[160];
        VramLine(dev, vram, sizeof(vram));
        // Is Unity's queue retiring work at all? A fence of our own, signalled behind everything submitted so far.
        int probe = -1;                                    // -1 not run, 0 did not complete within 1 s, 1 completed
        ULONGLONG probeMs = 0;
        ID3D12Fence* fence = NULL;
        HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (ev && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence) {
            const ULONGLONG t0 = GetTickCount64();
            if (SUCCEEDED(queue->Signal(fence, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, ev))) probe = WaitForSingleObject(ev, 1000) == WAIT_OBJECT_0 ? 1 : 0;
            probeMs = GetTickCount64() - t0;
        }
        SafeRelease(fence);
        if (ev) CloseHandle(ev);
        FgLog("host: WATCHDOG shadow Present #%ld (%s, epoch %u, present %ld of this chain) has not returned for %lld ms on tid %u: removed=0x%08X, Unity queue probe %s (%llu ms), %s",
              no, H.c.prov ? H.c.prov->Name() : "-", H.epoch, (long)H.nPresent, (LONGLONG)GetTickCount64() - since, (unsigned)H.presentTid,
              (unsigned)dev->GetDeviceRemovedReason(), probe == 1 ? "COMPLETED - the queue retires work, the provider waits on something of its own" :
              probe == 0 ? "DID NOT COMPLETE in 1 s - the queue itself stands still (a queue Wait nobody signals, or the GPU scheduler holds it: paging under memory overcommit)" : "not run",
              (unsigned long long)probeMs, vram);
    }
    return 0;
}

void WatchdogStart()
{
    if (g_watchThread) return;
    g_watchStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_watchStop) g_watchThread = CreateThread(NULL, 0, PresentWatchdog, NULL, 0, NULL);
}

} // namespace

// Plugin unload (UnityPluginUnload, after FgHostShutdown): the watchdog thread must not outlive the module's code.
void FgHostWatchdogStop(void)
{
    if (!g_watchThread) return;
    SetEvent(g_watchStop);
    WaitForSingleObject(g_watchThread, 2000);
    CloseHandle(g_watchThread); g_watchThread = NULL;
    CloseHandle(g_watchStop); g_watchStop = NULL;
}

namespace {

// TEST lever RENDERFORGE_FG_TEST_VRAM_MB=<n> (env, before launch): commit n MB of throwaway 4096x4096 RGBA8 textures on
// Unity's device before the first chain is built and keep them for the process - frame generation under video-memory
// pressure (chain rebuilds allocate and free hundreds of MB each). Main thread, once.
void TestVramPressure(ID3D12Device* dev)
{
    static bool done = false;
    if (done || !dev) return;
    done = true;
    char v[32] = {};
    if (!GetEnvironmentVariableA("RENDERFORGE_FG_TEST_VRAM_MB", v, sizeof(v)) || atoi(v) <= 0) return;
    const int want = atoi(v) / 64;
    int got = 0;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 4096; desc.Height = 4096; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    for (; got < want; ++got) {
        ID3D12Resource* tex = NULL;                        // never released: the pressure lasts for the process
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&tex))) || !tex) break;
        tex->SetName(L"Renderforge TEST vram pressure");
    }
    char vram[160];
    VramLine(dev, vram, sizeof(vram));
    FgLog("host: TEST RENDERFORGE_FG_TEST_VRAM_MB=%s - committed %d of %d textures (%d MB), %s", v, got, want, got * 64, vram);
}

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
    // Debug-layer attribution (RENDERFORGE_D3D12_DEBUG): which of Unity's back buffers this list read, on which queue
    // and thread, for the first presents of a chain - a later message naming a back buffer is matched by pointer.
    // The shadow chain's buffers get a name of ours; Unity names its own ("BackBufferN_0").
    if (RfDbg::On()) {
        dst->SetName(L"Renderforge FG shadow back buffer");
        if (H.nPresent <= 4) RfDbg::Log("FG copy: unity back buffer %p (app index %u) -> shadow %p on %s queue %p, epoch %u present %ld tid=%u, PRESENT -> COPY_SOURCE -> PRESENT inside this list",
                                        (void*)src, c.app->GetCurrentBackBufferIndex(), (void*)dst, q == H.queue ? "Unity's" : "the provider's", (void*)q, H.epoch, (long)H.nPresent, (unsigned)GetCurrentThreadId());
    }

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

bool CapturePendingRetire();

// Main thread, no lock held. GPU objects of a chain whose provider is already gone (or never came up).
bool ReleaseGpu(Chain& c)
{
    if ((!c.prep.WaitIdle(0) && !c.prep.Removed()) || (!c.copy.WaitIdle(0) && !c.copy.Removed())) return false;
    CapturePendingRetire();     // a late capture list holds its own references on the twins: still in flight = it keeps them
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

// Main thread, no lock held, chain detached. Where the host's shadow reference goes is the provider's SDK's rule
// (IFgProvider::ProxyOutlivesDestroy): BEFORE the provider (XeSS-FG's xefgSwapChainDestroy refuses while any proxy
// reference is outstanding; DLSS-G) or AFTER it, as the last reference of all (FSR: "release the final reference
// last", frame-interpolation-swap-chain.md:262-268 - ReleaseGpu does it). Then the remaining GPU objects.
// False = the provider refused and kept its handles: retried on the next pump.
bool DestroyChain(Chain& c, const char* why, bool force)
{
    ULONGLONG t0 = GetTickCount64();
    if (c.prov) c.prov->SetEnabled(false);
    if ((!c.prep.WaitIdle(0) && !c.prep.Removed()) || (!c.copy.WaitIdle(0) && !c.copy.Removed())) return false;
    if (!c.prov || !c.prov->ProxyOutlivesDestroy()) SafeRelease(c.shadow);
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
// `epoch` != 0 = the caller means THAT generation (a result that came back from a call made unpinned): a chain built
// since is not its business.
void Detach(const char* why, unsigned epoch = 0)
{
    {
        Locked lk;
        if (H.state != kLive) return;
        if (epoch && epoch != H.epoch) return;
        H.state = kDetached;
        H.enabled = 0;
        H.destroyTries = 0;
        strncpy_s(H.why, sizeof(H.why), why, _TRUNCATE);
    }
    WakeAllConditionVariable(&g_unpinned);             // a Pin waiting for the other thread gives up now, not at its timeout
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

// The prepare event runs on Unity's render thread; Present is issued by ANOTHER thread (Unity's D3D12 backend submits
// from its own: the log's "first prepare tid" / "first hooked present tid" differ), and it trails the render thread:
// frame N+1's prepare event can arrive while frame N's Present has not run yet, or sits inside the vendor chain's
// pacing wait. Left alone such a prepare was refused (pinned) or superseded frame N's inputs, and frame N+1 was then
// presented the idle way - each one costs the generator its history. Measured 2026-10-12, tactical, uncapped ~100 fps:
// FSR-FG 124 of 2901 prepares overlapped a Present; XeSS-FG with the window occluded (Windows 11 then ignores the
// process's timer resolution and the proxy's pacing wait stretches) 1003 prepare events against 1324 presents, 322 of
// them idle, presented/base 1.4, base frames hitching to 24 ms. So the two are SERIALISED here: a prepare waits
// (bounded) until the previous prepared frame has been presented, a Present waits (bounded) for a prepare in flight.
// A Present that never comes (the window stopped presenting) costs one timeout: `presentStalled` then lets prepares
// supersede at once until a Present shows up again.
// True with H.c pinned (stable until Unpin) and H.cur refreshed from the slot.
// `epoch` != 0 = the caller carries a queued event of THAT chain generation: the pin is refused (*stale = true) the
// moment the chain in H.c is another one - checked on entry and again after every wake of the wait, in the same
// critical section that takes the pin. INVARIANT: a thread that holds the pin sees one generation until it unpins
// (a new chain is published only after DestroyDetached saw nobody pinned, and FgHostInit runs on one thread).
bool Pin(bool forPrepare, unsigned epoch = 0, bool* stale = NULL)
{
    Locked lk;
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG until = start + kPinWaitMs;
    bool waited = false;
    for (;;) {
        if (epoch && epoch != H.epoch) { if (stale) *stale = true; return false; }
        if (H.state != kLive || !H.enabled) return false;
        if (!H.pinned && !(forPrepare && H.prepared && !H.presentStalled)) break;
        ULONGLONG now = GetTickCount64();
        if (now >= until) {
            InterlockedIncrement(&H.nStall);
            if (H.pinned) return false;                // never two threads inside the chain
            H.presentStalled = 1;                      // supersede the unpresented frame, as before
            break;
        }
        waited = true;
        SleepConditionVariableSRW(&g_unpinned, &g_lock, (DWORD)(until - now), 0);
    }
    if (waited && forPrepare) {
        InterlockedIncrement(&H.nWaited);
        unsigned ms = (unsigned)(GetTickCount64() - start);
        if (ms > H.maxWaitMs) H.maxWaitMs = ms;
    }
    H.pinned = 1;
    H.cur = H.slot;
    if (H.cur.hudless) H.cur.hudless->AddRef();
    if (H.cur.depth) H.cur.depth->AddRef();
    if (H.cur.mv) H.cur.mv->AddRef();
    return true;
}

// The pinning thread. `tearWhy` != NULL = this thread found the chain dead: detach (the main thread destroys).
void Unpin(const char* tearWhy)
{
    { Locked lk;
      SafeRelease(H.cur.hudless); SafeRelease(H.cur.depth); SafeRelease(H.cur.mv);
      H.pinned = 0;
    }
    WakeAllConditionVariable(&g_unpinned);
    if (tearWhy) Detach(tearWhy);
}

// Main thread: the chain to itself - no Prepare and no Present inside it - for a vendor call that must not overlap
// either: slDLSSGSetOptions "is NOT thread safe" (sl_dlss_g.h:207) and the render thread issues it from Prepare.
// Waits (bounded) for the thread inside to leave. False = no live chain, or the wait timed out: nothing is pinned
// then, and the caller must NOT issue its call (ApplyEnabled defers it). Unpin(NULL) ends it.
bool PinMain(DWORD waitMs)
{
    Locked lk;
    const ULONGLONG until = GetTickCount64() + waitMs;
    for (;;) {
        if (H.state != kLive) return false;
        if (!H.pinned) break;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return false;
        SleepConditionVariableSRW(&g_unpinned, &g_lock, (DWORD)(until - now), 0);
    }
    H.pinned = 1;
    return true;
}

// ---------------------------------------------------------------- capture (diagnostic, Fg_Capture)

struct Capture
{
    volatile LONG state;        // 0 idle, 1 armed (main thread), 2 constants noted (Prepare), 3 written, -1 failed
    ULONGLONG armedAt;          // GetTickCount64 of the arm: a capture nobody finished may be re-armed after kCaptureStaleMs
    wchar_t dir[MAX_PATH];
    char    notes[16384];
    int     len;
    unsigned epoch;             // the chain generation it was armed on: another chain neither notes nor writes it
};
Capture g_cap;

struct CapItem { const char* name; ID3D12Resource* res; ID3D12Resource* buf; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowBytes; };

// A capture whose readback list reached the queue and did not retire in time.
// INVARIANT: everything of OURS that list touches stays alive until its fence has completed (a removed device
// completes it): the allocator, the list, the readback buffers, the wait event its fence still holds registered, and
// a reference on every twin it reads (items 0..2) - the twins are released by FgTwins::Ensure / ReleaseGpu on the
// prep and copy rings' fences, which know nothing of this list. Unity's back buffer is deliberately NOT referenced:
// an outstanding reference makes Unity's own ResizeBuffers fail, and the swapchain keeps the buffer until Unity
// resizes or destroys the chain, which Unity does behind a flush of this very queue (our list is ahead of it).
// Pinned presenting thread (CaptureWrite) and the main thread with nobody pinned (ReleaseGpu) only.
struct CapPending
{
    bool live;
    ID3D12CommandAllocator* alloc; ID3D12GraphicsCommandList* cl; ID3D12Fence* fence; HANDLE ev;
    ID3D12Resource* buf[4]; ID3D12Resource* src[3];
};
CapPending g_capPending;

// True = no capture list is in flight (any more).
bool CapturePendingRetire()
{
    CapPending& p = g_capPending;
    if (!p.live) return true;
    if (p.fence->GetCompletedValue() < 1) return false;
    for (int i = 0; i < 4; ++i) SafeRelease(p.buf[i]);
    for (int i = 0; i < 3; ++i) SafeRelease(p.src[i]);
    SafeRelease(p.cl); SafeRelease(p.alloc); SafeRelease(p.fence);
    if (p.ev) CloseHandle(p.ev);
    p = CapPending();
    FgLog("host: capture: the late readback list has retired - its objects are released");
    return true;
}

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
    if (!CapturePendingRetire()) { FgLog("host: capture: refused - an earlier readback list is still in flight"); return false; }
    ID3D12CommandAllocator* alloc = NULL; ID3D12GraphicsCommandList* cl = NULL; ID3D12Fence* fence = NULL;
    bool ok = SUCCEEDED(H.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(H.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, IID_PPV_ARGS(&cl))) &&
              SUCCEEDED(H.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    if (ok) { alloc->SetName(L"Renderforge FG capture"); cl->SetName(L"Renderforge FG capture"); fence->SetName(L"Renderforge FG capture fence"); }
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
        it.buf->SetName(L"Renderforge FG capture readback");
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = it.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        dst.pResource = it.buf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = it.fp;
        OwnedSet12::Barrier(cl, it.res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
        OwnedSet12::Barrier(cl, it.res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    bool inFlight = false;      // the list reached the queue and nothing proves it retired
    HANDLE ev = NULL;
    if (ok) {
        ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        ID3D12CommandList* lists[1] = { cl };
        ok = ev && SUCCEEDED(cl->Close());
        if (ok) {
            H.queue->ExecuteCommandLists(1, lists);
            // The fence's own value decides, never the wait result alone (a failed Signal leaves it at 0 for good).
            if (SUCCEEDED(H.queue->Signal(fence, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, ev))) WaitForSingleObject(ev, 5000);
            ok = fence->GetCompletedValue() >= 1 && SUCCEEDED(H.device->GetDeviceRemovedReason());
            inFlight = fence->GetCompletedValue() < 1;
        }
    }
    if (inFlight) {
        // The queue may still execute the list: park everything it touches (CapPending) until its fence completes -
        // the next capture or the chain's teardown releases it. Freeing any of it here is a GPU use-after-free.
        CapPending& p = g_capPending;
        p.live = true; p.alloc = alloc; p.cl = cl; p.fence = fence; p.ev = ev;
        for (int i = 0; i < 4; ++i) p.buf[i] = items[i].buf;
        for (int i = 0; i < 3; ++i) { p.src[i] = items[i].res; if (p.src[i]) p.src[i]->AddRef(); }
        FgLog("host: capture: the readback list did not retire within 5 s - its allocator, buffers and source twins are kept until it does");
        return false;
    }
    if (ev) CloseHandle(ev);
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

// Main thread, the last step of FgHostInit: the chain built in H.c becomes generation `epoch` and pinnable.
// The epoch moves in the SAME critical section that makes the chain pinnable, and so does everything a queued event
// of the previous generation could still have written without a pin (EventRan -> evDone) or reads before it pins
// (lastEvPace): an event sees either the old generation, unpinnable, or the new one with a clean slate - never the
// new chain with the old one's ids.
static void PublishChain(IFgProvider* p, unsigned epoch)
{
    Locked lk;
    H.epoch = epoch;
    H.evDone = 0; H.lastEvPace = 0;
    H.c.prov = p;                                          // publish: the render thread may pin from here on
    H.state = kLive;
}

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
    s.epoch = H.epoch + 1 ? H.epoch + 1 : 1;               // this thread is the only writer (published below); never 0
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

    TestSdkFailInit();
    TestVramPressure(H.device);
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
    // A provider with a presenting queue of its own cannot be driven without the handoff fence (CopyBackBuffer would
    // run the copy on Unity's queue, unordered against that provider's presents): the chain is published DEAD -
    // detached, never live - and the ordinary teardown path destroys it.
    const bool noHandoff = p->PresentQueue() && (FAILED(H.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.xfence))) || !c.xfence);
    if (c.xfence) c.xfence->SetName(L"Renderforge FG queue handoff fence");
    if (noHandoff) {
        FgLog("host: CreateFence for the %s present-queue handoff failed (removed=0x%08X) - chain not started", p->Name(), (unsigned)H.device->GetDeviceRemovedReason());
        { Locked lk; c.prov = p; H.state = kDetached; H.enabled = 0; H.destroyTries = 0; strncpy_s(H.why, sizeof(H.why), "no handoff fence", _TRUNCATE); }
        H.lastError = FG_ERR_NO_SWAPCHAIN;
        DestroyDetached(0);
        return FG_ERR_NO_SWAPCHAIN;
    }
    H.lastError = FG_OK;
    H.reason = NULL;
    H.lastPresentHr = 0;
    H.prepared = 0; H.idle = 0; H.hidden = 0; H.prepFails = 0; H.havePaired = 0; H.haveSent = 0; H.twinsOk = 0;              // a fresh child starts shown (FgWnd WM_RF_CREATE)
    H.paceId = 0; H.simId = 0; H.fed = 0; H.maxWaitMs = 0;
    H.nPrepare = H.nPrepared = H.nPresent = H.nIdle = H.nWaited = H.nDup = H.nStall = 0;
    H.hudless = -1;
    H.presentStalled = 0; H.tidLogged = 0;
    H.provWant = -1; H.provOn = -1; H.quiesceSkip = 0; H.nQuiesce = H.nQuiesceFail = 0;
    H.nStale = 0; H.presentTid = H.prepareTid = 0; H.nPresentTidSw = H.nPrepareTidSw = H.nFenceMoved = H.nQuiesceBusy = 0;
    PublishChain(p, s.epoch);
    char vram[160];
    VramLine(H.device, vram, sizeof(vram));
    FgLog("host: provider=%s multiplier=%u epoch=%u shadow=%p %ux%u flags=0x%X caps=0x%X, %s",
          p->Name(), H.multiplier, H.epoch, (void*)c.shadow, H.outW, H.outH, c.scFlags, p->Caps(), vram);
    WatchdogStart();
    return FG_OK;
}

// Main thread. The presented-fps ceiling for the live provider (IFgProvider::SetFrameLimit), then what its SDK holds
// (IFgProvider::PacerState, FG_PACER_* bits). 0 = no live chain: the caller's limiter paces, undivided.
int FgHostSetFrameLimit(unsigned presentedFps)
{
    IFgProvider* p = NULL;
    {
        Locked lk;
        if (H.state == kLive) p = H.c.prov;
    }
    if (!p) return 0;
    p->SetFrameLimit(presentedFps);
    return p->PacerState();
}

int FgHostPacerState(void)
{
    IFgProvider* p = NULL;
    {
        Locked lk;
        if (H.state == kLive) p = H.c.prov;
    }
    return p ? p->PacerState() : 0;
}

// Main thread, both: Create / SetEnabled / Destroy are main-thread too, so the provider cannot go away underneath.
static IFgProvider* LiveEnabled()
{
    Locked lk;
    return H.state == kLive && H.enabled ? H.c.prov : NULL;
}

// Every frame of a live, enabled chain reaches the provider, paced or not: a latency SDK wants its sleep and its
// simulation markers on every frame, and its low-latency mode + frame cap on only while it is the pacer (`pace`).
unsigned FgHostFrameBegin(int pace)
{
    IFgProvider* p = LiveEnabled();
    if (p && H.paceId && !H.fed) p->FrameUnfed(H.paceId);   // the frame before this one was paced and fed nothing
    H.fed = 0;
    H.simId = p ? p->FrameBegin(pace != 0) : 0;
    H.paceId = pace ? H.simId : 0;
    return H.paceId;
}

void FgHostSimEnd(void)
{
    IFgProvider* p = H.simId ? LiveEnabled() : NULL;
    if (p) p->SimEnd(H.simId);
}

// The quiescent point (IFgProvider::WantsQuiescent / Quiescent). Presenting thread, chain pinned, top of the Present
// hook - the one place where "any GPU activity is finished" (xell guide :131) can be shown rather than hoped for:
//   - PROVEN here: the queue was empty at one instant (the drain below), the pin keeps the render thread's Prepare
//     out (so nothing of OURS is handed to Unity meanwhile), and the host has queued nothing of this Present yet.
//   - ASSUMED, not proven (docs\DESIGN.md "Present thread = submission thread"): that this thread is the one that puts
//     Unity's lists on the queue, so that while it sits in this hook nothing else gets there - not Unity's next frame,
//     not a list handed to ExecuteCommandList earlier. The hook patches the DXGI vtable and has no say in which
//     thread calls Present or ExecuteCommandLists. A drain from the main thread could not even claim this much: its
//     Signal only covers what was queued by then (D3D12Ring::DrainQueue).
//   - MEASURED because of that: the queue is probed again right behind the provider's call (quiesceBusy), Unity's
//     next-frame-fence value is watched across every hooked Present (fenceMoved), and the presenting / preparing
//     thread ids are tracked (presentTid / prepareTid) - all in Fg_Status and the log.
// INVARIANT: Quiescent() is called only after DrainQueue() returned true in this very call, on this thread, with no
// submission in between. No proof (timeout, failed Signal, quarantined ring) = no call: the request stays with the
// provider and is tried again kQuiesceRetry presents later, so a stuck GPU costs one bounded wait per retry, not one
// per Present. The copy ring's fence serves as the proof (only this thread touches that ring); with a provider that
// presents on a queue of its own that fence is signalled on THAT queue (CopyBackBuffer), so such a provider is not
// served at all - none asks.
enum { kQuiesceMs = 100, kQuiesceRetry = 30 };
static void ServeQuiescent(Chain& c)
{
    if (!c.prov->WantsQuiescent()) { H.quiesceSkip = 0; return; }
    if (H.quiesceSkip > 0) { --H.quiesceSkip; return; }
    const ULONGLONG t0 = GetTickCount64();
    const char* why = NULL;
    IUnityGraphicsD3D12v5* unity = g_unityD3D12;
    const UINT64 next0 = unity ? unity->GetNextFrameFenceValue() : 0;
    if (c.prov->PresentQueue()) why = "the provider presents on a queue of its own";
    else if (!c.copy.DrainQueue(kQuiesceMs)) why = "no proof that Unity's queue retired";
    const ULONGLONG t1 = GetTickCount64();
    // The drain proves the queue empty at one instant. That it STAYS empty across the provider's call rests on this
    // thread being the one that submits Unity's lists - an assumption (header above), so it is measured, not trusted:
    // one Signal right before the call (control: an empty queue completes it at once) and one right behind it. Control
    // complete + the second one not = somebody put work on the queue while the SDK call ran. Unity's next-frame-fence
    // value is read around the whole point for the same reason. Neither can undo the call; both are counted and logged.
    const bool calm0 = !why && c.copy.SignalDone();
    if (!why && !c.prov->Quiescent()) why = "the provider could not act";
    if (!why) {
        const bool calm1 = c.copy.SignalDone();
        const UINT64 next1 = unity ? unity->GetNextFrameFenceValue() : 0;
        if (calm0 && !calm1) InterlockedIncrement(&H.nQuiesceBusy);
        ID3D12Fence* ff = unity ? unity->GetFrameFence() : NULL;
        const LONG n = InterlockedIncrement(&H.nQuiesce);
        FgLog("host: quiescent point #%ld served: Unity's queue drained in %llu ms (own fence %llu; Unity frame fence done %llu, next %llu -> %llu; queue empty before the call %d, behind it %d%s) tid=%u", n,
              (unsigned long long)(t1 - t0), (unsigned long long)c.copy.fenceVal,
              (unsigned long long)(ff ? ff->GetCompletedValue() : 0), (unsigned long long)next0, (unsigned long long)next1, calm0 ? 1 : 0, calm1 ? 1 : 0,
              calm0 && !calm1 ? " - WORK REACHED THE QUEUE DURING THE CALL" : "", (unsigned)GetCurrentThreadId());
        return;
    }
    H.quiesceSkip = kQuiesceRetry;
    const LONG n = InterlockedIncrement(&H.nQuiesceFail);
    if (n <= 8 || n % 64 == 0)
        FgLog("host: quiescent point refused (#%ld, %llu ms): %s - the provider's request stays pending, retry in %d presents", n,
              (unsigned long long)(GetTickCount64() - t0), why, (int)kQuiesceRetry);
}

unsigned FgHostPreparedCount(void) { return (unsigned)H.nPrepared; }
void FgNoteHudless(bool tagged) { InterlockedExchange(&H.hudless, tagged ? 1 : 0); }
int FgHostHudless(void) { return (int)H.hudless; }
void FgHostTestFailPrepare(int on) { InterlockedExchange(&g_testFailPrepare, on ? 1 : 0); }

bool FgTestSdkFail(const char* what)
{
    if (!g_testSdk.on) return false;
    if (strcmp(g_testSdk.what, what)) return false;
    if (g_testSdk.skip > 0) { InterlockedDecrement(&g_testSdk.skip); return false; }   // skip=N: the first N go through
    if (g_testSdk.left >= 0 && InterlockedDecrement(&g_testSdk.left) < 0) { InterlockedExchange(&g_testSdk.left, 0); return false; }
    const LONG n = InterlockedIncrement(&g_testSdk.hits);
    if (n <= 16 || n % 256 == 0) FgLog("host: TEST forced SDK failure #%ld: %s", n, what);
    return true;
}

bool FgMainPacing(void) { return g_mainPacing; }
void FgSetMainPacing(bool on) { g_mainPacing = on; }

const char* FgHostLatency(void)
{
    static char buf[512];
    buf[0] = 0;
    IFgProvider* p = NULL;
    { Locked lk; if (H.state == kLive) p = H.c.prov; }
    if (p) p->Stats(buf, (int)sizeof(buf));
    return buf;
}

// Main thread. The provider's switch FgHostSetEnabled asked for (H.provWant), issued with the chain pinned by THIS
// thread: a Prepare or Present in flight has left, none enters. DLSS-G's SetEnabled(false) is slDLSSGSetOptions, which
// the render thread also issues from Prepare and which must be ordered against Present ("NOT thread safe",
// sl_dlss_g.h:207; ProgrammingGuideDLSS_G.md:486-494).
// INVARIANT: IFgProvider::SetEnabled runs on a LIVE chain only between a successful PinMain and its Unpin. No pin (a
// thread still inside after `waitMs`) = the switch is not issued; it stays in H.provWant with the gate CLOSED
// (H.enabled 0: no Prepare and no Present can enter any more, so the thread inside is the last one) and FgHostPump
// tries again every frame. A chain that is no longer live needs nothing: DestroyChain switches its provider off
// itself, detached and unpinned. True = nothing is pending.
static bool ApplyEnabled(DWORD waitMs)
{
    if (H.provWant < 0) return true;
    if (!PinMain(waitMs)) {
        Locked lk;
        if (H.state == kLive) return false;            // still someone inside: deferred
        H.provWant = -1;
        return true;
    }
    const int on = H.provWant;
    H.provWant = -1;
    H.c.prov->SetEnabled(on != 0);
    H.provOn = on;
    { Locked lk; H.enabled = on; }                     // the gate opens only behind the provider's own switch
    Unpin(NULL);
    FgLog("host: enabled=%d", on);
    return true;
}

void FgHostSetEnabled(int on)
{
    enum { kEnableWaitMs = 100 };
    on = on ? 1 : 0;
    // FrameGen.Apply repeats the wish on every config change: nothing to do when it already holds.
    if (H.provWant < 0 && H.provOn == on) {
        Locked lk;
        if (H.state != kLive || H.enabled == on) return;
    }
    { Locked lk; H.enabled = 0; }                      // closed in both directions until the provider has switched
    WakeAllConditionVariable(&g_unpinned);             // a waiting Pin re-reads `enabled` now
    H.provWant = on;
    if (!ApplyEnabled(kEnableWaitMs))
        FgLog("host: enabled=%d deferred: a thread is still inside the chain after %d ms - the gate stays closed, the pump retries", on, (int)kEnableWaitMs);
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
        H.slot.paceId = H.paceId;       // the frame the main thread is in (FgHostFrameBegin)
        H.slot.epoch = H.state == kLive ? H.epoch : 0;   // the chain this frame is fed to; none = no event of it may act
        H.fed = 1;
        r = &H.ring[H.ringIdx++ % kFrameRing];
        *r = H.slot;
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

// Render thread, EVERY exit of FgHostPrepare once the event's outcome is final: the prepare event of pace id `id`
// has run. INVARIANT: no pace id a prepare event was issued for stays "event still to come" after that event
// returned - whether it prepared the frame, was refused, or never got the pin (Pin timed out behind a Present, the
// gate closed). Without it such an id stopped the provider's marker walk (ProviderXess::OpenSkipped) for the whole
// idle stretch once the feed ended. Plain data, no pin needed; the provider hears of it from a pinned thread
// (FgHostPrepare's own tail, FgHostOnPresent). Written BEFORE the pin is dropped on the pinned path: a Present that
// pins next must already see it, and must never see it while the event can still prepare the frame.
// INVARIANT (chain epoch): an id is recorded only for the generation that issued it - under the lock the epoch moves
// under, so an event of a destroyed chain can never declare ids of the next one settled (their numbering restarts).
static void EventRan(unsigned epoch, unsigned id)
{
    Locked lk;
    if (!epoch || epoch != H.epoch) return;
    if ((LONG)id > H.evDone) H.evDone = (LONG)id;
}

void FgHostPrepare(const FgFrame* ev)
{
    // A second prepare event of a frame whose inputs were already taken (the bound camera rendered twice inside one
    // frame; the managed driver feeds one frame once, this is the backstop for a paced provider): the first one stands.
    // Checked BEFORE the pin and the reset below - Pin(true) would wait its full timeout for a Present that cannot
    // come before this event returns, and the reset would throw the prepared frame away (the provider then refuses
    // the duplicate id and the frame is presented the idle way).
    // CHAIN EPOCH first of all: this event was queued on the main thread and the chain it was queued for may be gone -
    // FrameGen.Apply releases and rebuilds the chain on a multiplier change without waiting for the render thread's
    // queue. Its pace id then numbers frames of a provider that no longer exists, and the new chain's ids start again
    // at 1. INVARIANT: an event whose block carries another generation than H.epoch touches NOTHING - not the
    // duplicate filter, not the pin, not `prepared`, not EventRan, not the provider. The block is copied under the
    // lock the main thread writes the ring under, so a slot being overwritten is never read half-way.
    FgFrame e;
    unsigned epochNow;
    bool dup = false;
    {
        Locked lk;
        e = ev ? *ev : H.slot;
        epochNow = H.epoch;
        dup = e.epoch && e.epoch == epochNow && e.paceId && e.paceId == H.lastEvPace;
    }
    const unsigned evPace = ev ? e.paceId : 0;
    bool stale = !e.epoch || e.epoch != epochNow;
    if (!stale) {
        if (dup && ev) { InterlockedIncrement(&H.nDup); return; }   // its first event settled it
        // The wait for the pin can outlast a teardown + rebuild: Pin re-checks the generation after every wake.
        if (!Pin(true, e.epoch, &stale) && !stale) { EventRan(e.epoch, evPace); return; }
    }
    if (stale) {
        const LONG n = InterlockedIncrement(&H.nStale);
        if (n <= 8 || n % 64 == 0) FgLog("host: stale prepare event dropped (#%ld): block of chain epoch %u (pace id %u, frame %llu), live epoch %u", n, e.epoch, e.paceId, e.frameId, epochNow);
        return;
    }
    H.lastEvPace = evPace;
    const DWORD tid = GetCurrentThreadId();
    if (tid != H.prepareTid) { if (H.prepareTid) InterlockedIncrement(&H.nPrepareTidSw); H.prepareTid = tid; }
    if (!(H.tidLogged & 1)) { H.tidLogged |= 1; FgLog("host: first prepare tid=%u", (unsigned)tid); }
    // A new frame supersedes whatever is still unpresented: an earlier success nobody presented, or a token a skipped
    // Present (GetBuffer/copy failed) left behind. Reset BEFORE the input checks, so a frame with a missing input or a
    // failing Prepare leaves prepared = 0 and no stale DLSS-G token that a later Present would pop for the wrong frame.
    H.prepared = 0;
    H.twinsOk = 0;
    InterlockedIncrement(&H.nPrepare);
    H.c.prov->DropPrepared();
    // The pass-through provider has nothing to prepare. Nothing is declared to Unity: providers read the
    // shim-owned twins (COMMON at rest), never the Unity RTs - declaring those as NON_PIXEL_SHADER_RESOURCE made
    // Unity transition them under the upscaler's own barriers (debug layer id=527 on every frame).
    if (H.cur.hudless && H.cur.depth && H.cur.mv && H.cur.epoch == H.epoch) {   // the slot's block is of THIS chain too
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
                const FgFrame f = PairFrame(ev ? e : H.cur, &paired);
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
                if (prepared && g_testFailPrepare) { H.c.prov->DropPrepared(); prepared = false; }
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
        if (ok) InterlockedIncrement(&H.nPrepared);
    }
    EventRan(e.epoch, evPace);
    if (H.evDone) H.c.prov->PrepareDone((unsigned)H.evDone);   // this event's id and any that gave up without the pin
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

// Pinned threads (Prepare): armed, and armed on the chain this thread has pinned - a capture armed on a chain that was
// rebuilt since is nobody's (FgHostCapture may arm a new one at once).
bool FgCaptureArmed(void) { return InterlockedCompareExchange(&g_cap.state, 1, 1) == 1 && g_cap.epoch == H.epoch; }

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
    if (!dir) return 0;
    // Armed with no Prepare following (frames on hold), or a chain that died between the notes and its Present: nobody
    // would ever finish that capture, so a stale one may be armed again. The notes (Prepare) and the write (OnPresent)
    // both run PINNED, so arming under the lock with nobody pinned can never race either of them: a writer still
    // inside CaptureWrite keeps the pin and the arm waits (bounded) or is refused.
    enum { kCaptureStaleMs = 2000 };
    Locked lk;
    const ULONGLONG until = GetTickCount64() + kPinWaitMs;
    for (;;) {
        if (H.state != kLive) return 0;
        if (!H.pinned) break;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return 0;
        SleepConditionVariableSRW(&g_unpinned, &g_lock, (DWORD)(until - now), 0);
    }
    LONG s = InterlockedCompareExchange(&g_cap.state, 0, 0);
    if ((s == 1 || s == 2) && g_cap.epoch == H.epoch && GetTickCount64() - g_cap.armedAt < kCaptureStaleMs) return 0;
    wcsncpy_s(g_cap.dir, MAX_PATH, dir, _TRUNCATE);
    g_cap.len = 0;
    g_cap.epoch = H.epoch;
    g_cap.armedAt = GetTickCount64();
    InterlockedExchange(&g_cap.state, 1);
    return 1;
}

int FgHostCaptureStatus(void) { return (int)InterlockedCompareExchange(&g_cap.state, 0, 0); }

// TEST ONLY, RENDERFORGE_FG_TEST_PRESENT_DELAY="ms,every": every very-th hooked Present of a live chain is held back
// ms before it pins - longer than kPinWaitMs the prepare of the next frame times out and supersedes the unpresented
// one (presentStalled), the path a busy or occluded presenting thread takes on its own.
static void TestPresentDelay()
{
    static int ms = -1, every = 0; static unsigned n = 0;
    if (ms < 0) { char v[32] = {}; ms = 0; if (GetEnvironmentVariableA("RENDERFORGE_FG_TEST_PRESENT_DELAY", v, sizeof(v)) && sscanf_s(v, "%d,%d", &ms, &every) == 2 && ms > 0 && every > 0) FgLog("host: TEST RENDERFORGE_FG_TEST_PRESENT_DELAY=%d ms every %d presents", ms, every); else ms = 0; }
    if (ms > 0 && FgHostAlive() && ++n % (unsigned)every == 0) Sleep((DWORD)ms);
}

// Always-on diagnostic for the assumption "the thread that calls Present is the thread that submits Unity's lists"
// (docs\DESIGN.md "Present thread = submission thread"): Unity's next-frame-fence value read when the Present hook
// enters the host and when it leaves. Unity moves that value when it closes a frame / flushes its queue. It moving
// WHILE the presenting thread sits in here means another thread did that - the assumption is then false for this
// process. Two interface calls per Present, no lock, no wait.
struct FenceWatch
{
    UINT64 next;
    FenceWatch() { IUnityGraphicsD3D12v5* u = g_unityD3D12; next = u ? u->GetNextFrameFenceValue() : 0; }
    ~FenceWatch()
    {
        IUnityGraphicsD3D12v5* u = g_unityD3D12;
        if (!u || u->GetNextFrameFenceValue() == next) return;
        const LONG n = InterlockedIncrement(&H.nFenceMoved);
        if (n <= 4 || n % 1024 == 0) FgLog("host: Unity's next frame fence value moved %llu -> %llu while tid %u was inside the Present hook (#%ld) - Present is NOT the only thread that advances Unity's frames",
                                           (unsigned long long)next, (unsigned long long)u->GetNextFrameFenceValue(), (unsigned)GetCurrentThreadId(), n);
    }
};

// The generation of the chain whose shadow Present this thread just made (FG_PRESENT_SHADOW): the hook reports the
// result of Unity's own Present after the pin is gone, and a chain built meanwhile must not be detached for it.
// Per thread: nothing here assumes there is only one presenting thread.
static thread_local unsigned t_presentEpoch;

int FgHostOnPresent(IDXGISwapChain* app, UINT syncInterval, UINT flags)
{
    FenceWatch watch;
    TestPresentDelay();
    if (!Pin(false)) return FG_PRESENT_FORWARD;
    const DWORD tid = GetCurrentThreadId();
    if (tid != H.presentTid) { if (H.presentTid) InterlockedIncrement(&H.nPresentTidSw); H.presentTid = tid; }
    if (!(H.tidLogged & 2)) { H.tidLogged |= 2; FgLog("host: first hooked present tid=%u", (unsigned)tid); }
    Chain& c = H.c;
    if (app != (IDXGISwapChain*)c.app || !c.shadow) { Unpin(NULL); return FG_PRESENT_FORWARD; }
    // Presents of OUR chain's app are arriving again: prepares may wait for them. Only here - a Present of some other
    // chain consumes nothing, and clearing the flag for it made every following prepare wait its full timeout.
    { Locked lk; H.presentStalled = 0; }

    // Before anything of this Present is queued: an SDK call that waits for the GPU to be idle (ServeQuiescent).
    ServeQuiescent(c);
    // Prepare events that ran since the last pinned thread told the provider, including those that gave up without
    // the pin (EventRan): their ids can be closed now.
    if (H.evDone) c.prov->PrepareDone((unsigned)H.evDone);

    // No frame prepared since the last present: the provider is not driven at all this frame (no copy, no shadow
    // Present, no vendor call), the hook forwards Unity's Present unchanged, and the child hides on the first such frame.
    InterlockedIncrement(&H.nPresent);
    if (!H.prepared) {
        InterlockedIncrement(&H.nIdle);
        if (++H.idle == 1) { H.hidden = 1; FgWndShow(false); FgLog("host: idle: no prepared frame - child hidden, provider untouched, Unity presents on its own"); }
        // A real idle stretch (a menu, a loading screen), not one skipped frame: tell the provider (DLSS-G goes eOff).
        if (H.idle == kIdleNotify) c.prov->Idle();
        // A latency SDK that opened the present of skipped frames gets their end right after the forwarded Present:
        // the pin stays with this thread until FgHostAfterForwardedPresent.
        if (c.prov->UnpreparedPresent()) return FG_PRESENT_HELD;
        Unpin(NULL);
        return FG_PRESENT_FORWARD;
    }
    H.prepared = 0;
    if (H.hidden) { H.hidden = 0; FgWndShow(true); FgLog("host: frames resumed after %lld idle presents - child shown", H.idle); }
    H.idle = 0;

    // Only the present flags the shadow chain opted into: tearing iff created with ALLOW_TEARING (and
    // Unity asked for it this frame); never TEST / DO_NOT_SEQUENCE / RESTART, which belong to Unity's chain.
    UINT pf = (c.scFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? (flags & DXGI_PRESENT_ALLOW_TEARING) : 0;
    if (pf && syncInterval) pf = 0;                    // tearing needs sync interval 0

    ID3D12Resource* src = NULL;
    if (FAILED(c.app->GetBuffer(c.app->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&src)) || !src) { Unpin(NULL); return FG_PRESENT_FORWARD; }

    // Constants noted by a Prepare of THIS chain only: the notes of a chain that was rebuilt since describe frames
    // this Present does not carry.
    if (InterlockedCompareExchange(&g_cap.state, 2, 2) == 2) InterlockedExchange(&g_cap.state, g_cap.epoch == H.epoch && CaptureWrite(src) ? 3 : -1);

    // Generated frame(s) first - they sit between the previous real frame and this one - then the real one.
    // The provider presents its own frames on the shadow chain, so the current back buffer is fetched after.
    int generated = c.prov->Generate(H.cur, src, c.shadow, syncInterval, pf);

    ID3D12Resource* dst = NULL;
    if (FAILED(c.shadow->GetBuffer(c.shadow->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&dst)) || !dst) {
        src->Release();
        Unpin(NULL);
        return FG_PRESENT_FORWARD;
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
    if (!copied) { H.lastError = FG_ERR_NO_SWAPCHAIN; Unpin("back-buffer copy failed"); return FG_PRESENT_FORWARD; }
    // A provider presenting on its own queue (DLSS-G) reads the back buffer we just wrote on ITS queues right after
    // Present, and nothing fences that read against our copy (measured with the debug layer on, DLSS-G 2x/3x/4x:
    // ~35-48/s id=1047 "fake-swapchain-buffer still referenced by cmdQ.dlssg / cmdQ.game" without this wait, 0 with
    // it, fps unchanged). So the copy retires on the CPU before Present - the same one-frame-in-flight cadence Reflex
    // low-latency imposes anyway. ponytail: a GPU-side handoff would need DLSS-G's own fence, which it does not expose.
    if (c.prov->PresentQueue() && c.copy.fence && c.copy.WaitOn(c.copy.fence, c.copy.fenceVal) != WAIT_OBJECT_0) {
        H.lastError = FG_ERR_NO_SWAPCHAIN;
        Unpin("copy fence wait failed");
        return FG_PRESENT_FORWARD;
    }
    c.prov->BeforePresent();                           // the copy is the last write into the shadow back buffer

    InterlockedIncrement(&g_presentNo);
    const ULONGLONG presentT0 = GetTickCount64();
    InterlockedExchange64(&g_presentSince, (LONGLONG)presentT0);
    HRESULT hr = c.shadow->Present(syncInterval, pf);
    InterlockedExchange64(&g_presentSince, 0);
    if (InterlockedExchange(&g_presentFlagged, 0))
        FgLog("host: WATCHDOG shadow Present #%ld returned 0x%08X after %llu ms", (long)g_presentNo, (unsigned)hr, (unsigned long long)(GetTickCount64() - presentT0));
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
        return FG_PRESENT_FORWARD;
    }
    // Unity's chain MUST still be presented (by the hook, once): Unity 2019.4 tracks its own back-buffer index
    // and renders the next frame into buffer (i+1)%n, while a flip-model chain only advances its index on
    // Present. Skipping it made Unity write to a non-current back buffer (debug layer id=907) and DXGI removed
    // the device with DXGI_ERROR_ACCESS_DENIED (0x887A002B) on the second frame.
    FgPresentedAdd(1 + (generated < 0 ? 0 : generated));
    t_presentEpoch = H.epoch;                          // still pinned: the generation this Present belonged to
    Unpin(NULL);
    return FG_PRESENT_SHADOW;
}

void FgHostAfterUnityPresent(HRESULT hr)
{
    if (FAILED(hr) || hr == DXGI_STATUS_OCCLUDED) {
        char why[64];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "unity Present 0x%08X", (unsigned)hr);
        H.lastError = FG_ERR_NO_SWAPCHAIN;
        Detach(why, t_presentEpoch);                   // that chain only: one built since is not detached for it
    }
}

// The presenting thread, still holding the pin FgHostOnPresent returned FG_PRESENT_HELD with: Unity's Present of an
// unprepared frame has just returned. The pin was never dropped, so the provider is the one UnpreparedPresent ran on.
void FgHostAfterForwardedPresent(void)
{
    H.c.prov->AfterUnpreparedPresent();
    Unpin(NULL);
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
    WakeAllConditionVariable(&g_unpinned);             // a Pin waiting on a chain that just went away gives up at once
    ApplyEnabled(0);                                   // a provider switch that found a thread inside the chain: try again, no wait
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
        "provider=%s enabled=%d multiplier=%u chain=%s child=%p childHr=0x%08X hit=%d focus=%s fg=%d shadow=%p out=%ux%u flags=0x%X caps=0x%X lastError=%d presentHr=0x%08X presented=%lld fps=%d frameId=%llu idle=%lld prepare=%ld/%ld presents=%ld idled=%ld waited=%ld maxWaitMs=%u stalls=%ld dup=%ld quiesce=%ld/%ld pendingEnable=%d epoch=%u stale=%ld presentTid=%lu/%ld prepareTid=%lu/%ld fenceMoved=%ld quiesceBusy=%ld%s%s",
        prov, H.enabled, H.multiplier,
        state == kNone ? "-" : "child", child, (unsigned)H.childHr, w->hit, focus,
        w->foreground == game ? 1 : 0,
        shadow, H.outW, H.outH, scFlags,
        caps, H.lastError, (unsigned)H.lastPresentHr, FgPresentCount(), FgPresentedFps(),
        frameId, state == kLive ? H.idle : 0LL, (long)H.nPrepared, (long)H.nPrepare, (long)H.nPresent, (long)H.nIdle, (long)H.nWaited,
        H.maxWaitMs, (long)H.nStall, (long)H.nDup, (long)H.nQuiesce, (long)H.nQuiesceFail, H.provWant,
        H.epoch, (long)H.nStale, (unsigned long)H.presentTid, (long)H.nPresentTidSw, (unsigned long)H.prepareTid, (long)H.nPrepareTidSw,
        (long)H.nFenceMoved, (long)H.nQuiesceBusy,
        H.reason ? " reason=" : "", H.reason ? H.reason : "");
    return H.status;
}
