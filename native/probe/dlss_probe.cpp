// dlss_probe.cpp - offline check of RenderforgeNative.dll: init -> optimal -> create -> 3x evaluate -> passthrough -> release.
// Usage: dlss_probe.exe <dir with nvngx_dlss.dll / amd_fidelityfx_*.dll / libxess.dll> [--d3d12|--fsr|--xess]. Exit 0 only if
// every result succeeded; 1 = a call failed, 2 = usage, 3 (--fsr / --xess only) = that provider cannot run on this GPU/driver
// (build warning, not an error).
#include <d3d11.h>
#include <d3d12.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include "RenderforgeNative.h"
#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphicsD3D12.h"
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "FfxLoader.h"
#include "xess/xess.h"
#include "xess/xess_d3d12.h"

#define NGX_OK(r) ((((unsigned)(r)) & 0xFFF00000u) != 0xBAD00000u)

typedef void(__stdcall* RenderEventFn)(int);
typedef void(__stdcall* RenderEventAndDataFn)(int, void*);

static int g_failed = 0;

static void Report(const char* what, int ngxResult)
{
    int ok = NGX_OK(ngxResult);
    printf("%-14s 0x%08X %s%s\n", what, (unsigned)ngxResult, Dlss_ResultString(ngxResult), ok ? "" : "  <-- FAIL");
    if (!ok) g_failed = 1;
}

// ---------------------------------------------------------------- D3D11

static ID3D11Texture2D* MakeTex(ID3D11Device* dev, unsigned w, unsigned h, DXGI_FORMAT fmt, UINT bind)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    ID3D11Texture2D* t = NULL;
    HRESULT hr = dev->CreateTexture2D(&d, NULL, &t);
    if (FAILED(hr)) { printf("CreateTexture2D %ux%u fmt %d failed hr=0x%08X\n", w, h, (int)fmt, (unsigned)hr); g_failed = 1; }
    return t;
}

