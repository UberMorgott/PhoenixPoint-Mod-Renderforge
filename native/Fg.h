// Fg.h - the frame-generation seam. FgHook.cpp owns the DXGI vtable patch and the present counter;
// FgHost.cpp owns the shadow swapchain and drives one IFgProvider; FgFsr/FgXess/FgStreamline implement it.
// Everything here runs on the render thread except FgHost::Init/Shutdown (main thread, render idle).
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>

// One line into <modDir>\renderforge_fg.log. Never throws, never allocates after the first call.
void FgLog(const char* fmt, ...);
void FgLogInit(const wchar_t* logDir);

// ---------------------------------------------------------------- hook (FgHook.cpp)

// Patch IDXGISwapChain::Present/Present1/ResizeBuffers in the shared DXGI vtable. Idempotent.
// `queue` = Unity's command queue (needed to create the throwaway swapchain the vtable is read from).
// Returns true when the three slots are patched.
bool FgHookInstall(ID3D12CommandQueue* queue);
void FgHookRemove(void);

// The application's swapchain, discovered on the first hooked Present. NULL until then.
IDXGISwapChain3* FgAppSwapChain(void);
HWND             FgAppHwnd(void);
const DXGI_SWAP_CHAIN_DESC1* FgAppDesc(void);   // NULL until discovered

// Present the application's swapchain through the saved original vtable entry (never re-enters the hook).
HRESULT FgOriginalPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags);
HRESULT FgOriginalResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);

// Presented frames per second over a 0.5 s window, counted in the hook (includes generated frames). 0 = no data.
int  FgPresentedFps(void);
long long FgPresentCount(void);

// Spike diagnostics, filled by FgHookSpike(). Read by Fg_SpikeStatus.
struct FgSpike
{
    int   installed;          // vtable patched
    int   sawPresent;         // the hook ran at least once
    int   flipModel;          // app swap effect is FLIP_DISCARD or FLIP_SEQUENTIAL
    int   waitable;           // app flags have DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
    unsigned format;          // DXGI_FORMAT of the app backbuffer
    unsigned bufferCount;
    unsigned swapEffect;
    unsigned scFlags;
    unsigned width, height;
    int   windowed;
    long   secondSwapChainHr; // HRESULT of a second CreateSwapChainForHwnd on the game HWND
    long   compositionHr;     // HRESULT of CreateSwapChainForComposition as the 4a fallback
    long   forwardedPresentHr;// HRESULT the saved original Present returned
};
const FgSpike* FgSpikeResult(void);
void FgHookSpike(void);       // runs the two creation probes once, on the render thread
