# Copyright  © 2026 Arm Limited.
# SPDX-License-Identifier: MIT

# This tree is normally consumed via add_subdirectory() from a host project, but
# it can also be configured standalone (which is how the Android sample and the
# per-ABI CI builds drive it).
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.22)
    project(Arm_ASR LANGUAGES CXX)
endif()

# Remove standalone VK backend.
# On Android the standalone backend is the whole point of the port, so default it in.
if(ANDROID)
    set(FFXM_REMOVE_BACKEND_DEFAULT OFF)
else()
    set(FFXM_REMOVE_BACKEND_DEFAULT ON)
endif()
set(FFXM_REMOVE_ARM_ASR_VK_STANDALONE_BACKEND ${FFXM_REMOVE_BACKEND_DEFAULT} CACHE BOOL "Compile away the VK standalone backend.")

# Use GLSL shaders
set(FFXM_USE_GLSL_SHADERS OFF CACHE BOOL "Use GLSL shaders instead of HLSL shaders.")

# Consume the SPIR-V blobs checked in under
# src/backends/shared/blob_accessors/prebuilt_shaders instead of invoking the
# FidelityFX shader compiler at build time. The shipped compiler binaries in
# tools/bin are Windows x86-64 executables, so this is the only option on any
# other host.
if(ANDROID)
    set(FFXM_PREBUILT_SHADERS_DEFAULT ON)
elseif(WIN32)
    set(FFXM_PREBUILT_SHADERS_DEFAULT OFF)
else()
    set(FFXM_PREBUILT_SHADERS_DEFAULT ON)
endif()
set(FFXM_USE_PREBUILT_SHADERS ${FFXM_PREBUILT_SHADERS_DEFAULT} CACHE BOOL "Use the checked-in prebuilt SPIR-V shader blobs instead of compiling shaders.")

if(NOT WIN32 AND NOT FFXM_USE_PREBUILT_SHADERS)
    message(FATAL_ERROR
        "FFXM_USE_PREBUILT_SHADERS=OFF requires a Windows host: tools/bin/FidelityFX_SC.exe, "
        "dxcompiler.dll and glslangValidator.exe are Windows x86-64 binaries.")
endif()

# Resolve Vulkan entry points through volk. On Android the NDK ships libvulkan.so
# in the sysroot, so the backend links it directly and volk is not needed.
if(ANDROID)
    set(FFXM_VKLOADER_VOLK_DEFAULT OFF)
else()
    set(FFXM_VKLOADER_VOLK_DEFAULT ON)
endif()
set(FFXM_VKLOADER_VOLK ${FFXM_VKLOADER_VOLK_DEFAULT} CACHE BOOL "Load Vulkan entry points through volk instead of linking the loader directly.")

if(CMAKE_GENERATOR STREQUAL "Ninja")
    set(USE_DEPFILE TRUE)
else()
    set(USE_DEPFILE FALSE)
endif()

if(WIN32 AND NOT ANDROID)
    # Set Visual Studio version
    if(CMAKE_GENERATOR STREQUAL "Visual Studio 14 2015")
        set(FFXM_VS_VERSION 2015)
    elseif(CMAKE_GENERATOR STREQUAL "Visual Studio 15 2017")
        set(FFXM_VS_VERSION 2017)
    elseif(CMAKE_GENERATOR STREQUAL "Visual Studio 16 2019")
        set(FFXM_VS_VERSION 2019)
    elseif(CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022")
        set(FFXM_VS_VERSION 2022)
    endif()

    # Add requred compile definitions
    add_compile_definitions(_UNICODE)
    add_compile_definitions(UNICODE)

    if(FFXM_VS_VERSION STREQUAL 2015 OR FFXM_VS_VERSION STREQUAL 2017)
        message(NOTICE "Forcing the SDK path for VS 2015 and VS 2017")
        set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "10.0.18362.0")
    endif()

    # Setup the shader compiler (Windows-only binary)
    set(FFXM_SC_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin/FidelityFX_SC.exe CACHE FILEPATH "Path to the FidelityFX shader compiler.")
endif()

# Setup common variables
set(FFXM_INCLUDE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(FFXM_SHARED_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/shared)
set(FFXM_HOST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include/host)
set(FFXM_GPU_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include/gpu)
set(FFXM_COMPONENTS_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/components)
set(FFXM_HOST_BACKENDS_PATH ${FFXM_HOST_PATH}/backends)
set(FFXM_SRC_BACKENDS_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/backends)
set(FFXM_PREBUILT_SHADERS_PATH ${FFXM_SRC_BACKENDS_PATH}/shared/blob_accessors/prebuilt_shaders)

# ------------------------------------------------------
# Define common source groupings used by all effects

# Init shader sources (for easy viewing)
set(FFXM_PUBLIC_SHADER_SOURCES
    "${FFXM_GPU_PATH}/ffxm_common_types.h"
    "${FFXM_GPU_PATH}/ffxm_core.h"
    "${FFXM_GPU_PATH}/ffxm_core_cpu.h"
    "${FFXM_GPU_PATH}/ffxm_core_glsl.h"
    "${FFXM_GPU_PATH}/ffxm_core_gpu_common.h"
    "${FFXM_GPU_PATH}/ffxm_core_gpu_common_half.h"
    "${FFXM_GPU_PATH}/ffxm_core_hlsl.h"
    "${FFXM_GPU_PATH}/ffxm_core_portability.h")

# Components
if(NOT FFXM_REMOVE_ARM_ASR_VK_STANDALONE_BACKEND)
add_subdirectory(${FFXM_SRC_BACKENDS_PATH}/vk)
endif()

set(SRC "${FFXM_SHARED_PATH}/ffxm_assert.cpp")
list(APPEND SRC "${FFXM_SHARED_PATH}/ffxm_object_management.cpp")
list(APPEND SRC "${FFXM_COMPONENTS_PATH}/fsr2/ffxm_fsr2.cpp")

add_library(Arm_ASR_api ${SRC})

target_include_directories(Arm_ASR_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFXM_INCLUDE_PATH}
    ${FFXM_HOST_PATH}
    ${FFXM_GPU_PATH}
    ${FFXM_HOST_BACKENDS_PATH}/vk)

target_compile_features(Arm_ASR_api PUBLIC cxx_std_20)

if(ANDROID)
    # ffxmAssertReport() reports through logcat.
    target_link_libraries(Arm_ASR_api PUBLIC log)
endif()
