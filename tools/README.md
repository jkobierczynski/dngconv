# Verification tools

The unit tests under `tests/` need no camera files and run everywhere. What
is here is the other half: the checks that need real raw files and other
people's software, the ones the claims in the main README rest on. With a
folder of raw files anyone can repeat them.

Nothing in this folder is needed to build or use dngconv.

## One command

```sh
tools/verify.sh ~/raw-samples
```

converts every raw file below the folder and checks the result in every way
there is a tool for:

```
  ok       convert   15 files, pixels and embedded originals verified (1 not readable by the decoder)
  ok       check     15 checked: 15 intact, 0 damaged
  ok       extract   15 originals restored, identical to the sources
  ok       update    second run converted nothing; after touching one source, that one only
  ok       content   touched sources not redone; the one with a changed byte redone, whatever its date; ...
  ok       checksums 15 DNGs: 'check --add' writes the checksums the converter wrote, and dng_validate accepts them
  ok       times     DNG and restored original carry the source's time
  ok       sidecar   the sidecar's keyword is in the DNG, and is not with --no-sidecar
  ok       exiftool  no error in 15 DNGs
  ok       adobe     dng_validate reads all 15 DNGs without an error (warnings: 3, see adobe.log)
  ok       render    14 pairs develop to the same picture (notes: 2, ...; see render.log)
  ok       metadata  report written to .../metadata.txt (for reading; nothing is judged)
  ok       lens      4 files with a distortion opcode match their camera JPEG (see lens.log)
  ok       preview   12 rendered previews have the geometry of their camera JPEG; 3 more decode ...
  ok       jxl       15 files read back by dngconv; Adobe's reader gets the same raw data from all 15 ...
  ok       lossy     14 files at distance 1 within coding error by dngconv's judgement and, read by Adobe's ...
  ok       profiles  9 DNGs carry the profiles made for their camera, value for value (...)

all steps that ran passed
```

The samples are not written to. The work folder (second argument, default
`verify-work`) receives the DNGs, the restored originals and a log per
step. A step whose tools are missing is skipped and says so; the exit status
is 0 if no step that ran failed.

| Step | Proves | Needs |
|---|---|---|
| convert | Every sample of the frame, and the embedded original, read back from the DNG equal the source (`--verify`) | dngconv |
| check | Each DNG agrees with its own checksums | dngconv |
| extract | The originals come back byte for byte | dngconv, `cmp` |
| update | `--update` skips what is done and redoes what changed | dngconv |
| content | `--update-by content` goes by what is in the files: sources with new dates are not redone, one with a byte more under an old date is, and a DNG made without the note of its source is left as it is | dngconv |
| checksums | With the checksums taken out of a copy of each DNG, `check --add` writes back exactly those the converter had written, changes nothing else in the file, and Adobe's reader accepts the result | Python; `dng_validate` if present |
| times | `--keep-times` carries the date through conversion and extraction | dngconv |
| sidecar | An XMP sidecar ends up in the DNG, and `--no-sidecar` prevents it | ExifTool |
| exiftool | ExifTool's validation finds no error | ExifTool |
| adobe | Adobe's reference reader decodes each file, recomputes the image digest and parses the opcodes | `dng_validate` |
| render | Source and DNG develop to the same picture: pattern, levels, white balance and matrix are described correctly | Python, rawpy |
| metadata | A report of which tags survive, for reading | ExifTool, Python |
| lens | The DNG, developed with its lens opcodes, has the geometry of the camera's own JPEG | `dng_validate`, ExifTool, Python, OpenCV |
| preview | Converted again with `--preview render`, every preview decodes as a JPEG and, where the raw file holds a camera JPEG, has its geometry | ExifTool, Python, OpenCV |
| jxl | Converted again with `-c jxl`, every sample is read back, and Adobe's reader gets the same raw data out of each file as out of its lossless-JPEG twin. Runs only if dngconv was built with libjxl; the second half needs the 1.7 validator | dngconv with libjxl; `dng_validate` 1.7, Python, OpenCV |
| lossy | Converted again with `--jxl-distance` (1, or `LOSSY_DISTANCE`), every file passes `--verify`'s rule and `check`; with the 1.7 validator, Adobe's reader arrives at the lossless file's linear values within the same limits | dngconv with libjxl; `dng_validate` 1.7, Python, OpenCV |
| profiles | Converted again with `--profile`, each DNG holds the profiles of its camera exactly as the `.dcp` files have them, and Adobe's reader parses them. Runs only when `PROFILES` names a folder of profiles | Python; `dng_validate` if present |

