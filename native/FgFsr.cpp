// FgFsr.cpp - AMD FidelityFX frame generation (SDK 2.3) as an IFgProvider, D3D12 only.
//
// ONE ffx-api context: the frame-GENERATION context, dispatched by hand. The SDK's own frame-interpolation
// swapchain is NOT used: all three of its creation shapes end in IDXGIFactory2::CreateSwapChainForHwnd on the
// game window (FrameInterpolationSwapchainDX12.cpp:1162 via :477 Wrap / :520 New / :523 ForHwnd), which DXGI
// refuses with E_ACCESSDENIED while Unity's chain owns that HWND (measured on Instance3), and Wrap additionally
// needs IDXGISwapChain::GetHwnd (:468), which a composition chain has not got. So the host's composition shadow
// chain stays, and this provider takes the "manual dispatch" path the SDK documents for engines where presenting
// through the proxy is unsafe (frame-interpolation-api.md:281): ffxConfigure with
// FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY ("allow for run configure without swapchain",
// ffx_provider_fsr3framegeneration.cpp:240-241), ffxDispatchDescFrameGenerationPrepareV2 in the FG_PREPARE render
// event, ffxDispatchDescFrameGeneration in the Present hook with presentColor = Unity's finished back buffer and
// outputs[0] = our own UAV texture, which is then copied into the shadow chain and presented BEFORE the real frame.
//
// Inputs are the shim-owned twins the running upscaler fills (D3D12Owned.h): depth/mv at render res, out = the
// hud-less frame at output res, all resting in COMMON; passed with FFX_API_RESOURCE_STATE_COMMON so the ffx DX12
// backend barriers them itself and restores COMMON (ffx_dx12.cpp:625 maps it 1:1). Unity RTs are never touched.
//
// Model: the ANALYTICAL 3.1.x provider is pinned through ffxOverrideVersion. The ML 4.0.1 model in the same DLL
// needs RDNA4 (frame-interpolation-ml.md:75) and would otherwise be the loader's silent pick.
//
// ponytail: no frame pacing - the generated frame and the real one are presented back to back on the render
// thread, so the presented-frame counter doubles but the display cadence does not smooth out. Upgrade path: a
// present thread that holds the real frame for half the average frame time (what the SDK's proxy does).
#include "Fg.h"
#include "RenderforgeNative.h"
#include "D3D12Ring.h"
#include "D3D12Owned.h"
#include "FfxLoader.h"

#include <string.h>
#include <stdio.h>

#include "ffx_api_types.h"
#include "ffx_api_dx12.h"
#include "ffx_framegeneration.h"

namespace {

struct ProviderFsr : IFgProvider
{
    const ffxFunctions* fn;
    ffxContext          fgCtx;
    ID3D12Device*       device;
    ID3D12CommandQueue* queue;
    D3D12Ring           ring;          // the generate list, executed straight on Unity's queue (EndDirect)
    ID3D12Resource*     interp;        // outputs[0]: display size, back-buffer format, UAV
    DXGI_FORMAT         backFmt;
    unsigned            outW, outH;
    int                 enabled;
    int                 lastRc;
    unsigned long long  preparedId;    // frameID of the last successful Configure + Prepare
    unsigned long long  generatedId;   // frameID of the last generated frame
    int                 hudlessWarned;
    char                version[64];

    // The create chain must outlive the context (ffx_api.h:140).
    ffxCreateContextDescFrameGeneration        descFg;
    ffxCreateContextDescFrameGenerationVersion descVer;
    ffxCreateBackendDX12Desc                   descBackend;
    ffxOverrideVersion                         descOverride;

    ProviderFsr() { Zero(); }
    void Zero()
    {
        fn = NULL; fgCtx = NULL; device = NULL; queue = NULL; interp = NULL;
        backFmt = DXGI_FORMAT_UNKNOWN; outW = outH = 0; enabled = 0; lastRc = 0;
        preparedId = generatedId = ~0ull; hudlessWarned = 0; version[0] = 0;
        ring.Zero();
        memset(&descFg, 0, sizeof(descFg)); memset(&descVer, 0, sizeof(descVer));
        memset(&descBackend, 0, sizeof(descBackend)); memset(&descOverride, 0, sizeof(descOverride));
    }

    int Id() const { return FG_PROVIDER_FSR; }
    unsigned Caps() const { return FG_CAP_2X; }        // numGeneratedFrames = 1, as the SDK sample (fsrapirendermodule.cpp:1612)
    const char* Name() const { return "fsr"; }

