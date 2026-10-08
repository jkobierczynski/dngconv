# Development notes

Where dngconv stands, how it got there, and what comes next. The README says
what the program does; this file records the progress and the reasons behind
the decisions, so that work can be picked up again without rediscovering them.

Last updated: 2026-10-09, at version 0.2.0.

## Status at a glance

| Area | State |
|---|---|
| Raw decoding (through LibRaw) | Working: Bayer, X-Trans, full-colour raws |
| DNG writing (own TIFF writer, lossless JPEG) | Working, both byte orders |
| Pixel fidelity | Bit-exact on all 15 sample files |
| Colour (matrix, white balance, levels) | Working, one illuminant (D65) |
| EXIF and GPS copy | Working for TIFF-based raws, CR3, RAF, RW2 |
| Maker notes | Copied, stored twice; verified with ExifTool and exiv2 |
| Embedded preview | Camera's JPEG copied into IFD0 |
| Defect pixels | Flagged with a DNG opcode (Panasonic and similar) |
| Linux build, GCC and Clang | Tested |
| LibRaw 0.21.2 and 0.22.0 | Both tested |
| macOS and Windows builds | Written, **never run** |
| Release workflow (binaries on a version tag) | Written and linted; Linux leg simulated locally; **never run on GitHub** |
| Adobe software (Lightroom, Camera Raw, `dng_validate`) | **Never tried** |
| Git history, published releases | None yet |

## History

### 0.1.0 (2026-10-08): first working converter

Built in this order:

1. **Survey.** A probe program printed what LibRaw reports for 16 sample
   files (geometry, filter pattern, levels, matrices, thumbnails). That
   settled the data model before any writer code existed.
2. **TIFF writer** (`tiff_writer`): nested directories, strips and tiles,
   rational conversion.
3. **Lossless JPEG encoder** (`ljpeg92`): predictor 1, one optimised Huffman
   table per component, tested against a decoder written separately for the
   tests.
4. **Reader** (`raw_reader`): LibRaw to the `RawImage` model.
5. **DNG writer** (`dng_writer`) and **command line** (`main`).
6. **Verification** on real files, which led to the fixes below.

Problems found while verifying, and what was done:

| Finding | Resolution |
|---|---|
| LibRaw scrambles a lossless-JPEG tile that is wider than the frame (it wraps rows at the frame width). Showed up on a 120-pixel test frame. | Tiles are never larger than the frame. An odd frame width below the tile size falls back to single-component coding. |
| Panasonic file: 50 pixels rendered differently. The camera stores 0 for dead pixels and LibRaw repairs them only for the original format. | `OpcodeList1` with `FixBadPixelsConstant`. darktable repairs them exactly as it does on the original; LibRaw-based readers ignore opcodes. |
| LibRaw normalises maker and model ("Nikon" / "D3S"), which breaks matching in other software. | Read `Make` and `Model` from the source file itself. `UniqueCameraModel` keeps the normalised form. |
| Matrix values came out as fractions like 0.8293010753. | Rational conversion prefers short decimals, then continued fractions: 0.8293 is 8293/10000, 1/30 stays 1/30. |
| Unknown lens apertures were written as 0/1 ("f/0"). | Written as 0/0, the TIFF convention for unknown. |
| The error callback has a different signature in LibRaw 0.22 (64-bit offset). Found by building against 0.22.0. | Template callback that fits either. |
| The Canon EOS R8 gets a different active area from LibRaw 0.21.2 (5999 x 3999) and 0.22.0 (6022 x 4024). | Nothing to fix: the samples are the same. Documented. |

Two things that looked like bugs and were not. Both were mistakes in the test
harness, worth remembering when comparing renders:

- LibRaw's automatic maximum adjustment has to be switched off on both sides
  (`adjust_maximum_thr=0`), or the two renders use different white points.
- `user_sat` is applied **after** black subtraction, so it must be given as
  white level minus black level.

### 0.2.0 (2026-10-08): EXIF, GPS and maker notes

1. **Source parser** (`source_metadata`, replacing the maker/model-only
   `source_identity`): reads directories from TIFF-based raws, the `CMT1` to
   `CMT4` blocks of a CR3, and the EXIF segment of the JPEG embedded in RAF
   and RW2 files. Values are converted to little-endian on the way in.
2. **Generic field copy** in the TIFF writer (`setRaw`), all twelve field
   types.
3. **Pinned values** in the TIFF writer (`setPinned`): a value can ask for a
   fixed file offset; the layout steps around it.
