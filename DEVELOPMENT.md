# Development notes

Where dngconv stands and how it got there. The README says what the program
does; this file records the progress and the reasons behind the decisions, so
that work can be picked up again without rediscovering them. What is left to
do is in `TODO.md`.

Last updated: 2026-10-09, at version 0.5.0.

## Status at a glance

| Area | State |
|---|---|
| Raw decoding (through LibRaw) | Working: Bayer, X-Trans, full-colour raws |
| DNG writing (own TIFF writer, lossless JPEG) | Working, both byte orders |
| Pixel fidelity | Bit-exact on all 15 sample files |
| Colour (matrix, white balance, levels) | Working, one illuminant (D65) |
| EXIF and GPS copy | Working for TIFF-based raws, CR3, RAF, RW2 |
| Maker notes | Copied, stored twice; verified with ExifTool and exiv2 |
| Previews | Standard layout: rendered thumbnail in IFD0, raw image and camera JPEG in sub-directories |
| Digests | `NewRawImageDigest` and `RawDataUniqueID`; the former confirmed by Adobe's validator |
| Defect pixels | Flagged with a DNG opcode (Panasonic and similar) |
| Embedded original (`--embed-original`, `extract`) | Working; all 15 samples restored byte-identical, by dngconv and by ExifTool |
| Lens corrections as opcodes | Sony, Fujifilm, Panasonic, Olympus; distortion checked against each camera's own JPEG on five files |
| Linux build, GCC and Clang | Tested |
| LibRaw 0.21.2 and 0.22.0 | Both tested |
| macOS and Windows builds | Build and pass the unit tests on GitHub's runners; no camera file converted there yet |
| Release workflow (binaries on a version tag) | Working: releases 0.2.0 and 0.3.0 were built and published by it |
| Adobe's `dng_validate` (DNG SDK 1.5.1) | All 15 sample DNGs pass without an error, with and without an embedded original |
| Adobe DNG Converter (extracting our originals, and ours extracting theirs) | **Never tried** |
| Lightroom, Camera Raw | **Never tried** |
| Repository | github.com/jkobierczynski/dngconv; 0.4.0 is committed but not tagged |

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
| exiv2 prints a line such as `Directory Canon with 25665 entries considered invalid` for most DNGs that carry a maker note. It reads `DNGPrivateData` as a bare maker note; 25665 is the "Ad" of "Adobe". | Left as is; harmless, and the EXIF copy is read correctly. See the questions in `TODO.md`. |
| ExifTool misreads the private-data copy of the Samsung note. | Open. The EXIF copy of the same note decodes correctly. |

### Since 0.2.0 (2026-10-09): release workflow

