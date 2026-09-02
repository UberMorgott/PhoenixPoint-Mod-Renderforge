// D3D12Owned.h - the shim-owned per-frame resources on D3D12 and the resource-state contract around them.
//
// WHY: Unity 2019.4's D3D12 resource-state tracker is not a partner a vendor SDK can work with. Measured with the
// debug layer (2026-09-02): the pre-state of a Unity RenderTexture handed to our list varied per frame
// (RENDER_TARGET, GENERIC_READ, COPY_DEST, DEPTH_WRITE on the same RT), so every barrier NGX/FSR/XeSS recorded on
// it - and every StateBefore we could hard-code - mismatched (id=527). So the shim OWNS the four resources the SDKs
// touch, and a Unity RenderTexture is only ever the SOURCE or DESTINATION of a CopyResource in our list. A copy is
// the one operation whose states are fixed (COPY_SOURCE / COPY_DEST), and those are exactly what we declare to
// IUnityGraphicsD3D12v5::ExecuteCommandList as expected == current, so Unity transitions its RT into that state
// before our list and its tracker agrees with reality after it (measured: zero id=527/538 on our list, see
// docs\DESIGN.md "D3D12 resource states").
//
// Why not Unity-side copies: CommandBuffer.CopyTexture refuses a Texture2D.CreateExternalTexture wrapper under D3D12
// ("can only copy between same texture format groups (d3d12 base formats: src=27 dst=0)" - Unity records base
// format 0 for an external resource), and CreateExternalTexture views the resource with its own format, so a
// TYPELESS one removes the device (id=28). Measured 2026-09-02, both dead ends.
//
// Unity does NOT transition a RenderTexture to the `expected` state we declare (2019.4 v5; measured 2026-09-02 with
// the debug layer: our CopyResource found the RTs in the state Unity's LAST use left them in, every frame). That
// state is deterministic per RT because the driver uses each RT the same way every frame:
//   colorRT  = the scene camera's target             -> RENDER_TARGET   (623/628 frames; 5x DEPTH_WRITE = Unity's own
//   depthRT  = target of Blit(Depth -> depthRT)      -> RENDER_TARGET    illegal barrier on it, id=524, not ours)
//   mvRT     = target of CopyTexture(MotionVectors)  -> COPY_DEST        (614/614)
//   outRT    = source of the present camera's Blit   -> GENERIC_READ     (617/617, incl. the first frame after creation)
// So our list transitions the Unity RTs from those states itself, copies, and puts them BACK, and declares
// expected == current == that same state, so Unity's tracker keeps believing what is true.
//
// Contract, per frame, inside ONE ring list (D3D11 untouched):
//   Unity colorRT / depthRT / mvRT : pre-state -> COPY_SOURCE (copy in) -> pre-state
//   Unity outRT                    : GENERIC_READ -> COPY_DEST (copy out) -> GENERIC_READ
//   owned colorIn/depthIn/mvIn      : COMMON -> COPY_DEST (copy in) -> NON_PIXEL_SHADER_RESOURCE (SDK) -> COMMON
//   owned out                       : COMMON -> UNORDERED_ACCESS (SDK, + our sharpen pass) -> COPY_SOURCE (copy out) -> COMMON
// Every owned resource starts and ends each list in COMMON, the state it was created in, so nothing depends on
// what happened in any other list. Formats: owned colour/out clone the Unity RT's format family as a fully typed
// UNORM/FLOAT (R8G8B8A8_TYPELESS -> R8G8B8A8_UNORM etc.), which CopyResource accepts (same family) and which every
// SDK can view; depth is R32_FLOAT, motion vectors R16G16_FLOAT.
#pragma once

#include <d3d12.h>
#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphicsD3D12.h"
#include "D3D12Ring.h"

struct OwnedSet12
{
    static const D3D12_RESOURCE_STATES kInSdk  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    static const D3D12_RESOURCE_STATES kOutSdk = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // The measured pre-states of the driver's Unity RTs (see the header). ponytail: constants, measured once on
    // Unity 2019.4.31; if the driver's use of an RT changes, the debug layer names the new state (id=527/538 on
    // 'Renderforge ring N') and these move with it.
    static const D3D12_RESOURCE_STATES kUnityColor = D3D12_RESOURCE_STATE_RENDER_TARGET;
    static const D3D12_RESOURCE_STATES kUnityDepth = D3D12_RESOURCE_STATE_RENDER_TARGET;
    static const D3D12_RESOURCE_STATES kUnityMv    = D3D12_RESOURCE_STATE_COPY_DEST;
    static const D3D12_RESOURCE_STATES kUnityOut   = D3D12_RESOURCE_STATE_GENERIC_READ;

    ID3D12Resource* color;
    ID3D12Resource* depth;
    ID3D12Resource* mv;
    ID3D12Resource* out;
    unsigned w, h, outW, outH;
    DXGI_FORMAT fmt, outFmt;

    OwnedSet12() { Zero(); }
    void Zero() { color = depth = mv = out = NULL; w = h = outW = outH = 0; fmt = outFmt = DXGI_FORMAT_UNKNOWN; }

