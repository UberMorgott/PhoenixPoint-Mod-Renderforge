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
#include <string.h>
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

    static const int kStates = 4;   // colour + depth + motion vectors + output, the widest declaration we make

    ID3D12Device* device;
    ID3D12CommandAllocator* alloc[kRing];
    ID3D12GraphicsCommandList* list[kRing];
    UINT64 submitted[kRing];      // our own fence value for that slot's submission, 0 = never used
    // Our own fence, signalled on Unity's queue right after ExecuteCommandList. The value Unity returns is a
    // FRAME fence value and retires independently of when the list actually runs, so gating allocator reuse on
    // it resets allocators mid-execution - the D3D12 debug layer says so outright: "A command allocator is being
    // reset before previous executions associated with the allocator have completed" (89x in one 200 s tactical
    // run), followed by "Failed to execute a command list because the command queue fence has not advanced past
    // previous executions". That is the DXGI_ERROR_DEVICE_REMOVED / INVALID_CALL (2026-09-02).
    ID3D12Fence* fence;
    UINT64 fenceVal;
    int ringIdx;
    int recording;                // a list is open; End() must be reached on every path
    HANDLE waitEvent;
    int failCode;                 // DLSS_ERR_* of the last Begin() failure, for the caller's lastError

    // The resource states handed to ExecuteCommandList, one array per ring slot. They MUST outlive the call:
    // Unity executes the list on a worker thread and reads this memory after our render event has returned, so
    // a stack local here is a use-after-free that shows up as garbage barriers and a DEVICE_REMOVED/INVALID_CALL
    // seconds later (root cause of the D3D12 crash, 2026-09-02). A slot's array is only rewritten once its
    // previous submission has retired, which Begin() already waits for.
    UnityGraphicsD3D12ResourceState states[kRing][kStates];

    D3D12Ring() { Zero(); }

    void Zero()
    {
        device = NULL;
        for (int i = 0; i < kRing; ++i) { alloc[i] = NULL; list[i] = NULL; submitted[i] = 0; }
        fence = NULL; fenceVal = 0;
        ringIdx = 0; recording = 0; waitEvent = NULL; failCode = 0;
        memset(states, 0, sizeof(states));
    }

    // The state array to fill for the submission currently being recorded (kStates entries).
    UnityGraphicsD3D12ResourceState* StateSlot() { return states[ringIdx]; }

    void Attach(ID3D12Device* dev)
    {
        device = dev;
        if (device && !fence) device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    }

    // WAIT_OBJECT_0 once `value` has retired (0 = never submitted, retired by definition), else the
    // WaitForSingleObject result (WAIT_TIMEOUT / WAIT_FAILED). Callers must not touch the slot otherwise.
    DWORD WaitFence(UINT64 value)
    {
        if (!value) return WAIT_OBJECT_0;
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

    // Closes the current list and hands it to Unity, together with the first `stateCount` entries of this
    // slot's StateSlot() array (which stays alive until the slot is reused - see `states` above).
    bool End(int stateCount)
    {
        if (!recording) return false;
        int i = ringIdx;
        recording = 0;
        ringIdx = (ringIdx + 1) % kRing;
        IUnityGraphicsD3D12v5* unity = g_unityD3D12;
        if (FAILED(list[i]->Close()) || !unity) return false;
        unity->ExecuteCommandList(list[i], stateCount, stateCount ? states[i] : NULL);
        ID3D12CommandQueue* q = unity->GetCommandQueue();
        if (q && fence && SUCCEEDED(q->Signal(fence, fenceVal + 1))) { submitted[i] = ++fenceVal; return true; }
        // No queue or no fence: we have no way to know when this list retires, and resetting its allocator
        // later would be the very bug above. Drop the pair instead - Begin() builds a fresh one, and a fresh
        // allocator is always safe to reset.
        ReleaseSlot(i);
        submitted[i] = 0;
        return true;
    }

    void Release()
    {
        WaitIdle();
        for (int i = 0; i < kRing; ++i) { ReleaseSlot(i); submitted[i] = 0; }
        if (waitEvent) { CloseHandle(waitEvent); waitEvent = NULL; }
        if (fence) { fence->Release(); fence = NULL; }
        fenceVal = 0;
        ringIdx = 0; recording = 0; device = NULL; failCode = 0;
    }
};
