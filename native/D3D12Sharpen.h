// D3D12Sharpen.h - the shim's own sharpen compute pass on D3D12, shared by every D3D12 backend that has no
// built-in sharpening (NGX DLSS, XeSS; FSR runs its own RCAS inside the dispatch and never uses this).
//
// The upscaler writes OUR `target` instead of Unity's RT whenever the pass runs, and the pass then reads it (SRV)
// and writes Unity's RT (UAV). Descriptors are written straight into the shader-visible heap, 2 per ring slot
// (SRV of target at +0, UAV of Unity's output at +1). Constants live in one persistently mapped upload buffer,
// 256 B per ring slot (a root CBV needs 256-byte alignment).
//
// NO BARRIER IS EVER ISSUED ON A UNITY RESOURCE HERE. Unity owns the state of its own RenderTextures; a transition
// of our own with a guessed StateBefore removed the device with DXGI_ERROR_DEVICE_REMOVED / INVALID_CALL
// (2026-09-02, bisected in-game). `target` is ours, so transitioning it is safe.
#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include "Device.h"
#include "RenderforgeNative.h"
#include "D3D12Ring.h"
#include "D3D12Debug.h"
#include "Sharpen.h"

struct SharpenPass12
{
    ID3D12Device* device;
    IDevice* owner;                 // sharpener / sharpenDead / lastError live on the backend
    ID3D12RootSignature* rootSig;
    ID3D12PipelineState* pso;
    ID3D12DescriptorHeap* descHeap;
    UINT descSize;
    ID3D12Resource* cb;
    unsigned char* cbCpu;
    ID3D12Resource* target;         // the upscaler's output when the pass runs; ours, created UNORDERED_ACCESS
    unsigned targetW, targetH;
    DXGI_FORMAT targetFmt;
    ID3D12Resource* logged;         // RENDERFORGE_D3D12_DEBUG: output whose pass was already logged

    SharpenPass12() { Zero(); }

    void Zero()
    {
        device = NULL; owner = NULL;
        rootSig = NULL; pso = NULL; descHeap = NULL; descSize = 0; cb = NULL; cbCpu = NULL;
        target = NULL; targetW = targetH = 0; targetFmt = DXGI_FORMAT_UNKNOWN;
        logged = NULL;
    }

    void Attach(ID3D12Device* dev, IDevice* own) { device = dev; owner = own; }

    void Fail() { owner->sharpenDead = 1; owner->lastError = DLSS_ERR_SHARPEN; }

    static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                        D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cl->ResourceBarrier(1, &b);
    }

    bool Ensure()
    {
        if (pso) return true;
        if (owner->sharpenDead || !device) return false;

        int kind = 0;
        ID3DBlob* blob = CompileSharpenBlob(&kind);
        if (!blob) { Fail(); return false; }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[3] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &ranges[0];
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &ranges[1];
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC ss = {};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderRegister = 0; ss.RegisterSpace = 0;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3; rsd.pParameters = rp;
        rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (err) err->Release();
        if (FAILED(hr) || !sig) { blob->Release(); Fail(); return false; }
        hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSig));
        sig->Release();
        if (FAILED(hr)) { blob->Release(); Fail(); return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig;
        pd.CS.pShaderBytecode = blob->GetBufferPointer();
        pd.CS.BytecodeLength = blob->GetBufferSize();
        hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        blob->Release();
        if (FAILED(hr)) { Fail(); return false; }

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2 * D3D12Ring::kRing;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descHeap)))) { Fail(); return false; }
        descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256 * D3D12Ring::kRing; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&cb)))) { Fail(); return false; }
        D3D12_RANGE noRead = { 0, 0 };
        if (FAILED(cb->Map(0, &noRead, (void**)&cbCpu)) || !cbCpu) { Fail(); return false; }

        owner->sharpener = kind;
        return true;
    }

    // Our own upscaler target, one per output size/format. Returns false (and disables the pass) when it cannot be
    // made, in which case the caller lets the upscaler write Unity's RT directly and simply skips sharpening.
    bool TargetEnsure(ID3D12Resource* output, D3D12Ring& ring)
    {
        if (owner->sharpenDead || !output || !device) return false;
        if (!Ensure()) return false;

        D3D12_RESOURCE_DESC od = output->GetDesc();
        // CreateUnorderedAccessView is void: a UAV on a resource without this flag is a debug-layer error
        // and garbage at runtime, so this is the D3D12 twin of the D3D11 CreateUnorderedAccessView guard.
        if (!(od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) { Fail(); return false; }
        unsigned w = (unsigned)od.Width, h = od.Height;
        if (target && targetW == w && targetH == h && targetFmt == od.Format) return true;

        if (target) { ring.WaitIdle(); target->Release(); target = NULL; }
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = od;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;   // the upscaler writes it, the sharpen pass reads it
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, IID_PPV_ARGS(&target)))) { Fail(); return false; }
        targetW = w; targetH = h; targetFmt = od.Format;
        RfDbg::Log("SharpenTarget: %ux%u fmt=%d created", w, h, (int)od.Format);
        return true;
    }

    // Runs on the SAME command list as the upscale, immediately after it: reads `target` (which the upscaler has
    // just written) and writes Unity's RT. Upscalers clobber the command-list state, so everything is re-bound.
    // `slot` is the ring index being recorded into.
    void Run(ID3D12GraphicsCommandList* cl, ID3D12Resource* output, float sharpness, int slot)
    {
        unsigned w = targetW, h = targetH;
        DXGI_FORMAT viewFmt = SharpenViewFormat(targetFmt);
        if (logged != output) {
            logged = output;
            RfDbg::Log("Sharpen: kind=%d sharpness=%.2f fmt=%d viewFmt=%d %ux%u slot=%d", owner->sharpener, sharpness, (int)targetFmt, (int)viewFmt, w, h, slot);
        }

        FillSharpenConstants(cbCpu + 256 * (size_t)slot, owner->sharpener, sharpness, w, h);

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = descHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = descHeap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T)(2 * slot) * descSize;
        gpu.ptr += (UINT64)(2 * slot) * descSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = viewFmt;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(target, &sv, cpu);

        D3D12_CPU_DESCRIPTOR_HANDLE cpuUav = cpu; cpuUav.ptr += descSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += descSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
        uv.Format = viewFmt;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output, NULL, &uv, cpuUav);

        Barrier(cl, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        ID3D12DescriptorHeap* heaps[1] = { descHeap };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(rootSig);
        cl->SetPipelineState(pso);
        cl->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress() + 256 * (UINT64)slot);
        cl->SetComputeRootDescriptorTable(1, gpu);
        cl->SetComputeRootDescriptorTable(2, gpuUav);
        unsigned g = SharpenGroupSize(owner->sharpener);
        cl->Dispatch((w + g - 1) / g, (h + g - 1) / g, 1);

        // target goes back to the state it was created in, so every list starts from the same known state.
        Barrier(cl, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // GPU must be idle (the owner waits on its ring first).
    void Release()
    {
        if (cb && cbCpu) { D3D12_RANGE noWrite = { 0, 0 }; cb->Unmap(0, &noWrite); cbCpu = NULL; }
        if (cb) { cb->Release(); cb = NULL; }
        if (target) { target->Release(); target = NULL; }
        if (descHeap) { descHeap->Release(); descHeap = NULL; }
        if (pso) { pso->Release(); pso = NULL; }
        if (rootSig) { rootSig->Release(); rootSig = NULL; }
        Zero();
    }
};
