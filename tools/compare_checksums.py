#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Check `dngconv check --add` against checksums another writer computed.

    compare_checksums.py [--dngconv PATH] [--dng-validate PATH] DNG...

For every DNG that carries checksums (RawImageDigest, NewRawImageDigest,
OriginalRawFileDigest), a copy is made, the checksums are taken out of the
copy, and `dngconv check --add` is asked to write them again. What it
writes must be, byte for byte, what the file's own writer had put there.
With files from Adobe's converter that compares dngconv's arithmetic with
Adobe's; with files from dngconv itself it compares the reader of the check
command with the writer.

A DNG without checksums (most cameras write none) has nothing to compare
with. It is given its checksums all the same, and Adobe's dng_validate, if
there is one, is asked whether it agrees with them: the validator recomputes
a checksum it finds and reports a mismatch.

Raw data dngconv cannot read (floating point, an image with a transparency
mask) gets no checksum; such files are listed as skipped.

In both cases the rest of the file must be untouched: apart from the four
bytes of the header that point to the first directory, every byte of the
file as it was is still there, and only the end is new.

The originals are never written to. Exit status 1 if anything differs.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

RAW_OLD, ORIGINAL, RAW_NEW, DNG_VERSION = 50972, 50973, 51111, 50706
NAMES = {RAW_OLD: "RawImageDigest", RAW_NEW: "NewRawImageDigest", ORIGINAL: "OriginalRawFileDigest"}


def first_directory(data):
    """Byte order, offset of the first directory and its entries."""
    order = {b"II": "<", b"MM": ">"}.get(data[:2])
    if order is None or struct.unpack(order + "H", data[2:4])[0] != 42:
        raise ValueError("not a TIFF file")
    at = struct.unpack(order + "I", data[4:8])[0]
    count = struct.unpack(order + "H", data[at:at + 2])[0]
    entries = [data[at + 2 + 12 * i:at + 14 + 12 * i] for i in range(count)]
    return order, at, entries


def checksums(data):
    """The checksum tags of the first directory, and the DNG version."""
    order, _, entries = first_directory(data)
    found, version = {}, None
    for e in entries:
        tag, _, count = struct.unpack(order + "HHI", e[:8])
        if tag in NAMES and count == 16:
            where = struct.unpack(order + "I", e[8:12])[0]
            found[tag] = data[where:where + 16]
        if tag == DNG_VERSION:
            version = tuple(e[8:12])
    return found, version


def without_checksums(data):
    """The file with the checksum entries taken out of its first directory."""
    order, at, entries = first_directory(data)
    kept = [e for e in entries if struct.unpack(order + "H", e[:2])[0] not in NAMES]
    link = data[at + 2 + 12 * len(entries):at + 6 + 12 * len(entries)]
    directory = struct.pack(order + "H", len(kept)) + b"".join(kept) + link
    out = bytearray(data)
    out[at:at + len(directory)] = directory
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dngs", nargs="+", metavar="DNG")
    parser.add_argument("--dngconv", default=os.environ.get("DNGCONV") or shutil.which("dngconv"))
    parser.add_argument("--dng-validate", default=os.environ.get("DNG_VALIDATE") or shutil.which("dng_validate"))
    args = parser.parse_args()
    if not args.dngconv:
        raise SystemExit("dngconv not found: pass --dngconv")

    failures = 0
    folder = tempfile.mkdtemp(prefix="dngconv-sums-")
    try:
        for path in args.dngs:
            name = os.path.basename(path)
            with open(path, "rb") as f:
                original = f.read()
            try:
                had, version = checksums(original)
            except (ValueError, struct.error) as e:
                print(f"{name}: FAILED: {e}")
                failures += 1
                continue
            stripped = without_checksums(original)
            copy = os.path.join(folder, "copy.dng")
            with open(copy, "wb") as f:
                f.write(stripped)

            run = subprocess.run([args.dngconv, "check", "--add", copy], capture_output=True, text=True)
            said = (run.stdout + run.stderr).strip().replace(copy + ": ", "")
            with open(copy, "rb") as f:
                after = f.read()
            got, _ = checksums(after)

            problems = []
            if run.returncode != 0:
                problems.append(f"dngconv said: {said}")
            if after[8:len(stripped)] != stripped[8:] or after[:4] != stripped[:4]:
                problems.append("bytes of the file as it was have changed")
            compared = []
            declined = ""
            for tag, value in had.items():
                # Of the two raw checksums dngconv writes the one the file's
                # DNG version defines; a file that had both gets that one.
                if tag not in got:
                    if tag in (RAW_OLD, RAW_NEW) and (RAW_OLD in got or RAW_NEW in got) and len(
                            [t for t in had if t in (RAW_OLD, RAW_NEW)]) == 2:
                        continue
                    # Forms of raw data dngconv says it cannot read (floating
                    # point, a transparency mask) are a limit, not an error.
                    if tag != ORIGINAL and ("cannot check" in said or "none can be" in said):
                        declined = said
                        continue
                    problems.append(f"{NAMES[tag]} was not written ({said})")
                elif got[tag] != value:
                    problems.append(f"{NAMES[tag]} is {got[tag].hex()}, the file's writer had {value.hex()}")
                else:
                    compared.append(NAMES[tag])
            added = [NAMES[t] for t in got if t not in had]

            validated = ""
            if args.dng_validate and got and not problems:
                report = subprocess.run([args.dng_validate, copy], capture_output=True, text=True)
                text = report.stdout + report.stderr
                wrong = [line.strip() for line in text.splitlines() if "igest" in line and "***" in line]
                if wrong:
                    problems.append("dng_validate: " + "; ".join(wrong))
                elif "Validation complete" in text:
                    validated = ", accepted by dng_validate"
                else:
                    problems.append("dng_validate could not read the file")

            if problems:
                print(f"{name}: DIFFERENT: " + "; ".join(problems))
                failures += 1
            elif compared:
                print(f"{name}: the same as the writer's ({', '.join(compared)}){validated}")
            elif added:
                print(f"{name}: had none; given {', '.join(added)}{validated}")
            elif declined:
                print(f"{name}: skipped: {declined}")
            else:
                print(f"{name}: nothing to compare: {said}")
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
