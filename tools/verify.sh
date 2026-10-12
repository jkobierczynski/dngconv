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
#   content      dngconv --update-by content      touched files not redone; a changed one is; a DNG
#                                                 that does not name its source is left alone
#   checksums    compare_checksums.py             (Python) checksums taken out of each DNG and written
#                                                 again by 'check --add' are the same; with
#                                                 dng_validate, Adobe's reader accepts them
#   times        dngconv -t, extract -t           dates carried over
#   sidecar      dngconv with an .xmp             (ExifTool) keywords arrive in the DNG
#   exiftool     exiftool -validate               (ExifTool) no error in any DNG
#   adobe        dng_validate                     (dng_validate) Adobe's reader accepts every DNG
#   render       compare_render.py                (Python) source and DNG develop to the same picture
#   metadata     compare_metadata.py              (ExifTool, Python) report only, see metadata.txt
#   lens         compare_camera_jpeg.py           (dng_validate, Python) corrected geometry matches
#                                                 the camera's JPEG, for files with a distortion opcode
#   preview      dngconv --preview render,        (ExifTool, Python) every rendered preview decodes and
#                compare_preview.py               has the geometry of the camera's JPEG
#   jxl          dngconv -c jxl --verify,         (a dngconv with JPEG XL) samples read back; with
#                compare_raw_data.py              dng_validate 1.7 and Python, Adobe's reader gets the
#                                                 same raw data as from the lossless-JPEG DNG
#   lossy        dngconv --jxl-distance --verify, (a dngconv with JPEG XL) every file within coding error;
#                compare_lossy.py                 with dng_validate 1.7 and Python, Adobe's reader gets
#                                                 the lossless values back within coding error too
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
#   LOSSY_DISTANCE  the distance for the lossy step (default 1)
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

# ---- update by content --------------------------------------------------------
mkdir -p "$work/content/src"
head -n 3 "$work/restored.list" | while IFS= read -r file; do cp "$file" "$work/content/src/"; done
"$dngconv" -q -o "$work/content/out" "$work/content/src" >"$work/content0.log" 2>&1
# Every source is now newer than its DNG: by time, all would be redone.
sleep 1
find "$work/content/src" -type f -exec touch {} +
"$dngconv" --update-by content -o "$work/content/out" "$work/content/src" >"$work/content1.log" 2>&1
# One more byte at the end of one source (decoders do not mind), under an old date.
changed=$(find "$work/content/src" -type f | sort | head -n 1)
printf 'x' >>"$changed"
find "$work/content/src" -type f -exec touch -t 200101010000.00 {} +
"$dngconv" --update-by content -o "$work/content/out" "$work/content/src" >"$work/content2.log" 2>&1
"$dngconv" --update-by content -o "$work/content/out" "$work/content/src" >"$work/content3.log" 2>&1
# A DNG that does not say what it was made from is not written over.
"$dngconv" -q -f --no-source-record -o "$work/content/out" "$changed" >"$work/content4.log" 2>&1
plain=$(cksum <"$work/content/out/$(basename "${changed%.*}").dng")
"$dngconv" --update-by content -o "$work/content/out" "$work/content/src" >>"$work/content4.log" 2>&1
if grep -q "^0 converted, $count already up to date, 0 failed" "$work/content1.log" &&
   grep -q "^1 converted, $((count - 1)) already up to date, 0 failed" "$work/content2.log" &&
   grep -q ', source changed' "$work/content2.log" &&
   grep -q "^0 converted, $count already up to date, 0 failed" "$work/content3.log" &&
   grep -q "^0 converted, $((count - 1)) already up to date, 1 left as it is" "$work/content4.log" &&
   [ "$plain" = "$(cksum <"$work/content/out/$(basename "${changed%.*}").dng")" ]; then
    passed content "touched sources not redone; the one with a changed byte redone, whatever its date; a DNG without a record of its source left alone"
else
    failed content "see $work/content1.log to content4.log"
fi

# ---- checksums written by check --add -------------------------------------------
if [ "$any_python" = yes ]; then
    find "$work/dng" -name '*.dng' | sort >"$work/checksums.list"
    : >"$work/checksums.log"
    bad=0
    while IFS= read -r file; do
        if [ -n "$validator" ] && [ -x "$validator" ]; then
            "$python" "$here/compare_checksums.py" --dngconv "$dngconv" --dng-validate "$validator" "$file" >>"$work/checksums.log" 2>&1 || bad=$((bad + 1))
        else
            "$python" "$here/compare_checksums.py" --dngconv "$dngconv" --dng-validate "" "$file" >>"$work/checksums.log" 2>&1 || bad=$((bad + 1))
        fi
    done <"$work/checksums.list"
    same=$(grep -c "the same as the writer's" "$work/checksums.log")
    if [ "$bad" -eq 0 ] && [ "$same" -eq "$converted" ]; then
        if grep -q 'accepted by dng_validate' "$work/checksums.log"; then
            passed checksums "$same DNGs: 'check --add' writes the checksums the converter wrote, and dng_validate accepts them"
        else
            passed checksums "$same DNGs: 'check --add' writes the checksums the converter wrote (no second opinion: dng_validate not found)"
        fi
    else
        failed checksums "$same of $converted the same; see $work/checksums.log"
    fi
else
    skipped checksums "Python not found"
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

