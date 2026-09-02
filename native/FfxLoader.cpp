// FfxLoader.cpp - see FfxLoader.h.
#include "FfxLoader.h"

#include <windows.h>
#include <string.h>
#include <wchar.h>

namespace {

ffxFunctions g_fns;
int g_loaded = 0;         // 1 = g_fns valid, -1 = tried and failed
HMODULE g_upscaler;       // pre-loaded so the loader's bare-name LoadLibrary binds to OUR copy
HMODULE g_framegen;       // same trick for the frame-generation DLL; optional (FG greys out without it)
HMODULE g_loader;

HMODULE LoadFrom(const wchar_t* dir, const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    if (!dir || !dir[0]) return NULL;
    if (wcslen(dir) + wcslen(name) + 2 >= MAX_PATH) return NULL;
    wcscpy_s(path, dir);
    size_t n = wcslen(path);
    if (n && path[n - 1] != L'\\' && path[n - 1] != L'/') wcscat_s(path, L"\\");
    wcscat_s(path, name);
    return LoadLibraryW(path);
}

} // namespace

const ffxFunctions* FfxLoad(const wchar_t* dir)
{
    if (g_loaded == 1) return &g_fns;
    if (g_loaded == -1) return NULL;
    g_loaded = -1;

    // Order matters: the effect DLL first, so the loader's own bare-name lookup finds this module. This pre-binding
    // only wins while no other module with that base name is loaded yet - LoadLibrary("amd_fidelityfx_upscaler_dx12.dll")
    // returns whichever copy is already in the process, so an earlier-loaded copy (another mod, an overlay) wins.
    g_upscaler = LoadFrom(dir, L"amd_fidelityfx_upscaler_dx12.dll");
    if (!g_upscaler) return NULL;
    g_framegen = LoadFrom(dir, L"amd_fidelityfx_framegeneration_dx12.dll");
    g_loader = LoadFrom(dir, L"amd_fidelityfx_loader_dx12.dll");
    if (!g_loader) { FreeLibrary(g_upscaler); g_upscaler = NULL; return NULL; }

    memset(&g_fns, 0, sizeof(g_fns));
    ffxLoadFunctions(&g_fns, g_loader);
    if (!g_fns.CreateContext || !g_fns.DestroyContext || !g_fns.Query || !g_fns.Dispatch) {
        FreeLibrary(g_loader); g_loader = NULL;
        FreeLibrary(g_upscaler); g_upscaler = NULL;
        return NULL;
    }
    g_loaded = 1;
    return &g_fns;
}
