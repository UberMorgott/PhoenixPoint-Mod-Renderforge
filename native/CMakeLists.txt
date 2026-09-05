cmake_minimum_required(VERSION 3.15)
project(RenderforgeNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")   # /MD -> nvsdk_ngx_d.lib

set(DLSS_SDK "${CMAKE_SOURCE_DIR}/../../refs/DLSS-sdk" CACHE PATH "NVIDIA DLSS SDK root")
get_filename_component(DLSS_SDK "${DLSS_SDK}" ABSOLUTE)
set(NGX_LIB "${DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
if(NOT EXISTS "${NGX_LIB}")
    message(FATAL_ERROR "nvsdk_ngx_d.lib not found at ${NGX_LIB} (DLSS_SDK=${DLSS_SDK})")
endif()

# AMD FidelityFX SDK 2.3: HEADERS ONLY. The ffx-api is loaded at runtime with LoadLibrary/GetProcAddress
# (FfxLoader.cpp), so no AMD .lib is linked and the shim loads fine without the AMD DLLs present.
set(FFX_SDK "${CMAKE_SOURCE_DIR}/../../refs/FidelityFX-SDK" CACHE PATH "AMD FidelityFX SDK root")
get_filename_component(FFX_SDK "${FFX_SDK}" ABSOLUTE)
set(FFX_API_INC "${FFX_SDK}/Kits/FidelityFX/api/include")
set(FFX_UPSCALE_INC "${FFX_SDK}/Kits/FidelityFX/upscalers/include")
set(FFX_FG_INC "${FFX_SDK}/Kits/FidelityFX/framegeneration/include")
if(NOT EXISTS "${FFX_API_INC}/ffx_api.h")
    message(FATAL_ERROR "ffx_api.h not found at ${FFX_API_INC} (FFX_SDK=${FFX_SDK})")
endif()

# Intel XeSS SDK 3.0.2: libxess.lib is linked but DELAY-LOADED, so RenderforgeNative.dll still loads without
# libxess.dll. Xess12::Init pins <modDir>\libxess.dll with LoadLibraryW before the first xess* call, which is the
# copy the delay-load helper then binds to (an already-loaded module wins the bare-name lookup).
set(XESS_SDK "${CMAKE_SOURCE_DIR}/../../refs/XeSS-sdk" CACHE PATH "Intel XeSS SDK root")
get_filename_component(XESS_SDK "${XESS_SDK}" ABSOLUTE)
set(XESS_LIB "${XESS_SDK}/lib/libxess.lib")
if(NOT EXISTS "${XESS_LIB}")
    message(FATAL_ERROR "libxess.lib not found at ${XESS_LIB} (XESS_SDK=${XESS_SDK})")
endif()
# XeSS-FG + XeLL (FgXess.cpp): the same delay-load shape; FgXess pins libxell.dll / libxess_fg.dll from the mod folder first.
set(XESS_FG_LIB "${XESS_SDK}/lib/libxess_fg.lib")
set(XELL_LIB "${XESS_SDK}/lib/libxell.lib")
if(NOT EXISTS "${XESS_FG_LIB}" OR NOT EXISTS "${XELL_LIB}")
    message(FATAL_ERROR "libxess_fg.lib / libxell.lib not found under ${XESS_SDK}/lib")
endif()

# NVIDIA Streamline 2.12.0 (FgStreamline.cpp, DLSS-G / MFG): HEADERS ONLY. sl.interposer.dll is LoadLibraryW'd from the
# mod folder and every entry point GetProcAddress'd, so nothing is linked and the shim loads without the NVIDIA FG DLLs.
set(SL_SDK "${CMAKE_SOURCE_DIR}/../../refs/Streamline" CACHE PATH "NVIDIA Streamline SDK root")
get_filename_component(SL_SDK "${SL_SDK}" ABSOLUTE)
if(NOT EXISTS "${SL_SDK}/include/sl_dlss_g.h")
    message(FATAL_ERROR "Streamline headers not found at ${SL_SDK}/include (SL_SDK=${SL_SDK})")
endif()

# NIS_Scaler.h (NVIDIA Image Scaling SDK, MIT, vendored in nis/) is HLSL compiled at runtime: embed it as a byte array.
set(NIS_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/nis/NIS_Scaler.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NIS_HLSL}")
file(READ "${NIS_HLSL}" NIS_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," NIS_BYTES "${NIS_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nis_scaler_hlsl.h" "// generated from nis/NIS_Scaler.h\nstatic const unsigned char kNisScalerHlsl[] = {${NIS_BYTES}0x00};\n")

add_library(RenderforgeNative SHARED
    RenderforgeNative.cpp
    RenderforgeNative.h
    Device.h
    Device11.cpp
    Device12.cpp
    D3D12Ring.h
    D3D12Sharpen.h
    D3D12Owned.h
    D3D12Debug.h
    D3D12Debug.cpp
    Fsr12.cpp
    FfxLoader.h
    FfxLoader.cpp
    Xess12.cpp
    Sharpen.h
    Sharpen.cpp
    Fg.h
    FgHook.cpp
    FgHost.cpp
    FgHudless.h
    FgWnd.cpp
    FgFsr.cpp
    FgXess.cpp
    FgStreamline.cpp
    unity/IUnityInterface.h
    unity/IUnityGraphics.h
    unity/IUnityGraphicsD3D12.h)
target_include_directories(RenderforgeNative
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${DLSS_SDK}/include" "${FFX_API_INC}" "${FFX_API_INC}/dx12" "${FFX_UPSCALE_INC}" "${FFX_FG_INC}" "${XESS_SDK}/inc" "${SL_SDK}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(RenderforgeNative PRIVATE "${NGX_LIB}" "${XESS_LIB}" "${XESS_FG_LIB}" "${XELL_LIB}" d3d11 d3d12 dxgi d3dcompiler delayimp)
target_link_options(RenderforgeNative PRIVATE "/DELAYLOAD:libxess.dll" "/DELAYLOAD:libxess_fg.dll" "/DELAYLOAD:libxell.dll")
# _WINDOWS is what ffx_api_loader.h:38 tests to compile ffxLoadFunctions; without it every pointer stays NULL.
target_compile_definitions(RenderforgeNative PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WINDOWS DLSSNATIVE_EXPORTS)

add_executable(dlss_probe probe/dlss_probe.cpp)
target_include_directories(dlss_probe PRIVATE "${FFX_API_INC}" "${FFX_API_INC}/dx12" "${FFX_UPSCALE_INC}" "${XESS_SDK}/inc")
# The probe calls xessGetVersion / xessGetIntelXeFXVersion itself (--xess) to print what the SDK reports.
target_link_libraries(dlss_probe PRIVATE RenderforgeNative "${XESS_LIB}" d3d11 d3d12 dxgi delayimp)
target_link_options(dlss_probe PRIVATE "/DELAYLOAD:libxess.dll")
target_compile_definitions(dlss_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WINDOWS)

# Optional, dependency-free shader readback test; does not start Unity or load an upscaler runtime.
add_executable(lut_probe EXCLUDE_FROM_ALL probe/lut_probe.cpp Sharpen.cpp)
target_include_directories(lut_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(lut_probe PRIVATE d3d11 d3dcompiler)
target_compile_definitions(lut_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(scene_style_probe EXCLUDE_FROM_ALL probe/scene_style_probe.cpp Sharpen.cpp)
target_include_directories(scene_style_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(scene_style_probe PRIVATE d3d11 d3dcompiler)
target_compile_definitions(scene_style_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
