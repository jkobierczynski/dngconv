# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(dngconv VERSION 0.11.0 LANGUAGES CXX
        DESCRIPTION "Camera raw to DNG converter")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DNGCONV_BUILD_TESTS "Build the test suite" ON)
set(DNGCONV_JXL "AUTO" CACHE STRING
    "JPEG XL compression through libjxl: AUTO (if the library is found), ON (required) or OFF")
set_property(CACHE DNGCONV_JXL PROPERTY STRINGS AUTO ON OFF)

# ---- LibRaw -----------------------------------------------------------------
# An explicit -DLIBRAW_INCLUDE_DIR=... -DLIBRAW_LIBRARY=... always wins (handy
# for a self-built LibRaw). Otherwise three ways to find it, in order: a CMake
# package (vcpkg), pkg-config (Linux, Homebrew), a plain header/library
# search. The thread-safe build (raw_r) is preferred when present; with the
# other one, decoding is done one file at a time.
add_library(dngconv_libraw INTERFACE)
set(_libraw_found FALSE)
set(_libraw_reentrant FALSE)

if(LIBRAW_INCLUDE_DIR AND LIBRAW_LIBRARY)
  target_include_directories(dngconv_libraw INTERFACE ${LIBRAW_INCLUDE_DIR})
  target_link_libraries(dngconv_libraw INTERFACE ${LIBRAW_LIBRARY})
  set(_libraw_found TRUE)
  if("${LIBRAW_LIBRARY}" MATCHES "raw_r")
    set(_libraw_reentrant TRUE)
  endif()
endif()

if(NOT _libraw_found)
  find_package(libraw CONFIG QUIET)
  if(TARGET libraw::raw_r)
    target_link_libraries(dngconv_libraw INTERFACE libraw::raw_r)
    set(_libraw_found TRUE)
    set(_libraw_reentrant TRUE)
  elseif(TARGET libraw::raw)
    target_link_libraries(dngconv_libraw INTERFACE libraw::raw)
    set(_libraw_found TRUE)
  endif()
endif()

if(NOT _libraw_found)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBRAW_PC QUIET IMPORTED_TARGET libraw_r>=0.21)
    if(LIBRAW_PC_FOUND)
      set(_libraw_reentrant TRUE)
    else()
      pkg_check_modules(LIBRAW_PC QUIET IMPORTED_TARGET libraw>=0.21)
    endif()
    if(LIBRAW_PC_FOUND)
      target_link_libraries(dngconv_libraw INTERFACE PkgConfig::LIBRAW_PC)
      set(_libraw_found TRUE)
    endif()
  endif()
endif()

if(NOT _libraw_found)
  find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h)
  find_library(LIBRAW_LIBRARY NAMES raw_r raw libraw_r libraw)
  if(LIBRAW_INCLUDE_DIR AND LIBRAW_LIBRARY)
    target_include_directories(dngconv_libraw INTERFACE ${LIBRAW_INCLUDE_DIR})
    target_link_libraries(dngconv_libraw INTERFACE ${LIBRAW_LIBRARY})
    set(_libraw_found TRUE)
    if("${LIBRAW_LIBRARY}" MATCHES "raw_r")
      set(_libraw_reentrant TRUE)
    endif()
  endif()
endif()

if(_libraw_reentrant)
  target_compile_definitions(dngconv_libraw INTERFACE DNGCONV_LIBRAW_REENTRANT=1)
  message(STATUS "LibRaw: thread-safe build (raw_r); files are decoded in parallel")
elseif(_libraw_found)
  message(STATUS "LibRaw: not the thread-safe build; files are decoded one at a time")
endif()

if(NOT _libraw_found)
  message(FATAL_ERROR
    "LibRaw 0.21 or newer was not found.\n"
    "  Debian/Ubuntu: sudo apt install libraw-dev\n"
    "  Fedora/RHEL:   sudo dnf install LibRaw-devel\n"
    "  macOS:         brew install libraw\n"
    "  Windows:       vcpkg install libraw")
endif()

find_package(Threads REQUIRED)

# zlib compresses the original raw file when it is embedded in the DNG.
find_package(ZLIB REQUIRED)

