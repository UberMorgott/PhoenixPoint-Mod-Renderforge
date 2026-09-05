// D3D12Ring.h - the shared command-list ring for every D3D12 backend (NGX DLSS, FSR, later XeSS).
//
// We never own a queue: Unity owns it, so a recorded list is handed to IUnityGraphicsD3D12v5::ExecuteCommandList,
// which orders it against Unity's own copies into / out of the resources we own (D3D12Owned.h). A slot is reused
// only after both our own fence and the frame-fence value ExecuteCommandList returned have retired.
//
// Unity keeps up to 3 frames in flight; one spare so a create + evaluate in the same frame never waits.
// More than kRing submissions in ONE frame would block in Begin() on Unity's frame fence, which only
// advances at frame end - i.e. a deadlock until kFenceWaitMs. Keep submissions per frame < kRing.
#pragma once

#include <windows.h>
#include <string.h>
#include <d3d12.h>
#include "RenderforgeNative.h"
#include "D3D12Debug.h"
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
    bool trackingLost; // a failed queue Signal cannot prove retirement
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
    // Unity's frame-fence value ExecuteCommandList returned for that slot. Unity submits our list on a worker
    // thread, so our own Signal (issued from the render thread) can land on the queue BEFORE the list itself and
    // retire early; the frame fence is signalled by that worker after the frame's lists. A slot is reused only
    // when BOTH have retired (measured 2026-09-02: see the "REUSE" log line in Begin()).
    UINT64 unityVal[kRing];
    UINT64 submissions;           // total End() count, for the debug log
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

    // GPU timestamps, kStamps per list: 0 = Begin, 1 = after copy-in, 2 = after the upscale (+ sharpen), 3 = End
    // (End() fills any stamp a caller skipped, so the deltas of a passthrough/create list are 0). Resolved into one
    // persistently mapped READBACK buffer per slot and read in Begin() once that slot has retired (kRing frames
    // later), so no allocation and no stall per frame. Together with the CPU time Begin() spent waiting on the
    // slot's fences they feed the four EMAs (alpha 1/60 ~ a 60-frame window) Dlss_Timings reports.
    static const int kStamps = 4;
    ID3D12QueryHeap* qheap;
    ID3D12Resource* qbuf;
    UINT64* qCpu;
    double msPerTick;             // 1000 / GetTimestampFrequency, 0 until the first End()
    int stamped;                  // bitmask of the stamps already recorded into the open list
    float copyInMs, evalMs, copyOutMs, ringWaitMs;

    D3D12Ring() { Zero(); }

    void Zero()
    {
        device = NULL; trackingLost = false;
        for (int i = 0; i < kRing; ++i) { alloc[i] = NULL; list[i] = NULL; submitted[i] = 0; unityVal[i] = 0; }
        fence = NULL; fenceVal = 0; submissions = 0;
        ringIdx = 0; recording = 0; waitEvent = NULL; failCode = 0;
        memset(states, 0, sizeof(states));
        qheap = NULL; qbuf = NULL; qCpu = NULL; msPerTick = 0; stamped = 0;
        copyInMs = evalMs = copyOutMs = ringWaitMs = 0;
    }

    // The state array to fill for the submission currently being recorded (kStates entries).
    UnityGraphicsD3D12ResourceState* StateSlot() { return states[ringIdx]; }

    void Attach(ID3D12Device* dev)
    {
        device = dev;
        if (device && !fence) device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (device && !qheap) {
            D3D12_QUERY_HEAP_DESC qd = {};
            qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qd.Count = kRing * kStamps;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = sizeof(UINT64) * kRing * kStamps; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_RANGE all = { 0, (SIZE_T)bd.Width };
            if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&qheap))) ||
                FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&qbuf))) ||
                FAILED(qbuf->Map(0, &all, (void**)&qCpu))) {
                ReleaseTimings();   // no timestamps, everything else works
            }
        }
    }

    void ReleaseTimings()
    {
        if (qbuf && qCpu) { D3D12_RANGE noWrite = { 0, 0 }; qbuf->Unmap(0, &noWrite); }
        if (qbuf) qbuf->Release();
        if (qheap) qheap->Release();
        qbuf = NULL; qheap = NULL; qCpu = NULL;
    }

    // Record timestamp `k` (1 = after copy-in, 2 = after the upscale) into the open list. 0 and 3 are the ring's own.
    void Stamp(int k)
    {
        if (!qheap || !recording || (stamped & (1 << k))) return;
        list[ringIdx]->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(ringIdx * kStamps + k));
        stamped |= 1 << k;
    }

    static void Ema(float& a, float x) { a += (x - a) * (1.0f / 60.0f); }

    // Slot `i` has retired: fold its four timestamps into the EMAs.
    void ReadStamps(int i)
    {
        if (!qCpu || !submitted[i] || msPerTick <= 0) return;
        const UINT64* t = qCpu + i * kStamps;
        if (t[0] > t[1] || t[1] > t[2] || t[2] > t[3]) return;   // never resolved / garbage
        Ema(copyInMs,  (float)((t[1] - t[0]) * msPerTick));
        Ema(evalMs,    (float)((t[2] - t[1]) * msPerTick));
        Ema(copyOutMs, (float)((t[3] - t[2]) * msPerTick));
    }

    // Stamps 1..3 (missing ones collapse onto 3) + the resolve into this slot's readback range, before Close().
    void FinishStamps(int i, ID3D12CommandQueue* q)
    {
        if (!qheap) return;
        if (msPerTick <= 0 && q) { UINT64 f = 0; if (SUCCEEDED(q->GetTimestampFrequency(&f)) && f) msPerTick = 1000.0 / (double)f; }
        Stamp(1); Stamp(2); Stamp(3);
        list[i]->ResolveQueryData(qheap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(i * kStamps), kStamps, qbuf, (UINT64)i * kStamps * sizeof(UINT64));
    }

    // WAIT_OBJECT_0 once `value` has retired on `f` (0 = never submitted, retired by definition), else the
    // WaitForSingleObject result (WAIT_TIMEOUT / WAIT_FAILED). Callers must not touch the slot otherwise.
    DWORD WaitOn(ID3D12Fence* f, UINT64 value, DWORD waitMs = kFenceWaitMs)
    {
        if (!value) return WAIT_OBJECT_0;
        if (!f) return WAIT_FAILED;
        if (f->GetCompletedValue() >= value) return WAIT_OBJECT_0;
        if (!waitMs) return WAIT_TIMEOUT;
        if (!waitEvent) waitEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!waitEvent) return WAIT_FAILED;
        if (FAILED(f->SetEventOnCompletion(value, waitEvent))) return WAIT_FAILED;
        return WaitForSingleObject(waitEvent, waitMs);
    }

    // Slot `i` fully retired: our own fence AND Unity's frame fence (see unityVal).
    DWORD WaitSlot(int i, DWORD waitMs = kFenceWaitMs)
    {
        DWORD r = WaitOn(fence, submitted[i], waitMs);
        if (r != WAIT_OBJECT_0) return r;
        IUnityGraphicsD3D12v5* unity = g_unityD3D12;
        ID3D12Fence* ff = unity ? unity->GetFrameFence() : NULL;
        if (!unityVal[i]) return WAIT_OBJECT_0;
        if (!ff) return WAIT_FAILED;
        if (waitMs && ff->GetCompletedValue() < unityVal[i])
            RfDbg::Log("REUSE slot=%d: own fence %llu retired but Unity frame fence %llu < %llu - waiting",
                       i, (unsigned long long)submitted[i], (unsigned long long)ff->GetCompletedValue(), (unsigned long long)unityVal[i]);
        return WaitOn(ff, unityVal[i], waitMs);
    }

    // Blocks until every list we ever submitted has retired. Required before destroying anything a submitted
    // list referenced (NGX feature handles - guide p.54 5.5 - or ffx contexts). False = a slot timed out or a
    // fence failed: something may still be executing (unless the device is gone - see Removed()).
    bool WaitIdle(DWORD waitMs = kFenceWaitMs)
    {
        if (trackingLost) return false;
        bool ok = true;
        for (int i = 0; i < kRing; ++i) if (WaitSlot(i, waitMs) != WAIT_OBJECT_0) ok = false;
        return ok;
    }

    bool Removed() { return device && FAILED(device->GetDeviceRemovedReason()); }

    // A failed Signal loses proof of retirement. Retain the complete ring and stop submitting until
    // device removal; starting another generation cannot make its referenced resources safe to destroy.
    void Quarantine(int i)
    {
        RfDbg::Log("ring: slot %d quarantined (fence Signal failed, removed=%d)", i, Removed() ? 1 : 0);
        trackingLost = true; // retain the allocator/list and every referenced resource until device removal
    }

    void ReleaseSlot(int i)
    {
        if (list[i]) { list[i]->Release(); list[i] = NULL; }
        if (alloc[i]) { alloc[i]->Release(); alloc[i] = NULL; }
    }

    // Returns an open, recording DIRECT command list, or NULL with failCode set.
    ID3D12GraphicsCommandList* Begin()
    {
        if (!device || !g_unityD3D12 || recording || trackingLost) { failCode = DLSS_ERR_NO_CONTEXT; return NULL; }
        int i = ringIdx;
        // An in-flight allocator must never be reset: on timeout/failure leave the slot alone and bail.
        LARGE_INTEGER t0, t1, qpf;
        QueryPerformanceCounter(&t0);
        DWORD w = WaitSlot(i);
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&qpf);
        Ema(ringWaitMs, (float)((t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qpf.QuadPart));
        if (w != WAIT_OBJECT_0) { failCode = DLSS_ERR_FENCE_TIMEOUT; return NULL; }
        ReadStamps(i);
        if (!alloc[i] || !list[i]) {
            ReleaseSlot(i);   // a half-built pair from an earlier failure
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], NULL, IID_PPV_ARGS(&list[i])))) {
                ReleaseSlot(i); failCode = DLSS_ERR_NO_CONTEXT; return NULL;
            }
            static const wchar_t* const names[kRing] = { L"Renderforge ring 0", L"Renderforge ring 1", L"Renderforge ring 2", L"Renderforge ring 3" };
            list[i]->SetName(names[i]);      // so debug-layer messages name OUR list, not "Unnamed ID3D12GraphicsCommandList"
            list[i]->Close();
        }
        if (FAILED(alloc[i]->Reset()) || FAILED(list[i]->Reset(alloc[i], NULL))) { failCode = DLSS_ERR_NO_CONTEXT; return NULL; }
        recording = 1;
        stamped = 0;
        Stamp(0);
        return list[i];
    }

    // Closes the current list and hands it to Unity, together with the first `stateCount` entries of this
    // slot's StateSlot() array (which stays alive until the slot is reused - see `states` above).
    bool End(int stateCount)
    {
        if (!recording) return false;
        int i = ringIdx;
        IUnityGraphicsD3D12v5* unity = g_unityD3D12;
        FinishStamps(i, unity ? unity->GetCommandQueue() : NULL);
        recording = 0;
        ringIdx = (ringIdx + 1) % kRing;
        if (FAILED(list[i]->Close()) || !unity) return false;
        unityVal[i] = unity->ExecuteCommandList(list[i], stateCount, stateCount ? states[i] : NULL);
        ID3D12CommandQueue* q = unity->GetCommandQueue();
        if (++submissions <= 16 && RfDbg::On()) {
            ID3D12Fence* ff = unity->GetFrameFence();
            RfDbg::Log("submit #%llu slot=%d states=%d unityRet=%llu unityNext=%llu unityDone=%llu own=%llu",
                       (unsigned long long)submissions, i, stateCount, (unsigned long long)unityVal[i],
                       (unsigned long long)unity->GetNextFrameFenceValue(), (unsigned long long)(ff ? ff->GetCompletedValue() : 0),
                       (unsigned long long)(fenceVal + 1));
        }
        if (q && fence && SUCCEEDED(q->Signal(fence, fenceVal + 1))) { submitted[i] = ++fenceVal; return true; }
        // No queue, no fence or a failed Signal: we have no way to know when this list retires, and resetting
        // (or releasing) its allocator later would be the very bug above. Quarantine the pair.
        Quarantine(i);
        return false;
    }

    // End() for work that must NOT go through Unity (the FG host's backbuffer copy inside the Present hook,
    // where Unity's own frame is already submitted): close + execute straight on `q`, gated by our fence only.
    bool EndDirect(ID3D12CommandQueue* q)
    {
        if (!recording || !q) return false;
        int i = ringIdx;
        FinishStamps(i, q);
        recording = 0;
        ringIdx = (ringIdx + 1) % kRing;
        if (FAILED(list[i]->Close())) return false;
        ID3D12CommandList* lists[1] = { list[i] };
        q->ExecuteCommandLists(1, lists);
        unityVal[i] = 0;
        if (fence && SUCCEEDED(q->Signal(fence, fenceVal + 1))) { submitted[i] = ++fenceVal; return true; }
        Quarantine(i);    // same reasoning as End(): an untracked allocator must never be reset
        return false;
    }

    // A timeout retains every handle and fence value for a later retry. Never abandon ownership.
    bool Release()
    {
        if (!WaitIdle(0) && !Removed()) return false;
        for (int i = 0; i < kRing; ++i) { ReleaseSlot(i); submitted[i] = 0; unityVal[i] = 0; }
        if (waitEvent) { CloseHandle(waitEvent); waitEvent = NULL; }
        if (fence) { fence->Release(); fence = NULL; }
        ReleaseTimings();
        fenceVal = 0; submissions = 0;
        ringIdx = 0; recording = 0; device = NULL; trackingLost = false; failCode = 0;
        return true;
    }
};
