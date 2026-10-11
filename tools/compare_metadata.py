#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Compare the metadata of each raw file with that of the DNG made from it,
tag by tag, as ExifTool reads them.

    compare_metadata.py RAW_FOLDER DNG_FOLDER [-v]

For every pair it counts how many of the source's tags are found in the DNG
with the same value, separately for
  - the maker note as reached through the EXIF MakerNote tag,
  - the maker note as reached through DNGPrivateData (the second copy),
  - the EXIF directory,
  - the GPS directory,
and it lists what ExifTool's validation says about the DNG that it did not
already say about the source. With -v the differing tags are named.

Expected differences, which are not errors: CFAPattern is left out of the
EXIF directory on purpose; a LensSpecification with 0/1 for an unknown
aperture is corrected to 0/0; tags that a maker note points to but does not
contain (Samsung's preview directory) do not travel; in a Canon CR3 five
items that ExifTool lists with the maker note are parts of the file
container (thumbnail, compressor version, offset tables); an empty GPS
directory holding nothing but its version is dropped.

This is a report for a human to read; the exit status is 0 unless ExifTool
cannot be run.
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys


def tags(path, selection):
    result = subprocess.run(["exiftool", "-a", "-G5", "-s", "-u"] + selection + [path],
                            capture_output=True, text=True, errors="replace")
    found = []
    for line in result.stdout.splitlines():
        match = re.match(r"\[([^\]]*)\]\s+(\S+)\s*: (.*)$", line)
        if match:
            found.append(match.groups())
    return found


def validation(path):
    result = subprocess.run(["exiftool", "-validate", "-warning", "-error", "-a", "-s3", path],
                            capture_output=True, text=True, errors="replace")
    return result.stdout.splitlines()


def compare(source, target):
    a = collections.Counter(set((tag, value) for _, tag, value in source))
    b = collections.Counter(set((tag, value) for _, tag, value in target))
    return sum((a & b).values()), sum(a.values()), a - b, b - a


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("raws")
    parser.add_argument("dngs")
    parser.add_argument("-v", "--verbose", action="store_true", help="name the tags that differ")
    args = parser.parse_args()
    try:
        subprocess.run(["exiftool", "-ver"], capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        print("exiftool is needed and was not found", file=sys.stderr)
        return 2

    pairs = 0
    for source in sorted(glob.glob(os.path.join(args.raws, "*"))):
        base = os.path.splitext(os.path.basename(source))[0]
        dng = os.path.join(args.dngs, base + ".dng")
        if not os.path.isfile(source) or not os.path.exists(dng):
            continue
        pairs += 1
        source_note = tags(source, ["-MakerNotes:all"])
        dng_note = tags(dng, ["-MakerNotes:all"])

        # In a CR3 the maker note sits in Canon's uuid box; the timed-metadata
        # track holds maker-note-style tags too, which are not part of the note.
        def in_note(group):
            return "ExifIFD" in group or ("UUID-Canon" in group and "CTMD" not in group)

        source_main = [t for t in source_note if in_note(t[0])]
        outside = collections.Counter(t[0].split("-MakerNote")[0].split("-")[-1]
                                      for t in source_note if not in_note(t[0]))
        rows = (("maker note via the EXIF tag  ", source_main, [t for t in dng_note if "ExifIFD" in t[0]]),
                ("maker note via private data  ", source_main, [t for t in dng_note if "ExifIFD" not in t[0]]),
                ("EXIF directory               ", tags(source, ["-ExifIFD:all"]), tags(dng, ["-ExifIFD:all"])),
                ("GPS directory                ", tags(source, ["-GPS:all"]), tags(dng, ["-GPS:all"])))
        print(f"== {base}")
        for name, a, b in rows:
            if not a and not b:
                continue
            same, total, missing, extra = compare(a, b)
            line = f"  {name} {same} of {total} identical"
            if missing or extra:
                line += f"; {sum(missing.values())} differ or are missing, {sum(extra.values())} only in the DNG"
            print(line)
            if args.verbose:
                for (tag, value) in list(missing)[:8]:
                    print(f"      source only: {tag} = {value[:70]}")
                for (tag, value) in list(extra)[:8]:
                    print(f"      DNG only:    {tag} = {value[:70]}")
        if outside:
            print("  maker data stored outside the maker note (not copied): "
                  + ", ".join(f"{group} {count}" for group, count in outside.items()))
        before, after = validation(source), validation(dng)
        print(f"  ExifTool validation   source: {before[0] if before else '?'} | DNG: {after[0] if after else '?'}")
        for line in after[1:]:
            if line not in before:
                print(f"      new in the DNG: {line}")
    print(f"{pairs} pairs compared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
