# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
#
# Builds Adobe's dng_validate from the DNG SDK 1.7.1, the first that reads
# JPEG XL. Used by build.sh. SDK_ROOT is a checkout of the SDK (the folder
# holding dng_sdk/ and libjxl/), JXL_ROOT the build folder of the libjxl
# that comes with it (installed into JXL_ROOT/install), XMP_ROOT raw2dng's
# libdng folder, whose packaging of the XMP SDK is reused.
cmake_minimum_required(VERSION 3.16)
project(dng_validate CXX C)

if(NOT SDK_ROOT OR NOT EXISTS "${SDK_ROOT}/dng_sdk/source/dng_validate.cpp")
  message(FATAL_ERROR "pass -DSDK_ROOT=<checkout of the DNG SDK 1.7.1>")
endif()
if(NOT JXL_ROOT OR NOT EXISTS "${JXL_ROOT}/install/include/jxl/encode.h")
  message(FATAL_ERROR "pass -DJXL_ROOT=<build folder of the SDK's libjxl, installed into its install/>")
endif()
if(NOT XMP_ROOT OR NOT EXISTS "${XMP_ROOT}/xmp-sdk/CMakeLists.txt")
  message(FATAL_ERROR "pass -DXMP_ROOT=<raw2dng checkout>/libdng")
endif()

add_subdirectory(${XMP_ROOT}/md5 md5)
add_subdirectory(${XMP_ROOT}/xmp-sdk xmp-sdk)
find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)
find_package(ZLIB REQUIRED)

file(GLOB DNG_SRC ${SDK_ROOT}/dng_sdk/source/*.cpp)
add_executable(dng_validate ${DNG_SRC})
target_include_directories(dng_validate PRIVATE
  ${SDK_ROOT}/dng_sdk/source ${JPEG_INCLUDE_DIR} ${JXL_ROOT}/install/include)
target_compile_definitions(dng_validate PRIVATE
  qWinOS=0 qMacOS=0 qLinux=1 qDNGThreadSafe=1 qDNGDebug=0 qDNGValidateTarget=1
  qDNGUseLibJPEG=1 qDNGXMPFiles=0 qDNGXMPDocOps=0 qDNGUseStdInt=1 qDNGLittleEndian=1)
target_compile_options(dng_validate PRIVATE -fexceptions -w)
# libjxl and what it stands on, all static: the validator then needs no
# libjxl installed, and uses the version the SDK was written for.
target_link_libraries(dng_validate PRIVATE xmp-sdk
  ${JXL_ROOT}/install/lib/libjxl.a
  ${JXL_ROOT}/install/lib/libjxl_cms.a
  ${JXL_ROOT}/install/lib/libjxl_threads.a
  ${JXL_ROOT}/install/lib/libhwy.a
  ${JXL_ROOT}/third_party/brotli/libbrotlienc.a
  ${JXL_ROOT}/third_party/brotli/libbrotlidec.a
  ${JXL_ROOT}/third_party/brotli/libbrotlicommon.a
  ${JPEG_LIBRARIES} ${ZLIB_LIBRARIES} Threads::Threads)