static int RunD3D11(const wchar_t* dllDir, const wchar_t* cwd)
{
    ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed hr=0x%08X\n", (unsigned)hr); return 1; }

    ID3D11Texture2D* any = MakeTex(dev, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    int init = Dlss_Init(any, dllDir, cwd);
    int c = 0, e = 0, alive = 0; Dlss_Status(&c, &e, &alive);
    printf("Dlss_Init      code=%d (0=ok 1=noDevice 2=initFailed 3=notAvailable 4=needsDriver 5=noUnityIface) api=%d lastResult=0x%08X %s\n",
           init, Dlss_Api(), (unsigned)c, init == DLSS_ERR_INIT_FAILED ? Dlss_ResultString(c) : "");
    if (init != DLSS_OK) { printf("NGX init not ok, see nvngx.log in %ls\n", cwd); return 1; }
    if (Dlss_Api() != 11) { printf("Dlss_Api()=%d, expected 11\n", Dlss_Api()); return 1; }

    unsigned rw = 0, rh = 0, mnw = 0, mnh = 0, mxw = 0, mxh = 0;
    int r = Dlss_GetOptimal(3840, 2160, DLSS_Q_QUALITY, &rw, &rh, &mnw, &mnh, &mxw, &mxh);
    Report("GetOptimal", r);
    printf("  3840x2160 Quality -> render %ux%u  range [%ux%u .. %ux%u]\n", rw, rh, mnw, mnh, mxw, mxh);

    const unsigned RW = 1920, RH = 1080, OW = 3840, OH = 2160;
    ID3D11Texture2D* color = MakeTex(dev, RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    ID3D11Texture2D* depth = MakeTex(dev, RW, RH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    ID3D11Texture2D* mv    = MakeTex(dev, RW, RH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    ID3D11Texture2D* out   = MakeTex(dev, OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (g_failed) return 1;

    RenderEventFn ev = (RenderEventFn)Dlss_GetRenderEventFunc();
    RenderEventAndDataFn evd = (RenderEventAndDataFn)Dlss_GetRenderEventAndDataFunc();
    Dlss_SetCreateParams(RW, RH, OW, OH, DLSS_Q_QUALITY, DLSS_F_HDR | DLSS_F_DEPTH_INVERTED | DLSS_F_MV_LOW_RES | DLSS_F_AUTO_EXPOSURE);
    ev(DLSS_EV_CREATE);
    Dlss_Status(&c, &e, &alive);
    Report("Create", c);
    printf("  featureAlive=%d\n", alive);

    const float jit[3][2] = { { 0.25f, -0.25f }, { -0.125f, 0.375f }, { 0.375f, 0.125f } };
    for (int i = 0; i < 3; ++i) {
        void* slot = Dlss_GetFrameSlot();
        Dlss_SetFrame(slot, color, depth, mv, out, jit[i][0], jit[i][1], (float)RW, (float)RH, i == 0, 16.6f, RW, RH, 1.0f, 0.5f);
        evd(DLSS_EV_EVALUATE, slot);
        Dlss_Status(&c, &e, &alive);
        char name[32]; sprintf_s(name, "Evaluate[%d]", i);
        Report(name, e);
    }
    ctx->Flush();
    printf("Sharpen        shader=%d (1=NIS 2=RCAS fallback, expect 1) lastError=%d (expect 0; %d = setup failed)\n", Dlss_Sharpener(), Dlss_LastError(), DLSS_ERR_SHARPEN);
    if (Dlss_LastError() != 0 || Dlss_Sharpener() != DLSS_SHARPEN_NIS) g_failed = 1;

    // Passthrough: same-size copy, no NGX. out2 = render-res UAV target.
    ID3D11Texture2D* out2 = MakeTex(dev, RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    Dlss_Passthrough(1);
    {
        void* slot = Dlss_GetFrameSlot();
        Dlss_SetFrame(slot, color, NULL, NULL, out2, 0, 0, 0, 0, 0, 16.6f, RW, RH, 1.0f, 0.0f);
        evd(DLSS_EV_EVALUATE, slot);
        Dlss_Status(&c, &e, &alive);
        Report("Passthrough", e);
        Dlss_SetFrame(slot, color, NULL, NULL, out, 0, 0, 0, 0, 0, 16.6f, RW, RH, 1.0f, 0.0f);
        evd(DLSS_EV_EVALUATE, slot);
        printf("Passthrough size mismatch -> lastError=%d (expect %d)\n", Dlss_LastError(), DLSS_ERR_PASSTHROUGH_SIZE);
        if (Dlss_LastError() != DLSS_ERR_PASSTHROUGH_SIZE) g_failed = 1;
    }
    Dlss_Passthrough(0);
    out2->Release();

    ev(DLSS_EV_RELEASE);
    Dlss_Status(&c, &e, &alive);
    printf("Release        featureAlive=%d\n", alive);
    if (alive) g_failed = 1;

    Dlss_Shutdown();
    color->Release(); depth->Release(); mv->Release(); out->Release(); any->Release();
    ctx->Release(); dev->Release();
    return g_failed ? 1 : 0;
}

// ---------------------------------------------------------------- D3D12

// Stand-in for Unity: owns a direct queue and a fence and behaves like IUnityGraphicsD3D12v5.
// Every resource below is created in the state DLSS requires and the guide says NGX restores those
// states, so no transition is ever recorded here - but the declared state array is VALIDATED against
// the state the probe knows each resource is in (Unity would insert barriers from exactly that array).
static ID3D12Device* g_dev12 = NULL;
static ID3D12CommandQueue* g_queue = NULL;
static ID3D12Fence* g_fence = NULL;
static UINT64 g_fenceValue = 0;
static int g_execCalls = 0;

struct KnownState { ID3D12Resource* res; D3D12_RESOURCE_STATES state; };
static KnownState g_known[16];
static int g_knownCount = 0;

static KnownState* FindKnown(ID3D12Resource* r)
{
    for (int i = 0; i < g_knownCount; ++i) if (g_known[i].res == r) return &g_known[i];
    return NULL;
}

static void CheckStates(int stateCount, UnityGraphicsD3D12ResourceState* states)
{
    for (int i = 0; i < stateCount; ++i) {
        KnownState* k = FindKnown(states[i].resource);
        if (!k) { printf("  stub ExecuteCommandList: state[%d] names a resource the probe never created\n", i); g_failed = 1; continue; }
        if ((D3D12_RESOURCE_STATES)states[i].expected != k->state) {
            printf("  stub ExecuteCommandList: state[%d] expected 0x%X but resource is in 0x%X\n", i, (unsigned)states[i].expected, (unsigned)k->state);
            g_failed = 1;
        }
        k->state = (D3D12_RESOURCE_STATES)states[i].current;   // Unity's contract: `current` = state after the list
    }
}

static ID3D12Device* UNITY_INTERFACE_API StubGetDevice() { return g_dev12; }
static ID3D12Fence* UNITY_INTERFACE_API StubGetFrameFence() { return g_fence; }
static UINT64 UNITY_INTERFACE_API StubGetNextFrameFenceValue() { return g_fenceValue + 1; }
static ID3D12CommandQueue* UNITY_INTERFACE_API StubGetCommandQueue() { return g_queue; }
static void UNITY_INTERFACE_API StubSetMem(const UnityGraphicsD3D12PhysicalVideoMemoryControlValues*) {}
static ID3D12Resource* UNITY_INTERFACE_API StubTextureFromRenderBuffer(UnityRenderBuffer*) { return NULL; }

static UINT64 UNITY_INTERFACE_API StubExecuteCommandList(ID3D12GraphicsCommandList* cl, int stateCount, UnityGraphicsD3D12ResourceState* states)
{
    ++g_execCalls;
    if (g_execCalls == 1) printf("  stub ExecuteCommandList: first call declared %d resource states\n", stateCount);
    CheckStates(stateCount, states);
    ID3D12CommandList* lists[1] = { cl };
    g_queue->ExecuteCommandLists(1, lists);
    ++g_fenceValue;
    g_queue->Signal(g_fence, g_fenceValue);
    return g_fenceValue;
}

static ID3D12Resource* MakeTex12(unsigned w, unsigned h, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* t = NULL;
    HRESULT hr = g_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, IID_PPV_ARGS(&t));
    if (FAILED(hr)) { printf("CreateCommittedResource %ux%u fmt %d failed hr=0x%08X\n", w, h, (int)fmt, (unsigned)hr); g_failed = 1; }
    else if (g_knownCount < 16) { g_known[g_knownCount].res = t; g_known[g_knownCount].state = state; ++g_knownCount; }
    return t;
}

static void WaitGpu()
{
    if (!g_fence || g_fence->GetCompletedValue() >= g_fenceValue) return;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ev) return;
    if (SUCCEEDED(g_fence->SetEventOnCompletion(g_fenceValue, ev))) WaitForSingleObject(ev, 5000);
    CloseHandle(ev);
}

static IUnityGraphicsD3D12v5 stub;

// Creates g_dev12/g_queue/g_fence and installs the stand-in IUnityGraphicsD3D12v5. 0 on success.
static int InitD3D12(void)
{
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev12));
    if (FAILED(hr) || !g_dev12) { printf("D3D12CreateDevice failed hr=0x%08X\n", (unsigned)hr); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) { printf("CreateCommandQueue failed\n"); return 1; }
    if (FAILED(g_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { printf("CreateFence failed\n"); return 1; }

    stub.GetDevice = &StubGetDevice;
    stub.GetFrameFence = &StubGetFrameFence;
    stub.GetNextFrameFenceValue = &StubGetNextFrameFenceValue;
    stub.ExecuteCommandList = &StubExecuteCommandList;
    stub.SetPhysicalVideoMemoryControlValues = &StubSetMem;
    stub.GetCommandQueue = &StubGetCommandQueue;
    stub.TextureFromRenderBuffer = &StubTextureFromRenderBuffer;
    Dlss_TestSetUnityD3D12(&stub);
    return 0;
}

static int RunD3D12(const wchar_t* dllDir, const wchar_t* cwd)
{
    if (InitD3D12() != 0) return 1;

    ID3D12Resource* any = MakeTex12(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!any) return 1;
    int init = Dlss_Init(any, dllDir, cwd);
    int c = 0, e = 0, alive = 0; Dlss_Status(&c, &e, &alive);
    printf("Dlss_Init      code=%d (0=ok 1=noDevice 2=initFailed 3=notAvailable 4=needsDriver 5=noUnityIface) api=%d lastResult=0x%08X %s\n",
           init, Dlss_Api(), (unsigned)c, init == DLSS_ERR_INIT_FAILED ? Dlss_ResultString(c) : "");
    if (init != DLSS_OK) { printf("NGX D3D12 init not ok, see nvngx.log in %ls\n", cwd); return 1; }
    if (Dlss_Api() != 12) { printf("Dlss_Api()=%d, expected 12\n", Dlss_Api()); return 1; }

    unsigned rw = 0, rh = 0, mnw = 0, mnh = 0, mxw = 0, mxh = 0;
    int r = Dlss_GetOptimal(3840, 2160, DLSS_Q_QUALITY, &rw, &rh, &mnw, &mnh, &mxw, &mxh);
    Report("GetOptimal", r);
    printf("  3840x2160 Quality -> render %ux%u  range [%ux%u .. %ux%u]\n", rw, rh, mnw, mnh, mxw, mxh);

    const unsigned RW = 1920, RH = 1080, OW = 3840, OH = 2160;
    ID3D12Resource* color = MakeTex12(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth = MakeTex12(RW, RH, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* mv    = MakeTex12(RW, RH, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* out   = MakeTex12(OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (g_failed) return 1;

    RenderEventFn ev = (RenderEventFn)Dlss_GetRenderEventFunc();
    RenderEventAndDataFn evd = (RenderEventAndDataFn)Dlss_GetRenderEventAndDataFunc();
    Dlss_SetCreateParams(RW, RH, OW, OH, DLSS_Q_QUALITY, DLSS_F_HDR | DLSS_F_DEPTH_INVERTED | DLSS_F_MV_LOW_RES | DLSS_F_AUTO_EXPOSURE);
    ev(DLSS_EV_CREATE);
    Dlss_Status(&c, &e, &alive);
    Report("Create", c);
    printf("  featureAlive=%d\n", alive);

    const float jit[3][2] = { { 0.25f, -0.25f }, { -0.125f, 0.375f }, { 0.375f, 0.125f } };
    for (int i = 0; i < 3; ++i) {
        void* slot = Dlss_GetFrameSlot();
        Dlss_SetFrame(slot, color, depth, mv, out, jit[i][0], jit[i][1], (float)RW, (float)RH, i == 0, 16.6f, RW, RH, 1.0f, 0.5f);
        evd(DLSS_EV_EVALUATE, slot);
        Dlss_Status(&c, &e, &alive);
        char name[32]; sprintf_s(name, "Evaluate[%d]", i);
        Report(name, e);
    }
    // sRGB output: Unity's sRGB RenderTextures are TYPELESS resources viewed as *_UNORM_SRGB; a UAV cannot be
    // sRGB, so the sharpen pass must view the output as R8G8B8A8_UNORM (SharpenViewFormat).
    ID3D12Resource* outSrgb = MakeTex12(OW, OH, DXGI_FORMAT_R8G8B8A8_TYPELESS, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!outSrgb) return 1;
    {
        void* slot = Dlss_GetFrameSlot();
        Dlss_SetFrame(slot, color, depth, mv, outSrgb, jit[0][0], jit[0][1], (float)RW, (float)RH, 0, 16.6f, RW, RH, 1.0f, 0.5f);
        evd(DLSS_EV_EVALUATE, slot);
        Dlss_Status(&c, &e, &alive);
        Report("Evaluate[sRGB]", e);
    }
    WaitGpu();
    printf("Submissions    ExecuteCommandList calls=%d (expect 5: 1 create + 4 evaluate)\n", g_execCalls);
    if (g_execCalls != 5) g_failed = 1;
    printf("Sharpen        shader=%d (1=NIS 2=RCAS fallback, expect 1) lastError=%d (expect 0; %d = setup failed)\n",
           Dlss_Sharpener(), Dlss_LastError(), DLSS_ERR_SHARPEN);
    if (Dlss_LastError() != 0 || Dlss_Sharpener() != DLSS_SHARPEN_NIS) g_failed = 1;

    // Passthrough: same-size copy, no NGX. out2 = render-res copy target.
    ID3D12Resource* out2 = MakeTex12(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* colorCopySrc = MakeTex12(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Dlss_Passthrough(1);
    {
        void* slot = Dlss_GetFrameSlot();
        Dlss_SetFrame(slot, colorCopySrc, NULL, NULL, out2, 0, 0, 0, 0, 0, 16.6f, RW, RH, 1.0f, 0.0f);
        evd(DLSS_EV_EVALUATE, slot);
        Dlss_Status(&c, &e, &alive);
        Report("Passthrough", e);
        Dlss_SetFrame(slot, colorCopySrc, NULL, NULL, out, 0, 0, 0, 0, 0, 16.6f, RW, RH, 1.0f, 0.0f);
        evd(DLSS_EV_EVALUATE, slot);
        printf("Passthrough size mismatch -> lastError=%d (expect %d)\n", Dlss_LastError(), DLSS_ERR_PASSTHROUGH_SIZE);
        if (Dlss_LastError() != DLSS_ERR_PASSTHROUGH_SIZE) g_failed = 1;
    }
    Dlss_Passthrough(0);
    WaitGpu();

    ev(DLSS_EV_RELEASE);
    Dlss_Status(&c, &e, &alive);
    printf("Release        featureAlive=%d\n", alive);
    if (alive) g_failed = 1;

    Dlss_Shutdown();
    WaitGpu();
    colorCopySrc->Release(); out2->Release(); outSrgb->Release();
    color->Release(); depth->Release(); mv->Release(); out->Release(); any->Release();
    g_fence->Release(); g_queue->Release(); g_dev12->Release();
    return g_failed ? 1 : 0;
}

// ---------------------------------------------------------------- FSR (provider 1)

// Prints every upscaler version the AMD DLL offers for this device. On a non-AMD GPU the 4.x ML provider is
// expected to be absent or unusable and the created context falls back to 3.1.5 - printed, never assumed.
static void PrintFsrVersions(ID3D12Device* dev, const wchar_t* dir)
{
    const ffxFunctions* ffx = FfxLoad(dir);
    if (!ffx) { printf("FSR versions    <amd_fidelityfx_*_dx12.dll not found in %ls>\n", dir); return; }
    uint64_t count = 0;
    struct ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = dev;
    q.outputCount = &count;
    if (ffx->Query(NULL, &q.header) != FFX_API_RETURN_OK || count == 0) { printf("FSR versions    <query failed>\n"); return; }
    if (count > 8) count = 8;
    uint64_t ids[8] = {}; const char* names[8] = {};
    q.versionIds = ids; q.versionNames = names;
    if (ffx->Query(NULL, &q.header) != FFX_API_RETURN_OK) { printf("FSR versions    <name query failed>\n"); return; }
    printf("FSR versions    %llu:", (unsigned long long)count);
    for (uint64_t i = 0; i < count; ++i) printf(" %s", names[i] ? names[i] : "?");
    printf("\n");
}

static int RunFsr(const wchar_t* dllDir, const wchar_t* cwd)
{
    if (InitD3D12() != 0) return 1;              // creates g_dev12/g_queue/g_fence and installs the Unity stub
    (void)cwd;

    Dlss_SetProvider(DLSS_PROVIDER_FSR);
    PrintFsrVersions(g_dev12, dllDir);

    ID3D12Resource* any = MakeTex12(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!any) return 1;
    int init = Dlss_Init(any, dllDir, dllDir);
    printf("Dlss_Init      code=%d (0=ok 6=noProviderDll 7=providerUnsupported) provider=%d api=%d\n",
           init, Dlss_Provider(), Dlss_Api());
    if (init != DLSS_OK) { printf("FSR init not ok: no usable D3D12 upscale provider on this machine\n"); return 3; }   // 3 = provider missing/unsupported, not a code defect
    if (Dlss_Provider() != DLSS_PROVIDER_FSR) { printf("Dlss_Provider()=%d, expected %d\n", Dlss_Provider(), DLSS_PROVIDER_FSR); return 1; }

    unsigned rw = 0, rh = 0, mnw = 0, mnh = 0, mxw = 0, mxh = 0;
    int r = Dlss_GetOptimal(1920, 1080, DLSS_Q_QUALITY, &rw, &rh, &mnw, &mnh, &mxw, &mxh);
    Report("GetOptimal", r);
    printf("  1920x1080 Quality -> render %ux%u (expect 1280x720)\n", rw, rh);
    if (rw != 1280 || rh != 720) { printf("  <-- unexpected render resolution\n"); g_failed = 1; }

    const unsigned RW = 1280, RH = 720, OW = 1920, OH = 1080;
    ID3D12Resource* color = MakeTex12(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth = MakeTex12(RW, RH, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* mv    = MakeTex12(RW, RH, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* out   = MakeTex12(OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (g_failed) return 1;

    RenderEventFn ev = (RenderEventFn)Dlss_GetRenderEventFunc();
    RenderEventAndDataFn evd = (RenderEventAndDataFn)Dlss_GetRenderEventAndDataFunc();
    Dlss_SetCamera(0.1f, 1000.0f, 1.0471976f);
    Dlss_SetCreateParams(RW, RH, OW, OH, DLSS_Q_QUALITY, DLSS_F_HDR | DLSS_F_DEPTH_INVERTED | DLSS_F_MV_LOW_RES | DLSS_F_AUTO_EXPOSURE);
    ev(DLSS_EV_CREATE);
    int c = 0, e = 0, alive = 0;
    Dlss_Status(&c, &e, &alive);
    Report("Create", c);
    char ver[64] = {};
    Dlss_ProviderVersion(ver, (int)sizeof(ver));
    printf("  contextAlive=%d provider version='%s'\n", alive, ver);
    if (!alive || !ver[0]) g_failed = 1;

    const float jit[3][2] = { { 0.25f, -0.25f }, { -0.125f, 0.375f }, { 0.375f, 0.125f } };
    for (int i = 0; i < 3; ++i) {
        void* slot = Dlss_GetFrameSlot();
        Dlss_SetFrame(slot, color, depth, mv, out, jit[i][0], jit[i][1], -(float)RW, -(float)RH, i == 0, 16.6f, RW, RH, 1.0f, 0.5f);
        evd(DLSS_EV_EVALUATE, slot);
        Dlss_Status(&c, &e, &alive);
        char name[32]; sprintf_s(name, "Dispatch[%d]", i);
        Report(name, e);
    }
    WaitGpu();
    // FSR creates its context on the CPU (no command list), so only the dispatches reach ExecuteCommandList.
    printf("Submissions    ExecuteCommandList calls=%d (expect 3: 3 dispatch)\n", g_execCalls);
    if (g_execCalls != 3) g_failed = 1;
    printf("Sharpener      %d (expect %d = FSR built-in RCAS)\n", Dlss_Sharpener(), DLSS_SHARPEN_RCAS);
    if (Dlss_Sharpener() != DLSS_SHARPEN_RCAS) g_failed = 1;

    ev(DLSS_EV_RELEASE);
    Dlss_Status(&c, &e, &alive);
    printf("Release        contextAlive=%d\n", alive);
    if (alive) g_failed = 1;

    Dlss_Shutdown();
    WaitGpu();
    color->Release(); depth->Release(); mv->Release(); out->Release(); any->Release();
    g_fence->Release(); g_queue->Release(); g_dev12->Release();
    return g_failed ? 1 : 0;
}

// ---------------------------------------------------------------- XeSS (provider 2, D3D12, cross-vendor)

// What the SDK itself reports for this device, straight from libxess.dll (delay-loaded, pinned from `dir` first so
// the same copy the shim uses answers). XeFX 0.0.0 = non-Intel platform = the cross-vendor DP4a path (xess.h:216).
static void PrintXessVersions(ID3D12Device* dev, const wchar_t* dir)
{
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\libxess.dll", dir);
    if (!LoadLibraryW(path)) { printf("XeSS versions   <libxess.dll not found in %ls>\n", dir); return; }
    xess_version_t v = {}, xefx = {};
    xess_result_t r = xessGetVersion(&v);
    printf("xessGetVersion  %d -> %u.%u.%u\n", (int)r, (unsigned)v.major, (unsigned)v.minor, (unsigned)v.patch);
    xess_context_handle_t ctx = NULL;
    r = xessD3D12CreateContext(dev, &ctx);
    if (r != XESS_RESULT_SUCCESS || !ctx) { printf("xessD3D12CreateContext %d (-1 = no SM 6.4/DP4a, -2 = driver)\n", (int)r); return; }
    r = xessGetIntelXeFXVersion(ctx, &xefx);
    printf("xessGetIntelXeFXVersion %d -> %u.%u.%u (%s)  xessIsOptimalDriver=%d (0 ok, 2 old driver)\n", (int)r,
           (unsigned)xefx.major, (unsigned)xefx.minor, (unsigned)xefx.patch,
           (xefx.major || xefx.minor || xefx.patch) ? "Intel XMX path" : "cross-vendor DP4a path", (int)xessIsOptimalDriver(ctx));
    xessDestroyContext(ctx);
}

// Colour/output are R8G8B8A8_UNORM: the game's D3D12 colour RT is Unity ARGB32 and XeSS is told so with
// XESS_INIT_FLAG_LDR_INPUT_COLOR (no DLSS_F_HDR). Every Unity resource is declared COMMON like Device12 does.
static int RunXess(const wchar_t* dllDir, const wchar_t* cwd)
{
    if (InitD3D12() != 0) return 1;
    (void)cwd;

    Dlss_SetProvider(DLSS_PROVIDER_XESS);
    PrintXessVersions(g_dev12, dllDir);

    ID3D12Resource* any = MakeTex12(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!any) return 1;
    int init = Dlss_Init(any, dllDir, dllDir);
    char ver[64] = {};
    Dlss_ProviderVersion(ver, (int)sizeof(ver));
    printf("Dlss_Init      code=%d (0=ok 2=initFailed 3=notAvailable 4=needsDriver 6=noProviderDll 7=providerUnsupported) provider=%d api=%d version='%s'\n",
           init, Dlss_Provider(), Dlss_Api(), ver);
    if (init != DLSS_OK) { printf("XeSS init not ok: this GPU/driver cannot run XeSS (needs SM 6.4 + DP4a)\n"); return 3; }   // 3 = unsupported here, not a code defect
    if (Dlss_Provider() != DLSS_PROVIDER_XESS) { printf("Dlss_Provider()=%d, expected %d\n", Dlss_Provider(), DLSS_PROVIDER_XESS); return 1; }
    if (!ver[0]) { printf("provider version empty\n"); g_failed = 1; }

    unsigned rw = 0, rh = 0, mnw = 0, mnh = 0, mxw = 0, mxh = 0;
    int r = Dlss_GetOptimal(1920, 1080, DLSS_Q_QUALITY, &rw, &rh, &mnw, &mnh, &mxw, &mxh);
    Report("GetOptimal", r);
    printf("  1920x1080 Quality -> render %ux%u  range [%ux%u .. %ux%u] (1.7x -> ~1129x635)\n", rw, rh, mnw, mnh, mxw, mxh);
    if (rw == 0 || rh == 0 || rw >= 1920 || rw < 1920 / 2) { printf("  <-- unexpected render resolution\n"); g_failed = 1; }
    unsigned aaW = 0, aaH = 0;
    r = Dlss_GetOptimal(1920, 1080, DLSS_Q_DLAA, &aaW, &aaH, &mnw, &mnh, &mxw, &mxh);
    Report("GetOptimal AA", r);
    printf("  1920x1080 Native AA -> render %ux%u (expect 1920x1080, XESS_QUALITY_SETTING_AA = 1.0x)\n", aaW, aaH);
    if (aaW != 1920 || aaH != 1080) g_failed = 1;
    unsigned uqW = 0, uqH = 0;
    r = Dlss_GetOptimal(1920, 1080, DLSS_Q_ULTRA_QUALITY_PLUS, &uqW, &uqH, &mnw, &mnh, &mxw, &mxh);
    Report("GetOptimal UQ+", r);
    printf("  1920x1080 Ultra Quality Plus -> render %ux%u (1.3x -> ~1477x831, between Quality and native)\n", uqW, uqH);
    if (uqW <= rw || uqW >= 1920) g_failed = 1;

    const unsigned RW = rw, RH = rh, OW = 1920, OH = 1080;
    ID3D12Resource* color = MakeTex12(RW, RH, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* depth = MakeTex12(RW, RH, DXGI_FORMAT_R32_FLOAT,     false, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* mv    = MakeTex12(RW, RH, DXGI_FORMAT_R16G16_FLOAT,  false, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* out   = MakeTex12(OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_COMMON);
    if (g_failed) return 1;

    RenderEventFn ev = (RenderEventFn)Dlss_GetRenderEventFunc();
    RenderEventAndDataFn evd = (RenderEventAndDataFn)Dlss_GetRenderEventAndDataFunc();
    Dlss_SetCreateParams(RW, RH, OW, OH, DLSS_Q_QUALITY, DLSS_F_DEPTH_INVERTED | DLSS_F_MV_LOW_RES);
    ev(DLSS_EV_CREATE);
    int c = 0, e = 0, alive = 0;
    Dlss_Status(&c, &e, &alive);
    Report("Create", c);
    printf("  initialised=%d lastError=%d (xessD3D12Init is a blocking CPU call: no ExecuteCommandList, calls=%d)\n", alive, Dlss_LastError(), g_execCalls);
    if (!alive || g_execCalls != 0) g_failed = 1;

    const float jit[3][2] = { { 0.25f, -0.25f }, { -0.125f, 0.375f }, { 0.375f, 0.125f } };
    for (int i = 0; i < 3; ++i) {
        void* slot = Dlss_GetFrameSlot();
        Dlss_SetFrame(slot, color, depth, mv, out, jit[i][0], jit[i][1], -(float)RW, -(float)RH, i == 0, 16.6f, RW, RH, 1.0f, 0.5f);
        evd(DLSS_EV_EVALUATE, slot);
        Dlss_Status(&c, &e, &alive);
        char name[32]; sprintf_s(name, "Execute[%d]", i);
        Report(name, e);
    }
    WaitGpu();
    printf("Submissions    ExecuteCommandList calls=%d (expect 3: 3 execute, no create)\n", g_execCalls);
    if (g_execCalls != 3) g_failed = 1;
    printf("Sharpen        shader=%d (1=NIS 2=RCAS fallback, expect 1: XeSS has no sharpness, the shim's pass runs) lastError=%d (expect 0)\n",
           Dlss_Sharpener(), Dlss_LastError());
    if (Dlss_LastError() != 0 || Dlss_Sharpener() != DLSS_SHARPEN_NIS) g_failed = 1;

    ev(DLSS_EV_RELEASE);
    Dlss_Status(&c, &e, &alive);
    printf("Release        initialised=%d\n", alive);
    if (alive) g_failed = 1;

    // Re-create after a release: the context is rebuilt on the render path, like a preset change in-game.
    ev(DLSS_EV_CREATE);
    Dlss_Status(&c, &e, &alive);
    Report("Re-create", c);
    if (!alive) g_failed = 1;
    ev(DLSS_EV_RELEASE);

    Dlss_Shutdown();
    WaitGpu();
    color->Release(); depth->Release(); mv->Release(); out->Release(); any->Release();
    g_fence->Release(); g_queue->Release(); g_dev12->Release();
    return g_failed ? 1 : 0;
}

// ----------------------------------------------------------------

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: dlss_probe.exe <dir with nvngx_dlss.dll / amd_fidelityfx_*.dll / libxess.dll> [--d3d12|--fsr|--xess]\n"); return 2; }
    int want12 = 0, wantFsr = 0, wantXess = 0;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--d3d12") == 0) want12 = 1;
        if (wcscmp(argv[i], L"--fsr") == 0) wantFsr = 1;
        if (wcscmp(argv[i], L"--xess") == 0) wantXess = 1;
    }
    wchar_t cwd[MAX_PATH]; _wgetcwd(cwd, MAX_PATH);
    printf("== dlss_probe %s ==\n", wantXess ? "XeSS (D3D12)" : wantFsr ? "FSR" : want12 ? "D3D12" : "D3D11");

    int rc = wantXess ? RunXess(argv[1], cwd) : wantFsr ? RunFsr(argv[1], cwd) : want12 ? RunD3D12(argv[1], cwd) : RunD3D11(argv[1], cwd);
    printf(rc ? "PROBE FAILED\n" : "PROBE OK\n");
    return rc;
}
