// D3D12Sharpen.h - the shim's own sharpen compute pass on D3D12, shared by every D3D12 backend that has no
// built-in sharpening (NGX DLSS, XeSS; FSR uses it for color grading after its own RCAS).
//
// The upscaler writes OUR `target` instead of Unity's RT whenever the pass runs, and the pass then reads it (SRV)
// and writes Unity's RT (UAV). Descriptors are written straight into the shader-visible heap, 2 per ring slot
// (SRV of target at +0, UAV of Unity's output at +1). Constants live in one persistently mapped upload buffer,
// 256 B per ring slot (a root CBV needs 256-byte alignment).
//
// Normal upscaler post-processing never barriers a Unity resource. The passthrough helper is the sole exception:
// it uses the same declared Unity states as D3D12Owned.h and restores them before submission returns.
#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include "Device.h"
#include "RenderforgeNative.h"
#include "D3D12Ring.h"
#include "D3D12Owned.h"
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
    bool psoHdr;                    // the PSO was compiled with NIS_HDR_MODE linear (FP16 output, D3D12HalfColor)
    bool psoGrade;                  // analytic RCAS + color-grade PSO instead of NIS/RCAS sharpen-only
    ID3D12Resource* logged;         // RENDERFORGE_D3D12_DEBUG: output whose pass was already logged

    SharpenPass12() { Zero(); }

    void Zero()
    {
        device = NULL; owner = NULL;
        rootSig = NULL; pso = NULL; descHeap = NULL; descSize = 0; cb = NULL; cbCpu = NULL;
        target = NULL; targetW = targetH = 0; targetFmt = DXGI_FORMAT_UNKNOWN; psoHdr = false; psoGrade = false;
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

    // hdr = the output format is FP16 (SharpenIsHdr): the PSO variant follows it. A flip (D3D12HalfColor toggled
    // live) rebuilds only the PSO; the caller has waited on the ring. Everything else is created once.
    bool Ensure(bool hdr, bool colorGrade)
    {
        if (pso && psoHdr == hdr && psoGrade == colorGrade) return true;
        if (owner->sharpenDead || !device) return false;

        int kind = 0;
        ID3DBlob* blob = CompileSharpenBlob(&kind, hdr, colorGrade);
        if (!blob) { Fail(); return false; }
        if (pso) { pso->Release(); pso = NULL; }
        if (rootSig) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = rootSig;
            pd.CS.pShaderBytecode = blob->GetBufferPointer();
            pd.CS.BytecodeLength = blob->GetBufferSize();
            HRESULT hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
            blob->Release();
            if (FAILED(hr)) { Fail(); return false; }
            psoHdr = hdr; psoGrade = colorGrade; owner->sharpener = kind;
            return true;
        }

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
        psoHdr = hdr; psoGrade = colorGrade;

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2 * D3D12Ring::kRing;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hh = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descHeap));
        // pso is already set above, so Ensure() would short-circuit on the next call and Run() would then
        // dereference a null heap. Undo the PSO too, and say what the heap actually came back as: the debug
        // layer's id=1315 ("GetGPUDescriptorHandleForHeapStart on a heap that is not SHADER_VISIBLE") is
        // otherwise unattributable between this heap and the vendor SDKs' own heaps.
        if (FAILED(hh) || !descHeap) {
            RfDbg::Log("Sharpen: CreateDescriptorHeap failed hr=0x%08X", (unsigned)hh);
            if (pso) { pso->Release(); pso = NULL; }
            Fail(); return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC got = descHeap->GetDesc();
        RfDbg::Log("Sharpen: descHeap=%p num=%u flags=0x%X (1 = SHADER_VISIBLE)", (void*)descHeap, got.NumDescriptors, (unsigned)got.Flags);
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
    bool TargetEnsure(ID3D12Resource* output, D3D12Ring& ring, bool colorGrade = false)
    {
        if (owner->sharpenDead || !output || !device) return false;

        D3D12_RESOURCE_DESC od = output->GetDesc();
        // CreateUnorderedAccessView is void: a UAV on a resource without this flag is a debug-layer error
        // and garbage at runtime, so this is the D3D12 twin of the D3D11 CreateUnorderedAccessView guard.
        if (!(od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) { Fail(); return false; }
        unsigned w = (unsigned)od.Width, h = od.Height;
        bool sameTarget = target && targetW == w && targetH == h && targetFmt == od.Format;
        if (sameTarget && pso
            && psoHdr == SharpenIsHdr(od.Format) && psoGrade == colorGrade) return true;

        // PSOs/resources referenced by prior ring slots must stay alive until their fences retire.
        // A timeout leaves the old PSO/target alive; callers skip the post pass for this frame.
        if ((target || pso) && !ring.WaitIdle()) return false;
        if (!Ensure(SharpenIsHdr(od.Format), colorGrade)) return false;
        if (sameTarget) return true;
        if (target) { target->Release(); target = NULL; }
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
    void Run(ID3D12GraphicsCommandList* cl, ID3D12Resource* output, float sharpness,
             int lutPreset, float lutStrength, int slot, const SceneStyleParams& style)
    {
        unsigned w = targetW, h = targetH;
        DXGI_FORMAT viewFmt = SharpenViewFormat(targetFmt);
        if (logged != output) {
            logged = output;
            RfDbg::Log("Post: kind=%d sharpness=%.2f lut=%d strength=%.2f fmt=%d viewFmt=%d hdr=%d %ux%u slot=%d",
                       owner->sharpener, sharpness, lutPreset, lutStrength, (int)targetFmt, (int)viewFmt, (int)psoHdr, w, h, slot);
        }

        FillSharpenConstants(cbCpu + 256 * (size_t)slot, owner->sharpener, sharpness, w, h,
                             lutPreset, lutStrength, psoHdr, style);

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

    // Full-resolution passthrough with a post effect: copy Unity color into our target, run the same post shader
    // into owned.out, then copy to Unity output. Unity resources are restored to their declared states.
    bool RunPassthrough(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* output,
                        OwnedSet12& owned, D3D12Ring& ring, bool srgbViews,
                        float sharpness, int lutPreset, float lutStrength, int slot, const SceneStyleParams& style)
    {
        bool grade = ColorGradeEnabled(lutPreset, lutStrength) || SceneStyleEnabled(style);
        if (!owned.Ensure(device, ring, color, output, srgbViews)
            || !TargetEnsure(owned.out, ring, grade)) return false;

        Barrier(cl, color, OwnedSet12::kUnityColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(target, color);
        Barrier(cl, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cl, color, D3D12_RESOURCE_STATE_COPY_SOURCE, OwnedSet12::kUnityColor);

        Barrier(cl, owned.out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Run(cl, owned.out, sharpness, lutPreset, lutStrength, slot, style);
        Barrier(cl, owned.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, output, OwnedSet12::kUnityOut, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(output, owned.out);
        Barrier(cl, output, D3D12_RESOURCE_STATE_COPY_DEST, OwnedSet12::kUnityOut);
        Barrier(cl, owned.out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        return true;
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
