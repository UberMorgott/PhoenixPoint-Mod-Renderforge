// Source-linked lifecycle proof: real WARP COM resources/fences; only FG vendor/window seams are mocked.
#include "../FgHost.cpp"
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
    Check(Pin(), "pin");
    Check(!Pin(), "nested pin refused");
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
    Check(Pin(), "re-enable after completed shutdown"); Unpin(NULL);
    Check(FgHostShutdown() && Refs(b) == br, "second lifecycle balanced");

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
    printf("retirement_probe: PASS %d checks (pinned/refused/success/re-enable/real WARP fence+resize)\n", checks);
}