    // Enumerate the FG providers this device offers and pick the first analytical (3.1.x) one; 0 = let the loader choose.
    uint64_t PickAnalyticalVersion()
    {
        uint64_t count = 0;
        ffxQueryDescGetVersions q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        q.device = device;
        q.outputCount = &count;
        if (fn->Query(NULL, &q.header) != FFX_API_RETURN_OK || count == 0) { FgLog("fsr: FG version query failed (count %llu)", (unsigned long long)count); return 0; }
        if (count > 8) count = 8;
        uint64_t ids[8] = {};
        const char* names[8] = {};
        q.versionIds = ids;
        q.versionNames = names;
        if (fn->Query(NULL, &q.header) != FFX_API_RETURN_OK) { FgLog("fsr: FG version query 2 failed"); return 0; }
        uint64_t pick = 0;
        for (uint64_t i = 0; i < count; ++i) {
            FgLog("fsr: FG version %llu = %s", (unsigned long long)ids[i], names[i] ? names[i] : "?");
            if (!pick && names[i] && strstr(names[i], "3.1")) pick = ids[i];
        }
        if (!pick) FgLog("fsr: no 3.1 model listed, letting the loader choose");
        return pick;
    }

    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        fn = FfxLoad(s.dllDir);
        if (!fn) { FgLog("fsr: AMD DLLs not loadable from the mod folder"); return FG_ERR_NO_PROVIDER; }
        device = s.device; queue = s.queue;
        outW = s.desc.Width; outH = s.desc.Height; backFmt = s.desc.Format;

        descFg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        descFg.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;      // Unity D3D12 = reversed-Z (DlssDriver.cs:253)
        descFg.displaySize.width = outW; descFg.displaySize.height = outH;
        descFg.maxRenderSize.width = outW; descFg.maxRenderSize.height = outH;
        descFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(backFmt);
        descVer.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        descVer.version = FFX_FRAMEGENERATION_VERSION;
        descBackend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        descBackend.device = device;
        descOverride.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        descOverride.versionId = PickAnalyticalVersion();
        descFg.header.pNext = &descVer.header;
        descVer.header.pNext = &descBackend.header;
        descBackend.header.pNext = descOverride.versionId ? &descOverride.header : NULL;

        ffxReturnCode_t rc = fn->CreateContext(&fgCtx, &descFg.header, NULL);
        if (rc != FFX_API_RETURN_OK) { FgLog("fsr: FG context %u", rc); fgCtx = NULL; return FG_ERR_PROVIDER_FAILED; }

        ffxQueryGetProviderVersion pv = {};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (fn->Query(&fgCtx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) strncpy_s(version, pv.versionName, _TRUNCATE);

        interp = OwnedSet12::Make(device, outW, outH, backFmt, true, L"Renderforge FG interp");
        if (!interp) { Destroy(); return FG_ERR_PROVIDER_FAILED; }
        // Make() creates in COMMON; keep it there at rest (Generate barriers to UAV and back).
        ring.Attach(device);

        int hostRc = FgHostCreateShadowSwapChain(s, out);
        if (hostRc != FG_OK) { Destroy(); return hostRc; }
        FgLog("fsr: created, display %ux%u fmt %u, provider '%s' (override %llu)", outW, outH, (unsigned)backFmt, version,
              (unsigned long long)descOverride.versionId);
        return FG_OK;
    }

    // Configure (once per frame, frameID must step by exactly 1) + the prepare pass, on the host's prep list.
    void Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f)
    {
        const OwnedSet12* o = FgOwned12();
        if (!fgCtx || !o || !o->depth || !o->mv || !o->out) return;

        ffxConfigureDescFrameGeneration cfg = {};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = NULL;
        cfg.frameGenerationEnabled = enabled != 0;
        cfg.allowAsyncWorkloads = false;                   // everything on Unity's queue, in frame order
        cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        cfg.generationRect.left = 0; cfg.generationRect.top = 0;
        cfg.generationRect.width = (int32_t)outW; cfg.generationRect.height = (int32_t)outH;
        cfg.frameID = f.frameId;
        // Hud-less UI mode (decision 5): the owned `out` is the upscaled frame before Unity draws the HUD. The FI
        // context copies it into its history, so its format must equal the back buffer's (ffx_frameinterpolation.cpp:887).
        if (o->outFmt == backFmt && o->outW == outW && o->outH == outH)
            cfg.HUDLessColor = ffxApiGetResourceDX12(o->out, FFX_API_RESOURCE_STATE_COMMON);
        else if (!hudlessWarned) { hudlessWarned = 1; FgLog("fsr: hudless skipped: out %ux%u fmt %u vs backbuffer %ux%u fmt %u", o->outW, o->outH, (unsigned)o->outFmt, outW, outH, (unsigned)backFmt); }
        lastRc = (int)fn->Configure(&fgCtx, &cfg.header);
        if (lastRc != FFX_API_RETURN_OK) { FgLog("fsr: configure %d", lastRc); return; }

        ffxDispatchDescFrameGenerationPrepareV2 pr = {};
        pr.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        pr.frameID = f.frameId;
        pr.flags = 0;
        pr.commandList = list;
        pr.renderSize.width = f.renderW; pr.renderSize.height = f.renderH;
        // Same conventions as the FSR upscaler (Fsr12.cpp Evaluate): J = (-jitterX, +jitterY), MV scale straight through.
        pr.jitterOffset.x = -f.jitterX; pr.jitterOffset.y = f.jitterY;
        pr.motionVectorScale.x = f.mvScaleX; pr.motionVectorScale.y = f.mvScaleY;
        pr.frameTimeDelta = f.dtMs;
        pr.reset = f.reset != 0;
        pr.cameraNear = f.cameraNear;
        pr.cameraFar = f.cameraFar;
        pr.cameraFovAngleVertical = f.cameraFovY;
        pr.viewSpaceToMetersFactor = 1.0f;                 // Unity units are metres
        pr.depth = ffxApiGetResourceDX12(o->depth, FFX_API_RESOURCE_STATE_COMMON);
        pr.motionVectors = ffxApiGetResourceDX12(o->mv, FFX_API_RESOURCE_STATE_COMMON);
        memcpy(pr.cameraPosition, f.camPos, sizeof(pr.cameraPosition));
        memcpy(pr.cameraUp, f.camUp, sizeof(pr.cameraUp));
        memcpy(pr.cameraRight, f.camRight, sizeof(pr.cameraRight));
        memcpy(pr.cameraForward, f.camFwd, sizeof(pr.cameraForward));
        lastRc = (int)fn->Dispatch(&fgCtx, &pr.header);
        if (lastRc != FFX_API_RETURN_OK) { FgLog("fsr: prepare %d", lastRc); return; }
        preparedId = f.frameId;
    }

