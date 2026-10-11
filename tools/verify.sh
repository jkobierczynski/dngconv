#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
#
# Runs the whole verification on a folder of camera raw files: converts
# them, and checks the result every way this folder has a tool for.
#
#   tools/verify.sh RAW_FOLDER [WORK_FOLDER]
#
# RAW_FOLDER is searched recursively and is not written to. WORK_FOLDER
# (default: ./verify-work) receives the DNGs, the restored originals and the
# logs; it is emptied first.
#
# What is run, and what each step needs:
#   convert      dngconv -e --verify              pixels and embedded original read back
#   check        dngconv check                    every DNG intact by its own checksums
#   extract      dngconv extract, cmp             originals restored byte for byte
#   update       dngconv -u, twice                nothing redone; a touched file redone
#   times        dngconv -t, extract -t           dates carried over
#   sidecar      dngconv with an .xmp             (ExifTool) keywords arrive in the DNG
#   exiftool     exiftool -validate               (ExifTool) no error in any DNG
#   adobe        dng_validate                     (dng_validate) Adobe's reader accepts every DNG
#   render       compare_render.py                (Python) source and DNG develop to the same picture
#   metadata     compare_metadata.py              (ExifTool, Python) report only, see metadata.txt
#   lens         compare_camera_jpeg.py           (dng_validate, Python) corrected geometry matches
#                                                 the camera's JPEG, for files with a distortion opcode
#   profiles     dngconv --profile, compare_profile.py   (Python; only with PROFILES set) every
#                                                 embedded camera profile equals its .dcp file
#
# Steps whose tools are missing are skipped and say so. The exit status is 0
# if no step failed.
#
# Environment:
#   DNGCONV       the program to test (default: build/dngconv next to this folder)
#   DNG_VALIDATE  Adobe's validator (default: dng_validate on the PATH); build
#                 it with tools/dng-validate/build.sh
#   PYTHON        the Python to use (default: python3)
#   PROFILES      a folder of camera profiles (.dcp) to embed and compare;
#                 without it the profiles step is skipped
set -u

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
raws=${1:-}
work=${2:-verify-work}
if [ -z "$raws" ] || [ ! -d "$raws" ]; then
    sed -n '5,9p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
dngconv=${DNGCONV:-$here/../build/dngconv}
validator=${DNG_VALIDATE:-$(command -v dng_validate 2>/dev/null || true)}
python=${PYTHON:-python3}

if [ ! -x "$dngconv" ]; then
    echo "dngconv not found at $dngconv (set DNGCONV)" >&2
    exit 2
fi

rm -rf "$work"
mkdir -p "$work"
work=$(CDPATH='' cd -- "$work" && pwd)
raws=$(CDPATH='' cd -- "$raws" && pwd)

have_exiftool=no
command -v exiftool >/dev/null 2>&1 && have_exiftool=yes
have_python=no   # with the packages the picture comparisons need
"$python" -c 'import numpy, scipy, cv2, rawpy' >/dev/null 2>&1 && have_python=yes
any_python=no
"$python" -c 'import sys' >/dev/null 2>&1 && any_python=yes

failures=0
passed() { printf '  ok       %-9s %s\n' "$1" "$2"; }
failed() { printf '  FAILED   %-9s %s\n' "$1" "$2"; failures=$((failures + 1)); }
skipped() { printf '  skipped  %-9s %s\n' "$1" "$2"; }
# True if neither file is newer than the other.
same_time() { [ -z "$(find "$1" -newer "$2")" ] && [ -z "$(find "$2" -newer "$1")" ]; }

echo "dngconv: $("$dngconv" --version | tr '\n' ' ')"
echo "work folder: $work"
echo

# ---- convert ------------------------------------------------------------------
"$dngconv" -e --verify -r -o "$work/dng" "$raws" >"$work/convert.log" 2>"$work/convert.err"
converted=$(grep -c ' -> ' "$work/convert.log")
if [ "$converted" -eq 0 ]; then
    failed convert "nothing was converted; see $work/convert.err"
    exit 1
fi
if grep -q 'verification failed' "$work/convert.err"; then
    failed convert "a DNG does not hold what its source did; see $work/convert.err"
else
    unread=$(grep -c ': cannot \|Unsupported\|not RAW' "$work/convert.err")
    passed convert "$converted files, pixels and embedded originals verified ($unread not readable by the decoder)"
fi

# ---- check --------------------------------------------------------------------
if "$dngconv" check -r "$work/dng" >"$work/check.log" 2>&1 &&
   ! grep -q 'not verified\|DAMAGED' "$work/check.log"; then
    passed check "$(tail -n 1 "$work/check.log")"
else
    failed check "see $work/check.log"
fi

