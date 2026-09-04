#include <windows.h>
#include <d3d12.h>

using InitFn = unsigned long (__cdecl *)(unsigned long long, const wchar_t*, ID3D12Device*, unsigned long, const void*);
using PopulateFn = unsigned long (__cdecl *)(void*);
using CreateFn = unsigned long (__cdecl *)(ID3D12GraphicsCommandList*, int, void*, void**);
using EvaluateFn = unsigned long (__cdecl *)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
using ReleaseFn = unsigned long (__cdecl *)(void*);
using ShutdownFn = unsigned long (__cdecl *)(ID3D12Device*);

static const unsigned long kPlatformError = 0xBAD00002UL;
static const unsigned long kInvalidParameter = 0xBAD00005UL;

#pragma optimize("", off)
#define BRIDGE_CALL(expr) __try { volatile unsigned long result = (expr); MemoryBarrier(); return result; } \
                          __except (EXCEPTION_EXECUTE_HANDLER) { return kPlatformError; }

extern "C" __declspec(dllexport) __declspec(noinline) unsigned long __cdecl NVNGXBridge_D3D12_InitExt(
    InitFn fn, unsigned long long appId, const wchar_t* path, ID3D12Device* device, unsigned long apiVersion, const void* common)
{
    if (!fn) return kInvalidParameter;
    BRIDGE_CALL(fn(appId, path, device, apiVersion, common));
}

extern "C" __declspec(dllexport) __declspec(noinline) unsigned long __cdecl NVNGXBridge_D3D12_PopulateParameters(PopulateFn fn, void* params)
{
    if (!fn || !params) return kInvalidParameter;
    BRIDGE_CALL(fn(params));
}

extern "C" __declspec(dllexport) __declspec(noinline) unsigned long __cdecl NVNGXBridge_D3D12_CreateFeature(
    CreateFn fn, ID3D12GraphicsCommandList* list, int feature, void* params, void** handle)
{
    if (!fn) return kInvalidParameter;
    BRIDGE_CALL(fn(list, feature, params, handle));
}

extern "C" __declspec(dllexport) __declspec(noinline) unsigned long __cdecl NVNGXBridge_D3D12_EvaluateFeature(
    EvaluateFn fn, ID3D12GraphicsCommandList* list, const void* handle, const void* params, void* progress)
{
    if (!fn) return kInvalidParameter;
    BRIDGE_CALL(fn(list, handle, params, progress));
}

extern "C" __declspec(dllexport) __declspec(noinline) unsigned long __cdecl NVNGXBridge_D3D12_ReleaseFeature(ReleaseFn fn, void* handle)
{
    if (!fn) return kInvalidParameter;
    BRIDGE_CALL(fn(handle));
}

extern "C" __declspec(dllexport) __declspec(noinline) unsigned long __cdecl NVNGXBridge_D3D12_Shutdown1(ShutdownFn fn, ID3D12Device* device)
{
    if (!fn) return kInvalidParameter;
    BRIDGE_CALL(fn(device));
}

#undef BRIDGE_CALL
#pragma optimize("", on)

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

