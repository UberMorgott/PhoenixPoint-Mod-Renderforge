// dlss_probe.cpp - offline check of RenderforgeNative.dll: init -> optimal -> create -> 3x evaluate -> passthrough -> release.
// Usage: dlss_probe.exe <dir containing nvngx_dlss.dll> [--d3d12]. Exit 0 only if every NGX result succeeded.
#include <d3d11.h>
#include <d3d12.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include "RenderforgeNative.h"
#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphicsD3D12.h"

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
// The state array is ignored on purpose - every resource below is created in the state DLSS requires
// and the guide says NGX restores those states, so no transition is ever needed here.
static ID3D12Device* g_dev12 = NULL;
static ID3D12CommandQueue* g_queue = NULL;
static ID3D12Fence* g_fence = NULL;
static UINT64 g_fenceValue = 0;
static int g_execCalls = 0;

static ID3D12Device* UNITY_INTERFACE_API StubGetDevice() { return g_dev12; }
static ID3D12Fence* UNITY_INTERFACE_API StubGetFrameFence() { return g_fence; }
static UINT64 UNITY_INTERFACE_API StubGetNextFrameFenceValue() { return g_fenceValue + 1; }
static ID3D12CommandQueue* UNITY_INTERFACE_API StubGetCommandQueue() { return g_queue; }
static void UNITY_INTERFACE_API StubSetMem(const UnityGraphicsD3D12PhysicalVideoMemoryControlValues*) {}
static ID3D12Resource* UNITY_INTERFACE_API StubTextureFromRenderBuffer(UnityRenderBuffer*) { return NULL; }

static UINT64 UNITY_INTERFACE_API StubExecuteCommandList(ID3D12GraphicsCommandList* cl, int stateCount, UnityGraphicsD3D12ResourceState* states)
{
    (void)states;
    ++g_execCalls;
    if (g_execCalls == 1) printf("  stub ExecuteCommandList: first call declared %d resource states\n", stateCount);
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

static int RunD3D12(const wchar_t* dllDir, const wchar_t* cwd)
{
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev12));
    if (FAILED(hr) || !g_dev12) { printf("D3D12CreateDevice failed hr=0x%08X\n", (unsigned)hr); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) { printf("CreateCommandQueue failed\n"); return 1; }
    if (FAILED(g_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { printf("CreateFence failed\n"); return 1; }

    static IUnityGraphicsD3D12v5 stub;
    stub.GetDevice = &StubGetDevice;
    stub.GetFrameFence = &StubGetFrameFence;
    stub.GetNextFrameFenceValue = &StubGetNextFrameFenceValue;
    stub.ExecuteCommandList = &StubExecuteCommandList;
    stub.SetPhysicalVideoMemoryControlValues = &StubSetMem;
    stub.GetCommandQueue = &StubGetCommandQueue;
    stub.TextureFromRenderBuffer = &StubTextureFromRenderBuffer;
    Dlss_TestSetUnityD3D12(&stub);

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
    WaitGpu();
    printf("Submissions    ExecuteCommandList calls=%d (expect 4: 1 create + 3 evaluate)\n", g_execCalls);
    if (g_execCalls != 4) g_failed = 1;

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
    colorCopySrc->Release(); out2->Release();
    color->Release(); depth->Release(); mv->Release(); out->Release(); any->Release();
    g_fence->Release(); g_queue->Release(); g_dev12->Release();
    return g_failed ? 1 : 0;
}

// ----------------------------------------------------------------

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: dlss_probe.exe <dir containing nvngx_dlss.dll> [--d3d12]\n"); return 2; }
    int want12 = 0;
    for (int i = 2; i < argc; ++i) if (wcscmp(argv[i], L"--d3d12") == 0) want12 = 1;
    wchar_t cwd[MAX_PATH]; _wgetcwd(cwd, MAX_PATH);
    printf("== dlss_probe %s ==\n", want12 ? "D3D12" : "D3D11");

    int rc = want12 ? RunD3D12(argv[1], cwd) : RunD3D11(argv[1], cwd);
    printf(rc ? "PROBE FAILED\n" : "PROBE OK\n");
    return rc;
}
