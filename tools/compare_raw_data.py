#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Check that two DNGs hold the same raw data, as Adobe's reader sees it.

    compare_raw_data.py DNG_A DNG_B [--dng-validate PATH]

Both files are read with Adobe's dng_validate, which writes the raw image as
it came out of the file ("stage 1": decompressed, rows and columns back in
place, nothing else done to it) to a TIFF. The two TIFFs are then compared
sample for sample.

This is the test for JPEG XL compression: convert a raw file twice, once
with lossless JPEG (which `dngconv --verify` has compared with the source
through LibRaw) and once with `-c jxl`, and the reference reader must get
the same numbers out of both. LibRaw cannot make that comparison, because
it does not read JPEG XL.

The validator has to be version 1.7 or newer to read JPEG XL
(tools/dng-validate/build.sh --sdk 1.7); it is found through --dng-validate,
the DNG_VALIDATE variable or the PATH. Needs OpenCV and numpy.

Exit status 1 if the data differs or a file cannot be read.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import cv2
import numpy as np


def stage1(validator, dng, folder, name):
    stem = os.path.join(folder, name)
    result = subprocess.run([validator, "-1", stem, dng], capture_output=True, text=True)
    report = result.stdout + result.stderr
    if "*** Error" in report or not os.path.exists(stem + ".tif"):
        raise SystemExit(f"{os.path.basename(dng)}: dng_validate could not read the raw image:\n{report.strip()}")
    image = cv2.imread(stem + ".tif", cv2.IMREAD_UNCHANGED)
    if image is None:
        raise SystemExit(f"{os.path.basename(dng)}: the stage 1 picture could not be loaded")
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dng_a")
    parser.add_argument("dng_b")
    parser.add_argument("--dng-validate", default=os.environ.get("DNG_VALIDATE") or shutil.which("dng_validate"))
    args = parser.parse_args()
    if not args.dng_validate:
        raise SystemExit("dng_validate not found: build it with tools/dng-validate/build.sh and pass --dng-validate")

    folder = tempfile.mkdtemp(prefix="dngconv-raw-")
    try:
        a = stage1(args.dng_validate, args.dng_a, folder, "a")
        b = stage1(args.dng_validate, args.dng_b, folder, "b")
    finally:
        shutil.rmtree(folder, ignore_errors=True)

    names = f"{os.path.basename(args.dng_a)} and {os.path.basename(args.dng_b)}"
    if a.shape != b.shape or a.dtype != b.dtype:
        print(f"{names}: DIFFERENT: {a.shape} {a.dtype} against {b.shape} {b.dtype}")
        return 1
    different = int(np.count_nonzero(a != b))
    planes = a.shape[2] if a.ndim == 3 else 1
    size = f"{a.shape[1]}x{a.shape[0]}, {planes} sample{'s' if planes > 1 else ''} per pixel, {a.dtype}"
    if different:
        print(f"{names}: DIFFERENT: {different} of {a.size} samples ({size})")
        return 1
    print(f"{names}: the same raw data ({size})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