- This file.
- `.github/workflows/release.yml`: on a version tag, builds self-contained
  binaries for Linux, macOS and Windows and publishes them as a GitHub
  release. See [Releases](#releases).
- `ci.yml` moved to `actions/checkout@v5`; the v4 line runs on Node 20, which
  GitHub is retiring from its runners.

### 0.3.0 (2026-10-09): preview layout, digests, reference validator

1. **Adobe's validator built from source.** A public repository (raw2dng)
   carries the DNG SDK 1.5.1; `dng_validate` was compiled from it and run on
   the 0.2.0 output before anything was changed. All 15 files passed without
   an error, which was the first confirmation from Adobe's own reader for the
   lossless JPEG encoder, the opcode list and the tag structure.
2. **Thumbnail renderer** (`thumbnail`): block averages per colour plane,
   colour matrix, white balance, sRGB curve, at most two stops of automatic
   brightening.
3. **Standard layout**: thumbnail in IFD0, raw image in the first
   sub-directory, the camera's JPEG in the second.
4. **MD5** (`md5`) and the two fingerprints.
5. **New test program** (`test_preview`): MD5 against RFC 1321, digests
   against values the validator accepted, thumbnail colours against an
   independently computed forward model.

Problems found, and what was done:

| Finding | Resolution |
|---|---|
| The image digest is not what one would guess. The SDK hashes tiles of 256 x 256 pixels, each with its colour planes **one after the other** rather than interleaved, then hashes the tile digests. | Implemented as defined. Proven by the validator on mosaic, big-endian and three-plane files; flipping one bit of a stored digest makes it report a mismatch, so the check is real. |
| Validator: "too little padding" on four edges for six files. It wants two pixels between the default crop and the active area of a mosaic. | The reader keeps a two-pixel margin. |
| Validator: "zero entry in LensSpecification" for the Canon CR3. The camera writes 0/1 for an unknown aperture. | Normalised to 0/0 when the EXIF directory is copied. The one place where a copied value is changed. |
| Thumbnails of night shots came out as grey fog with brightening up to eight times. | Limit of two stops. |
| UBSan: `memcpy` with a null pointer when an empty opcode list was fed to MD5. | Empty updates return at once; covered by a test. |
| One MD5 reference value in the new test was mistyped. | Checked all seven against Python's `hashlib`. |

What the validator's verbose output also settled:

- It reads the layout as intended: IFD0 "Preview Image", sub-directory 1
  "Main Image", sub-directory 2 "Preview Image".
- It parses **both** copies of the maker note: the EXIF one at its original
  offset, and "MakerNote inside DNGPrivateData". That answers the open
  question whether the private-data block is in the form Adobe's code expects.

### 0.4.0 (2026-10-09): embedded original and `extract`

1. **`tiff_source`** split out of `source_metadata`: the bounds-checked TIFF
   reading that the metadata parser already had, now shared with the code
   that looks for an embedded original in a DNG.
2. **`parallel.hpp`** split out of `dng_writer`: the worker loop, now also
   used to compress the blocks of the original.
3. **`original_raw`**: packs a file into the `OriginalRawFileData` layout,
   unpacks it, and finds tag, name and digest in a DNG. First use of zlib.
4. **TIFF writer**: values of 4 MiB or more are placed after the image data.
5. **`-e` / `--embed-original`**, off by default. `--verify` then also unpacks
   the copy from the finished DNG and compares it with the source file.
6. **`dngconv extract`**, and a line in `-i` output for a DNG that holds an
   original.
7. **New test program** (`test_original`), described below.

The layout of `OriginalRawFileData`, since the specification spends one
paragraph on it. Everything is big-endian, whatever the DNG's byte order:

```
fork 1 (the file's contents):
    uint32  length of the uncompressed file
    uint32  offset[blocks + 1]     blocks = ceil(length / 65536); offsets count
                                   from the start of the fork; the first is
                                   4 * (blocks + 2), the last is the fork's end
    bytes   one complete zlib stream per 64 KiB block
forks 2 to 8: uint32 0 each        Mac resource fork, file type, creator, and
                                   the same four again for a .THM sidecar
```

An empty file is a zero length and nothing else, followed by the seven zeros.
`OriginalRawFileDigest` is the MD5 of the tag's bytes as stored, not of the
original file.

Findings, and what was done:

| Finding | Resolution |
|---|---|
| Adobe's validator does **not** check `OriginalRawFileDigest`, and does not unpack the data. A DNG with one bit flipped inside the embedded original validates without a remark. | The validator is no proof for this feature. The independent proof is ExifTool, which implements the same layout on its own: `exiftool -b -OriginalRawImage` returns the camera file byte for byte for all 15 samples. |
| `dng_validate -dng`, which rewrites a file, drops the embedded original. | Documented in the README. Not something dngconv can prevent. |
| The copy hardly compresses: between 76 % and 100 % of the source's size, 94 % or more for 12 of the 15 samples. Raw files are compressed already. | `-e` stays opt-in, and the README says that it doubles the file. |
| The TIFF writer put all tag values before the image data. The copy would have pushed the first tile tens of megabytes into the file and made every pixel offset depend on `-e`. | Large values go after the image data, which also keeps the layout of the first part of the file the same with and without `-e`. |
| A name read from `OriginalRawFileName` decides where `extract` writes. A crafted DNG could name `../../something` or `C:\...`. | The name is cut down to its last component for either separator; control characters and colons are removed, trailing dots and spaces too; `.` and `..` are refused. Without a usable name the output is the DNG's name with `.original` in place of `.dng`. |
| A damaged length field could ask for a 4 GiB allocation from a file of a few kilobytes. | The announced length is checked against what the packed data could possibly hold before anything is allocated; block offsets must increase and stay inside the data; each block must unpack to exactly its expected size. |

What `test_original` covers: pack and unpack for sizes from 0 to 200001 bytes
around the block boundaries; the header layout byte by byte; streams built by
hand in the test, with stored (uncompressed) deflate blocks and its own
Adler-32, so the reader is checked against something our writer did not
produce; truncated, bit-flipped and random input, which must fail with an
error and never crash; a round trip through a real DNG in both byte orders;
that a 5 MiB original lands behind the tiles; that a changed byte is reported
as a digest mismatch; hostile file names; and DNGs without an original.

### Since 0.4.0 (2026-10-09): on GitHub

The project was published at github.com/jkobierczynski/dngconv. Both
workflows ran as written: `ci.yml` on every push, `release.yml` for the tags
`v0.2.0` and `v0.3.0`, each of which produced the three packages with their
checksums. That is the first time the code was compiled and its tests run on
macOS and Windows. `TODO.md` took over the list of open work from this file.

### 0.5.0 (2026-10-09): lens corrections

1. **`lens_correction.hpp`**: a maker-neutral model. Distortion, the two
   chromatic aberration curves and the vignetting gain are each a curve over
   the radius, in units of the picture's half diagonal.
2. **`lens_data`**: reads the parameters of four makers into that model.
3. **`lens_opcodes`**: fits the curves to DNG's polynomials and builds
   `OpcodeList3`.
4. **`--lens-corrections auto|all|none`**, and two lines in `-i` output.
5. **New test program** (`test_lens`), described below.

None of the makers documents these parameters. The reading of each was taken
from other people's work (credited in `lens_data.cpp`: darktable's lens
module for Sony, Fuji and Olympus; Raphael Rigo's notes for Panasonic; dcraw
for the SR2 scrambling) and then **tested against pictures**, which changed
several things.