4. **Maker note** written as the EXIF `MakerNote` tag at its original offset
   and again in `DNGPrivateData`.
5. **Big-endian output**, chosen automatically when the maker note comes from
   a big-endian source.
6. **`--no-maker-notes`**.
7. **New test program** for the source parser, plus both byte orders and the
   metadata copy in the existing ones.

Problems found while verifying, and what was done:

| Finding | Resolution |
|---|---|
| Panasonic RW2: only 14 EXIF tags and no maker note. The full directory lives in the JPEG stored in IFD0 tag 0x2e. | Follow that JPEG and merge what the file's own directory lacks. |
| ExifTool: "Non-standard format (undef)" for `DNGPrivateData`. | The tag is type BYTE, not UNDEFINED. |
| exiv2 read four Nikon tags wrongly (white balance levels, directory number) from a little-endian DNG, although the note has its own big-endian header. exiv2 decodes some binary tables in the byte order of the surrounding file. | Write the DNG in the source's byte order. This is why the TIFF writer learned big-endian. |
| A pinned offset beyond the end of the output would pad the file with zeros up to that point. | A pin is honoured only if it lies inside the file's natural size. |
| LibRaw gives up with an I/O error when a maker note is the last thing in the file and parsing runs past the end. Seen with a random test note. | Follows from the previous fix: a pinned note is always inside the file, an unpinned one sits before the image data. |
| LibRaw reports "." as a Samsung body serial. | Serials without a letter or digit are dropped. |
| exiv2 prints a line such as `Directory Canon with 25665 entries considered invalid` for most DNGs that carry a maker note. It reads `DNGPrivateData` as a bare maker note; 25665 is the "Ad" of "Adobe". | Left as is; harmless, and the EXIF copy is read correctly. See open questions. |
| ExifTool misreads the private-data copy of the Samsung note. | Open. The EXIF copy of the same note decodes correctly. |

### Since 0.2.0 (2026-10-09): release workflow

