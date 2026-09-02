// D3D12Ring.h - the shared command-list ring for every D3D12 backend (NGX DLSS, FSR, later XeSS).
//
// We never own a queue: Unity owns it and tracks the state of its own RenderTextures, so a recorded list is
// handed to IUnityGraphicsD3D12v5::ExecuteCommandList together with the resource states the vendor SDK needs.
// Unity inserts the transition barriers and orders our list against its own rendering. ExecuteCommandList
// returns the value that will be signalled on Unity's frame fence; a slot is reused only after that value
// has been reached.
//
// Unity keeps up to 3 frames in flight; one spare so a create + evaluate in the same frame never waits.
// More than kRing submissions in ONE frame would block in Begin() on Unity's frame fence, which only
// advances at frame end - i.e. a deadlock until kFenceWaitMs. Keep submissions per frame < kRing.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include "RenderforgeNative.h"
#include "unity/IUnityInterface.h"
#include "unity/IUnityGraphicsD3D12.h"

// Set by UnityPluginLoad (RenderforgeNative.cpp) or Dlss_TestSetUnityD3D12, NULLed on kUnityGfxDeviceEventShutdown.
// Never cached: read per use.
extern IUnityGraphicsD3D12v5* g_unityD3D12;

struct D3D12Ring
{
    static const int kRing = 4;
    static const DWORD kFenceWaitMs = 5000;   // a hung GPU must not deadlock the render thread forever

    ID3D12Device* device;
    ID3D12CommandAllocator* alloc[kRing];
    ID3D12GraphicsCommandList* list[kRing];
    UINT64 submitted[kRing];      // fence value ExecuteCommandList returned for that slot, 0 = never used
    int ringIdx;
    int recording;                // a list is open; End() must be reached on every path
    HANDLE waitEvent;
    int failCode;                 // DLSS_ERR_* of the last Begin() failure, for the caller's lastError

    D3D12Ring() { Zero(); }

    void Zero()
    {
        device = NULL;
        for (int i = 0; i < kRing; ++i) { alloc[i] = NULL; list[i] = NULL; submitted[i] = 0; }
        ringIdx = 0; recording = 0; waitEvent = NULL; failCode = 0;
    }

    void Attach(ID3D12Device* dev) { device = dev; }

    // WAIT_OBJECT_0 once `value` has retired (0 = never submitted, retired by definition), else the
    // WaitForSingleObject result (WAIT_TIMEOUT / WAIT_FAILED). Callers must not touch the slot otherwise.
    DWORD WaitFence(UINT64 value)
    {
        if (!value) return WAIT_OBJECT_0;
        IUnityGraphicsD3D12v5* unity = g_unityD3D12;
        ID3D12Fence* fence = unity ? unity->GetFrameFence() : NULL;
        if (!fence) return WAIT_FAILED;
        if (fence->GetCompletedValue() >= value) return WAIT_OBJECT_0;
        if (!waitEvent) waitEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!waitEvent) return WAIT_FAILED;
        if (FAILED(fence->SetEventOnCompletion(value, waitEvent))) return WAIT_FAILED;
        return WaitForSingleObject(waitEvent, kFenceWaitMs);
    }

    // Blocks until every list we ever submitted has retired. Required before destroying anything a submitted
    // list referenced (NGX feature handles - guide p.54 5.5 - or ffx contexts).
    void WaitIdle()
    {
        for (int i = 0; i < kRing; ++i) WaitFence(submitted[i]);
    }

    void ReleaseSlot(int i)
    {
        if (list[i]) { list[i]->Release(); list[i] = NULL; }
        if (alloc[i]) { alloc[i]->Release(); alloc[i] = NULL; }
    }

    // Returns an open, recording DIRECT command list, or NULL with failCode set.
    ID3D12GraphicsCommandList* Begin()
    {
        if (!device || !g_unityD3D12 || recording) { failCode = DLSS_ERR_NO_CONTEXT; return NULL; }
        int i = ringIdx;
        // An in-flight allocator must never be reset: on timeout/failure leave the slot alone and bail.
        if (WaitFence(submitted[i]) != WAIT_OBJECT_0) { failCode = DLSS_ERR_FENCE_TIMEOUT; return NULL; }
        if (!alloc[i] || !list[i]) {
            ReleaseSlot(i);   // a half-built pair from an earlier failure
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], NULL, IID_PPV_ARGS(&list[i])))) {
                ReleaseSlot(i); failCode = DLSS_ERR_NO_CONTEXT; return NULL;
            }
            list[i]->Close();
        }
        if (FAILED(alloc[i]->Reset()) || FAILED(list[i]->Reset(alloc[i], NULL))) { failCode = DLSS_ERR_NO_CONTEXT; return NULL; }
        recording = 1;
        return list[i];
    }

    // Closes the current list and hands it to Unity. `states` may be NULL when stateCount is 0.
    bool End(int stateCount, UnityGraphicsD3D12ResourceState* states)
    {
        if (!recording) return false;
        int i = ringIdx;
        recording = 0;
        ringIdx = (ringIdx + 1) % kRing;
        IUnityGraphicsD3D12v5* unity = g_unityD3D12;
        if (FAILED(list[i]->Close()) || !unity) return false;
        UINT64 v = unity->ExecuteCommandList(list[i], stateCount, states);
        // 0 would mark the slot "never used" and let Begin() reset an allocator the GPU may still read.
        // Unity signals the frame fence with GetNextFrameFenceValue() at the end of this frame, after our
        // list - waiting on that is the conservative, always-correct choice.
        submitted[i] = v ? v : unity->GetNextFrameFenceValue();
        return true;
    }

    void Release()
    {
        WaitIdle();
        for (int i = 0; i < kRing; ++i) { ReleaseSlot(i); submitted[i] = 0; }
        if (waitEvent) { CloseHandle(waitEvent); waitEvent = NULL; }
        ringIdx = 0; recording = 0; device = NULL; failCode = 0;
    }
};
