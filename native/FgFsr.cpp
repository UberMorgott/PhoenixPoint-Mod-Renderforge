// FgFsr.cpp - AMD FidelityFX frame generation (SDK 2.3) as an IFgProvider, D3D12 only.
//
// The SDK's own frame-interpolation SWAPCHAIN, created with ffxCreateContextDescFrameGenerationSwapChainForHwndDX12
// (ffx_api_framegeneration_dx12.h:52) on the host's child HWND (FgWnd.cpp) - the one window in this process where
// CreateSwapChainForHwnd is legal. The proxy IS the shadow chain the host presents: the host copies Unity's finished
// back buffer into proxy.GetBuffer(current) (a replacement buffer resting in PRESENT,
// FrameInterpolationSwapchainDX12.cpp:1808/:2234) and calls proxy->Present, inside which the SDK invokes our
// frameGenerationCallback (:1934) to dispatch the FG context into ITS interpolation list, then paces both frames from
// its own present thread (frame-interpolation-swap-chain.md:132-138). ffxConfigure per frame carries swapChain +
// frameID (ffx_provider_fsr3framegeneration.cpp:243 needs the proxy's ABI tag), presentCallback NULL = the SDK's
// default composition copy, HUDLessColor = the owned hud-less frame.
//
// Inputs are the shim-owned twins the running upscaler fills (D3D12Owned.h): depth/mv at render res, out = the
// hud-less frame at output res, all resting in COMMON; passed with FFX_API_RESOURCE_STATE_COMMON so the ffx DX12
// backend barriers them itself and restores COMMON (ffx_dx12.cpp:625 maps it 1:1). Unity RTs are never touched.
//
// Model: the ANALYTICAL 3.1.x provider is pinned through ffxOverrideVersion. The ML 4.0.1 model in the same DLL
// needs RDNA4 (frame-interpolation-ml.md:75) and would otherwise be the loader's silent pick.
#include "Fg.h"
#include "RenderforgeNative.h"
#include "D3D12Owned.h"
#include "FfxLoader.h"

#include <string.h>
#include <stdio.h>

#include "ffx_api_types.h"
#include "ffx_api_dx12.h"
#include "ffx_framegeneration.h"
#include "dx12/ffx_api_framegeneration_dx12.h"

namespace {

struct ProviderFsr : IFgProvider
{
    const ffxFunctions* fn;
    ffxContext          fgCtx;
    ffxContext          scCtx;         // the proxy swapchain context
    IDXGISwapChain4*    proxy;         // owned by scCtx + the host (the host's Release is the last one)
    ID3D12Device*       device;
    ID3D12CommandQueue* queue;
    DXGI_FORMAT         backFmt;
    unsigned            outW, outH;
    int                 enabled;
    int                 lastRc;
    unsigned long long  preparedId;    // frameID of the last successful Configure + Prepare
    unsigned long long  generatedId;   // frameID of the last generated frame
    long long           generated;     // frames the proxy's callback generated (chain=child)
    int                 hudlessWarned;
    char                version[64];
    char                scVersion[64];

    // The create chains must outlive their contexts (ffx_api.h:140).
    ffxCreateContextDescFrameGeneration        descFg;
    ffxCreateContextDescFrameGenerationVersion descVer;
    ffxCreateBackendDX12Desc                   descBackend;
    ffxOverrideVersion                         descOverride;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 descSc;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 descScVer;
    DXGI_SWAP_CHAIN_DESC1                      scDesc;

