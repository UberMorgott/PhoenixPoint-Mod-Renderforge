// FgTwins.h - the FG host's own copies of the three inputs every frame generator is fed: hud-less colour, depth and
// motion vectors, ROW-FLIPPED into the orientation of the swapchain's back buffer.
//
// WHY: a frame generator warps the BACK BUFFER with these inputs, so their rows must be the back buffer's rows. Unity
// renders a camera with a target texture through the render-texture projection (GL.GetGPUProjectionMatrix(p, true),
// y negated), so the scene camera's colorRT / depthRT / mvRT and the upscaler's out twin hold image row 0 at the
// BOTTOM, and the present Blit turns the picture over on its way into the back buffer, whose row 0 is the TOP. The
// upscalers never see the back buffer and are consistent among themselves; the generators got the owned twins raw,
// i.e. every input mirrored against the frame they interpolate (measured 2026-10-12, Fg_Capture: hud-less vs back
// buffer correlation 0.08 with the same rows, 0.39 mirrored; docs\DESIGN.md "FG input contract"). No SDK has a flag
// for bottom-up inputs, so the host flips: one compute pass per resource on the prep list, before the provider's
// Prepare. The upscaler path keeps reading the owned twins untouched.
//
// The hud-less colour must also have the back buffer's FORMAT (DLSS-G hudLessBufferFormat, FSR FI copies it into its
// history - ffx_frameinterpolation.cpp:887 -, XeSS-FG guide "must be the same as the back buffer format"): on the FP16
// path (D3D12HalfColor) the out twin is R16G16B16A16_FLOAT linear while the back buffer is R8G8B8A8_UNORM holding
// sRGB-encoded bytes, so the same pass encodes. All four resources are ours: the barriers are safe.
#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include "D3D12Ring.h"
#include "D3D12Owned.h"

struct FgTwins
{
    enum { kHud = 0, kDepth = 1, kMv = 2, kCount = 3 };

    ID3D12Device* device;
    ID3D12RootSignature* rootSig;
    ID3D12PipelineState* pso;
    ID3D12DescriptorHeap* heap;
    UINT descSize;
    ID3D12Resource* tex[kCount];    // COMMON at rest, the providers read them there
    unsigned w, h, outW, outH;      // render size (depth, mv) and output size (hud)
    DXGI_FORMAT hudFmt;             // the back buffer's format, UNKNOWN = no hud-less twin (unsupported pair)
    DXGI_FORMAT srcOutFmt;          // the out twin's format the hud twin was built for
    bool dead;                      // compile/create failed once: never retried (no twins = frames stay unprepared)

    FgTwins() { Zero(); }
    void Zero()
    {
        device = NULL; rootSig = NULL; pso = NULL; heap = NULL; descSize = 0;
        for (int i = 0; i < kCount; ++i) tex[i] = NULL;
        w = h = outW = outH = 0; hudFmt = srcOutFmt = DXGI_FORMAT_UNKNOWN; dead = false;
    }