# ---- extract ------------------------------------------------------------------
"$dngconv" extract -r -q -o "$work/restored" "$work/dng" >"$work/extract.log" 2>&1
same=0
different=0
# Every restored file has the name and place its source had.
find "$work/restored" -type f >"$work/restored.list"
while IFS= read -r file; do
    relative=${file#"$work/restored/"}
    if cmp -s "$file" "$raws/$relative"; then same=$((same + 1)); else different=$((different + 1)); fi
done <"$work/restored.list"
if [ "$same" -eq "$converted" ] && [ "$different" -eq 0 ]; then
    passed extract "$same originals restored, identical to the sources"
else
    failed extract "$same identical, $different different, of $converted"
fi

# ---- update -------------------------------------------------------------------
# A copy of a few sources, so that one can be touched.
mkdir -p "$work/batch/src"
head -n 3 "$work/restored.list" | while IFS= read -r file; do cp "$file" "$work/batch/src/"; done
"$dngconv" -u -o "$work/batch/out" "$work/batch/src" >"$work/update1.log" 2>&1
"$dngconv" -u -o "$work/batch/out" "$work/batch/src" >"$work/update2.log" 2>&1
first=$(find "$work/batch/src" -type f | sort | head -n 1)
sleep 1
touch "$first"
"$dngconv" -u -o "$work/batch/out" "$work/batch/src" >"$work/update3.log" 2>&1
count=$(find "$work/batch/src" -type f | wc -l | tr -d ' ')
if grep -q "^0 converted, $count already up to date, 0 failed" "$work/update2.log" &&
   grep -q "^1 converted, $((count - 1)) already up to date, 0 failed" "$work/update3.log"; then
    passed update "second run converted nothing; after touching one source, that one only"
else
    failed update "see $work/update2.log and update3.log"
fi

# ---- times --------------------------------------------------------------------
touch -t 202001021530.00 "$first"
"$dngconv" -q -t -e -o "$work/batch/timed.dng" "$first" >"$work/times.log" 2>&1
"$dngconv" extract -q -t -o "$work/batch/timed.restored" "$work/batch/timed.dng" >>"$work/times.log" 2>&1
if [ -f "$work/batch/timed.dng" ] && [ -f "$work/batch/timed.restored" ] &&
   same_time "$work/batch/timed.dng" "$first" && same_time "$work/batch/timed.restored" "$first"; then
    passed times "DNG and restored original carry the source's time"
else
    failed times "see $work/times.log"
fi

# ---- sidecar ------------------------------------------------------------------
if [ "$have_exiftool" = yes ]; then
    sidecar="${first%.*}.xmp"
    exiftool -q -o "$sidecar" -XMP-dc:Subject=dngconv-verify -XMP-xmp:Label=Checked "$first" >"$work/sidecar.log" 2>&1
    "$dngconv" -q -f -o "$work/batch/sidecar.dng" "$first" >>"$work/sidecar.log" 2>&1
    "$dngconv" -q -f --no-sidecar -o "$work/batch/nosidecar.dng" "$first" >>"$work/sidecar.log" 2>&1
    with=$(exiftool -s3 -XMP-dc:Subject "$work/batch/sidecar.dng" 2>/dev/null)
    without=$(exiftool -s3 -XMP-dc:Subject "$work/batch/nosidecar.dng" 2>/dev/null)
    if [ "$with" = dngconv-verify ] && [ "$without" != dngconv-verify ]; then
        passed sidecar "the sidecar's keyword is in the DNG, and is not with --no-sidecar"
    else
        failed sidecar "keyword read back: '$with' / '$without'; see $work/sidecar.log"
    fi
    rm -f "$sidecar"
else
    skipped sidecar "ExifTool not found"
fi

# ---- ExifTool's validation -------------------------------------------------------
if [ "$have_exiftool" = yes ]; then
    find "$work/dng" -name '*.dng' -exec exiftool -validate -error -s3 -q {} + >"$work/exiftool.log" 2>&1
    if grep -qi 'error' "$work/exiftool.log"; then
        failed exiftool "see $work/exiftool.log"
    else
        passed exiftool "no error in $converted DNGs"
    fi
else
    skipped exiftool "ExifTool not found"
fi

# ---- Adobe's validator -----------------------------------------------------------
if [ -n "$validator" ] && [ -x "$validator" ]; then
    : >"$work/adobe.log"
    bad=0
    find "$work/dng" -name '*.dng' | sort >"$work/dng.list"
    while IFS= read -r file; do
        report=$("$validator" "$file" 2>&1)
        printf '%s\n' "$report" >>"$work/adobe.log"
        case $report in
            *'*** Error'*) bad=$((bad + 1)) ;;
            *'Validation complete'*) ;;
            *) bad=$((bad + 1)) ;;   # the validator did not get to the end
        esac
    done <"$work/dng.list"
    if [ "$bad" -eq 0 ]; then
        passed adobe "dng_validate reads all $converted DNGs without an error (warnings: $(grep -c '\*\*\* Warning' "$work/adobe.log"), see adobe.log)"
    else
        failed adobe "$bad DNGs with errors; see $work/adobe.log"
    fi
else
    skipped adobe "dng_validate not found (tools/dng-validate/build.sh builds it)"
fi

