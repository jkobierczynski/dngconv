#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
#
# Fetches Adobe's DNG SDK and builds dng_validate, the reference validator,
# for use by the scripts in this folder.
#
#   tools/dng-validate/build.sh [work folder]      (default: ./dng-validate-build)
#
# The result is <work folder>/dng_validate. Point the other tools to it with
# DNG_VALIDATE=<path>, or put it on the PATH.
#
# Nothing of the SDK is part of this repository. It is Adobe's, under Adobe's
# own licence (see dng-sdk/Adobe-license.txt in the checkout), and is taken
# here from the raw2dng project, which carries version 1.5.1 together with
# the XMP SDK it needs.
#
# Needs: git, cmake, a C++ compiler, and the development files of libjpeg,
# zlib and expat (Debian/Ubuntu: libjpeg-dev zlib1g-dev libexpat1-dev).
# Linux only; on other systems use Adobe's own build of the SDK.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
work=${1:-dng-validate-build}
commit=f2e90198138b83b8e8d15e5124a7e5d9599bde32   # raw2dng, 2020-12-17

mkdir -p "$work"
work=$(CDPATH='' cd -- "$work" && pwd)

if [ ! -d "$work/raw2dng/.git" ]; then
    git clone https://github.com/Fimagena/raw2dng.git "$work/raw2dng"
fi
git -C "$work/raw2dng" checkout --quiet "$commit"

cmake -S "$here" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DSDK_ROOT="$work/raw2dng/libdng"
cmake --build "$work/build" --parallel
cp "$work/build/dng_validate" "$work/dng_validate"

echo
echo "built: $work/dng_validate"
"$work/dng_validate" 2>&1 | sed -n 2p
