#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
#
# Fetches Adobe's DNG SDK and builds dng_validate, the reference validator,
# for use by the scripts in this folder.
#
#   tools/dng-validate/build.sh [--sdk 1.5|1.7] [work folder]
#
# The work folder defaults to ./dng-validate-build; the result is
# <work folder>/dng_validate. Point the other tools to it with
# DNG_VALIDATE=<path>, or put it on the PATH.
#
#   --sdk 1.5   (default) DNG SDK 1.5.1. Builds in a few minutes. Reads
#               everything dngconv writes except JPEG XL.
#   --sdk 1.7   DNG SDK 1.7.1, the first that reads JPEG XL (dngconv -c jxl).
#               It also builds the libjxl that comes with the SDK, which
#               takes ten to twenty minutes more.
#
# Nothing of the SDK is part of this repository. It is Adobe's, under Adobe's
# own licence (see the licence texts in the checkouts), and is taken here
# from two projects that carry it: version 1.5.1 with the XMP SDK it needs
# from raw2dng, version 1.7.1 from hfiguiere/dng_sdk. The 1.7 build reuses
# raw2dng's packaging of the XMP SDK.
#
# Needs: git, cmake, a C++ compiler, and the development files of libjpeg,
# zlib and expat (Debian/Ubuntu: libjpeg-dev zlib1g-dev libexpat1-dev).
# Linux only; on other systems use Adobe's own build of the SDK.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
sdk=1.5
if [ "${1:-}" = "--sdk" ]; then
    if [ $# -lt 2 ]; then
        echo "--sdk needs a version: 1.5 or 1.7" >&2
        exit 2
    fi
    sdk=$2
    shift 2
fi
case $sdk in
    1.5 | 1.7) ;;
    *) echo "unknown SDK version '$sdk': use 1.5 or 1.7" >&2; exit 2 ;;
esac
work=${1:-dng-validate-build}
raw2dng_commit=f2e90198138b83b8e8d15e5124a7e5d9599bde32   # raw2dng, 2020-12-17
sdk17_commit=555b69eb47c5428100cebfafba60d70196b880ea     # hfiguiere/dng_sdk, tag dng_1.7.1_2410

mkdir -p "$work"
work=$(CDPATH='' cd -- "$work" && pwd)
# One compiler per processor: left to itself, make starts as many as there
# are files, and libjxl's need a gigabyte each.
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

if [ ! -d "$work/raw2dng/.git" ]; then
    git clone https://github.com/Fimagena/raw2dng.git "$work/raw2dng"
fi
git -C "$work/raw2dng" checkout --quiet "$raw2dng_commit"

if [ "$sdk" = 1.5 ]; then
    cmake -S "$here" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DSDK_ROOT="$work/raw2dng/libdng"
    cmake --build "$work/build" --parallel "$jobs"
    cp "$work/build/dng_validate" "$work/dng_validate"
else
    if [ ! -d "$work/dng_sdk/.git" ]; then
        git init --quiet "$work/dng_sdk"
        git -C "$work/dng_sdk" fetch --quiet --depth 1 https://github.com/hfiguiere/dng_sdk.git "$sdk17_commit"
        git -C "$work/dng_sdk" checkout --quiet FETCH_HEAD
    fi
    # The libjxl the SDK was written against, with the Highway and Brotli
    # sources it brings along, as static libraries.
    cmake -S "$work/dng_sdk/libjxl/libjxl" -B "$work/libjxl" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF \
        -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_JNI=OFF \
        -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_PLUGINS=OFF \
        -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_ENABLE_SKCMS=ON -DJPEGXL_BUNDLE_LIBPNG=OFF \
        -DCMAKE_INSTALL_PREFIX="$work/libjxl/install"
    cmake --build "$work/libjxl" --parallel "$jobs"
    cmake --install "$work/libjxl" >/dev/null

    cmake -S "$here/sdk-1.7" -B "$work/build-1.7" -DCMAKE_BUILD_TYPE=Release \
        -DSDK_ROOT="$work/dng_sdk" -DJXL_ROOT="$work/libjxl" -DXMP_ROOT="$work/raw2dng/libdng"
    cmake --build "$work/build-1.7" --parallel "$jobs"
    cp "$work/build-1.7/dng_validate" "$work/dng_validate"
fi

echo
echo "built: $work/dng_validate"
"$work/dng_validate" 2>&1 | sed -n 2p