- This file.
- `.github/workflows/release.yml`: on a version tag, builds self-contained
  binaries for Linux, macOS and Windows and publishes them as a GitHub
  release. See [Releases](#releases).
- `ci.yml` moved to `actions/checkout@v5`; the v4 line runs on Node 20, which
  GitHub is retiring from its runners.

## Design decisions

**LibRaw for decoding, own code for writing.** LibRaw covers the cameras; the
Adobe DNG SDK is not under an open-source licence, and libtiff has no DNG
knowledge to speak of. Writing TIFF and lossless JPEG ourselves cost about 800
lines and keeps LibRaw the only dependency.

**`RawImage` as the only meeting point.** The reader fills it, the writer
consumes it, neither includes the other. That is what allows the tests to
write synthetic frames without any camera file, and it leaves room for a
second decoder or a library interface later.

**Store the whole sensor frame.** Masked borders are kept and described with
`ActiveArea`, instead of cropping to what LibRaw calls the visible area.
Nothing the camera recorded is thrown away, and a later LibRaw with a better
idea of the active area only changes a tag.

**Camera values before decoder values.** For Make, Model and every EXIF tag,
what the camera wrote wins; LibRaw's interpretation only fills gaps.

**White level: the camera's own, when lower than the format maximum.** Too
low costs a little highlight headroom, too high gives magenta highlights. The
conservative side was chosen. It makes some DNGs render slightly brighter than
software that assumes the format maximum.

**Sixteen bits per sample, always.** Uncompressed output would otherwise need
bit packing; in lossless JPEG the declared precision costs nothing.

**Tiles of 512 pixels, two interleaved components for a mosaic.** Each sample
is then predicted from the previous one of the same colour. Tiles compress in
parallel and bound the damage of a corrupt region.

**Maker note at its original offset.** Many maker notes (Canon, Sony,
Panasonic) hold pointers counted from the start of the file. Rewriting those
pointers needs per-maker knowledge; putting the note back where it was needs
none and works for every reader. The file layout bends around it instead.

**Maker note stored twice.** The EXIF copy serves ExifTool, exiv2 and
everything built on them. The `DNGPrivateData` copy is the form Adobe's
converter writes and the one most likely to be read by Adobe software, and it
survives a rewrite of the file. The second claim is an assumption until tested.

**No patching of unsafe fields.** `CFAPattern`, `OECF`,
`SpatialFrequencyResponse` and `DeviceSettingDescription` have an inner layout
that depends on the file's byte order. They are left out rather than
converted.

**Never overwrite, write through a temporary file.** A converter people point
at their only copy of a photo archive should fail safe.

## How it is verified

Sample files: 16 raws from 10 makers, taken from the test data of the rawpy
project and from the metadata-extractor-images collection. They are not part
of this repository and their licences were not checked for redistribution.
One is damaged (Canon EOS 400D; it converts, with a warning), one is not
readable with the distribution's LibRaw (Sigma X3F).

| Check | Tool | What it proves |
|---|---|---|
| Unit tests (4 programs) | CTest | Encoder against a reference decoder, TIFF layout byte by byte, source parser on hand-built and damaged files, DNG round trips through LibRaw in both byte orders |
| `--verify` on every sample | LibRaw 0.21.2 and 0.22.0 | Every sample of the frame survives |
| Render comparison | rawpy (LibRaw 0.22.1) | Source and DNG develop to the same 8-bit picture, so levels, matrix, white balance and pattern agree |
| Structure | ExifTool 12.76 `-validate` | No complaints beyond those the source file already has, except two minor warnings about the private-data copy of the Samsung maker note |
| Independent decoders | darktable 4.6.1 (rawspeed), RawTherapee 5.10 | The files open elsewhere, with the right colours and orientation, including big-endian and uncompressed ones |
| Metadata comparison | ExifTool 12.76, exiv2 0.27.6 | Each maker-note, EXIF and GPS tag has the same value in source and DNG |
| Memory and undefined behaviour | GCC 13 ASan and UBSan | Clean on the tests (leak detection on), on full conversions, and on truncated and byte-flipped copies of real files |
| Second compiler | Clang 18 | No warnings at `-Wall -Wextra -Wpedantic` |

The comparison scripts were throwaway Python and are not in the repository.
The render comparison develops both files with camera white balance, linear
demosaicing, no auto-brightness and the two LibRaw settings noted in the 0.1.0
history, then compares the arrays. The metadata comparison runs
`exiftool -a -G5 -s -u` on both files and compares the sets of tag and value,
separating the EXIF copy of the maker note from the private-data copy by
their directory path.

Not covered by any check so far:

- Adobe software and Adobe's `dng_validate`.
- macOS and Windows, including Unicode file names on Windows.
- Four-colour sensors (CMYG, RGBE), monochrome sensors, non-square pixels,
  multi-frame files: the code paths exist and are untested on real files.
- XMP in a source file: none of the samples has any.
- Output above 4 GiB, which is refused by design.

## Open work

Suggested order:

1. **Try the files in Adobe software.** One hour with Lightroom or a build of
   the DNG SDK's `dng_validate` could reorder everything below. It also
   settles whether the private-data copy of the maker note earns its place.
2. **Put the project on GitHub and run both workflows.** `ci.yml` and
   `release.yml` exist and have never executed there. Start `release.yml` by
   hand first ("Run workflow"), which builds the packages without publishing
   anything, and tag only once that is green.
3. **Standard preview layout and digests.** Small uncompressed thumbnail in
   IFD0 with the JPEG preview in a sub-directory, plus `RawDataUniqueID` and
   `NewRawImageDigest`.
4. **Embed the original raw** (`OriginalRawFileData`) and an `extract`
   command, so conversion becomes reversible. Needs zlib.
5. **Maker data outside the maker note.** Sony `SR2Private`, which Adobe
   stores as its own block in `DNGPrivateData`; the Canon CR3 timed-metadata
   track.
6. **Lens corrections as opcodes** (`WarpRectilinear`, `FixVignetteRadial`)
   for the mirrorless makers that store them in maker notes. Large: needs
   per-maker parsing.
7. **Bring the comparison scripts into the repository** as a `tools/` folder,
   so the checks above can be repeated by anyone with sample files.
8. Smaller items: parallel conversion of several files, keeping file
   timestamps, picking up XMP sidecars, a `--byte-order` switch, detecting an
   input that already is a DNG before decoding it.
9. Further out: JPEG XL compression (DNG 1.7), Fuji Super CCD, floating-point
   raws, a second calibration illuminant, a library interface.

## Open questions

- **Is the private-data copy read by Adobe software, and is it needed?** If
  Lightroom takes lens data from the EXIF copy, the second copy could become
  optional and the exiv2 message would go away.
- **Samsung maker note in `DNGPrivateData`.** ExifTool decodes it wrongly.
  Either the original-offset convention differs for notes whose pointers are
  relative to the note itself, or it is a limitation in ExifTool. A DNG made
  by Adobe's converter from a Samsung file would tell.
- **`MakerNoteSafety`.** Not written, which means "unsafe to preserve". That
  is the cautious answer for notes with absolute pointers, but it tells a
  rewriting program to drop the EXIF copy.
- **White level policy.** The camera-reported level is used when lower than
  the format maximum. Whether that matches what users expect in practice needs
  more files with blown highlights.
- **Name.** `dngconv` is a working name.

## Working on the code

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

A sanitizer build, worth running after any change to a parser or to the
layout code:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Against a self-built LibRaw, to check a second version:

```sh
cmake -S . -B build22 -DLIBRAW_INCLUDE_DIR=/path/to/LibRaw \
      "-DLIBRAW_LIBRARY=/path/to/LibRaw/lib/libraw_r.a;z"
```

After a change, with sample files at hand:

```sh
build/dngconv --verify -o /tmp/out/ /path/to/samples     # must report no failure
exiftool -validate -warning -error -a /tmp/out/*.dng
```

## Releases

A release is made by pushing a tag that matches the version in
`CMakeLists.txt`:

```sh
# after setting project(dngconv VERSION x.y.z ...) and committing
git tag vX.Y.Z
git push origin vX.Y.Z
```

`release.yml` then:

1. checks that the tag and the CMake version agree (`v0.2.0` and `v0.2.0-rc1`
   both fit version 0.2.0; a tag with a suffix becomes a pre-release) and
   creates a **draft** release;
2. builds on three runners. LibRaw and what it needs (zlib, Little CMS,
   JasPer) are compiled from source by vcpkg and linked statically, so the
   binaries run without anything installed;
3. runs the tests and `dngconv --version` on each runner;
4. packs `dngconv`, `README.md`, `LICENSE` and the licences of the linked
   libraries into one archive per platform, with a SHA-256 file next to it;
5. publishes the release only if all three builds succeeded. A failed build
   leaves the draft unpublished.

| Package | Built on | Notes |
|---|---|---|
| `dngconv-x.y.z-linux-x86_64.tar.gz` | `ubuntu-24.04` | Built against glibc 2.39, so it needs a distribution of that age or newer; the C++ runtime is linked in |
| `dngconv-x.y.z-macos-arm64.tar.gz` | `macos-15` | Apple Silicon, macOS 15 or newer; not signed or notarised |
| `dngconv-x.y.z-windows-x64.zip` | `windows-2025` | Static C runtime, no redistributable needed; not signed |

Choices made in the workflow, and what to revisit:

- **vcpkg is pinned** to one commit (`VCPKG_COMMIT` at the top of the file),
  which at the time of writing provides LibRaw 0.22.2. Camera support in the
  released binaries follows that pin; move it forward to pick up a newer
  LibRaw.
- **Runner images are pinned** rather than `-latest`, so the minimum system
  requirements above do not shift when GitHub moves the `-latest` labels.
- **Release-only vcpkg triplets** (`x64-linux-release` and so on) halve the
  build time. They are community triplets.
- **No dependency cache.** Each release compiles LibRaw from scratch, a few
  minutes per platform. Not worth more moving parts for something that runs a
  few times a year.
- **No Intel Mac build.** It could be added as a fourth matrix entry with the
  `x64-osx-release` triplet on an Intel runner.
- Unsigned macOS and Windows binaries trigger the usual first-run warnings
  (Gatekeeper, SmartScreen).

What has and has not been tried: the workflow passes `actionlint` and
`shellcheck`; the version check was run against matching and non-matching
tags; the Linux build flags and the packaging step were run locally against a
statically built LibRaw 0.22.0. The vcpkg steps, the macOS and Windows legs
and everything involving the GitHub release itself have **not** been run. The
vcpkg port was read to confirm the CMake targets (`libraw::raw_r`) and that it
handles static linking on Windows, but the first real run may still need
adjustments.

Conventions used so far:

- C++17, no extensions, warnings clean on GCC and Clang.
- Errors travel as exceptions inside the library and become one line on
  stderr in `main`. The source-metadata parser is the exception: it never
  throws and returns what it could read.
- Every multi-byte value handed to `TiffIfd` is little-endian; only
  `TiffWriter::write` knows the byte order of the file.
- Tests are plain programs with counted checks (`tests/test_util.hpp`), no
  framework, and need no camera files.
- Each source file starts with its SPDX licence line and a comment saying
  what the file is for.