The test that carried most of the weight: every one of these cameras stores
a JPEG in the raw file, and that JPEG has the camera's own correction in it.
Points were matched between that JPEG and a render of the raw data (SIFT
features, several thousand per file), and a model fitted that maps one onto
the other. A render with the right correction leaves nothing but a scale and
a shift; a wrong reading of the parameters leaves a radial pattern.

| Finding | Resolution |
|---|---|
| All four readings of the **distortion** data hold: the residual falls from 5 to 16 pixels to under one (table in the README). For Sony, three other plausible knot spacings and "table laid out over the stored image" were tried and fit worse. | Implemented as read. |
| **Sony and Fuji zoom the corrected picture** until it just fills the frame. The camera's table alone would cut 6 % off towards the corners of a 24 mm Sony shot; the camera's JPEG loses 3.6 %. The factor is the largest one that takes nothing from outside the frame: predicted 1.0263 and 0.9630 for the two samples, measured 1.0261 and 0.9631. | `frameFillingZoom`. With it the DNG is framed like the camera's JPEG to 0.05 %. |
| **Olympus and Panasonic do not**: their formulas carry the scale. Panasonic leaves 4 % of the frame unused at 12 mm, as its JPEG does. | A flag per maker (`fitFrame`). |
| **Panasonic, word 12** of the tag is not the constant 2500 the published notes took it for: it is the half diagonal of the picture in pixels (2870 for 4592 x 3448; 2500 fits the 4000 x 3000 camera the notes were made with). | Used as the radius unit, if it lies near the half diagonal. |
| **Panasonic, the scale word.** The notes apply it to the higher terms only. Applied to the whole polynomial, the DNG matches the JPEG's scale to 0.04 % instead of 0.10 %. | Applied to the whole polynomial. The difference is a uniform zoom of a tenth of a percent. |
| **Olympus** records "distortion correction: off" in the sample, and the camera's JPEG is corrected all the same. | The setting is ignored; the parameters are always used. |
| **Sony vignetting is already in the raw data** when shading compensation is on. Two observations: the camera's JPEG is as bright in the corners as the raw data without any gain (within 0.1 stop, where the table says 0.73 and 0.28 stop); and in the A7 III night shot the noise variance at equal signal level rises towards the corners by 1.00, 1.00, 1.00, 1.22, 1.64, where the table's gain is 1.00, 1.03, 1.08, 1.21, 1.57. Digital gain multiplies noise; optical fall-off does not. | No vignetting opcode for Sony unless the camera's setting was off and the user asks for `all`. The first version of the code wrote it and would have brightened the corners twice. darktable 4.6 applies this table to the original file, so on these two files its result differs from ours there. |
| **Measuring chromatic aberration on a demosaiced image under-reads it.** Blue against green at the corner of the NEX-5N: 0.5 px on the demosaiced planes, 0.9 px on the sensor's own colour planes, 1.05 px from the table. | Measured on the mosaic: every second pixel for Bayer, 3 x 3 block averages for X-Trans (in which all three colours have their centroid at the block centre). Green against the other green reads 0.0 px, as it should. |
| **Chromatic aberration, results.** Sony NEX-5N: red and blue follow the table, including the change of sign of blue, within 0.35 px. Olympus: both within 0.1 px. Fuji: direction right for both colours, measured size about half the table's for blue. Sony A7 III: too noisy. | Implemented as read. The Fuji magnitude is an open point. |
| The 2011 Sony file has no correction tags next to its raw image; they are in the SR2 block, which is XORed with a key stream. | Unscrambled (`unscrambleSr2`). The values equal those ExifTool prints from the same block. In the 2018 file the block repeats the plain tags, which are read first. |
| A DNG polynomial has four terms per plane where Sony has sixteen knots. | Least squares over the radius, weighted so that the error is one of position. Worst case on the samples 0.39 px; reported per file. |

