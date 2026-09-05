// FgHudless.h - the FG host's 8-bit hud-less twin. Every FG SDK wants the hud-less colour in the BACK BUFFER's format
// (DLSS-G hudLessBufferFormat, FSR FI copies it into its history - ffx_frameinterpolation.cpp:887 -, XeSS-FG likewise),
// and on the FP16 path (D3D12HalfColor) the owned `out` twin is R16G16B16A16_FLOAT linear while the back buffer is
// R8G8B8A8_UNORM holding sRGB-encoded bytes. One compute pass on the host's prep list encodes `out` into this twin,
// so the providers tag exactly what the back buffer will hold minus the HUD. Both resources are ours: barriers are safe.
#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include "D3D12Ring.h"

struct Hudless8
{
    ID3D12Device* device;
    ID3D12RootSignature* rootSig;
    ID3D12PipelineState* pso;
    ID3D12DescriptorHeap* heap;
    UINT descSize;
    ID3D12Resource* tex;            // COMMON at rest, the providers read it there
    unsigned w, h;
    DXGI_FORMAT fmt;
    bool dead;                      // compile/create failed once: never retried (the provider warns "hudless skipped")

    Hudless8() { Zero(); }
    void Zero() { device = NULL; rootSig = NULL; pso = NULL; heap = NULL; descSize = 0; tex = NULL; w = h = 0; fmt = DXGI_FORMAT_UNKNOWN; dead = false; }

    static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER x = {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = a; x.Transition.StateAfter = b;
        cl->ResourceBarrier(1, &x);
    }

    // Only the UNORM 8-bit formats a UAV store accepts everywhere; anything else = no twin (provider skips hudless).
    static bool Encodable(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM; }

    bool Pipeline(ID3D12Device* dev)
    {
        if (pso) return true;
        if (dead) return false;
        device = dev;
        static const char kHlsl[] =
            "Texture2D<float4> src : register(t0);\n"
            "RWTexture2D<float4> dst : register(u0);\n"
            "[numthreads(8,8,1)]\n"
            "void main(uint3 id : SV_DispatchThreadID) {\n"
            "  uint w, h; dst.GetDimensions(w, h);\n"
            "  if (id.x >= w || id.y >= h) return;\n"
            "  float4 c = src.Load(int3(id.xy, 0));\n"
            "  float3 l = saturate(c.rgb);\n"
            "  float3 s = (l <= 0.0031308) ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;\n"
            "  dst[id.xy] = float4(s, c.a);\n"
            "}\n";
        ID3DBlob* code = NULL; ID3DBlob* err = NULL;
        HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "fg_hudless8", NULL, NULL, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (err) err->Release();
        if (FAILED(hr) || !code) { dead = true; return false; }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp[2] = {};
        for (int i = 0; i < 2; ++i) {
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[i].DescriptorTable.NumDescriptorRanges = 1; rp[i].DescriptorTable.pDescriptorRanges = &ranges[i];
            rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2; rsd.pParameters = rp;
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
        hd.NumDescriptors = 2 * D3D12Ring::kRing;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))) || !heap) { pso->Release(); pso = NULL; dead = true; return false; }
        descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    // The twin at `ow x oh` in the back buffer's format. A size/format change waits for the prep ring (render thread).
    bool Ensure(ID3D12Device* dev, D3D12Ring& ring, unsigned ow, unsigned oh, DXGI_FORMAT backFmt)
    {
        if (!Encodable(backFmt) || !Pipeline(dev)) return false;
        if (tex && w == ow && h == oh && fmt == backFmt) return true;
        if (tex) { if (!ring.WaitIdle()) return false; tex->Release(); tex = NULL; }
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = ow; d.Height = oh; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = backFmt;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&tex)))) { dead = true; return false; }
        tex->SetName(L"Renderforge hudless8");
        w = ow; h = oh; fmt = backFmt;
        return true;
    }

    // `src` = the owned FP16 out twin (COMMON at rest, back in COMMON after). `slot` = the prep ring index being recorded.
    void Run(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT srcFmt, int slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T)(2 * slot) * descSize; gpu.ptr += (UINT64)(2 * slot) * descSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = srcFmt; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(src, &sv, cpu);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuUav = cpu; cpuUav.ptr += descSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += descSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
        uv.Format = fmt; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(tex, NULL, &uv, cpuUav);

        Barrier(cl, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[1] = { heap };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(rootSig);
        cl->SetPipelineState(pso);
        cl->SetComputeRootDescriptorTable(0, gpu);
        cl->SetComputeRootDescriptorTable(1, gpuUav);
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(cl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }

    // GPU idle (the owner released its prep ring first).
    void Release()
    {
        if (tex) tex->Release();
        if (heap) heap->Release();
        if (pso) pso->Release();
        if (rootSig) rootSig->Release();
        Zero();
    }
};
