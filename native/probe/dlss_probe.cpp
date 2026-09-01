// dlss_probe.cpp - offline check of RenderforgeNative.dll: init -> optimal -> create -> 3x evaluate -> release.
// Usage: dlss_probe.exe <dir containing nvngx_dlss.dll>. Exit 0 only if every NGX result succeeded.
#include <d3d11.h>
#include <stdio.h>
#include <wchar.h>
#include "RenderforgeNative.h"

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

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: dlss_probe.exe <dir containing nvngx_dlss.dll>\n"); return 2; }
    wchar_t cwd[MAX_PATH]; _wgetcwd(cwd, MAX_PATH);

    ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL; D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed hr=0x%08X\n", (unsigned)hr); return 1; }

    ID3D11Texture2D* any = MakeTex(dev, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    int init = Dlss_Init(any, argv[1], cwd);
    int c = 0, e = 0, alive = 0; Dlss_Status(&c, &e, &alive);
    printf("Dlss_Init      code=%d (0=ok 1=noDevice 2=initFailed 3=notAvailable 4=needsDriver) lastResult=0x%08X %s\n",
           init, (unsigned)c, init == DLSS_ERR_INIT_FAILED ? Dlss_ResultString(c) : "");
    if (init != DLSS_OK) { printf("NGX init not ok, see nvngx.log in %ls\n", cwd); return 1; }

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

    printf(g_failed ? "PROBE FAILED\n" : "PROBE OK\n");
    return g_failed ? 1 : 0;
}
