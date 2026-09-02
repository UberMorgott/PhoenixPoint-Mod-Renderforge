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

# NIS_Scaler.h (NVIDIA Image Scaling SDK, MIT, vendored in nis/) is HLSL compiled at runtime: embed it as a byte array.
set(NIS_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/nis/NIS_Scaler.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NIS_HLSL}")
file(READ "${NIS_HLSL}" NIS_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," NIS_BYTES "${NIS_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nis_scaler_hlsl.h" "// generated from nis/NIS_Scaler.h\nstatic const unsigned char kNisScalerHlsl[] = {${NIS_BYTES}0x00};\n")

add_library(RenderforgeNative SHARED
    RenderforgeNative.cpp
    RenderforgeNative.h
    Sharpen.h
    Sharpen.cpp
    unity/IUnityInterface.h
    unity/IUnityGraphics.h
    unity/IUnityGraphicsD3D12.h)
target_include_directories(RenderforgeNative PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${DLSS_SDK}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(RenderforgeNative PRIVATE "${NGX_LIB}" d3d11 d3d12 dxgi d3dcompiler)
target_compile_definitions(RenderforgeNative PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(dlss_probe probe/dlss_probe.cpp)
target_link_libraries(dlss_probe PRIVATE RenderforgeNative d3d11 d3d12 dxgi)
target_compile_definitions(dlss_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