What `test_lens` covers: curve interpolation; the frame-filling zoom on
curves with a known answer; polynomial curves coming back exactly from the
fit, also for a frame that is smaller than the image and off centre; the
opcode list byte by byte; which corrections each mode writes; curves DNG
cannot hold; each maker's parser on a file built by hand in the test, with
variants (settings off, tables of the wrong length, both byte orders,
both Fuji layouts, the SR2 block under three keys and a wrong one); 1500
damaged copies of those files, which must neither crash nor yield a curve
outside plausible bounds; and the opcode list arriving in a DNG. One check
ties the test to reality: the Panasonic tag the test assembles, checksums
included, must equal the 32 bytes of the real GF7 file.

## Design decisions

**LibRaw for decoding, own code for writing.** LibRaw covers the cameras; the
Adobe DNG SDK is not under an open-source licence, and libtiff has no DNG
knowledge to speak of. Writing TIFF and lossless JPEG ourselves cost about 800
lines and kept LibRaw the only dependency until 0.4.0 added zlib.

**zlib as a second dependency, instead of our own deflate.** The embedded
original has to be readable by Adobe's converter and ExifTool, so the format
is fixed: zlib streams. An inflater is small, but a deflater that compresses
decently is not, and zlib is present on every system dngconv builds on (it is
already a dependency of LibRaw in most packages).

**The original is opt-in.** It doubles the output. People who convert to save
space, or who keep their camera files anyway, should not pay for it by
default. Adobe's converter makes the same choice.

