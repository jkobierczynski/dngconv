#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Compare the preview dngconv rendered with the camera's own JPEG.

    compare_preview.py RAW_FILE DNG_FILE [--keep FOLDER]

For a DNG converted with `--preview render` from a raw file that also holds
a camera JPEG, the two pictures show the same scene: one developed by the
camera, one by dngconv's quick renderer. The script
  1. takes the JPEG preview out of the DNG and decodes it (which shows that
     an ordinary JPEG reader accepts what dngconv's encoder wrote),
  2. takes the largest JPEG out of the raw file (ExifTool),
  3. matches points between the two (SIFT) and fits a scale and a shift,
  4. compares brightness and colour over a coarse grid.
Reported: the median distance between matched points in preview pixels
(geometry, including the lens corrections applied to the preview), and how
far the tones are apart: the median brightness of each picture on the 0..255
scale, and the typical difference per grid cell.

The geometry should agree to a pixel or so. The tones will not be equal:
every camera has its own curve and colours, and dngconv uses one curve for
all. A difference of a stop in the mid-tones is within what cameras differ
by among themselves.

Needs ExifTool and the packages in requirements.txt.

Exit status 1 if the DNG has no preview that decodes, or the median residual
exceeds --max-residual (1.5 preview pixels); 2 if there is nothing to compare
with (no JPEG in the raw file, or too little in common to match points).
"""
import argparse
import os
import shutil
import struct
import sys
import tempfile

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matching  # noqa: E402
from compare_camera_jpeg import camera_jpeg  # noqa: E402


def dng_preview(path):
    """The JPEG preview of a DNG: the reduced-resolution image stored as
    JPEG in the first directory or one of its sub-directories. Returns the
    bytes and the name of the application that made it, if the file says."""
    with open(path, "rb") as f:
        data = f.read()
    e = "<" if data[:2] == b"II" else ">"

    def directory(offset):
        count = struct.unpack(e + "H", data[offset:offset + 2])[0]
        out = {}
        for i in range(count):
            at = offset + 2 + 12 * i
            tag, kind, n = struct.unpack(e + "HHI", data[at:at + 8])
            size = {1: 1, 2: 1, 3: 2, 4: 4, 7: 1}.get(kind, 8) * n
            start = at + 8 if size <= 4 else struct.unpack(e + "I", data[at + 8:at + 12])[0]
            out[tag] = (kind, n, data[start:start + size])
        return out

    def numbers(entry):
        kind, n, raw = entry
        return struct.unpack(e + ("H" if kind == 3 else "I") * n, raw)

    first = directory(struct.unpack(e + "I", data[4:8])[0])
    candidates = [first] + [directory(o) for o in (numbers(first[330]) if 330 in first else ())]
    for d in candidates:
        if 259 in d and numbers(d[259])[0] == 7 and 254 in d and numbers(d[254])[0] == 1 and 273 in d:
            start, length = numbers(d[273])[0], numbers(d[279])[0]
            maker = d[50966][2].split(b"\0")[0].decode("utf-8", "replace") if 50966 in d else ""
            return data[start:start + length], maker
    return None, ""


def luma(image):
    return 0.2126 * image[..., 2] + 0.7152 * image[..., 1] + 0.0722 * image[..., 0]  # BGR


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("raw")
    parser.add_argument("dng")
    parser.add_argument("--max-residual", type=float, default=1.5)
    parser.add_argument("--keep", help="folder to keep the two pictures in")
    args = parser.parse_args()

    jpeg, maker = dng_preview(args.dng)
    name = os.path.basename(args.dng)
    if jpeg is None:
        raise SystemExit(name + ": no JPEG preview in the DNG")
    preview = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
    if preview is None:
        raise SystemExit(name + ": the preview does not decode as a JPEG")

    folder = args.keep or tempfile.mkdtemp(prefix="dngconv-preview-")
    os.makedirs(folder, exist_ok=True)
    try:
        try:
            camera = matching.load(camera_jpeg(args.raw, folder))
        except SystemExit as nothing:
            print(f"{name}: preview {preview.shape[1]}x{preview.shape[0]} decodes; not compared: {nothing}")
            return 2
        if args.keep:
            with open(os.path.join(folder, "preview.jpg"), "wb") as out:
                out.write(jpeg)
    finally:
        if not args.keep:
            shutil.rmtree(folder, ignore_errors=True)

    # Matched at the size of the preview: the camera's picture may be many
    # times larger, and nothing finer than the preview can be compared anyway.
    b, a = matching.matches(preview, camera)
    if len(a) < 50:
        print(f"{name}: preview {preview.shape[1]}x{preview.shape[0]} decodes; not compared: only {len(a)} "
              "matching points, the pictures have too little in common")
        return 2
    fit = matching.fit_scale_and_shift(a, b, (camera.shape[1], camera.shape[0]), (preview.shape[1], preview.shape[0]))

    # Tones, cell by cell, with the camera's picture brought to the preview's size.
    same = cv2.resize(camera, (preview.shape[1], preview.shape[0]), interpolation=cv2.INTER_AREA)
    cell = 16
    h, w = preview.shape[0] // cell * cell, preview.shape[1] // cell * cell

    def cells(image):
        return image[:h, :w].astype(float).reshape(h // cell, cell, w // cell, cell, 3).mean((1, 3))

    ours, theirs = cells(preview), cells(same)
    brightness = np.abs(luma(ours) - luma(theirs))
    colour = np.abs((ours - luma(ours)[..., None]) - (theirs - luma(theirs)[..., None])).max(2)

    zones = " ".join("%+6.2f" % z if z is not None else "   n/a" for z in fit["zones"])
    print(f"{name}: preview {preview.shape[1]}x{preview.shape[0]}, {len(jpeg)} bytes"
          f"{', made by ' + maker if maker else ''}; camera JPEG {camera.shape[1]}x{camera.shape[0]}")
    print(f"  geometry: {fit['used']} points, median residual {fit['median']:.2f} px; "
          f"radial part by zone, centre to corner: {zones}")
    print(f"  tones: median brightness {np.median(luma(ours)):.0f} (camera {np.median(luma(theirs)):.0f}) of 255; "
          f"typical difference per cell {np.median(brightness):.0f} in brightness, {np.median(colour):.0f} in colour")
    ok = fit["median"] <= args.max_residual
    print("  " + ("geometry matches the camera's JPEG" if ok else "GEOMETRY DIFFERS from the camera's JPEG"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
