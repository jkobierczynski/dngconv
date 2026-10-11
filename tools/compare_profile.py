#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Checks that the camera profiles in DNG files are the ones in a folder of
.dcp files, value for value.

    compare_profile.py PROFILE_FOLDER DNG_FILE...

For each DNG converted with `dngconv --profile`, the profile tags of the
first directory (colour matrices, illuminants, tables, tone curve, name,
copyright, embed policy) are compared with every .dcp file in the folder,
and so is each block listed in ExtraCameraProfiles. Every one of them must be
equal to a profile from the folder: same tags, same types, same values. The
byte order of the files does not matter; the camera model a .dcp file names
is not part of the comparison, because inside a DNG the file's own applies.

Needs nothing but Python. Exit status 1 if a profile in a DNG has no equal
in the folder, or a DNG has no profile name at all.
"""
import os
import struct
import sys

# The tags that make up a camera profile (DNG 1.4, 1.6 and 1.7).
PROFILE_TAGS = {
    50721, 50722, 50725, 50726, 50778, 50779, 50932, 50936, 50937, 50938, 50939, 50940,
    50941, 50942, 50964, 50965, 50981, 50982, 51107, 51108, 51109, 51110,
    52529, 52531, 52532, 52533, 52534, 52535, 52537, 52538, 52539, 52544, 52551, 52552,
}
EXTRA_CAMERA_PROFILES = 50933
PROFILE_NAME = 50936
# Bytes per value, and the unit that is byte-swapped (rationals: two halves).
SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8}
UNIT = {1: 1, 2: 1, 3: 2, 4: 4, 5: 4, 6: 1, 7: 1, 8: 2, 9: 4, 10: 4, 11: 4, 12: 8}


def directory(data, base):
    """The first directory of the TIFF-like structure at `base`: tag ->
    (type, count, value as little-endian bytes)."""
    order = data[base:base + 2]
    if order not in (b"II", b"MM"):
        raise ValueError("no byte-order mark")
    e = "<" if order == b"II" else ">"
    first = struct.unpack(e + "I", data[base + 4:base + 8])[0] + base
    count = struct.unpack(e + "H", data[first:first + 2])[0]
    out = {}
    for i in range(count):
        at = first + 2 + 12 * i
        tag, kind, n = struct.unpack(e + "HHI", data[at:at + 8])
        if kind not in SIZE:
            continue
        size = SIZE[kind] * n
        where = at + 8 if size <= 4 else base + struct.unpack(e + "I", data[at + 8:at + 12])[0]
        value = data[where:where + size]
        if len(value) != size:
            raise ValueError("tag %d lies outside the file" % tag)
        if e == ">" and UNIT[kind] > 1:
            u = UNIT[kind]
            value = b"".join(value[k:k + u][::-1] for k in range(0, size, u))
        out[tag] = (kind, n, value)
    return out, e


def profile_of(entries):
    return {tag: value for tag, value in entries.items() if tag in PROFILE_TAGS}


def name_of(profile):
    if PROFILE_NAME not in profile:
        return "(unnamed)"
    return profile[PROFILE_NAME][2].split(b"\0")[0].decode("utf-8", "replace")


def main():
    if len(sys.argv) < 3 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    folder, dngs = sys.argv[1], sys.argv[2:]

    library = []  # (file name, profile)
    for root, _, files in os.walk(folder):
        for name in sorted(files):
            if not name.lower().endswith(".dcp"):
                continue
            path = os.path.join(root, name)
            try:
                with open(path, "rb") as f:
                    entries, _ = directory(f.read(), 0)
                library.append((os.path.relpath(path, folder), profile_of(entries)))
            except (ValueError, struct.error, OSError):
                pass  # not a profile; dngconv leaves those out as well
    if not library:
        print("no .dcp files in", folder)
        return 2

    def source(profile):
        for name, candidate in library:
            if candidate == profile:
                return name
        return None

    failures = 0
    for path in dngs:
        with open(path, "rb") as f:
            data = f.read()
        entries, e = directory(data, 0)
        main_profile = profile_of(entries)
        found = [("profile", main_profile)]
        if EXTRA_CAMERA_PROFILES in entries:
            _, n, value = entries[EXTRA_CAMERA_PROFILES]
            for offset in struct.unpack("<%dI" % n, value):
                if data[offset + 2:offset + 4] not in (b"RC", b"CR"):
                    print("%s: the block at %d is not a camera profile" % (os.path.basename(path), offset))
                    failures += 1
                    continue
                block, _ = directory(data, offset)
                found.append(("extra profile", profile_of(block)))
        if PROFILE_NAME not in main_profile:
            print("%s: no camera profile (only the decoder's colour matrix)" % os.path.basename(path))
            failures += 1
            continue
        parts = []
        for kind, profile in found:
            origin = source(profile)
            if origin is None:
                failures += 1
                parts.append("%s '%s' HAS NO EQUAL in the folder" % (kind, name_of(profile)))
            else:
                parts.append("%s '%s' = %s (%d tags)" % (kind, name_of(profile), origin, len(profile)))
        print("%s: %s" % (os.path.basename(path), "; ".join(parts)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