    static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        if (!res || from == to) return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cl->ResourceBarrier(1, &b);
    }

    // A fully typed twin of a Unity RT format: CopyResource needs the same family, the SDKs need a typed view.
    static DXGI_FORMAT Typed(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
        default: return f;
        }
    }

    static ID3D12Resource* Make(ID3D12Device* dev, unsigned w, unsigned h, DXGI_FORMAT fmt, bool uav, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* r = NULL;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&r)))) return NULL;
        r->SetName(name);   // debug-layer messages then name OUR resource instead of "RenderTexture-2D-..."
        return r;
    }

    // Sized/formatted after the Unity RTs of this frame; idempotent while they do not change. A change waits for
    // every list still referencing the old set before it is released (render thread).
    bool Ensure(ID3D12Device* dev, D3D12Ring& ring, ID3D12Resource* unityColor, ID3D12Resource* unityOut)
    {
        D3D12_RESOURCE_DESC cd = unityColor->GetDesc(), od = unityOut->GetDesc();
        DXGI_FORMAT cf = Typed(cd.Format), of = Typed(od.Format);
        unsigned cw = (unsigned)cd.Width, ch = cd.Height, ow = (unsigned)od.Width, oh = od.Height;
        if (color && fmt == cf && outFmt == of && w == cw && h == ch && outW == ow && outH == oh) return true;
        if (color) ring.WaitIdle();
        Release();
        fmt = cf; outFmt = of; w = cw; h = ch; outW = ow; outH = oh;
        color = Make(dev, cw, ch, cf, false, L"Renderforge colorIn");
        depth = Make(dev, cw, ch, DXGI_FORMAT_R32_FLOAT, false, L"Renderforge depthIn");
        mv    = Make(dev, cw, ch, DXGI_FORMAT_R16G16_FLOAT, false, L"Renderforge mvIn");
        out   = Make(dev, ow, oh, of, true, L"Renderforge out");
        if (!color || !depth || !mv || !out) { Release(); return false; }
        return true;
    }

    void Release()
    {
        if (color) color->Release();
        if (depth) depth->Release();
        if (mv)    mv->Release();
        if (out)   out->Release();
        Zero();
    }

    // The Unity RTs are in their pre-states before AND after our list (we put them back), and that is what Unity's
    // tracker is told. Depth/mv are NULL in passthrough.
    static int Declare(UnityGraphicsD3D12ResourceState* st, ID3D12Resource* uColor, ID3D12Resource* uDepth, ID3D12Resource* uMv, ID3D12Resource* uOut)
    {
        int n = 0;
        ID3D12Resource* res[4] = { uColor, uDepth, uMv, uOut };
        D3D12_RESOURCE_STATES pre[4] = { kUnityColor, kUnityDepth, kUnityMv, kUnityOut };
        for (int i = 0; i < 4; ++i) if (res[i]) { st[n].resource = res[i]; st[n].expected = st[n].current = pre[i]; ++n; }
        return n;
    }

    // Copy a Unity RT (in `pre`) into `dst` (ours, COMMON), leaving both where the caller wants them.
    static void CopyFromUnity(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, D3D12_RESOURCE_STATES dstAfter,
                              ID3D12Resource* src, D3D12_RESOURCE_STATES pre)
    {
        Barrier(cl, src, pre, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(dst, src);
        Barrier(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, pre);
        Barrier(cl, dst, D3D12_RESOURCE_STATE_COPY_DEST, dstAfter);
    }

    // Unity RTs -> owned inputs, then the inputs and the output into the SDK states.
    void Enter(ID3D12GraphicsCommandList* cl, ID3D12Resource* uColor, ID3D12Resource* uDepth, ID3D12Resource* uMv)
    {
        CopyFromUnity(cl, color, kInSdk, uColor, kUnityColor);
        CopyFromUnity(cl, depth, kInSdk, uDepth, kUnityDepth);
        CopyFromUnity(cl, mv,    kInSdk, uMv,    kUnityMv);
        Barrier(cl, out, D3D12_RESOURCE_STATE_COMMON, kOutSdk);
    }

    // Inputs back to COMMON; owned output -> Unity's outRT (put back into its pre-state), out back to COMMON.
    // Starts from the SDK states (NGX and ffx restore them, XeSS never changes them).
    void Leave(ID3D12GraphicsCommandList* cl, ID3D12Resource* uOut)
    {
        Barrier(cl, color, kInSdk, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, depth, kInSdk, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, mv,    kInSdk, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, out, kOutSdk, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, uOut, kUnityOut, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(uOut, out);
        Barrier(cl, uOut, D3D12_RESOURCE_STATE_COPY_DEST, kUnityOut);
        Barrier(cl, out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // Passthrough (DebugView): Unity colorRT -> Unity outRT directly, same-size, both put back.
    static void Passthrough(ID3D12GraphicsCommandList* cl, ID3D12Resource* uColor, ID3D12Resource* uOut)
    {
        Barrier(cl, uColor, kUnityColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, uOut, kUnityOut, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(uOut, uColor);
        Barrier(cl, uColor, D3D12_RESOURCE_STATE_COPY_SOURCE, kUnityColor);
        Barrier(cl, uOut, D3D12_RESOURCE_STATE_COPY_DEST, kUnityOut);
    }
};
