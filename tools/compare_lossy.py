#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Check a lossy DNG against its lossless twin, as Adobe's reader sees them.

    compare_lossy.py LOSSLESS_DNG LOSSY_DNG DISTANCE [--dng-validate PATH]

Both files are read with Adobe's dng_validate, which writes the raw image
after linearisation and OpcodeList2 ("stage 2": black level off, scaled to
the white level, for the lossy file the MapPolynomial that undoes the curve
applied) to a TIFF. For the lossless DNG that is the sensor's data, for the
lossy one what a reader makes of it. The two are compared:

- in the coded domain: both put through the curve dngconv codes with
  (y = inverse of s y + (1 - s) y^3, s = 1/16), where the coder's error is
  about even over the tones. Judged by the rules `dngconv --verify` uses:
  root mean square at most 2.5 % x distance, the mean of every 32 x 32
  block of one colour within 1 % x distance + 0.1 %;
- in linear light: reported (largest error, and how many samples are off
  by more than 5 %), not judged.

This tests what dngconv's own check cannot: that the opcode, the black and
white levels and the active area written into the file make Adobe's reader
arrive at the original values. Needs dng_validate 1.7 or newer, numpy and
OpenCV. Exit status 1 if the rules are not met or a file cannot be read.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import cv2
import numpy as np

S = 1.0 / 16.0


def encode(x):
    """The inverse of s y + (1 - s) y^3, by Cardano, as dngconv does it."""
    x = np.clip(x, 0.0, 1.0)
    k0 = 2.0 ** (1.0 / 3.0)
    k1 = 108.0 * S ** 3 * (1 - S) ** 3
    k2 = 27.0 * x * (1 - S) ** 2
    k3 = np.cbrt(k2 + np.sqrt(k1 + k2 * k2))
    return np.clip(k3 / (3.0 * k0 * (1 - S)) - k0 * S / k3, 0.0, 1.0)


def stage2(validator, dng, folder, name):
    stem = os.path.join(folder, name)
    result = subprocess.run([validator, "-2", stem, dng], capture_output=True, text=True)
    report = result.stdout + result.stderr
    if "*** Error" in report or not os.path.exists(stem + ".tif"):
        raise SystemExit(f"{os.path.basename(dng)}: dng_validate could not read it:\n{report.strip()}")
    image = cv2.imread(stem + ".tif", cv2.IMREAD_UNCHANGED)
    if image is None:
        raise SystemExit(f"{os.path.basename(dng)}: the stage 2 picture could not be loaded")
    return image.astype(np.float64) / 65535.0


def block_means(d, block=32):
    """Means of 32 x 32 blocks of each colour of a 2 x 2 mosaic."""
    worst = 0.0
    for fy in range(2):
        for fx in range(2):
            f = d[fy::2, fx::2]
            h, w = (f.shape[0] // block) * block, (f.shape[1] // block) * block
            if h == 0 or w == 0:
                continue
            m = f[:h, :w].reshape(h // block, block, w // block, block).mean(axis=(1, 3))
            worst = max(worst, float(np.abs(m).max()))
    return worst


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lossless")
    parser.add_argument("lossy")
    parser.add_argument("distance", type=float)
    parser.add_argument("--dng-validate", default=os.environ.get("DNG_VALIDATE") or shutil.which("dng_validate"))
    args = parser.parse_args()
    if not args.dng_validate:
        raise SystemExit("dng_validate not found: build it with tools/dng-validate/build.sh --sdk 1.7")

    folder = tempfile.mkdtemp(prefix="dngconv-lossy-")
    try:
        a = stage2(args.dng_validate, args.lossless, folder, "a")
        b = stage2(args.dng_validate, args.lossy, folder, "b")
    finally:
        shutil.rmtree(folder, ignore_errors=True)

    name = os.path.basename(args.lossy)
    if a.shape != b.shape:
        print(f"{name}: DIFFERENT: stage 2 is {b.shape} against {a.shape}")
        return 1
    coded = encode(b) - encode(a)
    rms = float(np.sqrt((coded * coded).mean()))
    block = block_means(coded) if a.ndim == 2 else float(np.abs(coded).mean())
    linear = b - a
    far = int((np.abs(linear) > 0.05).sum())
    d = args.distance
    ok = rms <= 0.025 * d and block <= 0.01 * d + 0.001
    print(f"{name}: {'within coding error' if ok else 'DIFFERENT'} at distance {d:g}: coded rms {rms * 100:.3f} % "
          f"(limit {2.5 * d:.3f} %), worst block mean {block * 100:.3f} % (limit {d + 0.1:.3f} %); linear: largest "
          f"error {np.abs(linear).max() * 100:.2f} %, {far} of {linear.size} samples off by more than 5 %")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