**Large values after the image data.** See the 0.4.0 history. The threshold of
4 MiB is far above any maker note or preview pointer table and far below any
camera file worth embedding.

**`extract` refuses a copy whose checksum fails.** A restored raw file that is
silently wrong is worse than none, because it looks like a backup. There is no
override switch; ExifTool will extract the damaged data for anyone who wants
to salvage it.

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
converter writes, and it survives a rewrite of the file. Adobe's SDK was later
seen to read both copies (0.3.0 history).

**No patching of unsafe fields.** `CFAPattern`, `OECF`,
`SpatialFrequencyResponse` and `DeviceSettingDescription` have an inner layout
that depends on the file's byte order. They are left out rather than
converted.

**Thumbnail rendered from the raw data, not from the camera's JPEG.** Decoding
the JPEG would need a JPEG decoder; rendering needs about 230 lines and no
dependency, works for files without a preview, and shows what the raw data
actually holds. The camera's JPEG is kept next to it, untouched.

**Thumbnail in sensor orientation.** DNG has one `Orientation` tag for the
whole file; rotating the thumbnail ourselves would turn it twice in every
reader that honours the tag.

**Lens corrections follow the camera's setting by default.** A raw file
carries the parameters even when the photographer switched the correction
off. `auto` writes what the camera applied, so the DNG renders like the
camera's JPEG; `all` is there for those who want everything.