    // Formats every D3D12 device can write through a typed UAV. B8G8R8A8 is optional there: no hud-less twin then.
    static bool Storable(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    // Same format = a plain flipped copy; FP16 linear -> 8-bit = flipped + sRGB-encoded. Anything else is not the back
    // buffer's picture and is not tagged.
    static bool HudPair(DXGI_FORMAT outFmt, DXGI_FORMAT backFmt)
    {
        return Storable(backFmt) && (outFmt == backFmt || (outFmt == DXGI_FORMAT_R16G16B16A16_FLOAT && backFmt == DXGI_FORMAT_R8G8B8A8_UNORM));
    }

    bool Pipeline(ID3D12Device* dev)
    {
        if (pso) return true;
        if (dead) return false;
        device = dev;
        static const char kHlsl[] =
            "Texture2D<float4> src : register(t0);\n"
            "RWTexture2D<float4> dst : register(u0);\n"
            "cbuffer C : register(b0) { uint encode; };\n"
            "[numthreads(8,8,1)]\n"
            "void main(uint3 id : SV_DispatchThreadID) {\n"
            "  uint w, h; dst.GetDimensions(w, h);\n"
            "  if (id.x >= w || id.y >= h) return;\n"
            "  float4 c = src.Load(int3(id.x, h - 1 - id.y, 0));\n"
            "  if (encode) {\n"
            "    float3 l = saturate(c.rgb);\n"
            "    c.rgb = (l <= 0.0031308) ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;\n"
            "  }\n"
            "  dst[id.xy] = c;\n"
            "}\n";
        ID3DBlob* code = NULL; ID3DBlob* err = NULL;
        HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "fg_twins", NULL, NULL, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (err) err->Release();
        if (FAILED(hr) || !code) { dead = true; return false; }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp[3] = {};
        for (int i = 0; i < 2; ++i) {
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[i].DescriptorTable.NumDescriptorRanges = 1; rp[i].DescriptorTable.pDescriptorRanges = &ranges[i];
            rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[2].Constants.ShaderRegister = 0; rp[2].Constants.Num32BitValues = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3; rsd.pParameters = rp;
        ID3DBlob* sig = NULL; ID3DBlob* serr = NULL;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &serr);
        if (serr) serr->Release();
        if (SUCCEEDED(hr) && sig) { hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSig)); sig->Release(); }
        if (FAILED(hr) || !rootSig) { code->Release(); dead = true; return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig;
        pd.CS.pShaderBytecode = code->GetBufferPointer(); pd.CS.BytecodeLength = code->GetBufferSize();
        hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        code->Release();
        if (FAILED(hr) || !pso) { dead = true; return false; }

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2 * kCount * D3D12Ring::kRing;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))) || !heap) { pso->Release(); pso = NULL; dead = true; return false; }
        descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    void ReleaseTex() { for (int i = 0; i < kCount; ++i) if (tex[i]) { tex[i]->Release(); tex[i] = NULL; } }

    // Sized after the owned set; a size/format change waits for the prep ring AND for Unity's queue (render thread):
    // FSR-FG reads the hud-less twin from inside its Present, queued after the list that wrote it, so the ring's own
    // slots retiring does not prove the old textures idle. False = no depth/mv twins (the frame cannot be prepared).
    // A missing hud-less twin alone is not a failure (hudFmt stays UNKNOWN).
    bool Ensure(ID3D12Device* dev, D3D12Ring& ring, const OwnedSet12& o, DXGI_FORMAT backFmt)
    {
        if (dead || !o.depth || !o.mv || !o.out || !Pipeline(dev)) return false;   // dead: a failed create is never retried
        DXGI_FORMAT wantHud = HudPair(o.outFmt, backFmt) ? backFmt : DXGI_FORMAT_UNKNOWN;
        if (tex[kDepth] && w == o.w && h == o.h && outW == o.outW && outH == o.outH && hudFmt == wantHud && srcOutFmt == o.outFmt) return true;
        if (tex[kDepth] && (!ring.WaitIdle() || !ring.DrainQueue())) return false;
        ReleaseTex();
        w = o.w; h = o.h; outW = o.outW; outH = o.outH; hudFmt = wantHud; srcOutFmt = o.outFmt;
        tex[kDepth] = OwnedSet12::Make(device, w, h, DXGI_FORMAT_R32_FLOAT, true, L"Renderforge fg depth");
        tex[kMv]    = OwnedSet12::Make(device, w, h, DXGI_FORMAT_R16G16_FLOAT, true, L"Renderforge fg mv");
        if (hudFmt != DXGI_FORMAT_UNKNOWN) tex[kHud] = OwnedSet12::Make(device, outW, outH, hudFmt, true, L"Renderforge fg hudless");
        if (!tex[kDepth] || !tex[kMv]) { ReleaseTex(); dead = true; return false; }
        return true;
    }

    void Pass(ID3D12GraphicsCommandList* cl, int k, ID3D12Resource* src, DXGI_FORMAT srcFmt, DXGI_FORMAT dstFmt, unsigned pw, unsigned ph, bool encode, int slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
        const UINT at = (UINT)(2 * (kCount * slot + k));
        cpu.ptr += (SIZE_T)at * descSize; gpu.ptr += (UINT64)at * descSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = srcFmt; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(src, &sv, cpu);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuUav = cpu; cpuUav.ptr += descSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += descSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
        uv.Format = dstFmt; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(tex[k], NULL, &uv, cpuUav);

        OwnedSet12::Barrier(cl, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        OwnedSet12::Barrier(cl, tex[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->SetComputeRootDescriptorTable(0, gpu);
        cl->SetComputeRootDescriptorTable(1, gpuUav);
        cl->SetComputeRoot32BitConstant(2, encode ? 1u : 0u, 0);
        cl->Dispatch((pw + 7) / 8, (ph + 7) / 8, 1);
        OwnedSet12::Barrier(cl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        OwnedSet12::Barrier(cl, tex[k], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }

    // The owned twins (COMMON at rest, back in COMMON after) -> the flipped copies. `slot` = the prep ring index being recorded.
    void Run(ID3D12GraphicsCommandList* cl, const OwnedSet12& o, int slot)
    {
        ID3D12DescriptorHeap* heaps[1] = { heap };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(rootSig);
        cl->SetPipelineState(pso);
        Pass(cl, kDepth, o.depth, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, w, h, false, slot);
        Pass(cl, kMv, o.mv, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, w, h, false, slot);
        if (tex[kHud]) Pass(cl, kHud, o.out, o.outFmt, hudFmt, outW, outH, o.outFmt != hudFmt, slot);
    }

    // GPU idle (the owner released its prep ring first).
    void Release()
    {
        ReleaseTex();
        if (heap) heap->Release();
        if (pso) pso->Release();
        if (rootSig) rootSig->Release();
        Zero();
    }
};
