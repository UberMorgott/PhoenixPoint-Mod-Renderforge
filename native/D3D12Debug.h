// D3D12Debug.h - opt-in D3D12 validation, gated on the env var RENDERFORGE_D3D12_DEBUG=1.
// Everything here is a no-op when it is unset, so the shipped DLL pays one env lookup.
//
// The debug layer and DRED can only be armed BEFORE the device is created, and our DLL is loaded
// long after Unity has created it (UnityPluginLoad never sees kUnityGfxDeviceEventInitialize).
// So EarlyEnable() is best-effort only; the reliable lever is Unity's own command line switch
// `-force-d3d12-debug`, after which QI(ID3D12InfoQueue) on Unity's device succeeds and Drain()
// pulls the validation messages into %TEMP%\renderforge-d3d12.log.
#pragma once

struct ID3D12Device;
struct ID3D12Resource;
struct UnityGraphicsD3D12ResourceState;

namespace RfDbg
{
    bool On();
    bool NoEvents();   // RENDERFORGE_D3D12_NOEVENTS=1 - diagnostic: swallow every render event
    void Log(const char* fmt, ...);
    void EarlyEnable();                                   // debug layer + DRED settings (best effort)
    void Attach(ID3D12Device* dev);                       // QI InfoQueue / DRED once
    void Drain();                                         // info-queue messages -> log
    void Removed(ID3D12Device* dev, const char* where);   // GetDeviceRemovedReason + DRED dump
    void Resource(const char* tag, ID3D12Resource* r);
    void States(int n, const UnityGraphicsD3D12ResourceState* st);
}
