// Source-linked lifecycle proof: real WARP COM resources/fences; only FG vendor/window seams are mocked.
#include "../FgHost.cpp"
#include "../FgLatch.h"
#include <stdlib.h>
IUnityGraphicsD3D12v5* g_unityD3D12 = NULL;
namespace RfDbg { void Log(const char*, ...) {} bool On() { return false; } void Removed(ID3D12Device*, const char*) {} }
void FgLog(const char*, ...) {}
void FgLogInit(const wchar_t*) {}
bool FgHookInstall(ID3D12CommandQueue*) { return true; }
void FgHookHide() {}
void FgHookShow() {}
IDXGISwapChain3* FgAppAcquire(HWND*, DXGI_SWAP_CHAIN_DESC1*) { return NULL; }
bool FgAppIs(IDXGISwapChain*) { return true; }
HWND FgWndCreate(HWND) { return NULL; }
void FgWndDestroy() {}
void FgWndShow(bool) {}
const FgWndProbe* FgWndProbeNow() { static FgWndProbe p = {}; return &p; }
long long FgPresentCount() { return 0; }
int FgPresentedFps() { return 0; }
void FgPresentedAdd(int) {}
const OwnedSet12* FgOwned12() { return NULL; }
IFgProvider* MakeFgProviderFsr() { return NULL; }
IFgProvider* MakeFgProviderXess() { return NULL; }
IFgProvider* MakeFgProviderStreamline() { return NULL; }
static int checks;
static void Check(bool value, const char* label) { ++checks; if (!value) { fprintf(stderr, "FAIL %s\n", label); exit(1); } }
static ULONG Refs(IUnknown* p) { p->AddRef(); return p->Release(); }
struct PendingProvider : ProviderNone {
    bool ready = false; int calls = 0;
    bool Destroy(bool force) override { ++calls; Check(!force, "retries never force destruction"); return ready; }
};
// Review 2 (2026-10-12): what the host asks of a provider at its deferred-request points.
static ID3D12CommandQueue* s_queue;
struct ProbeProvider : ProviderNone {
    int setEnabled = 0, lastOn = -1, quiescent = 0, unprepared = 0, after = 0; unsigned done = 0; bool wants = false, hold = false;
    int refuse = 0;                     // review 3: the next `refuse` Quiescent calls report "the SDK refused"
    ID3D12Fence* block = NULL;          // review 3: Quiescent leaves the queue waiting on this fence = work behind the SDK call
    void SetEnabled(bool on) override { ++setEnabled; lastOn = on ? 1 : 0; }
    bool WantsQuiescent() override { return wants; }
    bool Quiescent() override
    {
        ++quiescent;
        if (refuse > 0) { --refuse; return false; }
        if (block) s_queue->Wait(block, 1);
        wants = false; return true;
    }
    void PrepareDone(unsigned id) override { if (id > done) done = id; }
    bool UnpreparedPresent() override { ++unprepared; return hold; }
    void AfterUnpreparedPresent() override { ++after; }
};
// A Unity D3D12 interface over a real WARP queue: D3D12Ring::DrainQueue signals and waits on it.
static ID3D12Device* s_dev; static ID3D12Fence* s_frameFence;
static ID3D12Device* UNITY_INTERFACE_API FakeDevice() { return s_dev; }
static ID3D12Fence* UNITY_INTERFACE_API FakeFrameFence() { return s_frameFence; }
static UINT64 UNITY_INTERFACE_API FakeNextFrame() { return 1; }
static ID3D12CommandQueue* UNITY_INTERFACE_API FakeQueue() { return s_queue; }
// D3D12Ring::WaitOn as it stood at da602a4: the wait result alone decides.
static DWORD OldWaitOn(HANDLE ev, ID3D12Fence* f, UINT64 value, DWORD ms)
{
    if (f->GetCompletedValue() >= value) return WAIT_OBJECT_0;
    if (FAILED(f->SetEventOnCompletion(value, ev))) return WAIT_FAILED;
    return WaitForSingleObject(ev, ms);
}
static DWORD WINAPI SignalThreeLater(void* f) { Sleep(120); ((ID3D12Fence*)f)->Signal(3); return 0; }
static DWORD WINAPI PrepareThread(void* ev) { FgHostPrepare((const FgFrame*)ev); return 0; }
int main()
{
    Dlss_BeginRelease();
    Check(Dlss_ReleaseStatus() == 0, "release acknowledgement starts queued");
    ((void(__stdcall*)(int))Dlss_GetRenderEventFunc())(DLSS_EV_RELEASE);
    Check(Dlss_ReleaseStatus() == 1, "no-backend release still acknowledges completion");
    Check(Dlss_Shutdown() == 1 && Dlss_ReleaseNow() == 1, "empty backend shutdown idempotent");
    IDXGIFactory4* factory = NULL; IDXGIAdapter* warp = NULL; ID3D12Device* device = NULL;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "factory");
    Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP");
    Check(SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "device");
    auto a = OwnedSet12::Make(device, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false, L"A");
    auto b = OwnedSet12::Make(device, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, false, L"B");
    Check(a && b, "textures");
    ULONG ar = Refs(a), br = Refs(b);
    PendingProvider provider;
    H.state = kLive; H.enabled = 1; H.c.prov = &provider;
    FgFrame f = {}; f.hudless = a; FgHostSetFrame(f);
    Check(Refs(a) == ar + 1, "slot retains");
    Check(Pin(false), "pin");
    Check(!Pin(false), "nested pin refused");
    f.hudless = b; FgHostSetFrame(f);
    Check(Refs(a) == ar + 1 && H.cur.hudless == a, "slot replacement keeps pinned frame alive");
    Check(!FgHostShutdown() && !FgHostRetired(), "pinned shutdown pending");
    Check(Refs(b) == br + 1 && provider.calls == 0, "pending retains slot and provider");
    Unpin(NULL);
    Check(Refs(a) == ar, "unpin releases snapshot once");
    for (int i = 0; i < 150; ++i) Check(!FgHostShutdown(), "refused provider stays pending");
    Check(Refs(b) == br + 1 && H.c.prov == &provider, "refused provider retains owner");
    provider.ready = true;
    Check(FgHostShutdown() && FgHostRetired(), "retry completes");
    Check(Refs(b) == br, "completed shutdown releases slot once");
    Check(FgHostShutdown() && Refs(b) == br, "repeat shutdown idempotent");
    H.state = kLive; H.enabled = 1; H.c.prov = &provider; FgHostSetFrame(f);
    Check(Pin(false), "re-enable after completed shutdown"); Unpin(NULL);
    Check(FgHostShutdown() && Refs(b) == br, "second lifecycle balanced");

    // ---- review 2 (2026-10-12) ------------------------------------------------------------------------------
    // D3D12Ring::WaitOn: a registration left armed by a timed-out wait must not pass for the retirement of a later value.
    {
        ID3D12Fence* f1 = NULL; ID3D12Fence* f2 = NULL;
        Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f1))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f2))), "wait fences");
        HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        Check(OldWaitOn(ev, f1, 1, 10) == WAIT_TIMEOUT, "old wait: unretired 1 times out, registration stays armed");
        f1->Signal(1); Sleep(60);
        Check(OldWaitOn(ev, f1, 2, 200) == WAIT_OBJECT_0 && f1->GetCompletedValue() == 1, "defect reproduced: the da602a4 wait reports 2 retired on the stale wake for 1");
        D3D12Ring w;
        Check(w.WaitOn(f2, 1, 10) == WAIT_TIMEOUT, "unretired 1 times out");
        f2->Signal(1); Sleep(60);
        ULONGLONG t0 = GetTickCount64();
        Check(w.WaitOn(f2, 2, 200) == WAIT_TIMEOUT && GetTickCount64() - t0 >= 150 && f2->GetCompletedValue() == 1, "stale wake for 1 is not retirement of 2: the wait runs to its original deadline");
        f2->Signal(2); Sleep(60);                          // the timed-out wait for 2 left ITS registration armed: the event is set again
        HANDLE th = CreateThread(NULL, 0, SignalThreeLater, f2, 0, NULL);
        t0 = GetTickCount64();
        Check(w.WaitOn(f2, 3, 5000) == WAIT_OBJECT_0 && f2->GetCompletedValue() >= 3 && GetTickCount64() - t0 >= 80, "the wait goes on past a stale wake until 3 has really retired");
        Check(w.WaitOn(f2, 3, 0) == WAIT_OBJECT_0 && w.WaitOn(f2, 4, 0) == WAIT_TIMEOUT, "zero-wait polls read the fence");
        WaitForSingleObject(th, INFINITE); CloseHandle(th);
        CloseHandle(w.waitEvent); CloseHandle(ev); f1->Release(); f2->Release();
    }
    // Fg_Capture: a readback list still in flight keeps the twins it reads.
    {
        auto t = OwnedSet12::Make(device, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false, L"cap");
        ID3D12Fence* cf = NULL;
        Check(t && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cf))), "capture objects");
        ULONG tr = Refs(t);
        g_capPending.live = true; g_capPending.fence = cf; g_capPending.src[0] = t; t->AddRef();
        Check(!CapturePendingRetire() && Refs(t) == tr + 1 && g_capPending.live, "in-flight capture list keeps its source twin");
        cf->Signal(1);
        Check(CapturePendingRetire() && Refs(t) == tr && !g_capPending.live, "retired capture list releases it exactly once");
        Check(CapturePendingRetire(), "nothing pending is idempotent");
        t->Release();
    }
    // The host's deferred requests, on a live chain with a counting provider and a real queue behind the Unity interface.
    {
        D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&s_queue))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_frameFence))), "queue");
        s_dev = device;
        static IUnityGraphicsD3D12v5 fake = {};
        fake.GetDevice = FakeDevice; fake.GetFrameFence = FakeFrameFence; fake.GetNextFrameFenceValue = FakeNextFrame; fake.GetCommandQueue = FakeQueue;
        g_unityD3D12 = &fake;
        ProbeProvider pp;
        H.state = kLive; H.enabled = 0; H.c.prov = &pp; H.provWant = -1; H.provOn = -1; H.evDone = 0; H.quiesceSkip = 0; H.prepared = 0; H.idle = 0;
        H.nQuiesce = H.nQuiesceFail = 0;
        H.epoch = 5;                                                  // a chain generation, as FgHostInit publishes one
        H.c.copy.Attach(device);

        // Provider on/off: only between a successful PinMain and its Unpin.
        FgHostSetEnabled(1);
        Check(pp.setEnabled == 1 && pp.lastOn == 1 && H.enabled == 1 && H.pinned == 0, "enable: provider switched under the main pin, gate opened behind it");
        FgHostSetEnabled(1);
        Check(pp.setEnabled == 1, "a repeated wish is a no-op");
        Check(Pin(false), "a thread inside the chain");
        FgHostSetEnabled(0);
        Check(pp.setEnabled == 1 && H.enabled == 0 && H.provWant == 0 && H.pinned == 1, "disable with a thread inside: provider NOT switched, gate closed, wish pending");
        Check(FgHostPump() == 1 && pp.setEnabled == 1 && H.provWant == 0, "pump with the thread still inside: still deferred");
        Unpin(NULL);
        FgHostPump();
        Check(pp.setEnabled == 2 && pp.lastOn == 0 && H.provWant == -1 && H.enabled == 0 && H.pinned == 0, "pump after the thread left: switched off under the pin");
        FgHostSetEnabled(1);
        Check(pp.setEnabled == 3 && pp.lastOn == 1 && H.enabled == 1, "enabled again");

        // A prepare event that never gets the pin still counts as run.
        Check(Pin(false), "a Present inside the chain");
        FgFrame e7 = {}; e7.paceId = 7; e7.epoch = 5;
        FgHostPrepare(&e7);
        Check(H.evDone == 7 && pp.done == 0 && H.pinned == 1, "prepare event that timed out on the pin is recorded as run");
        Unpin(NULL);

        // The Present hook's side: quiescent point, settled events, the pin held across a forwarded Present.
        IDXGISwapChain* app = (IDXGISwapChain*)(INT_PTR)0x1000;       // compared, never dereferenced on the idle path
        H.c.app = (IDXGISwapChain3*)app; H.c.shadow = (IDXGISwapChain4*)(INT_PTR)0x2000;
        pp.wants = true; pp.hold = true;
        int r = FgHostOnPresent(app, 0, 0);
        Check(r == FG_PRESENT_HELD && H.pinned == 1 && pp.unprepared == 1 && pp.after == 0, "unprepared Present with opened ids: forwarded with the pin held");
        // Three Signals of the copy ring's fence: the drain (waited for), the control probe before the provider's call
        // and the probe behind it (review 3, ServeQuiescent).
        Check(pp.quiescent == 1 && H.nQuiesce == 1 && H.c.copy.fenceVal == 3 && H.c.copy.fence->GetCompletedValue() >= 1, "quiescent point served behind a real drain of the queue");
        Check(pp.done == 7, "the presenting thread tells the provider of the event that gave up without the pin");
        FgHostAfterForwardedPresent();
        Check(pp.after == 1 && H.pinned == 0, "present end reported after the forwarded Present, pin dropped");
        pp.wants = true; pp.hold = false;
        H.c.copy.trackingLost = true;                                  // no retirement proof to be had
        r = FgHostOnPresent(app, 0, 0);
        Check(r == FG_PRESENT_FORWARD && pp.quiescent == 1 && pp.wants && H.nQuiesceFail == 1 && H.quiesceSkip == kQuiesceRetry && H.pinned == 0,
              "no retirement proof: the SDK call is NOT made, the request stays pending");
        H.c.copy.trackingLost = false;
        for (int i = 0; i < kQuiesceRetry; ++i) FgHostOnPresent(app, 0, 0);
        Check(pp.quiescent == 1, "backoff: no drain per Present while the request waits");
        FgHostOnPresent(app, 0, 0);
        Check(pp.quiescent == 2 && !pp.wants && H.nQuiesce == 2, "served at the retry");
        FgFrame e8 = {}; e8.paceId = 8; e8.epoch = 5;
        FgHostPrepare(&e8);
        Check(H.evDone == 8 && pp.done == 8 && H.pinned == 0, "a prepare event that ran settles its id before it unpins");

        // ---- review 3 (2026-10-12) --------------------------------------------------------------------------
        // Finding 1, the host's half: a provider whose SDK call was refused (Quiescent false) keeps its request - the
        // point is neither counted as served nor dropped, and it is tried again with the same back-off as a failed drain.
        {
            const int q0 = pp.quiescent; const LONG s0 = H.nQuiesce, f0 = H.nQuiesceFail;
            pp.wants = true; pp.hold = false; pp.refuse = 2; H.quiesceSkip = 0;
            FgHostOnPresent(app, 0, 0);
            Check(pp.quiescent == q0 + 1 && pp.wants && H.nQuiesce == s0 && H.nQuiesceFail == f0 + 1 && H.quiesceSkip == kQuiesceRetry, "SDK refusal: not served, the request stays pending, back-off armed");
            for (int i = 0; i < kQuiesceRetry; ++i) FgHostOnPresent(app, 0, 0);
            Check(pp.quiescent == q0 + 1, "SDK refusal: no call per Present during the back-off");
            FgHostOnPresent(app, 0, 0);
            Check(pp.quiescent == q0 + 2 && pp.wants && H.nQuiesceFail == f0 + 2, "SDK refusal: retried after the back-off, refused again, still pending");
            for (int i = 0; i < kQuiesceRetry; ++i) FgHostOnPresent(app, 0, 0);
            FgHostOnPresent(app, 0, 0);
            Check(pp.quiescent == q0 + 3 && !pp.wants && H.nQuiesce == s0 + 1, "SDK accepts at the third try: served once");
            Check(H.nQuiesceBusy == 0, "an empty queue behind the SDK call is not reported busy");
            // Unknown 3: work that reaches the queue while the provider's call runs is noticed (the queue is probed
            // before and behind the call). The queue is left waiting on an unsignalled fence = something in flight.
            ID3D12Fence* blk = NULL;
            Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blk))), "block fence");
            pp.wants = true; pp.block = blk;
            FgHostOnPresent(app, 0, 0);
            Check(!pp.wants && H.nQuiesce == s0 + 2 && H.nQuiesceBusy == 1, "work on the queue behind the SDK call is counted (quiesceBusy)");
            blk->Signal(1);
            Check(H.c.copy.WaitOn(H.c.copy.fence, H.c.copy.fenceVal, 2000) == WAIT_OBJECT_0, "the queue runs on once that work is done");
            pp.block = NULL; blk->Release();
            Check(H.presentTid == GetCurrentThreadId() && H.nPresentTidSw == 0, "one presenting thread: tid recorded, no switch");
        }
        // Finding 2: a prepare event queued for a chain that was destroyed and rebuilt before the render thread ran it.
        {
            auto in = OwnedSet12::Make(device, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, false, L"in");
            Check(in != NULL, "input texture");
            FgFrame fr = {}; fr.hudless = in; fr.depth = in; fr.mv = in; fr.frameId = 100;
            H.prepared = 0; H.presentStalled = 0; H.evDone = 0; H.lastEvPace = 0; pp.done = 0;
            H.paceId = 500;                                           // the old chain's pacer is at id 500
            const FgFrame* evOld = FgHostSetFrame(fr);
            Check(evOld->epoch == 5 && evOld->paceId == 500, "a fed block carries its chain's epoch and pace id");
            // ... the chain is released and rebuilt (FrameGen.Apply, multiplier change); the event is still queued.
            PublishChain(&pp, 6);
            Check(H.epoch == 6 && H.evDone == 0 && H.lastEvPace == 0, "a new generation is published with a clean slate");
            const LONG prep0 = H.nPrepare;
            FgHostPrepare(evOld);
            Check(H.nStale == 1 && H.evDone == 0 && pp.done == 0, "stale event: the new chain's pending ids are NOT declared settled");
            Check(H.prepared == 0 && H.nPrepare == prep0 && H.lastEvPace == 0 && H.pinned == 0, "stale event: nothing prepared, nothing pinned, the duplicate filter untouched");
            EventRan(5, 900);
            Check(H.evDone == 0, "EventRan of another generation records nothing");
            // The same event with a thread inside the chain: dropped at once, it does not even wait for the pin.
            Check(Pin(false), "a Present inside the new chain");
            ULONGLONG t0 = GetTickCount64();
            FgHostPrepare(evOld);
            Check(GetTickCount64() - t0 < 50 && H.nStale == 2 && H.evDone == 0 && H.pinned == 1, "stale event behind a Present: dropped without the pin wait");
            // An event of the LIVE generation that waits for the pin while the chain is replaced under it: the wait
            // re-checks the generation when it wakes and gives up without recording its id.
            H.paceId = 1;
            fr.frameId = 101;
            const FgFrame* evLive = FgHostSetFrame(fr);
            Check(evLive->epoch == 6 && evLive->paceId == 1, "the new chain's ids start again at 1");
            HANDLE th = CreateThread(NULL, 0, PrepareThread, (void*)evLive, 0, NULL);
            Sleep(30);                                                // it is inside Pin(true) now, waiting for this thread
            PublishChain(&pp, 7);
            Unpin(NULL);
            WaitForSingleObject(th, INFINITE); CloseHandle(th);
            Check(H.nStale == 3 && H.evDone == 0 && H.prepared == 0 && H.pinned == 0, "generation changed during the pin wait: re-checked after the wake, event dropped");
            // Control: the generation is the only thing between such an event and the chain.
            H.paceId = 2;
            fr.frameId = 102;
            const FgFrame* evNew = FgHostSetFrame(fr);
            FgHostPrepare(evNew);
            Check(H.nStale == 3 && H.prepared == 1 && H.evDone == 2 && pp.done == 2 && H.lastEvPace == 2, "an event of the live generation is prepared and settles its own id");
            FgHostPrepare(evNew);
            Check(H.nDup == 1 && H.prepared == 1, "its duplicate is still refused as a duplicate");
            // A capture armed on one generation is neither noted nor written by the next.
            H.prepared = 0;
            Check(FgHostCapture(L".") == 1 && FgCaptureArmed(), "capture armed on the live chain");
            PublishChain(&pp, 8);
            Check(!FgCaptureArmed() && FgHostCaptureStatus() == 1, "a rebuilt chain does not take the old chain's capture");
            Check(FgHostCapture(L".") == 1 && FgCaptureArmed(), "and a new one can be armed at once");
            InterlockedExchange(&g_cap.state, 0);
            // The result of Unity's own Present comes back unpinned: it detaches the chain it belonged to, no other.
            t_presentEpoch = 7;
            FgHostAfterUnityPresent(E_FAIL);
            Check(FgHostAlive() == 1, "a failed Unity Present of the previous generation does not detach the new chain");
            H.paceId = 0;
            fr.hudless = fr.depth = fr.mv = NULL; FgHostSetFrame(fr);  // drop the slot's references
            in->Release();
        }

        H.c.app = NULL; H.c.shadow = NULL;
        Check(FgHostShutdown() && FgHostRetired() && pp.lastOn == 0, "probe chain torn down, provider switched off detached");
        g_unityD3D12 = NULL;
        s_queue->Release(); s_frameFence->Release();
    }
    D3D12Ring ring; ring.Attach(device); ring.submitted[0] = 1;
    auto fence = ring.fence; auto heap = ring.qheap;
    Check(!ring.Release(), "unsignalled fence release pending");
    Check(ring.fence == fence && ring.qheap == heap && ring.submitted[0] == 1, "pending preserves all ring ownership");
    OwnedSet12 owned;
    D3D12Ring idle; idle.Attach(device);
    Check(owned.Ensure(device, idle, a, a), "initial owned set");
    auto original = owned.color;
    Check(!owned.Ensure(device, ring, b, b) && owned.color == original && owned.w == 8, "resize refuses unretired old set");
    Check(SUCCEEDED(fence->Signal(1)), "retire fence");
    Check(owned.Ensure(device, ring, b, b) && owned.w == 16, "resize retry succeeds");
    Check(ring.Release() && !ring.fence && !ring.qheap, "retired ring releases");
    Check(ring.Release(), "ring release idempotent");
    owned.Release(); idle.Release(); a->Release(); b->Release();
    device->Release(); warp->Release(); factory->Release();
    {
        // ---- review 4 (2026-10-12): the Reflex latch of FgStreamline.cpp ReflexApply (FgLatch.h), driven the way the
        // provider drives it, the SDK's answer coming from the real lever. Modes: 0 = eOff, 1 = eLowLatency.
        SetEnvironmentVariableA("RENDERFORGE_FG_TEST_SDK_FAIL", "sl_reflex@6944,1,skip=1");
        TestSdkFailInit();
        SetEnvironmentVariableA("RENDERFORGE_FG_TEST_SDK_FAIL", NULL);
        FgReflexLatch l = {}; l.mode = -1;
        int calls = 0; bool refuse = false;
        auto ask = [&](int m, unsigned us) {
            return l.Apply(m, us, m != 0, [&]() {
                ++calls;
                char name[32]; _snprintf_s(name, sizeof(name), _TRUNCATE, "sl_reflex@%u", us);
                return !refuse && !FgTestSdkFail("sl_reflex") && !FgTestSdkFail(name);
            });
        };
        Check(ask(0, 0) == FgReflexLatch::kAccepted && ask(1, 8333) == FgReflexLatch::kAccepted && !l.bad, "reflex latch: chain start, cap 120 accepted");
        Check(ask(1, 6944) == FgReflexLatch::kAccepted && ask(1, 8333) == FgReflexLatch::kAccepted && calls == 4, "lever skip=1: the first matching call goes through");
        Check(ask(1, 6944) == FgReflexLatch::kRefused && l.bad == 1 && l.mode == 1 && l.us == 8333 && l.fails == 1, "cap 144 refused: nothing recorded as applied, DLSS-G not fed");
        const int c0 = calls;
        Check(ask(1, 8333) == FgReflexLatch::kInEffect && l.bad == 0 && calls == c0, "back to the accepted cap 120: in effect without a call, and the refusal no longer stands (DLSS-G is fed again)");
        Check(ask(1, 8333) == FgReflexLatch::kInEffect && l.bad == 0, "and it stays clear frame after frame");
        Check(ask(1, 6944) == FgReflexLatch::kBackoff && l.bad == 1 && calls == c0, "the refused cap asked again inside its back-off: no call, not in effect");
        Check(ask(0, 0) == FgReflexLatch::kAccepted && l.bad == 0, "eOff accepted (no frames flow): nothing needed stands refused");
        Check(ask(1, 6944) == FgReflexLatch::kBackoff && l.bad == 1, "low latency asked again while backed off: DLSS-G is not fed behind an eOff Reflex");
        int waits = 0, step;
        while ((step = ask(1, 6944)) == FgReflexLatch::kBackoff) ++waits;
        Check(waits == FgReflexLatch::kRetry - 2 && step == FgReflexLatch::kAccepted && !l.bad && !l.fails && !l.skip && l.us == 6944, "retried after exactly the back-off; an accepted call clears the refusal and its counters");
        refuse = true;
        Check(ask(1, 4166) == FgReflexLatch::kRefused && ask(0, 0) == FgReflexLatch::kRefused && l.bad == 1 && l.fails == 2, "a refused eOff leaves the low-latency refusal standing");
        l.NewChain();
        refuse = false;
        Check(!l.bad && !l.skip && !l.fails && l.mode == 1 && l.us == 6944, "a new chain starts without the old chain's refusal or back-off; the accepted state is the SDK's");
        Check(ask(0, 0) == FgReflexLatch::kAccepted, "and its first request is issued at once");
    }
    printf("retirement_probe: PASS %d checks (pinned/refused/success/re-enable/real WARP fence+resize/stale fence wake/deferred enable/quiescent point/event settling/capture retention/SDK refusal back-off/queue probe/stale chain epoch/reflex latch 120-144-120)\n", checks);
}
