# dngconv

A command-line converter from camera raw files to Adobe's Digital Negative
(DNG) format, written in C++17.

Decoding is done by [LibRaw](https://www.libraw.org/), so every camera LibRaw
knows can be read. The DNG file itself is written by dngconv's own code: no
Adobe DNG SDK, no libtiff, no libjpeg. The only dependencies are LibRaw and
zlib.

Status: **0.7.0, early**. It works on the cameras listed under
[What has been tested](#what-has-been-tested); expect rough edges elsewhere.

## What it does

- Stores the sensor data **unchanged**: the complete frame, masked borders
  included, with the active area, default crop, black and white levels
  described in tags. No demosaicing, no white balance, no scaling.
- Lossless JPEG compression in tiles (the standard DNG scheme), compressed on
  all CPU cores, or uncompressed output.
- Bayer sensors, Fuji X-Trans, and full-colour raws such as Canon sRAW.
- Colour: calibrated matrix (D65) and as-shot white balance, or the **camera
  profiles** you choose (`--profile`): DCP files such as Adobe Standard, the
  camera-look profiles or one made from a colour chart are stored in the DNG,
  matched to each picture's camera. See [Camera profiles](#camera-profiles).
- Metadata: the camera's complete EXIF and GPS directories and its maker
  note are carried over, read directly from the source file. Maker and model
  keep the camera's own spelling.
- Lens corrections: the distortion, chromatic aberration and vignetting
  parameters that Sony, Fujifilm, Panasonic and Olympus cameras record are
  turned into DNG opcodes, so a DNG reader straightens the picture the way
  the camera's own JPEG is. See [Lens corrections](#lens-corrections).
- Previews in the standard DNG arrangement: a small thumbnail rendered from
  the raw data in the first image directory, where file browsers look, and
  the camera's own full-size JPEG as a second preview.
- Fingerprints (`NewRawImageDigest`, `RawDataUniqueID`), so a reader can
  detect damaged raw data and recognise the same exposure again.
- Optionally stores the **original camera file** inside the DNG
  (`--embed-original`), and `dngconv extract` gets it back byte for byte, so
  a conversion can be undone.
- `--verify` re-reads each DNG and compares every sample with the source.
- `dngconv check` tells, years later, whether a DNG is still intact: it
  recomputes the checksums of the raw data and of the embedded original.
  It works on DNGs from other converters too.
- Made for whole folders: several files are converted at once, `--update`
  skips what is already done, `--keep-times` gives each DNG the date of its
  source, and an XMP sidecar next to a raw file (ratings, keywords, edits)
  is taken into the DNG.
- Writes through a temporary file, so an interrupted run leaves no half DNG.

## Download

[Tagged releases on GitHub](https://github.com/jkobierczynski/dngconv/releases)
carry ready-made binaries for Linux (x86_64), macOS
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
dngconv -r -u -t -o ~/dng ~/photos    # the same again later: only what is new
dngconv --verify -o safe.dng shot.ARW # convert, then prove nothing was lost
dngconv -e -o archive/ *.CR3          # keep the camera file inside each DNG
dngconv --lens-corrections none *.ARW # leave the lens corrections out
dngconv --profile ~/profiles *.CR3    # embed the camera profiles that fit
dngconv -i shot.RAF                   # show what the decoder finds
```

```
  -o, --output <path>       output file (one input) or output folder
  -c, --compression <mode>  lossless (default) or none
      --no-preview          do not copy the embedded JPEG preview
      --no-maker-notes      do not copy the camera maker's private metadata
  -e, --embed-original      store the source file inside the DNG, so that
                            'dngconv extract' can restore it later
      --lens-corrections <which>
                            pass the camera's lens corrections on to the
                            DNG reader: auto (default; those the camera was
                            set to apply), all, or none
      --no-sidecar          do not take over an XMP sidecar file (IMG.xmp or
                            IMG.CR3.xmp next to IMG.CR3)
      --profile <path>      embed camera profiles: a .dcp file, or a folder
                            that is searched for them. Each picture gets
                            the profiles made for its camera, the first
                            given as its default. May be repeated
      --profile-any-camera  embed the profiles whatever camera they name
  -r, --recursive           descend into sub-folders
  -f, --force               overwrite existing DNG files
  -u, --update              skip files whose DNG exists and is not older than
                            the source; convert the others
  -t, --keep-times          give each DNG the modification time of its source
      --verify              re-read each DNG and compare it with the source
                            (pixels, and the embedded original if any)
  -j, --jobs <n>            threads in total (default: all cores)
  -p, --parallel <n>        files converted at the same time (default: up to
                            4, as far as the threads allow)
  -i, --info                show what the decoder finds; write nothing
  -q, --quiet               only report problems
  -V, --version             show version information
```

Existing files are never overwritten without `--force`. The exit status is 0
when everything converted, 1 when at least one file failed, 2 for a usage
error.

### Working through a folder

**Only what is new.** Without `--update`, a DNG that already exists is an
error for that file (`--force` overwrites). With `--update` (`-u`) a file is
skipped when its DNG exists and is at least as new as the source, converted
when there is no DNG yet, and converted again when the source has changed
since:

```
$ dngconv -r -u -o ~/dng ~/photos
3 converted, 4812 already up to date, 0 failed
```

**Dates.** A new DNG normally carries the time of its conversion.
`--keep-times` (`-t`) gives it the modification time of the raw file instead,
which keeps a folder sorted by date in order. `dngconv extract -t` does the
same for a restored original, using the DNG's time.

**Sidecars.** Programs that must not change a raw file keep ratings,
keywords and edits in an XMP file next to it. dngconv looks for `IMG_0001.xmp`
(Adobe's programs and most others) and then `IMG_0001.CR3.xmp` (darktable)
next to `IMG_0001.CR3`, and stores what it finds as the DNG's XMP metadata,
which is where a DNG keeps such things. A sidecar takes precedence over XMP
inside the camera file. With a sidecar, "the time of the source" above means
the later of the two times, so a rating changed after conversion makes
`--update` convert the file again. `--no-sidecar` switches all of this off.

**Several files at once.** With more than one file to do, up to four are
worked on at the same time, each using its share of the threads; the lines
of the report still come in the order of the files. `-j` sets the number of
threads in total, `-p` the number of files at once. Every file being
converted is held in memory in full (some hundred megabytes for a large
sensor, twice that with `-e`), which is why the default stops at four.
`check` and `extract` take the same two options.

Two sources that would produce the same DNG (`a/IMG.CR3` and `b/IMG.CR3`
sent to one folder, or `IMG.CR2` next to `IMG.NEF`) are not left to
overwrite each other: the first is converted, the second refused with a
message.

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
  -t, --keep-times          give each restored file the modification time of
                            its DNG
  -j, --jobs <n>            threads in total (default: all cores)
  -p, --parallel <n>        files worked on at the same time (default: up to 4)
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

### Checking that files are still intact

A DNG written by dngconv carries a checksum of its raw image data
(`NewRawImageDigest`) and, with `-e`, one of the embedded original.
`dngconv check` recomputes both from what is in the file now:

```sh
dngconv check shot.dng                # one file
dngconv check -r ~/dng                # a whole archive
dngconv check -q -r ~/dng             # only say what is not intact
```

```
  -r, --recursive           descend into sub-folders
  -j, --jobs <n>            threads in total (default: all cores)
  -p, --parallel <n>        files checked at the same time (default: up to 4)
  -q, --quiet               only report files that are not intact
```

```
2019/IMG_0412.dng: intact (raw image, embedded original)
2019/IMG_0413.dng: DAMAGED: the raw image data does not match its checksum
2019/IMG_0414.dng: DAMAGED: the file is cut short: 300000 bytes are missing at its end
2019/L1000123.dng: not verified: the file carries no checksum of its raw data
4 checked: 1 intact, 2 damaged, 1 not verified
```

The exit status is 0 when no file is damaged, 1 when one is or could not be
read, 2 for a usage error. A file without a checksum counts as neither.

It works on DNGs from other converters and from cameras. Files made by
Adobe's software carry the same checksum (or its predecessor,
`RawImageDigest`, which is understood too); most cameras that write DNG do
not record one, and such files can only be checked for being complete.

What it covers, and what it cannot:

- The raw image is decoded by dngconv's own code, not by LibRaw, and hashed
  the way Adobe's SDK defines. A change of a single bit in the picture is
  found.
- The embedded original is compared with its checksum and unpacked.
- Previews, thumbnail and metadata have no checksum in the DNG format. For
  those the check can only tell whether the file is long enough to hold
  everything its directories point to, which catches a file that was cut
  short but not a changed byte.
- The unused part of the last tiles (they are always stored at full size) is
  not part of the picture, so a change there goes unreported; Adobe's
  validator treats it the same.
- Raw data stored as lossy JPEG, JPEG XL, Deflate or floating point is not
  decoded; those files are reported as "not verified".

## What has been tested

Each file below was converted and then checked in up to eight ways:

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

7. **Lens corrections** (the five files that carry them): the DNG was
   developed by Adobe's code and by darktable, with the opcodes applied, and
   the result laid over the JPEG the camera itself made of the same shot. See
   [Lens corrections](#lens-corrections) for the numbers.

8. **Camera profiles** (the nine files whose camera has a profile among
   RawTherapee's open ones): every profile tag in the DNG equals the one in
   the `.dcp` file, and Adobe's code develops the DNG exactly as it develops
   a reference in which ExifTool wrote the same profile. See
   [Camera profiles](#camera-profiles).

| Camera | Format | Sensor data | Source | DNG | Rendering difference |
|---|---|---|---:|---:|---|
| Canon EOS R8 | CR3 | Bayer | 35.9 MB | 34.7 MB | none, see "What LibRaw decides" |
| Canon EOS 5D Mark II | CR2 | Bayer | 25.2 MB | 24.1 MB | none |
| Canon EOS 40D | CR2 sRAW | full colour | 6.5 MB | 8.0 MB | none |
| Fujifilm X-E1 | RAF | X-Trans | 24.9 MB | 19.3 MB | none |
| Fujifilm FinePix S5500 | RAF | Bayer | 8.1 MB | 5.8 MB | none |
| Kodak DC50 | KDC | Bayer | 0.1 MB | 0.6 MB | none |
| Nikon D3S | NEF | Bayer | 10.2 MB | 9.3 MB | none |
| Nikon D4 | NEF | Bayer | 16.4 MB | 15.3 MB | none |
| Olympus E-M10 | ORF | Bayer | 13.8 MB | 15.9 MB | none |
| Panasonic DMC-GF7 | RW2 | Bayer | 18.8 MB | 18.9 MB | 50 defect pixels, see below |
| Pentax K-1 Mark II | PEF | Bayer | 41.9 MB | 38.7 MB | none |
| Samsung NX3000 | SRW | Bayer | 26.2 MB | 25.4 MB | none |
| Sony ILCE-7M3 | ARW | Bayer | 23.6 MB | 26.8 MB | none |
| Sony NEX-5N | ARW | Bayer | 16.3 MB | 20.8 MB | none |

Sizes include the thumbnail (about 130 KB, uncompressed), the camera's JPEG
preview and the maker note, which is stored twice (see
[Metadata](#metadata)). A DNG can be larger than its source when the
camera uses lossy compression (Sony ARW), when decoding yields more samples
than the camera stored (Canon sRAW keeps colour at half resolution), or when
the maker note is large: the Olympus one is 1.4 MB because it contains a
preview image.

The scripts behind these checks are in `tools/`; `tools/verify.sh` runs them
all on a folder of raw files (see `tools/README.md`).

Checked with LibRaw 0.21.2 and 0.22.0, GCC 13 and Clang 18 on Linux, and under
AddressSanitizer and UndefinedBehaviorSanitizer, including extraction from
DNGs that were truncated or overwritten with random bytes. The macOS and
Windows builds are compiled and run through the unit tests by the GitHub
workflows; no camera file has been converted on those systems. Adobe's
validator is the reference reader, but the files have not been opened in
Lightroom or Camera Raw themselves. Two things about embedded originals are untested for
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

## Lens corrections

Many mirrorless lenses are designed to be corrected in software: the camera
straightens its JPEGs and records, in every raw file, the numbers it used.
dngconv reads those numbers and writes them as DNG opcodes (`WarpRectilinear`
and `FixVignetteRadial` in `OpcodeList3`). The raw data is not touched; a
reader that honours the opcodes applies them after demosaicing.

| Maker | Where the camera keeps the parameters | Distortion | Chromatic aberration | Vignetting |
|---|---|---|---|---|
| Sony (ARW) | tags next to the raw image; in bodies before 2012, the scrambled SR2 block | yes | yes | already applied by the camera, see below |
| Fujifilm (RAF, X-Trans) | a directory in front of the raw data | yes | yes | yes |
| Panasonic (RW2) | the `DistortionInfo` tag | yes | no | no |
| Olympus (ORF) | the maker note | yes | yes | no |

Files of other makers (Canon, Nikon, Pentax, Samsung and the rest) are
converted as before, without lens opcodes.

`--lens-corrections` chooses what is written:

- `auto` (the default): the corrections the camera itself was set to apply,
  so the DNG renders like the camera's JPEG. A Sony body with distortion
  compensation switched off gets no distortion opcode.
- `all`: every correction the file has parameters for.
- `none`: no lens opcodes.

`dngconv -i` shows what a file holds and what would be written:

```
  lens data     Sony: distortion (off in the camera), chromatic aberration, vignetting (already applied to the raw data by the camera)
  corrections   chromatic aberration (geometry within 0.02 px)
```

**How good is it.** For each of the five sample files that carry parameters,
the DNG was developed with the opcodes applied, once by Adobe's DNG SDK and
once by darktable 4.6, and matched point by point against the JPEG the
camera stored in the same file. The table gives the median distance between
matched points, in pixels of the full-size picture; without the opcodes it
is what the lens's distortion makes it.

| Camera, lens | Without opcodes | With opcodes, Adobe SDK | With opcodes, darktable |
|---|---:|---:|---:|
| Sony A7 III, FE 24-105mm F4 at 24 mm | 9.3 px | 0.9 px | 0.8 px |
| Sony NEX-5N, E 24mm F1.8 (distortion off in the camera) | 0.3 px | 0.3 px | 0.4 px |
| Fujifilm X-E1, XF 18-55mm | 6.2 px | 0.9 px | 0.9 px |
| Panasonic GF7, 12-32mm at 12 mm | 15.8 px | 0.6 px | 0.7 px |
| Olympus E-M10, 14-42mm EZ at 42 mm | 5.2 px | 0.4 px | 0.4 px |

The remaining half pixel to one pixel is the accuracy of the comparison (the
camera JPEGs are previews of 1616 to 3200 pixels width). The picture is also
framed like the camera's: the scale agrees to 0.05 % or better.

What the figures do not cover:

- **Chromatic aberration** was checked on the sensor data itself. The colour
  fringes measured there match the camera's parameters in direction and size
  for the Olympus (within 0.1 px) and the Sony NEX-5N (within 0.35 px, at
  the corners). For the Fuji the direction matches and the measured size is
  smaller than the table says; for the Sony A7 III (a night shot) the
  measurement is too noisy to tell.
- **Fuji vignetting** is written as the table gives it. In the sample it is
  7 % at the corners, too little to confirm against the JPEG.
- The newer Fuji table layout (nine knots, X-Trans IV and later) and Fuji's
  cropped shooting modes are implemented from the description in darktable's
  source and have not seen a real file here.

**Sony and vignetting.** When "shading compensation" is on, a Sony camera
multiplies the raw data itself with the gain before storing it; the table in
the file then only records what was done. Applying it again would brighten
the corners twice, so dngconv does not write it. (In the A7 III sample the
noise in the corners is higher by exactly the table's factor, and in both
Sony samples the camera's JPEG is as bright in the corners as the raw data
without any further gain.) With shading compensation off, the table is only
used with `--lens-corrections all`.

**Which readers apply them.** Adobe's DNG SDK, on which Lightroom and Camera
Raw are built, applies opcodes whenever it renders a file. darktable (tried
with 4.6) offers them in its lens correction module under "embedded
metadata". Programs built on LibRaw, and RawTherapee 5.10, ignore them and
show the picture uncorrected, as they would the original raw file. The
thumbnail in the DNG is rendered without the corrections.

**Accuracy of the conversion.** The cameras use tables and formulas of their
own; DNG wants polynomials. The fit is reported for every file (`-i`, see
above) and stayed below 0.4 pixels and 0.5 % brightness on the samples.

## Camera profiles

A DNG tells its reader how the camera sees colour with a *camera profile*.
Left alone, dngconv writes the plainest one there is: a single matrix for
daylight, from LibRaw's camera table. Raw developers can do better with a
full profile: matrices for two light sources, hue and saturation tables, a
tone curve. "Adobe Standard" is such a profile, so are the ones that imitate
a camera's picture styles, and so is a profile made from a photograph of a
colour chart. They are files with the extension `.dcp`, and `--profile`
stores them in the DNG, where they travel with the picture:

```sh
dngconv --profile "Canon EOS R8 Adobe Standard.dcp" IMG_0036.CR3
dngconv --profile ~/dcp/standard --profile ~/dcp/looks -r -o ~/dng ~/photos
dngconv -i --profile ~/dcp IMG_0036.CR3    # which profiles this file would get
dngconv -i "Canon EOS R8 Adobe Standard.dcp"   # what a profile file holds
```

**Which picture gets which profile.** Every profile names the camera it was
made for, and each picture gets the profiles made for its camera; a profile
that names none fits all. Case, spaces and punctuation are ignored in the
comparison ("OLYMPUS E-M10" is "Olympus E-M10"). A folder holding profiles
for many cameras can therefore be given for a mixed batch. A file from a
camera without a profile is converted as before, and dngconv says so.

**Default and alternatives.** The first profile that fits, in the order the
options were given and by path within a folder, becomes *the* profile of the
DNG: it takes the place of the decoder's matrix and is what a raw developer
uses unless told otherwise. The others are stored next to it
(`ExtraCameraProfiles`) for the developer to offer in its profile menu. Two
profiles with the same name cannot both be offered; the first is kept.

**When the names differ.** LibRaw and the maker of a profile do not always
spell a camera the same way, and some cameras are sold under two names.
`-i` shows both spellings: the `camera` line of a raw file, the `for camera`
line of a profile. `--profile-any-camera` embeds the profiles given whatever
they name. Use it with files from one camera only: a profile for another
sensor gives wrong colours, and nothing in the file will say why.

**What is respected.** A profile marked "never embed" by its maker is left
out, with a note. Copyright notice and embed policy are carried along. A
profile for a different kind of sensor (four colours instead of three) is
left out as well.

**Where profiles come from.** Adobe's are installed with its free DNG
Converter and with Camera Raw, under `CameraRaw/CameraProfiles` in
`C:\ProgramData\Adobe` or `/Library/Application Support/Adobe`; they are
Adobe's, under Adobe's terms. [RawTherapee](https://rawtherapee.com/) ships
open ones for some 160 cameras (`rtdata/dcpprofiles` in its source). The
free [dcamprof](https://torger.se/anders/dcamprof.html) and the software
that comes with colour charts make profiles for your own camera and light.
None are included here.

**What was checked.** For nine sample files and RawTherapee's profiles:
every profile tag read back from the DNG with ExifTool equals the one in the
`.dcp` file; Adobe's `dng_validate` develops the DNG to exactly the picture
it develops from a reference made the other way round (the same DNG without
a profile, the profile's tags then written into it by ExifTool), for four
cameras, pixel for pixel; extra profiles are parsed by Adobe's reader, in
little- and big-endian files, and are still there after its writer has
rewritten the DNG. `tools/compare_profile.py` repeats the first comparison
without ExifTool.

**What to expect.**

- Not seen in Lightroom or Camera Raw: that the profiles appear in the menu
  there is what the format promises and what Adobe's reader code does, but
  nobody has looked.
- Adobe's own profiles are tuned for DNGs from Adobe's converter, which
  carry a per-camera brightness offset (`BaselineExposure`) that dngconv
  does not know. With them a picture can come out somewhat lighter or
  darker than from an Adobe-made DNG.
- Software built on LibRaw or rawspeed (darktable, RawTherapee) reads the
  matrices and ignores tables and curves in a DNG. RawTherapee can load the
  `.dcp` file itself.
- The thumbnail is rendered with the decoder's matrix, as before.
- Profiles add their size to every file: from a few kilobytes for matrices
  alone to about a megabyte with a look table. Ten of the latter are 10 MB
  per picture.

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

**Colour.** Without `--profile` there is one colour matrix, for daylight
(D65), taken from LibRaw's camera table. Adobe's converter ships two matrices
per camera plus tone and look profiles; colours in Adobe software will
therefore not be identical to those of an Adobe-made DNG. Embedding the
profile you want closes most of that gap, see
[Camera profiles](#camera-profiles). For cameras missing from LibRaw's table the matrix
is derived from LibRaw's built-in camera-to-sRGB matrix, and dngconv says so.

**What LibRaw decides.** Active area, black level and the colour matrix
come from LibRaw, so a DNG depends on the LibRaw version dngconv was built
with. The Canon EOS R8 file shows all three: version 0.21.2 gives an active
area of 5999 x 3999, a black level of 2048 and one colour matrix, version
0.22 gives 6022 x 4024, 2047 and a different, newer matrix. The stored
samples are the same either way, and a DNG develops exactly like its source
when both are read by the same LibRaw; read by another version, the colours
of this camera differ slightly. The released binaries are built with the
newer LibRaw.

## Not there yet

- Maker-specific data outside the maker note (see [Metadata](#metadata)).
- Lens corrections for makers other than the four listed under
  [Lens corrections](#lens-corrections), and vignetting for Panasonic and
  Olympus.
- Fuji Super CCD sensors with the diagonal layout, floating-point raws, and
  multi-frame files beyond their first frame (pixel shift, dual exposure).
- Sigma X3F works only if your LibRaw was built with X3F support; most
  distribution packages are not.
- A second calibration illuminant of dngconv's own (a profile given with
  `--profile` brings its own), DNG 1.6/1.7 features (JPEG XL compression
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
| `src/lens_correction.hpp` | Lens corrections as curves over the radius, independent of the maker |
| `src/lens_data.*` | Reads each maker's lens-correction parameters into those curves |
| `src/lens_opcodes.*` | Fits the curves to DNG's polynomials and builds the opcode list |
| `src/camera_profile.*` | Reads camera profiles (`.dcp`), picks those that fit a camera, packs them for the DNG |
| `src/thumbnail.*` | Renders the small RGB thumbnail from the raw data |
| `src/ljpeg92.*`, `src/ljpeg92_decode.cpp` | Lossless JPEG encoder, and a decoder for checking files |
| `src/md5.*` | MD5, for the DNG fingerprints |
| `src/dng_digest.*` | The two raw image digests DNG defines |
| `src/dng_check.*` | Reads a DNG's raw data as stored and compares the file with its checksums |
| `src/parallel.hpp` | Runs a job list on all cores |
| `src/tiff_writer.*` | Minimal TIFF container writer, either byte order, with values pinned to a file offset and blocks addressed by offset |
| `src/sidecar.*` | Finds and reads XMP sidecar files |
| `src/job_runner.hpp` | Runs jobs on several threads and keeps their reports in order |
| `src/main.cpp` | Command line: conversion, `extract` and `check` |
| `tests/` | Lossless JPEG against a reference decoder, TIFF layout, the source-metadata parser on hand-built and damaged files, MD5, digests and thumbnail colours, embedded originals (layout, hand-built and damaged data, hostile file names), lens corrections (fit, opcode bytes, each maker's parser on hand-built and damaged files), the check (decoder against hand-built streams with every predictor, digests against independent values, verdicts on damaged files), the job runner and sidecars, camera profiles (hand-built sound and broken files in both byte orders, the choice per camera, the tags and blocks in the DNG), the program itself run on generated DNGs, DNG round trips on synthetic frames |

The reader and the writer only meet in `RawImage`, so either side can be
replaced or reused on its own. The tests need no camera files.

`tools/` holds the scripts that check the converter against real camera
files and other software; they are not needed to build or use it.

Development history and design decisions are in `DEVELOPMENT.md`, open work
in `TODO.md`.

## Licence

GPL-3.0-or-later, see `LICENSE`.

LibRaw is used under the LGPL 2.1, zlib under the zlib licence. Camera
profiles are not part of dngconv; a profile you embed keeps the licence its
maker gave it. DNG is a format published by Adobe; this
project is not affiliated with or endorsed by Adobe.
