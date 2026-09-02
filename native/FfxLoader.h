// FfxLoader.h - runtime loading of the AMD FidelityFX ffx-api from the mod folder.
// AMD documents LoadLibrary + GetProcAddress as the recommended path (docs/getting-started/ffx-api.md:15);
// linking amd_fidelityfx_loader_dx12.lib would make RenderforgeNative.dll unloadable without the AMD DLLs.
#pragma once

#include "RenderforgeNative.h"   // DLSS_API: exported so dlss_probe --fsr can print the version list itself
#include "ffx_api.h"
#include "ffx_api_loader.h"

// Loads amd_fidelityfx_upscaler_dx12.dll then amd_fidelityfx_loader_dx12.dll from `dir` (absolute paths) and
// resolves the five entry points. Idempotent: repeated calls return the same table. Returns NULL when either
// DLL is missing or an entry point is absent. Not part of the managed ABI (C++ linkage, probe-only export).
DLSS_API const ffxFunctions* FfxLoad(const wchar_t* dir);
