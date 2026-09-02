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
    D3D12Debug.h
    D3D12Debug.cpp
    Fsr12.cpp
    FfxLoader.h
    FfxLoader.cpp
    Sharpen.h
    Sharpen.cpp
    unity/IUnityInterface.h
    unity/IUnityGraphics.h
    unity/IUnityGraphicsD3D12.h)
target_include_directories(RenderforgeNative
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${DLSS_SDK}/include" "${FFX_API_INC}" "${FFX_API_INC}/dx12" "${FFX_UPSCALE_INC}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(RenderforgeNative PRIVATE "${NGX_LIB}" d3d11 d3d12 dxgi d3dcompiler)
# _WINDOWS is what ffx_api_loader.h:38 tests to compile ffxLoadFunctions; without it every pointer stays NULL.
target_compile_definitions(RenderforgeNative PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WINDOWS DLSSNATIVE_EXPORTS)

add_executable(dlss_probe probe/dlss_probe.cpp)
target_include_directories(dlss_probe PRIVATE "${FFX_API_INC}" "${FFX_API_INC}/dx12" "${FFX_UPSCALE_INC}")
target_link_libraries(dlss_probe PRIVATE RenderforgeNative d3d11 d3d12 dxgi)
target_compile_definitions(dlss_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WINDOWS)