# ---- same picture ----------------------------------------------------------------
find "$raws" -type d | sort >"$work/dirs.list"
if [ "$have_python" = yes ]; then
    # The script pairs files by name in one folder each; do it per source folder.
    : >"$work/render.log"
    status=0
    while IFS= read -r dir; do
        relative=${dir#"$raws"}
        [ -d "$work/dng$relative" ] || continue
        "$python" "$here/compare_render.py" "$dir" "$work/dng$relative" >>"$work/render.log" 2>/dev/null || status=1
    done <"$work/dirs.list"
    same=$(grep -c 'same picture\|same raw data' "$work/render.log")
    if [ "$status" -eq 0 ] && [ "$same" -gt 0 ]; then
        passed render "$same pairs develop to the same picture (notes: $(grep -c 'note:' "$work/render.log"), sources LibRaw could not develop: $(grep -c 'skipped, LibRaw' "$work/render.log"); see render.log)"
    else
        failed render "see $work/render.log"
    fi
else
    skipped render "Python with numpy, scipy, opencv and rawpy not found (tools/requirements.txt)"
fi

# ---- metadata report -------------------------------------------------------------
if [ "$have_exiftool" = yes ] && [ "$any_python" = yes ]; then
    : >"$work/metadata.txt"
    while IFS= read -r dir; do
        relative=${dir#"$raws"}
        [ -d "$work/dng$relative" ] || continue
        "$python" "$here/compare_metadata.py" "$dir" "$work/dng$relative" >>"$work/metadata.txt" 2>&1
    done <"$work/dirs.list"
    passed metadata "report written to $work/metadata.txt (for reading; nothing is judged)"
else
    skipped metadata "needs ExifTool and Python"
fi

# ---- lens corrections ------------------------------------------------------------
if [ "$have_python" = yes ] && [ "$have_exiftool" = yes ] && [ -n "$validator" ] && [ -x "$validator" ]; then
    grep 'lens: distortion' "$work/convert.log" >"$work/lens.list" || true
    if [ -s "$work/lens.list" ]; then
        : >"$work/lens.log"
        good=0
        bad=0
        while IFS= read -r line; do
            source=${line%% -> *}
            rest=${line#* -> }
            dng=${rest%%  (*}
            if DNG_VALIDATE="$validator" "$python" "$here/compare_camera_jpeg.py" "$source" "$dng" >>"$work/lens.log" 2>/dev/null; then
                good=$((good + 1))
            else
                bad=$((bad + 1))
            fi
        done <"$work/lens.list"
        if [ "$bad" -eq 0 ]; then
            passed lens "$good files with a distortion opcode match their camera JPEG (see lens.log)"
        else
            failed lens "$bad of $((good + bad)) files differ from their camera JPEG; see $work/lens.log"
        fi
    else
        skipped lens "no file with a distortion opcode among the samples"
    fi
else
    skipped lens "needs dng_validate, ExifTool and Python with opencv"
fi

# ---- camera profiles -------------------------------------------------------------
profiles=${PROFILES:-}
if [ -z "$profiles" ] || [ ! -d "$profiles" ]; then
    skipped profiles "set PROFILES to a folder of camera profiles (.dcp) to run this step"
elif [ "$any_python" != yes ]; then
    skipped profiles "Python not found"
else
    "$dngconv" --profile "$profiles" -r -o "$work/profiled" "$raws" >"$work/profiles.log" 2>"$work/profiles.err"
    grep ', profile: ' "$work/profiles.log" >"$work/profiles.list" || true
    without=$(grep -c 'profiles\{0,1\} given is for' "$work/profiles.err")
    if [ -s "$work/profiles.list" ]; then
        : >"$work/profiles.cmp"
        good=0
        bad=0
        while IFS= read -r line; do
            rest=${line#* -> }
            dng=${rest%%  (*}
            ok=yes
            "$python" "$here/compare_profile.py" "$profiles" "$dng" >>"$work/profiles.cmp" 2>&1 || ok=no
            if [ -n "$validator" ] && [ -x "$validator" ]; then
                report=$("$validator" "$dng" 2>&1)
                case $report in
                    *'*** Error'* | *'Unable to parse extra camera profile'*) ok=no ;;
                    *'Validation complete'*) ;;
                    *) ok=no ;;
                esac
                [ "$ok" = yes ] || printf '%s\n' "$report" >>"$work/profiles.cmp"
            fi
            if [ "$ok" = yes ]; then good=$((good + 1)); else bad=$((bad + 1)); fi
        done <"$work/profiles.list"
        if [ "$bad" -eq 0 ]; then
            passed profiles "$good DNGs carry the profiles made for their camera, value for value ($without files from cameras the folder has none for; see profiles.cmp)"
        else
            failed profiles "$bad of $((good + bad)) DNGs; see $work/profiles.cmp"
        fi
    else
        failed profiles "no profile in the folder fits any of the cameras; see $work/profiles.err"
    fi
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "all steps that ran passed"
    exit 0
fi
echo "$failures step(s) failed"
exit 1