**Lens opcodes are flagged optional.** The raw data is complete without
them, and a reader that does not know an opcode should skip it, not refuse
the file. (Adobe's converter writes them as mandatory.)

**Vignetting before the warp.** The makers' gain curves run over the stored
image. `FixVignetteRadial` placed first in the list is applied there, which
needs no conversion; placed second it would have to be re-expressed over the
corrected radius.

**The lens parsers refuse rather than guess.** Tables of an unknown length,
checksums that do not match, values outside what a lens could need: no
opcode. A wrong warp is worse than none.

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
| Unit tests (7 programs) | CTest, on Linux locally and on macOS and Windows through the GitHub workflows | Encoder against a reference decoder, TIFF layout byte by byte, source parser on hand-built and damaged files, MD5, digests and thumbnail colours, embedded originals, lens corrections, DNG round trips through LibRaw in both byte orders |
| `--verify` on every sample | LibRaw 0.21.2 and 0.22.0 | Every sample of the frame survives |
| Render comparison | rawpy (LibRaw 0.22.1) | Source and DNG develop to the same 8-bit picture, so levels, matrix, white balance and pattern agree |
| Reference reader | Adobe `dng_validate`, DNG SDK 1.5.1 | Adobe's own code decodes the raw data, recomputes the image digest, reads the previews and both maker-note copies. No errors; three warnings about values the cameras wrote |
| Structure | ExifTool 12.76 `-validate` | No complaints beyond those the source file already has, except two minor warnings about the private-data copy of the Samsung maker note |
| Independent decoders | darktable 4.6.1 (rawspeed), RawTherapee 5.10 | The files open elsewhere, with the right colours and orientation, including big-endian and uncompressed ones |
| Thumbnail | Pillow, reading IFD0 as a plain TIFF | A generic TIFF reader finds and shows the thumbnail; looked at next to the camera previews for all 15 files |
| Metadata comparison | ExifTool 12.76, exiv2 0.27.6 | Each maker-note, EXIF and GPS tag has the same value in source and DNG |
| Reversibility | `dngconv extract`, ExifTool 12.76 `-OriginalRawImage`, `cmp` | The embedded copy comes back identical to the camera file, through our reader and through an independent one |
| Lens corrections, geometry | Adobe `dng_validate` (render with opcodes), darktable 4.6.1 (lens module, embedded metadata), OpenCV SIFT matching against the camera's JPEG | Two independent readers apply our opcodes and arrive at the camera's own geometry and framing |
| Lens corrections, colour fringes and brightness | Phase correlation between the sensor's colour planes; noise statistics; brightness of the camera JPEG against the render | The sign and size of the aberration parameters; that Sony's vignetting gain is already in the data |
| Memory and undefined behaviour | GCC 13 ASan and UBSan | Clean on the tests (leak detection on), on full conversions with and without `-e`, on extraction, on truncated and byte-flipped copies of real raw files, and on `extract` and `-i` run over 60 truncated or overwritten copies of a DNG with an embedded original, and on `-i` run over 300 damaged copies of the five files with lens data, the damage aimed at the directories the lens parsers read |
| Second compiler | Clang 18 | No warnings at `-Wall -Wextra -Wpedantic` |

`dng_validate` is not part of this repository and is not needed to build or
test dngconv. To build it, compile every `.cpp` in the SDK's `source` folder
into one program with `qLinux=1 qDNGValidateTarget=1 qDNGUseLibJPEG=1
qDNGThreadSafe=1 qDNGUseStdInt=1 qDNGLittleEndian=1`, link it with the XMP
SDK, libjpeg and zlib, and force-include `<cctype>`, `<cstring>`, `<cstdlib>`
and `<cstdio>` for current compilers. `dng_validate -v file.dng` prints every
tag it reads.

The comparison scripts are Python (NumPy, SciPy, OpenCV, rawpy) and are not
in the repository yet; bringing them in is on the list in `TODO.md`.
The render comparison develops both files with camera white balance, linear
demosaicing, no auto-brightness and the two LibRaw settings noted in the 0.1.0
history, then compares the arrays. The metadata comparison runs
`exiftool -a -G5 -s -u` on both files and compares the sets of tag and value,
separating the EXIF copy of the maker note from the private-data copy by
their directory path.

Not covered by any check so far:

- Lightroom and Camera Raw themselves. The validator shares their reading
  code but says nothing about how the pictures look there.
- Adobe's DNG Converter on embedded originals, in both directions: whether it
  extracts ours, and whether `extract` reads theirs. The first is likely,
  since ExifTool reads ours with code written for Adobe's files; the second
  rests on the specification and on the hand-built streams in the test. No
  Adobe-made DNG with an embedded original was at hand.
- Originals of 4 GiB or more (refused), and memory use on very large files:
  the source file and its packed copy are both held in memory.
- Lens corrections beyond the five sample files: Fuji's newer nine-knot
  tables and its cropped modes, a Sony file with shading compensation off,
  Fuji's vignetting gain (7 % in the sample, too small to confirm), and the
  size of Fuji's chromatic aberration values.
- Converting camera files on macOS and Windows (the unit tests run there),
  including Unicode file names on Windows.
- Four-colour sensors (CMYG, RGBE), monochrome sensors, non-square pixels,
  multi-frame files: the code paths exist and are untested on real files.
- XMP in a source file: none of the samples has any.
- Output above 4 GiB, which is refused by design.

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
2. builds on three runners. LibRaw, zlib and what LibRaw needs besides
   (Little CMS, JasPer) are compiled from source by vcpkg and linked statically, so the
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

What has and has not been tried: the workflow built and published releases
0.2.0 and 0.3.0 without changes. Since then zlib joined the dependencies
(0.4.0, added to the vcpkg step); that version of the release workflow has
not run yet, because 0.4.0 was never tagged.

Conventions used so far:

- C++17, no extensions, warnings clean on GCC and Clang.
- Errors travel as exceptions inside the library and become one line on
  stderr in `main`. The source-metadata parser is the exception: it never
  throws and returns what it could read.
- Anything read from a file is untrusted, DNGs included: offsets and counts
  go through `tiff_source`, and names from a file never reach the file system
  unfiltered.
- Every multi-byte value handed to `TiffIfd` is little-endian; only
  `TiffWriter::write` knows the byte order of the file.
- Tests are plain programs with counted checks (`tests/test_util.hpp`), no
  framework, and need no camera files.
- Each source file starts with its SPDX licence line and a comment saying
  what the file is for.
