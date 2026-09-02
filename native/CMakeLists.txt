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
    unity/IUnityInterface.h
    unity/IUnityGraphics.h
    unity/IUnityGraphicsD3D12.h)
target_include_directories(RenderforgeNative
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${DLSS_SDK}/include" "${FFX_API_INC}" "${FFX_API_INC}/dx12" "${FFX_UPSCALE_INC}" "${XESS_SDK}/inc" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(RenderforgeNative PRIVATE "${NGX_LIB}" "${XESS_LIB}" d3d11 d3d12 dxgi dcomp d3dcompiler delayimp)
target_link_options(RenderforgeNative PRIVATE "/DELAYLOAD:libxess.dll")
# _WINDOWS is what ffx_api_loader.h:38 tests to compile ffxLoadFunctions; without it every pointer stays NULL.
target_compile_definitions(RenderforgeNative PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WINDOWS DLSSNATIVE_EXPORTS)

add_executable(dlss_probe probe/dlss_probe.cpp)
target_include_directories(dlss_probe PRIVATE "${FFX_API_INC}" "${FFX_API_INC}/dx12" "${FFX_UPSCALE_INC}" "${XESS_SDK}/inc")
# The probe calls xessGetVersion / xessGetIntelXeFXVersion itself (--xess) to print what the SDK reports.
target_link_libraries(dlss_probe PRIVATE RenderforgeNative "${XESS_LIB}" d3d11 d3d12 dxgi delayimp)
target_link_options(dlss_probe PRIVATE "/DELAYLOAD:libxess.dll")
target_compile_definitions(dlss_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WINDOWS)
