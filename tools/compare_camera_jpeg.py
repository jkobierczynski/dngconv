#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Check a DNG's lens corrections against the camera's own JPEG.

    compare_camera_jpeg.py RAW_FILE DNG_FILE [--dng-validate PATH] [--keep FOLDER]

Every camera that records lens-correction parameters also stores a JPEG in
the raw file, and that JPEG has the camera's correction in it. This script
  1. takes the largest JPEG out of the raw file (ExifTool),
  2. develops the DNG with Adobe's dng_validate, which applies the opcodes,
  3. finds a few thousand corresponding points in the two pictures (SIFT),
  4. fits the JPEG onto the render with a scale and a shift.
What the fit cannot explain is reported: the median distance between
matched points, in pixels of the full-size render, and how much of it is
radial, in five zones from the centre to the corner. A correct lens
correction leaves a fraction of a pixel to about one pixel (the JPEGs are
small) and no radial trend; a missing or misread one leaves a trend of many
pixels. The scales say whether the picture is framed like the camera's.

Run it on a DNG converted with --lens-corrections none to see the
uncorrected figure for the same file.

Needs ExifTool, dng_validate (tools/dng-validate/build.sh; found through
--dng-validate, the DNG_VALIDATE variable or the PATH), and the packages in
requirements.txt.

Exit status 1 if the median residual exceeds --max-residual (2 pixels).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matching  # noqa: E402


def camera_jpeg(raw, folder):
    """The largest JPEG embedded in the raw file."""
    best = None
    for tag in ("JpgFromRaw", "PreviewImage", "OtherImage"):
        data = subprocess.run(["exiftool", "-b", "-" + tag, raw], capture_output=True).stdout
        if data[:2] == b"\xff\xd8" and (best is None or len(data) > len(best)):
            best = data
    if best is None:
        raise SystemExit("no JPEG found inside " + raw)
    path = os.path.join(folder, "camera.jpg")
    with open(path, "wb") as out:
        out.write(best)
    return path


def develop(dng, validator, folder):
    stem = os.path.join(folder, "render")
    result = subprocess.run([validator, "-tif", stem, dng], capture_output=True, text=True)
    if not os.path.exists(stem + ".tif"):
        raise SystemExit("dng_validate did not produce a picture:\n" + result.stdout + result.stderr)
    return stem + ".tif"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("raw")
    parser.add_argument("dng")
    parser.add_argument("--dng-validate", default=os.environ.get("DNG_VALIDATE") or shutil.which("dng_validate"))
    parser.add_argument("--max-residual", type=float, default=2.0)
    parser.add_argument("--keep", help="folder to keep the two pictures in")
    args = parser.parse_args()
    if not args.dng_validate:
        raise SystemExit("dng_validate not found: build it with tools/dng-validate/build.sh and pass --dng-validate")

    folder = args.keep or tempfile.mkdtemp(prefix="dngconv-lens-")
    os.makedirs(folder, exist_ok=True)
    try:
        jpeg = matching.load(camera_jpeg(args.raw, folder))
        tif = develop(args.dng, args.dng_validate, folder)
        # dng_validate turns the picture upright; the camera's JPEG is stored
        # as the sensor saw it. Try the render as it is and turned back.
        best = None
        for turns in (0, 3, 1, 2):
            render = matching.load(tif, turns)
            landscape_match = (render.shape[1] >= render.shape[0]) == (jpeg.shape[1] >= jpeg.shape[0])
            if not landscape_match:
                continue
            a, b = matching.matches(jpeg, render)
            if best is None or len(a) > len(best[0]):
                best = (a, b, render.shape)
            if len(a) >= 200:
                break
        a, b, shape = best
        if len(a) < 50:
            raise SystemExit(f"only {len(a)} matching points: the pictures have too little in common")
        fit = matching.fit_scale_and_shift(a, b, (jpeg.shape[1], jpeg.shape[0]), (shape[1], shape[0]))
    finally:
        if not args.keep:
            shutil.rmtree(folder, ignore_errors=True)

    zones = " ".join("%+6.2f" % z if z is not None else "   n/a" for z in fit["zones"])
    print(f"{os.path.basename(args.dng)}: {fit['matches']} matching points, {fit['used']} used; "
          f"camera JPEG {jpeg.shape[1]}x{jpeg.shape[0]}, render {shape[1]}x{shape[0]}")
    print(f"  median residual {fit['median']:.2f} px; radial part by zone, centre to corner: {zones}")
    print(f"  scale {fit['scale_x']:.4f} x {fit['scale_y']:.4f} of nominal, shift ({fit['shift'][0]:+.1f}, {fit['shift'][1]:+.1f}) px")
    ok = fit["median"] <= args.max_residual
    print("  " + ("geometry matches the camera's JPEG" if ok else "GEOMETRY DIFFERS from the camera's JPEG"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
