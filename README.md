# dngconv

A command-line converter from camera raw files to Adobe's Digital Negative
(DNG) format, written in C++17.

Decoding is done by [LibRaw](https://www.libraw.org/), so every camera LibRaw
knows can be read. The DNG file itself is written by dngconv's own code: no
Adobe DNG SDK, no libtiff, no libjpeg. The only dependencies are LibRaw and
zlib.

Status: **0.4.0, early**. It works on the cameras listed under
[What has been tested](#what-has-been-tested); expect rough edges elsewhere.

## What it does

- Stores the sensor data **unchanged**: the complete frame, masked borders
  included, with the active area, default crop, black and white levels
  described in tags. No demosaicing, no white balance, no scaling.
- Lossless JPEG compression in tiles (the standard DNG scheme), compressed on
  all CPU cores, or uncompressed output.
- Bayer sensors, Fuji X-Trans, and full-colour raws such as Canon sRAW.
- Colour: calibrated matrix (D65) and as-shot white balance.
- Metadata: the camera's complete EXIF and GPS directories and its maker
  note are carried over, read directly from the source file. Maker and model
  keep the camera's own spelling.
- Previews in the standard DNG arrangement: a small thumbnail rendered from
  the raw data in the first image directory, where file browsers look, and
  the camera's own full-size JPEG as a second preview.
- Fingerprints (`NewRawImageDigest`, `RawDataUniqueID`), so a reader can
  detect damaged raw data and recognise the same exposure again.
- Optionally stores the **original camera file** inside the DNG
  (`--embed-original`), and `dngconv extract` gets it back byte for byte, so
  a conversion can be undone.
- `--verify` re-reads each DNG and compares every sample with the source.
- Writes through a temporary file, so an interrupted run leaves no half DNG.

## Download

Tagged releases on GitHub carry ready-made binaries for Linux (x86_64), macOS
(Apple Silicon) and Windows (x64). They are self-contained: LibRaw and zlib
are linked in. Unpack the archive and run `dngconv` from a terminal.

The binaries are built by `.github/workflows/release.yml` whenever a version
tag is pushed; see `DEVELOPMENT.md` for how a release is made.

## Build

Needs CMake 3.16+, a C++17 compiler, LibRaw 0.21 or newer and zlib.

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake libraw-dev zlib1g-dev
# Fedora / RHEL (EPEL)
sudo dnf install gcc-c++ cmake LibRaw-devel zlib-devel
# macOS (zlib comes with the system)
brew install cmake libraw

cmake -S . -B build
cmake --build build
ctest --test-dir build        # optional
sudo cmake --install build    # optional, installs the dngconv binary
```

On Windows, install both through vcpkg
(`vcpkg install libraw:x64-windows zlib:x64-windows`) and pass vcpkg's toolchain file to CMake; see `.github/workflows/ci.yml`.

To build against a LibRaw you compiled yourself:

```sh
cmake -S . -B build -DLIBRAW_INCLUDE_DIR=/path/to/LibRaw \
      "-DLIBRAW_LIBRARY=/path/to/LibRaw/lib/libraw_r.a;z"
```

## Use

```sh
dngconv IMG_0036.CR3                  # writes IMG_0036.dng next to it
dngconv -o out/ *.NEF                 # into a folder
dngconv -r -o ~/dng ~/photos/2026     # a whole tree, keeping its structure
dngconv --verify -o safe.dng shot.ARW # convert, then prove nothing was lost
dngconv -e -o archive/ *.CR3          # keep the camera file inside each DNG
dngconv -i shot.RAF                   # show what the decoder finds
```

```
  -o, --output <path>       output file (one input) or output folder
  -c, --compression <mode>  lossless (default) or none
      --no-preview          do not copy the embedded JPEG preview
      --no-maker-notes      do not copy the camera maker's private metadata
  -e, --embed-original      store the source file inside the DNG, so that
                            'dngconv extract' can restore it later
  -r, --recursive           descend into sub-folders
  -f, --force               overwrite existing DNG files
      --verify              re-read each DNG and compare it with the source
                            (pixels, and the embedded original if any)
  -j, --jobs <n>            compression threads (default: all cores)
  -i, --info                show what the decoder finds; write nothing
  -q, --quiet               only report problems
  -V, --version             show version information
```

Existing files are never overwritten without `--force`. The exit status is 0
when everything converted, 1 when at least one file failed, 2 for a usage
error.

### Keeping and restoring the original

With `-e` the DNG carries a complete copy of the file it was made from, in
the standard `OriginalRawFileData` tag, with a checksum
(`OriginalRawFileDigest`). `dngconv extract` writes that copy out again:

```sh
dngconv extract shot.dng              # writes shot.ARW next to the DNG
dngconv extract -o restored/ *.dng    # into a folder
dngconv extract -r -o ~/raw ~/dng     # a whole tree, keeping its structure
dngconv -i shot.dng                   # says whether an original is inside
```

```
  -o, --output <path>       output file (one input) or output folder
  -r, --recursive           descend into sub-folders
  -f, --force               overwrite existing files
  -q, --quiet               only report problems
```

The restored file gets the name it had before conversion and is identical to
it byte for byte. The checksum is verified first; if it does not match,
nothing is written. A DNG without an embedded original is an error when named
on the command line and is skipped when found while scanning a folder.

Embedding roughly doubles the size of the DNG. Camera files are already
compressed, so the copy shrinks very little: on the samples below it added
between 76 % and 100 % of the source's size. `dngconv -e --verify` unpacks the
copy again after writing and compares it with the source file, which is the
check to run before deleting any original.

The tag is the one Adobe's DNG Converter uses for the same purpose, so
ExifTool can extract the file too
(`exiftool -b -OriginalRawImage shot.dng > shot.ARW`), and `dngconv extract`
is written to read originals embedded by other converters.

## What has been tested

Each file below was converted and then checked six ways:

1. **Bit-exact**: the DNG was decoded again and all samples compared with the
   source (`--verify`).
2. **Same picture**: source and DNG were developed with identical settings
   through LibRaw and the 8-bit results compared pixel by pixel.
3. **Readable elsewhere**: ExifTool's `-validate` reports no problem; darktable
   4.6 (which uses the independent rawspeed decoder) and RawTherapee 5.10
   develop the files with the expected colours and orientation.
4. **Same metadata**: every EXIF, GPS and maker-note tag was read from source
   and DNG with ExifTool and with exiv2 and the values compared; see
   [Metadata](#metadata).
5. **Adobe's reference validator**: `dng_validate` from the DNG SDK 1.5.1
   reads every file without an error. It decodes the raw data with Adobe's
   own code, recomputes the image digest and compares it with the stored one,
   and parses both copies of the maker note. Its only remarks are three
   warnings about values the cameras themselves wrote (an Olympus user
   comment, two Samsung exposure fields).
6. **Reversible**: converted again with `--embed-original`, then restored
   twice, with `dngconv extract` and with ExifTool. Both results are
   identical to the camera file for every sample, and the larger DNGs pass
   checks 1, 3 and 5 as well.

| Camera | Format | Sensor data | Source | DNG | Rendering difference |
|---|---|---|---:|---:|---|
| Canon EOS R8 | CR3 | Bayer | 35.9 MB | 38.0 MB | none (max 1/255) |
| Canon EOS 5D Mark II | CR2 | Bayer | 25.2 MB | 25.3 MB | none |
| Canon EOS 40D | CR2 sRAW | full colour | 6.5 MB | 8.9 MB | none |
| Fujifilm X-E1 | RAF | X-Trans | 24.9 MB | 20.6 MB | none |
| Fujifilm FinePix S5500 | RAF | Bayer | 8.1 MB | 6.5 MB | none |
| Kodak DC50 | KDC | Bayer | 0.1 MB | 0.6 MB | none |
| Nikon D3S | NEF | Bayer | 10.2 MB | 9.7 MB | none |
| Nikon D4 | NEF | Bayer | 16.4 MB | 16.1 MB | none |
| Olympus E-M10 | ORF | Bayer | 13.8 MB | 16.1 MB | none |
| Panasonic DMC-GF7 | RW2 | Bayer | 18.8 MB | 19.1 MB | 50 defect pixels, see below |
| Pentax K-1 Mark II | PEF | Bayer | 41.9 MB | 39.4 MB | none |
| Samsung NX3000 | SRW | Bayer | 26.2 MB | 27.0 MB | none |
| Sony ILCE-7M3 | ARW | Bayer | 23.6 MB | 27.1 MB | none |
| Sony NEX-5N | ARW | Bayer | 16.3 MB | 22.2 MB | none |

Sizes include the thumbnail (about 130 KB, uncompressed), the camera's JPEG
preview and the maker note, which is stored twice (see
[Metadata](#metadata)). A DNG can be larger than its source when the
camera uses lossy compression (Sony ARW) or a more modern lossless coder
(Canon CR3), which DNG's lossless JPEG cannot match, or when the maker note is
large: the Olympus one is 1.4 MB because it contains a preview image.

Checked with LibRaw 0.21.2 and 0.22.0, GCC 13 and Clang 18 on Linux, and under
AddressSanitizer and UndefinedBehaviorSanitizer, including extraction from
DNGs that were truncated or overwritten with random bytes. The macOS and
Windows builds in the CI workflow have not been run yet. Adobe's validator is
the reference reader, but the files have not been opened in Lightroom or
Camera Raw themselves. Two things about embedded originals are untested for
lack of the software: extracting a dngconv-embedded file with Adobe's DNG
Converter, and extracting an original embedded by Adobe's converter with
`dngconv extract`.

## Metadata

dngconv reads the source file's own TIFF structures instead of relying on what
the decoder interprets:

| Source | Where the metadata is found |
|---|---|
| TIFF-based raws (CR2, NEF, ARW, PEF, SRW, ORF, DNG-like) | IFD0, its EXIF and GPS directories |
| Canon CR3 | the `CMT1` to `CMT4` blocks inside the Canon `uuid` box |
| Fuji RAF | the EXIF segment of the embedded JPEG |
| Panasonic RW2 | its own short EXIF directory, completed from the embedded JPEG |

Other containers (Sigma X3F, Minolta MRW, Canon CRW, old Kodak files) fall
back to the values LibRaw reports: exposure, aperture, ISO, focal length,
lens, date, serial number, GPS position.

**EXIF and GPS** directories are copied tag by tag, converted to the DNG's
byte order. Left out are the pointer to the JPEG interoperability directory,
padding, and four rarely used fields whose internal layout depends on the byte
order of the file around them (`CFAPattern`, `OECF`,
`SpatialFrequencyResponse`, `DeviceSettingDescription`). Where the camera's
directory lacks a value LibRaw knows, LibRaw's value is added. From IFD0 only
description, artist, copyright and XMP are taken.

**The maker note** is copied byte for byte and stored twice:

- as the EXIF `MakerNote` tag, placed at the **same file offset** it had in
  the camera file. Many maker notes contain pointers counted from the start of
  the file; keeping the offset keeps them valid for any reader.
- in `DNGPrivateData`, in the block layout the DNG specification documents for
  Adobe's converter, which records the note's byte order and original offset.
  This copy still decodes after a program rewrites the DNG and moves things.

A DNG made from a big-endian source (Nikon NEF, for instance) is written
big-endian. Parts of some maker notes are decoded in the byte order of the
surrounding file, so the DNG has to keep the order the camera used.

Results on the sample files, counting distinct tags ExifTool decodes from the
maker note of the source and finds with the same value in the DNG:

| Camera | Maker-note tags | EXIF directory | GPS |
|---|---:|---:|---|
| Canon EOS R8 (CR3) | 234 of 234 | 38 of 39 | none |
| Canon EOS 5D Mark II | 303 of 303 | 29 of 29 | none |
| Canon EOS 40D | 282 of 282 | 29 of 29 | none |
| Fujifilm X-E1 | 47 of 47 | 40 of 40 | none |
| Fujifilm FinePix S5500 | 22 of 22 | 34 of 34 | none |
| Nikon D3S | 189 of 189 | 30 of 31 | none |
| Nikon D4 | 213 of 213 | 31 of 32 | none |
| Olympus E-M10 | 347 of 347 | 29 of 30 | none |
| Panasonic DMC-GF7 | 145 of 145 | 36 of 36 | none |
| Pentax K-1 Mark II | 204 of 204 | 22 of 23 | 20 of 20 |
| Samsung NX3000 | 58 of 65 | 33 of 33 | none |
| Sony ILCE-7M3 | 178 of 178 | 40 of 40 | none |
| Sony NEX-5N | 132 of 132 | 35 of 35 | none |
| Kodak DC50 | no maker note | no EXIF directory | none |

The one EXIF tag missing in four rows is `CFAPattern`, left out on purpose.
In the Canon EOS R8 row it is `LensSpecification`, which is copied with one
correction: the camera writes 0/1 for an aperture it does not know, where the
EXIF standard asks for 0/0.
The seven Samsung tags belong to a preview directory the maker note points to
but which lies outside it. exiv2 0.27 agrees with the table: every tag it
decodes from a source's maker note has the same value in the DNG.

What is **not** carried over is maker-specific data that the camera stores
outside the maker note:

- Canon CR3: the timed-metadata track (`CTMD`), which holds the colour
  calibration tables.
- Sony ARW: the encrypted `SR2Private` directory (white balance presets,
  correction parameters).
- Fuji RAF and Panasonic RW2: tags of the raw container itself.
- Preview images that a maker note points to but does not contain (Samsung).

Two things readers do with the private-data copy:

- exiv2-based programs (darktable, RawTherapee, digiKam) try to read
  `DNGPrivateData` as a bare maker note, fail, and print a line such as
  `Directory Canon with 25665 entries considered invalid; not read`. It is
  harmless; they then read the EXIF copy correctly.
- ExifTool decodes the private-data copy for every sample except the Samsung
  one, where it reports wrong values and two "bad offset" warnings. The EXIF
  copy of the same note decodes correctly.

`--no-maker-notes` leaves out both copies and writes a little-endian file.
Maker notes hold serial numbers, shutter counts and sometimes owner names;
keep that in mind before sharing files.

## Things worth knowing

**Defect pixels.** Panasonic cameras (and a few others) mark dead pixels by
storing the value 0. dngconv keeps those samples as they are and adds a DNG
opcode (`FixBadPixelsConstant`) asking the reader to interpolate over them.
Readers that implement opcodes repair them; readers that do not (LibRaw-based
ones, for instance) show them as dark dots.

**Thumbnail.** The 256-pixel thumbnail is a quick rendering of the raw data:
block averages instead of demosaicing, the colour matrix, as-shot white
balance, and automatic brightening by at most two stops. It follows the
default crop and, like the raw data, relies on the `Orientation` tag for
rotation. It will not match the camera's JPEG exactly; that one is stored next
to it, unchanged. `--no-preview` leaves out the camera's JPEG but keeps the
thumbnail.

**Default crop.** When the source says which part of the sensor is the
picture, that becomes the default crop. Otherwise the whole active area is
used. Either way a margin of two pixels is kept to the edge of the active
area, which a mosaic needs for interpolation and DNG readers check for.

**Fingerprints.** `NewRawImageDigest` is computed over the stored samples the
way Adobe's SDK defines it, so any DNG reader can verify the raw data.
`RawDataUniqueID` combines it with the camera model, crop and opcodes: two
conversions of the same file get the same ID.

**Embedded originals and other programs.** The copy of the camera file
includes everything in it, maker notes and GPS position too;
`--no-maker-notes` does not reach inside it. Programs that rewrite a DNG may
drop the copy: Adobe's `dng_validate`, for one, does when asked to save the
file again. Adobe's validator also does not check the copy's checksum, so
`dngconv extract` (or `--verify`) is the way to find out whether it is intact.
The format limits an embedded file to 4 GiB. dngconv holds the source file and
its compressed copy in memory while writing, on top of the raw data.

**White level.** When the camera recorded its own saturation level and that
level is lower than the format maximum, dngconv uses the camera's value. This
is the safer choice for highlight recovery, but it can make a DNG render a
little brighter than the same file developed from the original by software
that assumes the format maximum.

**Colour.** There is one colour matrix, for daylight (D65), taken from LibRaw's
camera table. Adobe's converter ships two matrices per camera plus tone and
look profiles; colours in Adobe software will therefore not be identical to
those of an Adobe-made DNG. For cameras missing from LibRaw's table the matrix
is derived from LibRaw's built-in camera-to-sRGB matrix, and dngconv says so.

**What LibRaw decides.** Active area, black level and the camera table come
from LibRaw, so results can differ slightly between LibRaw versions. The Canon
EOS R8 file, for example, gets an active area of 5999 x 3999 from 0.21.2 and
6022 x 4024 from 0.22.0. The stored samples are the same either way.

## Not there yet

- Maker-specific data outside the maker note (see [Metadata](#metadata)), and
  turning the lens corrections found in maker notes into DNG opcodes.
- Fuji Super CCD sensors with the diagonal layout, floating-point raws, and
  multi-frame files beyond their first frame (pixel shift, dual exposure).
- Sigma X3F works only if your LibRaw was built with X3F support; most
  distribution packages are not.
- A second calibration illuminant, DNG 1.6/1.7 features (JPEG XL compression
  among them).
- A JPEG preview for cameras whose files carry none (only the thumbnail is
  written then), and re-rendering previews at a chosen size.

## How the source is organised

| File | Role |
|---|---|
| `src/raw_image.hpp` | Plain data model: one raw frame plus the metadata a DNG needs |
| `src/raw_reader.*` | LibRaw to `RawImage` |
| `src/source_metadata.*` | Reads maker, model, EXIF, GPS and the maker note from the source's own structures (TIFF-based raws, CR3, RAF, RW2) |
| `src/tiff_source.*` | Bounds-checked TIFF reading for files that are not trusted, shared by the metadata reader and `extract` |
| `src/original_raw.*` | Packs the source file for `OriginalRawFileData`, unpacks it, and finds it in a DNG |
| `src/dng_writer.*` | `RawImage` to DNG |
| `src/thumbnail.*` | Renders the small RGB thumbnail from the raw data |
| `src/ljpeg92.*` | Lossless JPEG encoder |
| `src/md5.*` | MD5, for the DNG fingerprints |
| `src/parallel.hpp` | Runs a job list on all cores |
| `src/tiff_writer.*` | Minimal TIFF container writer, either byte order, with values pinned to a file offset |
| `src/main.cpp` | Command line: conversion and `extract` |
| `tests/` | Lossless JPEG against a reference decoder, TIFF layout, the source-metadata parser on hand-built and damaged files, MD5, digests and thumbnail colours, embedded originals (layout, hand-built and damaged data, hostile file names), DNG round trips on synthetic frames |

The reader and the writer only meet in `RawImage`, so either side can be
replaced or reused on its own. The tests need no camera files.

Development history, design decisions and open work are in `DEVELOPMENT.md`.

## Licence

GPL-3.0-or-later, see `LICENSE`.

LibRaw is used under the LGPL 2.1, zlib under the zlib licence. DNG is a format published by Adobe; this
project is not affiliated with or endorsed by Adobe.
