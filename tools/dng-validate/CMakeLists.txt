# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
#
# Builds Adobe's dng_validate from a checkout of the DNG SDK 1.5.1 as carried
# by the raw2dng project. Used by build.sh; SDK_ROOT is raw2dng's libdng folder.
cmake_minimum_required(VERSION 3.16)
project(dng_validate CXX C)

if(NOT SDK_ROOT OR NOT EXISTS "${SDK_ROOT}/dng-sdk/source/dng_validate.cpp")
  message(FATAL_ERROR "pass -DSDK_ROOT=<raw2dng checkout>/libdng")
endif()

add_subdirectory(${SDK_ROOT}/md5 md5)
add_subdirectory(${SDK_ROOT}/xmp-sdk xmp-sdk)
find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)
find_package(ZLIB REQUIRED)

file(GLOB DNG_SRC ${SDK_ROOT}/dng-sdk/source/*.cpp)
add_executable(dng_validate ${DNG_SRC})
target_include_directories(dng_validate PRIVATE ${SDK_ROOT}/dng-sdk/source ${JPEG_INCLUDE_DIR})
target_compile_definitions(dng_validate PRIVATE
  qWinOS=0 qMacOS=0 qLinux=1 qDNGThreadSafe=1 qDNGDebug=0 qDNGValidateTarget=1
  qDNGUseLibJPEG=1 qDNGXMPFiles=0 qDNGXMPDocOps=0 qDNGUseStdInt=1 qDNGLittleEndian=1)
# The SDK predates current compilers: it relies on headers that used to be
# pulled in indirectly (prefix.h supplies them) and on the C++14 dialect.
target_compile_options(dng_validate PRIVATE
  -fexceptions -w -std=gnu++14 -include ${CMAKE_CURRENT_SOURCE_DIR}/prefix.h)
target_link_libraries(dng_validate PRIVATE xmp-sdk ${JPEG_LIBRARIES} ${ZLIB_LIBRARIES} Threads::Threads)
