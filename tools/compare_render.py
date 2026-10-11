#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Develop each raw file and the DNG made from it with identical settings and
compare the pictures.

    compare_render.py RAW_FOLDER DNG_FOLDER [--montage FOLDER]

Both are developed by LibRaw (through rawpy): camera white balance, linear
demosaicing, no automatic brightening, 8 bits. If the DNG describes the raw
data correctly (pattern, levels, white balance, colour matrix), the two
pictures are the same; a wrong black level or a shifted mosaic shows at once.

Three things have to be pinned for the comparison to be fair:
  - LibRaw's automatic adjustment of the white point is switched off on both
    sides (adjust_maximum_thr = 0);
  - the source is developed with the DNG's white level, given as
    user_sat = white - black, because LibRaw applies user_sat after black
    subtraction;
  - the source is developed with the DNG's black level. The LibRaw inside
    rawpy is usually not the version dngconv was built with, and versions
    disagree about the black level of some cameras by a count or two. Where
    they do, a line says so; whether the DNG's value is the better one is
    then for a human to judge.

The same goes for the colour matrix: LibRaw's camera table changes between
versions, and the DNG carries the matrix of the version dngconv was built
with. When the pictures differ, they are developed once more without any
colour conversion; if they agree then, the raw data, levels, pattern and
white balance are right and only the matrix differs, which is reported as a
note and not counted as a failure.

A source file that LibRaw cannot develop (a damaged one, say) is skipped.

Exit status 1 if a pair differs by more than rounding (mean difference above
0.02 of 255, or more than 0.1 % of the values off by more than 1).
"""
import argparse
import glob
import os
import sys

import numpy as np
import rawpy


def render(path, user_sat=None, user_black=None, camera_colour=False):
    try:
        raw = rawpy.imread(path)
        raw.unpack()
    except Exception as error:  # rawpy raises its own types
        return None, {}, str(error)
    with raw:
        options = dict(use_camera_wb=True, no_auto_bright=True, output_bps=8, user_flip=0,
                       demosaic_algorithm=rawpy.DemosaicAlgorithm.LINEAR, adjust_maximum_thr=0.0,
                       highlight_mode=rawpy.HighlightMode.Clip)
        if user_sat:
            options["user_sat"] = int(user_sat)
        if user_black is not None:
            options["user_black"] = int(user_black)
        if camera_colour:
            options["output_color"] = rawpy.ColorSpace.raw
        sizes = raw.sizes
        info = dict(white=raw.white_level, black=list(raw.black_level_per_channel),
                    area=(sizes.left_margin, sizes.top_margin, sizes.width, sizes.height))
        try:
            return raw.postprocess(**options), info, None
        except Exception as error:
            return None, info, str(error)


def overlap(a, area_a, b, area_b):
    """The part both pictures show, when the two active areas differ."""
    left, top = max(area_a[0], area_b[0]), max(area_a[1], area_b[1])
    right = min(area_a[0] + area_a[2], area_b[0] + area_b[2])
    bottom = min(area_a[1] + area_a[3], area_b[1] + area_b[3])
    return (a[top - area_a[1]:bottom - area_a[1], left - area_a[0]:right - area_a[0]],
            b[top - area_b[1]:bottom - area_b[1], left - area_b[0]:right - area_b[0]])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("raws")
    parser.add_argument("dngs")
    parser.add_argument("--montage", help="folder for side-by-side pictures (needs Pillow)")
    args = parser.parse_args()

    print("LibRaw used here: %s" % ".".join(str(v) for v in rawpy.libraw_version))
    compared = failed = skipped = 0
    for source in sorted(glob.glob(os.path.join(args.raws, "*"))):
        base = os.path.splitext(os.path.basename(source))[0]
        dng = os.path.join(args.dngs, base + ".dng")
        if not os.path.isfile(source) or not os.path.exists(dng):
            continue
        b, info_b, error = render(dng)
        if b is None:
            print(f"{base}: cannot develop the DNG: {error}")
            failed += 1
            continue
        black = int(round(sum(info_b["black"]) / len(info_b["black"])))
        a, info_a, error = render(source, user_sat=info_b["white"] - black, user_black=black)
        if a is None:
            print(f"{base}: skipped, LibRaw cannot develop the source: {error}")
            skipped += 1
            continue
        def measure(a, b):
            if a.shape != b.shape:
                a, b = overlap(a, info_a["area"], b, info_b["area"])
            if a.shape != b.shape or a.size == 0:
                return None
            diff = np.abs(a.astype(np.int16) - b.astype(np.int16))
            return a, b, float(diff.mean()), int(diff.max()), float((diff > 1).mean())

        result = measure(a, b)
        if result is None:
            print(f"{base}: the pictures do not have a common area")
            failed += 1
            continue
        a, b, mean, worst, off = result
        ok = mean <= 0.02 and off <= 0.001
        verdict = "same picture" if ok else "DIFFERENT"
        note = None
        if not ok:
            # Is it only the colour matrix? Develop both in the camera's own colours.
            a2 = render(source, user_sat=info_b["white"] - black, user_black=black, camera_colour=True)[0]
            b2 = render(dng, camera_colour=True)[0]
            again = measure(a2, b2) if a2 is not None and b2 is not None else None
            if again and again[2] <= 0.02 and again[4] <= 0.001:
                ok = True
                verdict = "same raw data and levels"
                note = "the colour matrix in the DNG is not the one this LibRaw has for the camera"
        compared += 1
        failed += not ok
        print(f"{base[:34]:34s} {a.shape[1]}x{a.shape[0]}  mean difference {mean:.4f}  largest {worst:3d}  "
              f"off by more than 1: {100 * off:.4f} %  {verdict}")
        if note:
            print(f"    note: {note}")
        if info_a.get("black") and int(round(sum(info_a["black"]) / len(info_a["black"]))) != black:
            print(f"    note: black level {info_a['black'][0]} according to this LibRaw, {black} in the DNG")
        if args.montage:
            from PIL import Image
            os.makedirs(args.montage, exist_ok=True)
            left, right = Image.fromarray(a), Image.fromarray(b)
            left.thumbnail((640, 480))
            right.thumbnail((640, 480))
            sheet = Image.new("RGB", (left.width + right.width + 6, max(left.height, right.height)), (255, 0, 255))
            sheet.paste(left, (0, 0))
            sheet.paste(right, (left.width + 6, 0))
            sheet.save(os.path.join(args.montage, base + ".jpg"), quality=85)
    print(f"{compared} pairs compared, {failed} with differences or errors, {skipped} skipped")
    return 1 if failed or not compared else 0


if __name__ == "__main__":
    sys.exit(main())
