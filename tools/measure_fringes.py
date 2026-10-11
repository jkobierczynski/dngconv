#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Measure lateral chromatic aberration in a raw file, on the sensor data
itself, and compare it with what a DNG's lens opcodes will correct.

    measure_fringes.py RAW_FILE [DNG_FILE]

Lateral chromatic aberration makes the red and blue images slightly larger
or smaller than the green one. The script measures, patch by patch, how far
the red and the blue samples are displaced against the green ones along the
radius, and prints the result by distance from the centre. With a DNG it
also prints the displacement that the DNG's WarpRectilinear opcode takes
out, so that the two columns can be compared: they should have the same
sign and about the same size.

The measurement is made on the mosaic, not on a developed picture:
demosaicing mixes the colours and makes the fringes look smaller than they
are (by half, for blue, in the files this was tried on). For a Bayer sensor
each colour's samples form a picture of half the size; for X-Trans the
samples of each colour are averaged over 3 x 3 blocks, in which all three
colours have their centre of gravity in the middle. On Bayer files the two
green planes are compared with each other as a control, which must read 0.

It needs detail in the picture and a low ISO setting: on a noisy night shot
the figures mean little. Expect an accuracy of about 0.1 pixel on a good
file. This is a report for a human; the exit status is always 0.

Needs ExifTool (only with a DNG) and the packages in requirements.txt.
"""
import argparse
import struct
import subprocess
import sys

import cv2
import numpy as np
import rawpy


def colour_planes(raw):
    """Co-sited pictures of the red, green and blue samples, the factor by
    which they are smaller than the frame, and the position of plane sample
    (0, 0) in frame pixels for each."""
    data = raw.raw_image.astype(np.float32)
    colours = raw.raw_colors
    names = raw.color_desc.decode()
    black = raw.black_level_per_channel
    for c in range(len(black)):
        data[colours == c] -= black[c]
    data = np.maximum(data, 0)

    if raw.raw_pattern.shape == (2, 2):
        planes = {}
        for y in range(2):
            for x in range(2):
                name = names[colours[y, x]]
                if name == "G" and "G" in planes:
                    name = "G2"
                planes[name] = (data[y::2, x::2], (x, y))
        return planes, 2
    if raw.raw_pattern.shape == (6, 6):
        # Find the 3 x 3 grid whose blocks have green in the centre and the corners.
        letters = np.array(list(names))[colours]
        for oy in range(3):
            for ox in range(3):
                block = letters[oy:oy + 60, ox:ox + 60]
                if np.all(block[1::3, 1::3] == "G") and np.all(block[0::3, 0::3] == "G"):
                    h = (data.shape[0] - oy) // 3 * 3
                    w = (data.shape[1] - ox) // 3 * 3
                    d, l = data[oy:oy + h, ox:ox + w], letters[oy:oy + h, ox:ox + w]
                    planes = {}
                    for name in "RGB":
                        mask = (l == name).astype(np.float32)
                        total = (d * mask).reshape(h // 3, 3, w // 3, 3).sum(axis=(1, 3))
                        count = mask.reshape(h // 3, 3, w // 3, 3).sum(axis=(1, 3))
                        planes[name] = (total / np.maximum(count, 1), (ox + 1, oy + 1))
                    return planes, 3
    raise SystemExit("neither a Bayer nor an X-Trans sensor")


def displacement(a, b, step, centre, unit):
    """Radial displacement of plane b's content against plane a's, per patch:
    rows of (radius / unit, radial shift in frame pixels)."""
    (image_a, origin_a), (image_b, origin_b) = a, b
    h = min(image_a.shape[0], image_b.shape[0])
    w = min(image_a.shape[1], image_b.shape[1])
    patch = 128 if step == 2 else 96
    window = cv2.createHanningWindow((patch, patch), cv2.CV_32F)
    log_a, log_b = np.log1p(image_a[:h, :w]), np.log1p(image_b[:h, :w])
    rows = []
    for y0 in range(0, h - patch, patch * 3 // 4):
        for x0 in range(0, w - patch, patch * 3 // 4):
            u = log_a[y0:y0 + patch, x0:x0 + patch]
            v = log_b[y0:y0 + patch, x0:x0 + patch]
            if u.std() < 0.08:
                continue  # nothing to hold on to
            u = u - cv2.GaussianBlur(u, (0, 0), 6)
            v = v - cv2.GaussianBlur(v, (0, 0), 6)
            (dx, dy), response = cv2.phaseCorrelate(u, v, window)
            if response < 0.2 or abs(dx) > 4 or abs(dy) > 4:
                continue
            # In frame pixels, allowing for where each plane's samples sit.
            fx = step * dx + (origin_b[0] - origin_a[0])
            fy = step * dy + (origin_b[1] - origin_a[1])
            px = origin_a[0] + step * (x0 + patch / 2) - centre[0]
            py = origin_a[1] + step * (y0 + patch / 2) - centre[1]
            radius = np.hypot(px, py)
            if radius > 1:
                rows.append((radius / unit, (fx * px + fy * py) / radius))
    return np.array(rows).reshape(-1, 2)


def by_zone(rows, zones=10):
    out = []
    for i in range(zones):
        inside = rows[(rows[:, 0] >= i / zones) & (rows[:, 0] < (i + 1) / zones)]
        out.append((float(np.median(inside[:, 1])) if len(inside) > 4 else None, len(inside)))
    return out


def opcode_warp(dng):
    """The WarpRectilinear opcode of a DNG: per-plane coefficients, the centre
    in frame pixels and the radius unit; None if there is none."""
    data = subprocess.run(["exiftool", "-b", "-OpcodeList3", dng], capture_output=True).stdout
    area = subprocess.run(["exiftool", "-n", "-s3", "-ActiveArea", dng], capture_output=True, text=True).stdout.split()
    if len(data) < 4 or len(area) != 4:
        return None
    top, left, bottom, right = (int(v) for v in area)
    at = 4
    for _ in range(struct.unpack(">I", data[:4])[0]):
        code, _version, _flags, size = struct.unpack(">4I", data[at:at + 16])
        body = data[at + 16:at + 16 + size]
        at += 16 + size
        if code != 1:
            continue
        planes = struct.unpack(">I", body[:4])[0]
        k = [struct.unpack(">6d", body[4 + 48 * p:52 + 48 * p])[:4] for p in range(planes)]
        cx, cy = struct.unpack(">2d", body[4 + 48 * planes:20 + 48 * planes])
        width, height = right - left, bottom - top
        centre = (left + cx * width, top + cy * height)
        unit = np.hypot(max(cx, 1 - cx) * width, max(cy, 1 - cy) * height)
        return dict(k=k, centre=centre, unit=unit)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("raw")
    parser.add_argument("dng", nargs="?")
    args = parser.parse_args()

    with rawpy.imread(args.raw) as raw:
        planes, step = colour_planes(raw)
        sizes = raw.sizes
        centre = (sizes.left_margin + sizes.width / 2, sizes.top_margin + sizes.height / 2)
        unit = np.hypot(sizes.width / 2, sizes.height / 2)

    warp = opcode_warp(args.dng) if args.dng else None
    if warp:
        centre, unit = warp["centre"], warp["unit"]
    if args.dng and (not warp or len(warp["k"]) != 3):
        print("the DNG has no chromatic aberration correction (no three-plane WarpRectilinear)")
        warp = None

    def corrected(plane, r):
        """Displacement against green that the opcode removes at stored radius r (in units)."""
        ratio = lambda k, x: k[0] + x * x * (k[1] + x * x * (k[2] + x * x * k[3]))
        # Find the corrected radius whose green source radius is r.
        x = r
        for _ in range(20):
            x = r / ratio(warp["k"][1], x)
        return warp["unit"] * x * (ratio(warp["k"][plane], x) - ratio(warp["k"][1], x))

    print(f"{args.raw}: {'Bayer' if step == 2 else 'X-Trans'} sensor; displacement against green along the radius, in pixels")
    header = "  radius    red measured" + ("  opcode" if warp else "") + "    blue measured" + ("  opcode" if warp else "") + "   patches"
    print(header)
    red = by_zone(displacement(planes["G"], planes["R"], step, centre, unit))
    blue = by_zone(displacement(planes["G"], planes["B"], step, centre, unit))
    for i, ((r, n), (b, _)) in enumerate(zip(red, blue)):
        mid = (i + 0.5) / 10
        cell = lambda v: "   n/a" if v is None else "%+6.2f" % v
        line = f"  {mid:4.2f}        {cell(r)}"
        if warp:
            line += "    %+6.2f" % corrected(0, mid)
        line += f"           {cell(b)}"
        if warp:
            line += "    %+6.2f" % corrected(2, mid)
        print(line + f"     {n:4d}")
    if "G2" in planes:
        control = by_zone(displacement(planes["G"], planes["G2"], step, centre, unit))
        values = [abs(v) for v, _ in control if v is not None]
        if values:
            print(f"  control (second green plane against the first, should be 0): at most {max(values):.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