# ---- libjxl (optional) --------------------------------------------------------
# JPEG XL compression of the raw data (DNG 1.7) needs libjxl 0.7 or newer.
# Without it everything else works; asking for JPEG XL is then refused.
#
# -DJXL_INCLUDE_DIR=... "-DJXL_LIBRARY=a;b;c" names it explicitly. Otherwise
# pkg-config, where there is one, only tells where to look: the libraries
# are then searched for by name, libjxl itself and those a static libjxl
# stands on (its colour module, Highway, Brotli, Little CMS), and whichever
# exist are linked in that order. That one rule serves a shared libjxl from
# a distribution and a static one from vcpkg alike. A small program is
# linked as a test before the result is trusted.
add_library(dngconv_jxl INTERFACE)
set(_jxl_found FALSE)
if(NOT DNGCONV_JXL STREQUAL "OFF")
  set(_jxl_includes "")
  set(_jxl_libraries "")
  if(JXL_INCLUDE_DIR AND JXL_LIBRARY)
    set(_jxl_includes ${JXL_INCLUDE_DIR})
    set(_jxl_libraries ${JXL_LIBRARY})
  else()
    set(_jxl_hints "")
    set(_jxl_include_hints "")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(JXL_PC QUIET libjxl)
      if(JXL_PC_FOUND)
        set(_jxl_hints ${JXL_PC_LIBRARY_DIRS} ${JXL_PC_STATIC_LIBRARY_DIRS} ${JXL_PC_LIBDIR})
        set(_jxl_include_hints ${JXL_PC_INCLUDE_DIRS} ${JXL_PC_STATIC_INCLUDE_DIRS} ${JXL_PC_INCLUDEDIR})
      endif()
    endif()
    find_path(DNGCONV_JXL_INCLUDE jxl/encode.h HINTS ${_jxl_include_hints})
    find_library(DNGCONV_JXL_LIB NAMES jxl libjxl HINTS ${_jxl_hints})
    if(DNGCONV_JXL_INCLUDE AND DNGCONV_JXL_LIB)
      set(_jxl_includes ${DNGCONV_JXL_INCLUDE})
      set(_jxl_libraries ${DNGCONV_JXL_LIB})
      foreach(_name jxl_cms hwy brotlienc brotlidec brotlicommon lcms2)
        find_library(DNGCONV_JXL_DEP_${_name}
                     NAMES ${_name} lib${_name} ${_name}-static lib${_name}-static
                     HINTS ${_jxl_hints})
        if(DNGCONV_JXL_DEP_${_name})
          list(APPEND _jxl_libraries ${DNGCONV_JXL_DEP_${_name}})
        endif()
      endforeach()
      if(UNIX)
        list(APPEND _jxl_libraries m)
      endif()
    endif()
  endif()

  if(_jxl_libraries)
    include(CheckCXXSourceCompiles)
    # Visual Studio would build the test in its Debug configuration, which
    # does not link with the release libraries a package manager provides.
    if(CMAKE_CONFIGURATION_TYPES)
      set(_jxl_saved_configuration "${CMAKE_TRY_COMPILE_CONFIGURATION}")
      set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
    endif()
    set(CMAKE_REQUIRED_INCLUDES ${_jxl_includes})
    set(CMAKE_REQUIRED_LIBRARIES ${_jxl_libraries} Threads::Threads)
    # A new variable name for every set of libraries tried, so that a cached
    # failure does not outlive its cause.
    string(MD5 _jxl_key "${_jxl_includes};${_jxl_libraries}")
    check_cxx_source_compiles("
      #include <jxl/decode.h>
      #include <jxl/encode.h>
      int main() {
        JxlEncoder* e = JxlEncoderCreate(nullptr);
        JxlEncoderFrameSettings* f = JxlEncoderFrameSettingsCreate(e, nullptr);
        JxlEncoderSetFrameLossless(f, JXL_TRUE);
        JxlEncoderCloseInput(e);
        unsigned char buffer[64];
        unsigned char* next = buffer;
        size_t left = sizeof buffer;
        JxlEncoderProcessOutput(e, &next, &left);
        JxlEncoderDestroy(e);
        JxlDecoder* d = JxlDecoderCreate(nullptr);
        JxlDecoderCloseInput(d);
        JxlDecoderProcessInput(d);
        JxlDecoderDestroy(d);
        return JxlEncoderVersion() < 7000 ? 1 : 0;
      }" DNGCONV_JXL_LINKS_${_jxl_key})
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(CMAKE_CONFIGURATION_TYPES)
      set(CMAKE_TRY_COMPILE_CONFIGURATION "${_jxl_saved_configuration}")
    endif()
    if(DNGCONV_JXL_LINKS_${_jxl_key})
      target_include_directories(dngconv_jxl INTERFACE ${_jxl_includes})
      target_link_libraries(dngconv_jxl INTERFACE ${_jxl_libraries})
      target_compile_definitions(dngconv_jxl INTERFACE DNGCONV_HAVE_JXL=1)
      set(_jxl_found TRUE)
    else()
      message(STATUS "libjxl was found (${_jxl_libraries}) but a test program does not build with it")
    endif()
  endif()
endif()

if(_jxl_found)
  message(STATUS "JPEG XL: enabled (libjxl)")
elseif(DNGCONV_JXL STREQUAL "ON")
  message(FATAL_ERROR
    "DNGCONV_JXL=ON, but libjxl 0.7 or newer was not found or cannot be linked.\n"
    "  Debian/Ubuntu: sudo apt install libjxl-dev\n"
    "  Fedora:        sudo dnf install libjxl-devel\n"
    "  macOS:         brew install jpeg-xl\n"
    "  Windows:       vcpkg install libjxl")
elseif(DNGCONV_JXL STREQUAL "OFF")
  message(STATUS "JPEG XL: switched off (DNGCONV_JXL=OFF)")
else()
  message(STATUS "JPEG XL: libjxl not found; building without JPEG XL compression")
endif()

# ---- warnings ---------------------------------------------------------------
add_library(dngconv_warnings INTERFACE)
if(MSVC)
  target_compile_options(dngconv_warnings INTERFACE /W4 /utf-8)
  target_compile_definitions(dngconv_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(dngconv_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ---- library and tool -------------------------------------------------------
configure_file(src/version.hpp.in generated/version.hpp @ONLY)

add_library(dngconv_core STATIC
  src/camera_profile.cpp
  src/dng_append.cpp
  src/dng_check.cpp
  src/dng_digest.cpp
  src/dng_writer.cpp
  src/lens_data.cpp
  src/lens_opcodes.cpp
  src/lossy_jxl.cpp
  src/jpeg_encoder.cpp
  src/jxl_codec.cpp
  src/ljpeg92.cpp
  src/ljpeg92_decode.cpp
  src/md5.cpp
  src/original_raw.cpp
  src/raw_reader.cpp
  src/sidecar.cpp
  src/source_record.cpp
  src/source_metadata.cpp
  src/render.cpp
  src/tiff_source.cpp
  src/tiff_writer.cpp)
target_include_directories(dngconv_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(dngconv_core
  PUBLIC Threads::Threads
  PRIVATE dngconv_libraw dngconv_jxl ZLIB::ZLIB dngconv_warnings)

add_executable(dngconv src/main.cpp)
target_link_libraries(dngconv PRIVATE dngconv_core dngconv_warnings)
if(WIN32)
  target_link_libraries(dngconv PRIVATE shell32)  # CommandLineToArgvW
endif()

include(GNUInstallDirs)
install(TARGETS dngconv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- tests ------------------------------------------------------------------
if(DNGCONV_BUILD_TESTS)
  enable_testing()

  add_executable(test_ljpeg92 tests/test_ljpeg92.cpp)
  target_link_libraries(test_ljpeg92 PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME ljpeg92 COMMAND test_ljpeg92)

  add_executable(test_tiff tests/test_tiff.cpp)
  target_link_libraries(test_tiff PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME tiff COMMAND test_tiff)

  add_executable(test_source_metadata tests/test_source_metadata.cpp)
  target_link_libraries(test_source_metadata PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME source_metadata COMMAND test_source_metadata ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_preview tests/test_preview.cpp)
  target_link_libraries(test_preview PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME preview COMMAND test_preview ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_render tests/test_render.cpp)
  target_link_libraries(test_render PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME render COMMAND test_render)

  add_executable(test_original tests/test_original.cpp)
  target_link_libraries(test_original PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME original COMMAND test_original ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_lens tests/test_lens.cpp)
  target_link_libraries(test_lens PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME lens COMMAND test_lens ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_profile tests/test_profile.cpp)
  target_link_libraries(test_profile PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME profile COMMAND test_profile ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_check tests/test_check.cpp)
  target_link_libraries(test_check PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME check COMMAND test_check ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_archive tests/test_archive.cpp)
  target_link_libraries(test_archive PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME archive COMMAND test_archive ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_lossy tests/test_lossy.cpp)
  target_link_libraries(test_lossy PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME lossy COMMAND test_lossy ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_jxl tests/test_jxl.cpp)
  target_link_libraries(test_jxl PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME jxl COMMAND test_jxl ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_batch tests/test_batch.cpp)
  target_link_libraries(test_batch PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME batch COMMAND test_batch ${CMAKE_CURRENT_BINARY_DIR})

  # Runs the program itself.
  add_executable(test_cli tests/test_cli.cpp)
  target_link_libraries(test_cli PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME cli COMMAND test_cli $<TARGET_FILE:dngconv> ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_roundtrip tests/test_roundtrip.cpp)
  target_link_libraries(test_roundtrip PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME roundtrip COMMAND test_roundtrip ${CMAKE_CURRENT_BINARY_DIR})
endif()
