// FgXess.cpp - Intel XeSS Frame Generation: SDK-BLOCKED, deliberately NO provider.
//
// XeSS-FG (libxess_fg.dll 1.3.1.78, XeSS SDK 3.0.2) only ever generates frames INSIDE its own proxy IDXGISwapChain,
// and that proxy is always an HWND swapchain the library creates itself:
//   - xefgSwapChainD3D12InitFromSwapChain      (xefg_swapchain_d3d12.h:190): the app chain must have refcount 1 and is
//     released, "Creates a new swap chain for the underlying window" (xess_fg_developer_guide_english.md:270-276).
//     Unity keeps its reference and keeps calling GetBuffer/ResizeBuffers on its chain -> impossible.
//   - xefgSwapChainD3D12InitFromSwapChainDesc  (xefg_swapchain_d3d12.h:216, :212 "a swap chain will be created
//     according to hWnd, pSwapChainDesc"): IDXGIFactory2::CreateSwapChainForHwnd on the game window, which DXGI
//     refuses with E_ACCESSDENIED (0x80070005) while Unity's flip-model chain owns that HWND (measured, FgHost.cpp:7-9).
//     There is no composition-swapchain overload.
//   - The complete export surface (xefg_swapchain.h:334-512, xefg_swapchain_d3d12.h:120-320, xefg_swapchain_debug.h:56,
//     and dumpbin /exports of libxess_fg.dll: 31 entries = the 24 documented + xefgAIL{GetDecision,GetVersion,
//     SetAppXeFGVersion} + 4 undocumented xefgSwapChain{D3D12GetProfilingData,D3D12SetDiagnosticCallbacks,
//     GetParameterP,SetParameterP}) has no dispatch/execute entry: no output texture, no present callback, no
//     "generate into this resource" call. xefg_swapchain.h:321-324 calls the swapchain API "utils" over an
//     "underlying XeSS-FG API" that the SDK does not ship.
// So, unlike FSR-FG (FgFsr.cpp, manual ffxDispatch into the host's composition shadow chain), XeSS-FG cannot be
// driven from inside a process whose HWND is already presented by someone else. The provider factory returns NULL
// and the host reports FG_ERR_NO_PROVIDER with the reason below in Fg_Status. XeLL is not touched.
//
// libxess_fg.dll is still verified, staged and copied by build-native.ps1 / deploy.ps1 (explicit LoadLibraryW +
// GetProcAddress here, never hard-linked) so the reason line carries the runtime's real version; nothing else loads it.
#include "Fg.h"
#include "RenderforgeNative.h"

#include <stdio.h>

#include "xess_fg/xefg_swapchain.h"

namespace {

char g_reason[160];

} // namespace

const char* FgXessBlockedReason(const wchar_t* dllDir)
{
    if (g_reason[0]) return g_reason;
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s\\libxess_fg.dll", dllDir ? dllDir : L".");
    HMODULE m = LoadLibraryW(p);
    typedef xefg_swapchain_result_t (*GetVersionFn)(xefg_swapchain_version_t*);
    GetVersionFn getVersion = m ? (GetVersionFn)GetProcAddress(m, "xefgSwapChainGetVersion") : NULL;
    xefg_swapchain_version_t v = {};
    if (!getVersion || getVersion(&v) != XEFG_SWAPCHAIN_RESULT_SUCCESS) v.major = v.minor = v.patch = 0;
    if (v.major)
        _snprintf_s(g_reason, sizeof(g_reason), _TRUNCATE, "XeSS-FG %u.%u.%u needs its own HWND swapchain - unavailable in-process", v.major, v.minor, v.patch);
    else
        _snprintf_s(g_reason, sizeof(g_reason), _TRUNCATE, "XeSS-FG needs its own HWND swapchain - unavailable in-process");
    FgLog("xess: %s (libxess_fg.dll %s)", g_reason, m ? "loaded" : "not in the mod folder");
    if (m) FreeLibrary(m);
    return g_reason;
}

IFgProvider* MakeFgProviderXess(void) { return NULL; }