# ---- rendered previews -----------------------------------------------------------
if [ "$have_python" = yes ] && [ "$have_exiftool" = yes ]; then
    "$dngconv" --preview render -r -q -o "$work/rendered" "$raws" >"$work/preview.err" 2>&1
    : >"$work/preview.log"
    good=0
    bad=0
    alone=0
    grep ' -> ' "$work/convert.log" >"$work/preview.list" || true
    while IFS= read -r line; do
        source=${line%% -> *}
        rest=${line#* -> }
        dng=${rest%%  (*}
        rendered="$work/rendered${dng#"$work/dng"}"
        "$python" "$here/compare_preview.py" "$source" "$rendered" >>"$work/preview.log" 2>&1
        case $? in
            0) good=$((good + 1)) ;;
            2) alone=$((alone + 1)) ;;
            *) bad=$((bad + 1)) ;;
        esac
    done <"$work/preview.list"
    if [ "$bad" -eq 0 ] && [ $((good + alone)) -gt 0 ]; then
        passed preview "$good rendered previews have the geometry of their camera JPEG; $alone more decode but have nothing to be compared with (tones: see preview.log)"
    else
        failed preview "$bad of $((good + bad + alone)) previews; see $work/preview.log"
    fi
else
    skipped preview "needs ExifTool and Python with opencv"
fi

# ---- JPEG XL ---------------------------------------------------------------------
if "$dngconv" --version | grep -q 'JPEG XL: libjxl'; then
    "$dngconv" -c jxl --verify -r -o "$work/jxl" "$raws" >"$work/jxl.log" 2>"$work/jxl.err"
    written=$(grep -c ' -> ' "$work/jxl.log")
    if grep -q 'verification failed' "$work/jxl.err" || [ "$written" -ne "$converted" ]; then
        failed jxl "$written of $converted converted and read back; see $work/jxl.err"
    elif ! "$dngconv" check -r -q "$work/jxl" >"$work/jxl.check" 2>&1; then
        failed jxl "the check command does not find them intact; see $work/jxl.check"
    else
        # A second reader, if there is one that knows the format.
        version=$( { [ -n "$validator" ] && [ -x "$validator" ] && "$validator" 2>&1; } | sed -n 's/.*version \([0-9]*\)\.\([0-9]*\).*/\1\2/p' | head -n 1)
        if [ -n "$version" ] && [ "$version" -ge 17 ] && [ "$have_python" = yes ]; then
            : >"$work/jxl.cmp"
            good=0
            bad=0
            grep ' -> ' "$work/convert.log" >"$work/jxl.list" || true
            while IFS= read -r line; do
                rest=${line#* -> }
                dng=${rest%%  (*}
                if DNG_VALIDATE="$validator" "$python" "$here/compare_raw_data.py" "$work/jxl${dng#"$work/dng"}" "$dng" >>"$work/jxl.cmp" 2>&1; then
                    good=$((good + 1))
                else
                    bad=$((bad + 1))
                fi
            done <"$work/jxl.list"
            if [ "$bad" -eq 0 ]; then
                passed jxl "$written files read back by dngconv; Adobe's reader gets the same raw data from all $good as from the lossless-JPEG DNGs"
            else
                failed jxl "Adobe's reader gets other raw data from $bad of $((good + bad)) files; see $work/jxl.cmp"
            fi
        else
            passed jxl "$written files, samples read back by dngconv and checksums intact (no second reader: that needs dng_validate 1.7 and Python with opencv)"
        fi
    fi
else
    skipped jxl "this dngconv was built without libjxl"
fi

# ---- lossy JPEG XL ----------------------------------------------------------------
distance=${LOSSY_DISTANCE:-1}
if "$dngconv" --version | grep -q 'JPEG XL: libjxl'; then
    "$dngconv" --jxl-distance "$distance" --verify -r -o "$work/lossy" "$raws" >"$work/lossy.log" 2>"$work/lossy.err"
    written=$(grep -c ' -> ' "$work/lossy.log")
    lossy=$(grep -c 'verified within coding error' "$work/lossy.log")
    if grep -q 'verification failed' "$work/lossy.err" || [ "$written" -ne "$converted" ]; then
        failed lossy "$written of $converted converted and judged; see $work/lossy.err"
    elif ! "$dngconv" check -r -q "$work/lossy" >"$work/lossy.check" 2>&1; then
        failed lossy "the check command does not find them intact; see $work/lossy.check"
    else
        version=$( { [ -n "$validator" ] && [ -x "$validator" ] && "$validator" 2>&1; } | sed -n 's/.*version \([0-9]*\)\.\([0-9]*\).*/\1\2/p' | head -n 1)
        if [ -n "$version" ] && [ "$version" -ge 17 ] && [ "$have_python" = yes ] && [ -d "$work/jxl" ]; then
            : >"$work/lossy.cmp"
            good=0
            bad=0
            grep 'verified within coding error' "$work/lossy.log" >"$work/lossy.list" || true
            while IFS= read -r line; do
                rest=${line#* -> }
                dng=${rest%%  (*}
                if DNG_VALIDATE="$validator" "$python" "$here/compare_lossy.py" "$work/jxl${dng#"$work/lossy"}" "$dng" "$distance" >>"$work/lossy.cmp" 2>&1; then
                    good=$((good + 1))
                else
                    bad=$((bad + 1))
                fi
            done <"$work/lossy.list"
            if [ "$bad" -eq 0 ]; then
                passed lossy "$lossy files at distance $distance within coding error by dngconv's judgement and, read by Adobe's reader, against the lossless files ($((written - lossy)) stored losslessly; see lossy.cmp)"
            else
                failed lossy "Adobe's reader gets more than coding error from $bad of $((good + bad)) files; see $work/lossy.cmp"
            fi
        else
            passed lossy "$lossy files at distance $distance within coding error by dngconv's judgement ($((written - lossy)) stored losslessly; no second reader: that needs dng_validate 1.7, Python with opencv, and the jxl step)"
        fi
    fi
else
    skipped lossy "this dngconv was built without libjxl"
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