    int Generate(const FgFrame& f, ID3D12Resource* src, IDXGISwapChain4* shadow, UINT sync, UINT pf)
    {
        if (!fgCtx || !enabled || !interp || f.frameId != preparedId || f.frameId == generatedId) return 0;
        ID3D12GraphicsCommandList* l = ring.Begin();
        if (!l) return 0;

        OwnedSet12::Barrier(l, interp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ffxDispatchDescFrameGeneration dg = {};
        dg.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        dg.commandList = l;
        dg.presentColor = ffxApiGetResourceDX12(src, FFX_API_RESOURCE_STATE_PRESENT);   // Unity's finished frame, at Present
        dg.outputs[0] = ffxApiGetResourceDX12(interp, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dg.numGeneratedFrames = 1;
        dg.reset = f.reset != 0;
        dg.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;      // SDR back buffer
        dg.minMaxLuminance[0] = 0.0f; dg.minMaxLuminance[1] = 300.0f;
        dg.generationRect.left = 0; dg.generationRect.top = 0;
        dg.generationRect.width = (int32_t)outW; dg.generationRect.height = (int32_t)outH;
        dg.frameID = f.frameId;
        lastRc = (int)fn->Dispatch(&fgCtx, &dg.header);      // restores src -> PRESENT, interp -> UAV
        if (lastRc != FFX_API_RETURN_OK) {
            static int logged = 0;
            if (!logged) { logged = 1; FgLog("fsr: dispatch %d", lastRc); }
            OwnedSet12::Barrier(l, interp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            ring.EndDirect(queue);
            return 0;
        }

        ID3D12Resource* dst = NULL;
        if (FAILED(shadow->GetBuffer(shadow->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&dst)) || !dst) {
            OwnedSet12::Barrier(l, interp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            ring.EndDirect(queue);
            return 0;
        }
        OwnedSet12::Barrier(l, interp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        OwnedSet12::Barrier(l, dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(dst, interp);
        OwnedSet12::Barrier(l, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        OwnedSet12::Barrier(l, interp, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        dst->Release();
        if (!ring.EndDirect(queue)) return 0;

        HRESULT hr = shadow->Present(sync, pf);
        generatedId = f.frameId;
        if (FAILED(hr)) {
            static int logged = 0;
            if (!logged) { logged = 1; FgLog("fsr: generated Present 0x%08X", (unsigned)hr); }
            return 0;                                      // the host's real Present sees the same failure and tears down
        }
        return 1;
    }

    void SetEnabled(bool on) { enabled = on ? 1 : 0; }    // applied by the next Prepare's Configure

    void Destroy(void)
    {
        ring.WaitIdle();                                   // no submitted list may still reference the context or interp
        if (fgCtx) { fn->DestroyContext(&fgCtx, NULL); fgCtx = NULL; }
        if (interp) { interp->Release(); interp = NULL; }
        ring.Release();
        FgLog("fsr: destroyed (provider '%s')", version);
        Zero();                                            // the AMD modules stay resident; FfxLoad is idempotent
    }
};

ProviderFsr g_fsr;

} // namespace

IFgProvider* MakeFgProviderFsr(void) { return &g_fsr; }