Environment variables: `LOSSY_DISTANCE` (for the lossy step, default 1), `DNGCONV` (the program under test; default
`build/dngconv`), `DNG_VALIDATE`, `PYTHON`, and `PROFILES` (a folder of
camera profiles for the last step; RawTherapee's `rtdata/dcpprofiles` is a
freely available one).

## Setting up

```sh
pip install -r tools/requirements.txt       # numpy, scipy, OpenCV, rawpy, Pillow
sudo apt install libimage-exiftool-perl     # ExifTool
tools/dng-validate/build.sh ~/dng-validate  # Adobe's validator, see below
# or, to check JPEG XL files as well:  build.sh --sdk 1.7 ~/dng-validate
export DNG_VALIDATE=~/dng-validate/dng_validate
```

### Adobe's validator

`dng-validate/build.sh` downloads Adobe's DNG SDK and builds
`dng_validate` from it: version 1.5.1 by default, which takes a few minutes
and reads everything but JPEG XL, or with `--sdk 1.7` version 1.7.1, which
reads JPEG XL too and takes ten to twenty minutes because the libjxl that
comes with the SDK is built first. The SDK is Adobe's and under Adobe's own licence;
none of it is in this repository. The script takes it from the
[raw2dng](https://github.com/Fimagena/raw2dng) project (1.5.1) and from
[hfiguiere/dng_sdk](https://github.com/hfiguiere/dng_sdk) (1.7.1), each
pinned to one commit, and compiles every source file of the SDK into one program together
with the XMP SDK that comes with it. It needs git, CMake, a C++ compiler and
the development packages of libjpeg, zlib and expat, and has been run on
Linux only.

Useful on its own:

```sh
dng_validate file.dng               # read everything, report errors and warnings
dng_validate -v file.dng            # ... and print every tag and opcode it parses
dng_validate -tif out file.dng      # develop the picture, opcodes applied, to out.tif
dng_validate -3 out file.dng        # the linear image after the opcodes, 16 bit
dng_validate -dng out file.dng      # read and write again, as Adobe would
dng_validate -1 out file.dng        # the raw image as read from the file
dng_validate -losslessJXL -dng out file.dng   # 1.7 only: rewrite with JPEG XL
```

`-dng` is how the `check` command was tested against a second writer: the
files it produces carry Adobe's lossless JPEG streams and Adobe's digests.

## The scripts one by one

Each prints its own explanation with `--help`.

**`compare_render.py RAW_FOLDER DNG_FOLDER`** develops each pair with LibRaw
under identical settings and compares the pictures value by value. Three
things are pinned so that the comparison is fair, and they are worth knowing
when comparing renders by other means:

- LibRaw's automatic white-point adjustment is off on both sides;
- the source is developed with the DNG's white level, passed as white minus
  black, because LibRaw applies it after black subtraction;
- the source is developed with the DNG's black level, and, if the pictures
  still differ, both are developed again without colour conversion.

The last point exists because the LibRaw inside rawpy is rarely the version
dngconv was built with, and LibRaw's camera table changes: for the Canon
EOS R8, version 0.21.2 and 0.22 have different colour matrices and black
levels one count apart. The script then says "same raw data and levels" and
notes what differs, instead of calling the DNG wrong.

**`compare_metadata.py RAW_FOLDER DNG_FOLDER [-v]`** reads both files with
ExifTool and counts, for the maker note (through each of its two copies),
the EXIF directory and the GPS directory, how many of the source's tags are
in the DNG with the same value. A report, not a test: a few differences are
intended and are listed in the script's help.

**`compare_camera_jpeg.py RAW_FILE DNG_FILE`** is the test the lens
corrections rest on. The camera's JPEG inside the raw file has the camera's
own correction applied. The script develops the DNG with Adobe's code,
opcodes included, matches a few thousand points between the two pictures and
reports what a scale and a shift cannot explain. Run on a DNG converted with
`--lens-corrections none` it shows the lens's distortion; with the opcodes
it should show a pixel or less, and no trend from centre to corner.

**`measure_fringes.py RAW_FILE [DNG_FILE]`** measures lateral chromatic
aberration on the sensor's colour planes and, given the DNG, prints next to
it what the DNG's opcode will remove. It needs a detailed picture taken at
low ISO.

**`matching.py`** is the point matching the two lens scripts share.

**`compare_raw_data.py DNG_A DNG_B`** has Adobe's validator write out the
raw image it reads from each of the two files (decompressed, rows and
columns back in place, nothing else done) and compares them sample for
sample. With a lossless-JPEG DNG and a JPEG XL DNG of the same raw file it
shows that the reference reader gets the same data from both, which LibRaw
cannot show because it does not read JPEG XL.

**`compare_checksums.py DNG_FILE...`** tests `dngconv check --add` against
checksums somebody else computed. It copies each DNG, takes the checksums
(`RawImageDigest`, `NewRawImageDigest`, `OriginalRawFileDigest`) out of the
copy, has dngconv write them again and compares: they must be the ones the
file's own writer had put there, and apart from the pointer in the header
no byte of the file as it was may have changed. Given DNGs from Adobe's
software, that compares dngconv's arithmetic with Adobe's. A DNG without
checksums, as most cameras write them, has nothing to compare with; it is
given its checksums, and `dng_validate` (`--dng-validate`, or the
`DNG_VALIDATE` variable) is asked whether it accepts them. The files named
are never written to. Options: `--dngconv PATH` (default: the `DNGCONV`
variable, or the PATH).

**`compare_lossy.py LOSSLESS_DNG LOSSY_DNG DISTANCE`** reads both files
with Adobe's validator, which writes the raw image after linearisation and
`OpcodeList2` (where the lossy file's polynomial undoes its curve), and
compares the two: in the coded domain by the limits `--verify` uses (root
mean square, averages of 32 x 32 blocks of one colour), in linear light by
report only. It checks what dngconv's own verification cannot: that the
opcode and levels written make Adobe's reader arrive at the original
values.

**`compare_preview.py RAW_FILE DNG_FILE`** is for DNGs converted with
`--preview render`. It takes the preview out of the DNG, decodes it, and
compares it with the camera's own JPEG from the raw file: matched points for
the geometry (which includes the lens corrections applied to the preview),
brightness and colour over a grid for the tones. Geometry is judged; tones
are reported, because every camera has its own.

**`compare_profile.py PROFILE_FOLDER DNG_FILE...`** reads the camera profile
tags of each DNG, and each of its extra profile blocks, with a few lines of
its own TIFF parsing, and looks for a `.dcp` file in the folder with the same
tags, types and values. Byte order is evened out; the camera name inside the
`.dcp` is not compared, since dngconv does not embed it.

## What these tools do not replace

- Opening the files in Lightroom or Camera Raw.
- Running the converter on macOS and Windows: the scripts are POSIX shell
  and Python and should work there, but have only been run on Linux.
- Judgement. `metadata.txt`, the notes in `render.log` and the warnings in
  `adobe.log` are meant to be read.
