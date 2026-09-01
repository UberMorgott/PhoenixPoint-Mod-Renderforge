cmake_minimum_required(VERSION 3.15)
project(DlssNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")   # /MD -> nvsdk_ngx_d.lib

set(DLSS_SDK "${CMAKE_SOURCE_DIR}/../../refs/DLSS-sdk" CACHE PATH "NVIDIA DLSS SDK root")
get_filename_component(DLSS_SDK "${DLSS_SDK}" ABSOLUTE)
set(NGX_LIB "${DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
if(NOT EXISTS "${NGX_LIB}")
    message(FATAL_ERROR "nvsdk_ngx_d.lib not found at ${NGX_LIB} (DLSS_SDK=${DLSS_SDK})")
endif()

add_library(DlssNative SHARED DlssNative.cpp DlssNative.h)
target_include_directories(DlssNative PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${DLSS_SDK}/include")
target_link_libraries(DlssNative PRIVATE "${NGX_LIB}" d3d11 dxgi d3dcompiler)
target_compile_definitions(DlssNative PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(dlss_probe probe/dlss_probe.cpp)
target_link_libraries(dlss_probe PRIVATE DlssNative d3d11 dxgi)
target_compile_definitions(dlss_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
