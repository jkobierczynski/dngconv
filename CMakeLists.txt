# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(dngconv VERSION 0.3.0 LANGUAGES CXX
        DESCRIPTION "Camera raw to DNG converter")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DNGCONV_BUILD_TESTS "Build the test suite" ON)

# ---- LibRaw -----------------------------------------------------------------
# An explicit -DLIBRAW_INCLUDE_DIR=... -DLIBRAW_LIBRARY=... always wins (handy
# for a self-built LibRaw). Otherwise three ways to find it, in order: a CMake
# package (vcpkg), pkg-config (Linux, Homebrew), a plain header/library
# search. The thread-safe build (raw_r) is preferred when present.
add_library(dngconv_libraw INTERFACE)
set(_libraw_found FALSE)

if(LIBRAW_INCLUDE_DIR AND LIBRAW_LIBRARY)
  target_include_directories(dngconv_libraw INTERFACE ${LIBRAW_INCLUDE_DIR})
  target_link_libraries(dngconv_libraw INTERFACE ${LIBRAW_LIBRARY})
  set(_libraw_found TRUE)
endif()

if(NOT _libraw_found)
  find_package(libraw CONFIG QUIET)
  if(TARGET libraw::raw_r)
    target_link_libraries(dngconv_libraw INTERFACE libraw::raw_r)
    set(_libraw_found TRUE)
  elseif(TARGET libraw::raw)
    target_link_libraries(dngconv_libraw INTERFACE libraw::raw)
    set(_libraw_found TRUE)
  endif()
endif()

if(NOT _libraw_found)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBRAW_PC QUIET IMPORTED_TARGET libraw_r>=0.21)
    if(NOT LIBRAW_PC_FOUND)
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
  endif()
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
  src/dng_writer.cpp
  src/ljpeg92.cpp
  src/md5.cpp
  src/raw_reader.cpp
  src/source_metadata.cpp
  src/thumbnail.cpp
  src/tiff_writer.cpp)
target_include_directories(dngconv_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(dngconv_core
  PUBLIC Threads::Threads
  PRIVATE dngconv_libraw dngconv_warnings)

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

  add_executable(test_roundtrip tests/test_roundtrip.cpp)
  target_link_libraries(test_roundtrip PRIVATE dngconv_core dngconv_warnings)
  add_test(NAME roundtrip COMMAND test_roundtrip ${CMAKE_CURRENT_BINARY_DIR})
endif()