    ProviderFsr() { Zero(); }
    void Zero()
    {
        fn = NULL; fgCtx = NULL; scCtx = NULL; proxy = NULL; device = NULL; queue = NULL;
        backFmt = DXGI_FORMAT_UNKNOWN; outW = outH = 0; enabled = 0; lastRc = 0;
        preparedId = generatedId = ~0ull; generated = 0; hudlessWarned = 0; version[0] = 0; scVersion[0] = 0;
        memset(&descFg, 0, sizeof(descFg)); memset(&descVer, 0, sizeof(descVer));
        memset(&descBackend, 0, sizeof(descBackend)); memset(&descOverride, 0, sizeof(descOverride));
        memset(&descSc, 0, sizeof(descSc)); memset(&descScVer, 0, sizeof(descScVer)); memset(&scDesc, 0, sizeof(scDesc));
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

    void QueryVersion(ffxContext* ctx, char* out, size_t n)
    {
        ffxQueryGetProviderVersion pv = {};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (fn->Query(ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) strncpy_s(out, n, pv.versionName, _TRUNCATE);
    }

    // The SDK proxy swapchain on the host's child HWND. Returns the proxy or NULL.
    IDXGISwapChain4* CreateProxy(const FgSetup& s)
    {
        HWND child = FgHostChildHwnd(s);
        if (!child) return NULL;
        scDesc = s.desc;                                   // FLIP_DISCARD, app format/size, >= 3 buffers, flags 0: the proxy
        scDesc.Scaling = DXGI_SCALING_STRETCH;             // adds WAITABLE + ALLOW_TEARING to the real chain itself (:1245-1249);
        scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;    // STRETCH: child rect vs buffer size disagree for a frame on resize/DPI
        descScVer.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        descScVer.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        descSc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        descSc.header.pNext = &descScVer.header;
        IDXGISwapChain4* sc = NULL;
        descSc.swapchain = &sc;
        descSc.hwnd = child;
        descSc.desc = &scDesc;
        descSc.fullscreenDesc = NULL;
        descSc.dxgiFactory = s.factory;
        descSc.gameQueue = s.queue;
        ffxReturnCode_t rc = fn->CreateContext(&scCtx, &descSc.header, NULL);
        if (rc != FFX_API_RETURN_OK || !sc) {              // unwind whichever half came up
            FgLog("fsr: swapchain context %u on child %p (chain %p)", rc, (void*)child, (void*)sc);
            if (scCtx) { fn->DestroyContext(&scCtx, NULL); scCtx = NULL; }
            if (sc) sc->Release();
            return NULL;
        }
        QueryVersion(&scCtx, scVersion, sizeof(scVersion));
        s.factory->MakeWindowAssociation(child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        FgLog("fsr: proxy swapchain %p on child hwnd %p, swapchain provider '%s'", (void*)sc, (void*)child, scVersion);
        return sc;
    }

    int Create(const FgSetup& s, IDXGISwapChain4** out)
    {
        fn = FfxLoad(s.dllDir);
        if (!fn) { FgLog("fsr: AMD DLLs not loadable from the mod folder"); return FG_ERR_NO_PROVIDER; }
        device = s.device; queue = s.queue;
        outW = s.desc.Width; outH = s.desc.Height; backFmt = s.desc.Format;

        proxy = CreateProxy(s);
        if (!proxy) return FG_ERR_NO_SWAPCHAIN;

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
        if (rc != FFX_API_RETURN_OK) { FgLog("fsr: FG context %u", rc); fgCtx = NULL; return Fail(FG_ERR_PROVIDER_FAILED); }
        QueryVersion(&fgCtx, version, sizeof(version));

        *out = proxy;
        FgLog("fsr: created, display %ux%u fmt %u, provider '%s' (override %llu), SDK swapchain (paced)", outW, outH, (unsigned)backFmt, version,
              (unsigned long long)descOverride.versionId);
        return FG_OK;
    }

    // Create failed after the proxy exists: the host never took its reference, so drop it here.
    int Fail(int rc) { IDXGISwapChain4* p = proxy; Destroy(true); if (p) p->Release(); return rc; }

    // The proxy calls this from inside its Present with its own interpolation list + output texture.
    static ffxReturnCode_t OnGenerate(ffxDispatchDescFrameGeneration* d, void* ctx)
    {
        ProviderFsr* p = (ProviderFsr*)ctx;
        ffxReturnCode_t rc = p->fn->Dispatch(&p->fgCtx, &d->header);
        p->lastRc = (int)rc;
        if (rc == FFX_API_RETURN_OK && d->numGeneratedFrames > 0) {
            ++p->generated;
            FgPresentedAdd((int)d->numGeneratedFrames);        // the proxy presents these itself; the host counts the real one
            p->generatedId = d->frameID;
        } else {
            static int logged = 0;
            if (!logged) { logged = 1; FgLog("fsr: callback dispatch %u (generated %u)", rc, d->numGeneratedFrames); }
        }
        return rc;
    }

    // Configure (once per frame, frameID must step by exactly 1) + the prepare pass, on the host's prep list.
    void Prepare(ID3D12GraphicsCommandList* list, const FgFrame& f)
    {
        const OwnedSet12* o = FgOwned12();
        if (!fgCtx || !o || !o->depth || !o->mv || !o->out) return;

        ffxConfigureDescFrameGeneration cfg = {};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = proxy;
        cfg.frameGenerationEnabled = enabled != 0;
        cfg.allowAsyncWorkloads = false;                   // everything on Unity's queue, in frame order
        cfg.flags = 0;
        cfg.frameGenerationCallback = &OnGenerate; cfg.frameGenerationCallbackUserContext = this;
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

    // The proxy generates inside its own Present (OnGenerate): nothing to do here.
    int Generate(const FgFrame&, ID3D12Resource*, IDXGISwapChain4*, UINT, UINT) { return 0; }

    void SetEnabled(bool on) { enabled = on ? 1 : 0; }    // applied by the next Prepare's Configure

    // Main thread, chain detached (the render thread no longer presents the proxy; the host's prep ring retired).
    bool Destroy(bool force)
    {
        if (fgCtx && proxy) {
            // Disable on the proxy first: this Configure waits for the presents that reference FG resources
            // (frame-interpolation-api.md:319-320), so DestroyContext never hits OBJECT_DELETED_WHILE_STILL_IN_USE.
            ffxConfigureDescFrameGeneration off = {};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = proxy;
            off.frameGenerationEnabled = false;
            off.frameID = preparedId == ~0ull ? 0 : preparedId + 1;
            fn->Configure(&fgCtx, &off.header);
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait = {};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
            fn->Dispatch(&scCtx, &wait.header);
        }
        if (fgCtx) {
            ffxReturnCode_t rc = fn->DestroyContext(&fgCtx, NULL);
            if (rc != FFX_API_RETURN_OK && !force) { FgLog("fsr: DestroyContext(fg) %u - retained", rc); return false; }
            fgCtx = NULL;
        }
        if (scCtx) {                                       // drops its ref; the host's Release is the last
            ffxReturnCode_t rc = fn->DestroyContext(&scCtx, NULL);
            if (rc != FFX_API_RETURN_OK && !force) { FgLog("fsr: DestroyContext(swapchain) %u - retained", rc); return false; }
            scCtx = NULL;
        }
        FgLog("fsr: destroyed (provider '%s', swapchain '%s', generated %lld)%s", version, scVersion, generated, force ? " (forced)" : "");
        Zero();                                            // the AMD modules stay resident; FfxLoad is idempotent
        return true;
    }
};

ProviderFsr g_fsr;

} // namespace

IFgProvider* MakeFgProviderFsr(void) { return &g_fsr; }
