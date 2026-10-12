# Development notes

Where dngconv stands and how it got there. The README says what the program
does; this file records the progress and the reasons behind the decisions, so
that work can be picked up again without rediscovering them. What is left to
do is in `TODO.md`.

Last updated: 2026-10-12, at version 0.11.1.

## Status at a glance

| Area | State |
|---|---|
| Raw decoding (through LibRaw) | Working: Bayer, X-Trans, full-colour raws |
| DNG writing (own TIFF writer, lossless JPEG) | Working, both byte orders |
| JPEG XL compression (`-c jxl`, DNG 1.7, through libjxl) | Working, lossless; Adobe's SDK 1.7.1 reads the same raw data from all 15 samples, and dngconv reads Adobe's files |
| Lossy JPEG XL (`--jxl-distance`) | Working for mosaics and monochrome sensors; Adobe's SDK 1.7.1 arrives at the linear values within coding error on all 14 mosaic samples; `--verify` judges by a rule calibrated on them |
| Pixel fidelity | Bit-exact on all 15 sample files |
| Colour (matrix, white balance, levels) | Working, one illuminant (D65) |
| Camera profiles (`--profile`) | Working: DCP files embedded as main and extra profiles; identical renders to an ExifTool-made reference in Adobe's code |
| EXIF and GPS copy | Working for TIFF-based raws, CR3, RAF, RW2 |
| Maker notes | Copied, stored twice; verified with ExifTool and exiv2 |
| Previews | Standard layout: rendered thumbnail in IFD0, raw image and a JPEG preview in sub-directories. The preview is the camera's, or rendered here (own renderer and JPEG encoder) when there is none or on request |
| Digests | `NewRawImageDigest` and `RawDataUniqueID`; the former confirmed by Adobe's validator |
| Defect pixels | Flagged with a DNG opcode (Panasonic and similar) |
| Embedded original (`--embed-original`, `extract`) | Working; all 15 samples restored byte-identical, by dngconv and by ExifTool |
| Checking existing DNGs (`check`) | Working; agrees with Adobe's validator on sound and damaged files, also for DNGs written by Adobe's SDK and by cameras |
| Writing missing checksums into existing DNGs (`check --add`) | Working; the same checksums as Adobe's code on 25 of its files, accepted by both validators on six camera-written DNGs, nothing else in the file changed |
| Batch work (`--update`, `--keep-times`, several files at once, XMP sidecars) | Working; clean under the thread sanitizer |
| Updating by content (`--update-by content`, the note of its source in each DNG) | Working |
| Lens corrections as opcodes | Sony, Fujifilm, Panasonic, Olympus; distortion checked against each camera's own JPEG on five files |
| Linux build, GCC and Clang | Tested |
| LibRaw 0.21.2 and 0.22.0 | Both tested |
| macOS and Windows builds | Build and pass the unit tests on GitHub's runners; no camera file converted there yet |
| Release workflow (binaries on a version tag) | Working: every release up to 0.5.0 was built and published by it |
| Adobe's `dng_validate` (DNG SDK 1.5.1) | All 15 sample DNGs pass without an error, with and without an embedded original |
| Adobe's `dng_validate` (DNG SDK 1.7.1) | Reads the JPEG XL DNGs without an error; also all the others |
| libjxl 0.7.0 and 0.11.1; a build without libjxl | All three tested |
| Adobe DNG Converter (extracting our originals, and ours extracting theirs) | **Never tried** |
| Lightroom, Camera Raw | **Never tried** |
| Repository | github.com/jkobierczynski/dngconv, tagged up to v0.5.0 |

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

### 0.5.1 (2026-10-11): the `check` command

1. **Lossless JPEG decoder** (`ljpeg92_decode.cpp`): every predictor, up to
   four components, restart intervals, bounded on any input.
2. **`dng_digest`**: the image digest moved out of the writer, joined by the
   older `RawImageDigest` and by the 8-bit form of the newer one.
3. **`dng_check`**: finds the raw image in a DNG, reads it as stored (strips
   or tiles; uncompressed at 8, 16 or any packed depth in between, or
   lossless JPEG), compares the digests, verifies the embedded original, and
   checks that the file is long enough for everything its directories name.
4. **`dngconv check`**.
5. **New test program** (`test_check`), and the TIFF builder of the tests
   moved into a header of its own.

Why an own decoder, when LibRaw already reads DNG: the digests are defined
over the samples **as stored**. LibRaw applies a DNG's linearisation table
while decoding, so for the many files that have one its output cannot be
hashed.

Findings, and what was done:

| Finding | Resolution |
|---|---|
| The first run reported 13 of 30 sound files as damaged, differently on each run. Tiles were read from one file stream by several threads. | Reading is serialised; decoding and hashing stay parallel. The unit tests had not caught it because their files fit in one tile; `test_check` now uses frames of several tiles on three threads. |
| Of 150 single-bit changes made inside the raw data of six files, 6 went unnoticed. All six were in the last row of tiles, in the part of the stream that codes rows below the picture. Adobe's validator passes the same bytes. | Correct as it is: the picture is unchanged. Documented. |
| **Those unused rows were expensive.** The writer filled them by repeating the last two rows, which lossless JPEG's horizontal predictor codes at full price. A bottom tile of the NEX-5N file spent 20 % or more of its bytes on rows nobody sees. | Rows below the picture are now flat, each component holding the value its predictor expects. DNGs of the 15 samples shrank by 4.8 % together, up to 9.8 % for frames that end shortly after a tile boundary; the samples are unchanged. |
| A file cut off inside its preview or metadata passed, since no checksum covers those parts. | The check also computes how long the file must be for all strips, tiles and tag values of every directory, and reports the number of missing bytes. The odd padding byte at the very end is the only thing that can go missing unnoticed. |
| Which sample width the newer digest hashes is not in the specification. From the SDK source: bytes for data stored with up to 8 bits, and also for deeper data that comes with a linearisation table of at most 256 entries; 16-bit words otherwise. | Implemented so, and confirmed on files Adobe's SDK wrote from 8-bit input with and without such a table. |

How the decoder and the digests were verified beyond the unit tests:

- **Against LibRaw**, on the six camera-written DNGs of the
  metadata-extractor collection (DJI, LG, two Leica, Pentax: 16-bit and
  packed 12-bit strips, one with 3028 strips of one row) and three of our
  own: the samples read by `dng_check` equal LibRaw's.
- **Against Adobe's writer**: `dng_validate -dng` rewrote those six, five of
  ours and three hand-made 8- and 12-bit files. The results carry Adobe's
  lossless JPEG streams (tiles of 256, 8- and 16-bit precision, one with
  three components) and Adobe's digests. `check` calls all 14 intact.
- **The older digest**: no writer at hand produces it, so
  `NewRawImageDigest` was renamed to `RawImageDigest` in three of our files
  and filled with our value. Adobe's validator accepts them and rejects a
  fourth with one bit changed; so does `check`.
- **Damage**: on 36 files with one flipped bit `check` and Adobe's validator
  gave the same verdict 36 times.

### 0.6.0 (2026-10-11): batch conveniences

1. **Several files at once** (`job_runner.hpp`): a small pool that runs the
   per-file work on a few threads and hands the reports back in the order of
   the file list. Conversion, `extract` and `check` all go through it.
   `-j` now means threads in total; `-p` the number of files at once.
2. **`--update`**: skip what has a DNG that is not older than its source.
3. **`--keep-times`**, for conversion and for `extract`.
4. **XMP sidecars** (`sidecar`): found next to the raw file, stored as the
   DNG's XMP tag.
5. **Two new test programs**: `test_batch` (runner, sidecars, the XMP tag)
   and `test_cli`, which runs the program itself on DNGs it generates.

Findings, and what was done:

| Finding | Resolution |
|---|---|
| LibRaw comes in two builds, and only `libraw_r` may decode two files at the same time; the other keeps state in static storage. Which one is linked is decided by the build system. | CMake records it (`DNGCONV_LIBRAW_REENTRANT`) and prints it. Without the thread-safe build, decoding takes turns behind a mutex; everything else still runs in parallel. Both variants were built and tested. |
| The thread sanitizer reported two data races in the first parallel run. Both lie inside LibRaw's CR3 decoder, between a thread and the OpenMP workers that same decoder started: the usual false alarm with an OpenMP runtime the sanitizer cannot see into. There is only one CR3 file among the samples, so two conversions cannot have been involved. | Suppressed for the test (`race:libraw_r.so`). With that, conversion with `-e --verify`, `check`, `extract` and the unit tests run clean with eight threads on four files. |
| Two inputs can map to one output: the same file name in two source folders sent to one flat folder, or `IMG.CR2` beside `IMG.NEF`. One at a time, the second was refused because the file existed; in parallel both would have written the same temporary file. | Targets are worked out before anything starts; the first claimant gets the name, the others an error. `extract` does a first pass that reads only the recorded names, for the same reason. |
| `--update` compares times, and `--keep-times` sets the DNG's time to the source's. A sidecar edited after conversion would then never trigger a new conversion, or, compared naively, always. | "The source's time" is the later of raw file and sidecar, for the comparison and for the time that is kept. |
| A sidecar need not be XMP just because it is called `.xmp`. | It must hold an `x:xmpmeta` or `rdf:RDF` element and be under 16 MiB; otherwise a warning, and the conversion goes on without it. |

Measured on the 15 samples with two cores: 10.9 s one file at a time, 8.2 s
with two at once. The output is byte-identical either way, and so is the
report apart from the timings.

### 0.6.1 (2026-10-11): the verification tools

1. **`tools/`**: the comparison scripts, rewritten to be run by others
   (`compare_render.py`, `compare_metadata.py`, `compare_camera_jpeg.py`,
   `measure_fringes.py`), a build recipe for Adobe's validator that starts
   from nothing, and `verify.sh`, which converts a folder of raw files and
   runs every check in turn.
2. One fix in the converter that the tools turned up (last row below).

Findings while turning the scripts into tools:

| Finding | Resolution |
|---|---|
| Run again, the render comparison reported the Canon EOS R8 as different by a mean of 1 in 255, where 0.1.0 had recorded "none". The DNG was written with LibRaw 0.21.2 and compared by the LibRaw 0.22 inside rawpy, and the two have **different colour matrices** for that camera (and black levels one count apart). The earlier figure came from a DNG written with 0.22. | Not an error of the converter, but a real property: a DNG carries the matrix of the LibRaw it was made with. The script now pins the black level and, when pictures differ, compares again without colour conversion, reporting "same raw data and levels" with a note. The README says so too. |
| `dngconv -i` showed a black level of 0 for the same file when built with LibRaw 0.22, which learns it only while decoding. Conversion was never affected, since it decodes. | In report mode a file whose black level is zero before decoding is decoded after all. |
| A validator that cannot run produces no error messages either. | `verify.sh` counts a file as accepted only if `dng_validate` reached "Validation complete". |
| Measured on a developed picture, blue fringes read half their size (0.5 px against 0.9 px on the sensor planes of the NEX-5N), with the SDK's demosaicing and with LibRaw's linear one. | `measure_fringes.py` works on the mosaic and prints the DNG opcode's correction next to the measurement. The green planes against each other read 0.05 to 0.10 px, which is its accuracy. |

`verify.sh` on the 16 sample files: all eleven steps pass; one file is not
readable by the decoder (Sigma X3F), one source cannot be developed by
rawpy (the damaged Canon file), four files have a distortion opcode and
match their camera JPEG. It was also run on a folder with spaces in file
and folder names, and with the validator and Python missing or broken.

### 0.7.0 (2026-10-11): camera profiles

1. **`--profile <file or folder>`** (repeatable) and
   **`--profile-any-camera`**. Each picture gets the DCP profiles whose
   `UniqueCameraModel` is its camera (letters and digits compared, nothing
   else). The first becomes the profile of the file: its tags go into IFD0
   in place of the decoder's `ColorMatrix1`. The others follow in
   `ExtraCameraProfiles`.
2. **`camera_profile.*`**: a bounds-checked parser for the DCP container (a
   TIFF with the magic number 0x4352), a table of the 34 tags that make up a
   profile in DNG 1.4, 1.6 and 1.7 with the type each must have, consistency
   checks (matrix sizes against the number of colours, table sizes against
   their dimensions, invertible matrix, finite numbers), and the packer for
   extra-profile blocks.
3. **`-i`** describes a `.dcp` file, and with `--profile` lists the profiles
   a raw file would get. `TiffIfd::setBlocks` stores blocks addressed by
   offset. `tools/compare_profile.py` and a `profiles` step in `verify.sh`
   (run when `PROFILES` names a folder).

How it was verified, with RawTherapee's 161 open profiles (159 distinct)
and the 15 samples, nine of which have a profile among them:

| Check | Result |
|---|---|
| Every profile tag of the nine DNGs read with ExifTool and compared with the `.dcp` file | Equal, large tables included |
| Adobe `dng_validate` on all 15 DNGs | No error, no new warning; `-v` lists the main profile and each extra profile with its tables |
| The render test: `dng_validate -tif` on our DNG, and on a reference made by letting ExifTool write the profile's tags into the profile-less DNG | Maximum difference 0 for Sony ILCE-7M3, Canon EOS R8, Olympus E-M10 and Canon EOS 40D (sRAW). Against the DNG without profile the mean difference is 1.5 to 8 of 255, so the profile is in use |
| Extra profiles | Parsed by Adobe's reader in a little-endian and a big-endian (Nikon) DNG; still present after `dng_validate -dng` rewrote the file; blocks equal to the `.dcp` files tag for tag |
| LibRaw 0.22 (rawpy) on a DNG with a two-illuminant profile | Develops; it takes the profile's matrices |
| Parser under ASan and UBSan | 18,000 mutated copies of three profiles: 5,359 accepted, the rest refused, no finding |

Decisions:

- **The camera model inside a profile is not embedded.** Adobe's reader
  treats it as a restriction ("only for this model"). Inside a DNG the
  profile has already been matched, and our spelling of the model may differ
  from the profile's in case or spacing; a restriction could only hide it.
- **Tags are carried, not interpreted.** Values go from the `.dcp` to the
  DNG untouched (integer tags are widened to the type the specification
  names). Tags that are not part of a profile are dropped. A profile that
  fails a consistency check is refused as a whole instead of repaired.
- **"Never embed" is obeyed, "embed if used" is read as permission**, since
  the profile is the one the file will be developed with.
- **The decoder's matrix is not kept as an extra profile** when a DCP takes
  its place. It could be, under a name of its own; nobody has asked.
- **No Adobe profile was at hand.** The format is the same and Adobe's
  reader parses what we write, but a `BaselineExposure` to go with Adobe's
  profiles is not written (LibRaw has no such table), which the README
  states.

### 0.8.0 (2026-10-11): rendered previews

1. **`--preview auto|camera|render|none`** and **`--preview-size <n|max>`**.
   Automatic keeps the camera's JPEG when it is at least 1000 pixels long
   and renders one otherwise; `--no-preview` stays as another word for
   `none`.
2. **The renderer** (`render.*`, grown out of `thumbnail.*`) makes pictures
   of any size up to one pixel per mosaic block: cell averages, an area
   filter down to the size asked for, lens corrections, a tone curve, all
   on a few threads. The thumbnail is the same picture at 256 pixels.
3. **A baseline JPEG encoder** (`jpeg_encoder.*`): 4:2:0, quantisation
   tables of the standard scaled the usual way, Huffman tables computed per
   picture and limited to 16 bits.
4. The preview directory of a rendered preview names its maker
   (`PreviewApplicationName`, `PreviewApplicationVersion`) and its colour
   space (`PreviewColorSpace` = sRGB). `-i` says which preview a file gets.
   `check` now counts extra camera profiles when it works out whether a
   file is complete. `tools/compare_preview.py`, and a `preview` step in
   `verify.sh`.

Findings:

| Finding | Resolution |
|---|---|
| The thumbnail renderer of 0.3.0 brightened every picture until its brightest percent was white, by up to two stops. Measured against the cameras' JPEGs this is wrong for ordinary pictures: their brightest percent mostly sits at 14 to 65 % of the sensor's range, by the cameras' own choice, and the JPEGs are that dark there too. | Automatic exposure removed. Exposure is as shot. |
| What cameras do instead is a tone curve. Measured on thirteen cameras (near-neutral 16-pixel cells of the camera's JPEG against the renderer's linear output): mid-tones about twice as bright as the linear value (0.12 becomes 0.28), highlights compressed, values below 0.02 darker than linear. Cameras differ by a factor of 2.5 at a linear value of 0.06 (Fujifilm S5500 lowest, Olympus E-M10 highest). | One curve for all, close to the median: `x (1 + a) / (1 + a x)` with a = 1.5, times a toe `(x^2 + f s^2) / (x^2 + s^2)` with s = 0.025 and f = 0.3. The toe is gentler than the cameras' so that shadows stay readable. |
| X-Trans repeats every six pixels, and the first version used that period as its cell, giving previews a sixth of the sensor wide. | Every 3 x 3 block of the pattern already holds five green, two red and two blue samples. The renderer now looks for the smallest block that is balanced wherever it lies on its own grid, and aligns its cells with that grid. |
| A Huffman tree for symbol frequencies in exact Fibonacci proportion is ambiguous: every merge is a tie, and this encoder's tie-breaking happens to give a bushy tree of depth 12. The test meant to exercise the 16-bit limit did not. | The test's frequencies grow by one more than Fibonacci's, which leaves no ties and gives a chain 20 deep; the limit is reached and the picture decodes. |
| A JPEG strip of odd length is followed by a padding byte, so "the last 100 bytes missing" reads as 99 for some files. | `check`'s message was right; its test now accepts both. |
| After a change to an options structure the scratch program linking the library showed pictures 15 dB off: it had been compiled against the old layout. | Not a bug in the converter; noted because the first reading was "the encoder is broken". |

How it was verified:

| Check | Result |
|---|---|
| The encoder against libjpeg (Pillow, quality 90, 4:2:0, optimised tables) on five previews | Same error to 0.01 dB (one file 0.05 dB better); 0.5 to 4 % smaller |
| Decoders | libjpeg through Pillow and OpenCV give the same pixels; the unit tests' own decoder (separate zigzag table, Huffman decoding from the file's tables, plain inverse DCT) reads every test picture, including noise at quality 100 and the 20-deep code tree |
| Rendered preview against the camera's JPEG, 12 files (a thirteenth, a night sky, cannot be matched) | Median residual of matched points 0.18 to 0.53 preview pixels; with the lens corrections off 4.0 (Panasonic) and 1.4 (Olympus) |
| Tones against the camera's JPEG | Median brightness within 18 of 255; typical difference per cell 1 to 18 |
| Adobe `dng_validate`, ExifTool `-validate`, exiv2 | No error on 15 DNGs with rendered previews at 1024 pixels and four at the largest size; exiv2 lists thumbnail and preview |
| Sanitizers | ASan/UBSan and TSan clean on the tests and on conversions with `--preview render` |

Decisions:

- **Averages over mosaic blocks, no demosaicing.** A real demosaicer for
  Bayer and another for X-Trans would be most of a raw developer. Half
  resolution is ample for a preview, and block averages have no artefacts
  to argue about.
- **One curve, exposure as shot.** A per-camera curve would need a table
  nobody has; matching each rendered preview to the camera's JPEG is
  possible only where there is one, which is where rendering is needed
  least.
- **Huffman tables per picture.** Slightly smaller files, and no table to
  copy from the standard but the two quantisation ones.
- **No time stamp in the preview directory** (`PreviewDateTime`): two
  conversions of the same file stay byte-identical.

### 0.9.0 (2026-10-11): JPEG XL compression

1. **`-c jxl`** and **`--jxl-effort <1..9>`**: the raw data as lossless
   JPEG XL, stored the way Adobe's SDK stores a mosaic losslessly. Rows and
   columns go field by field (`RowInterleaveFactor`,
   `ColumnInterleaveFactor` = the period of the mosaic), the result is cut
   into tiles, each tile is a 16-bit JPEG XL picture. DNG version 1.7.1.0.
2. **libjxl as an optional dependency** (`jxl_codec.*`). CMake looks for
   it, links a test program before trusting what it found, and builds
   without JPEG XL otherwise; `DNGCONV_JXL=ON` or `OFF` settles it.
   `dngconv --version` reports it.
3. **The reader behind `check`** learnt JPEG XL tiles, interleaved rows and
   columns (for any compression), and the checksum DNG uses for JPEG XL and
   lossy JPEG data, which is of the compressed bytes. So `check` now
   verifies those files too, lossy JPEG ones from other converters
   included, and needs no libjxl for it. `--verify` uses this reader for
   JPEG XL files, since LibRaw does not read them.
4. `tools/compare_raw_data.py`, `tools/dng-validate/build.sh --sdk 1.7`, a
   `jxl` step in `verify.sh`; libjxl in both workflows, with a warning in
   the log where a platform's build ends up without it.
5. A DNG given as input is now named as one even when the decoder cannot
   open it.

Findings:

| Finding | Resolution |
|---|---|
| The specification's list of image kinds allowed with JPEG XL leaves out the colour filter array. Adobe's SDK accepts it, one of the sample files that come with the SDK is a Bayer mosaic in JPEG XL, and the specification's own text on `ColumnInterleaveFactor` describes the case. | Followed the SDK: mosaic kept as a mosaic, fields by the size of the pattern (2 x 2, or 6 x 6 for X-Trans, where 3 x 3 fields would mix red and blue). |
| **The checksum is not of the samples.** For a raw image in JPEG XL, lossless or not, Adobe's reader compares `NewRawImageDigest` with the MD5 of the MD5s of the compressed tiles (for lossy JPEG, with that of the shared tables appended). A file with the usual checksum of its samples is reported as damaged. Not in the specification; read from the SDK's source. | The writer stores that checksum; `RawDataUniqueID` is still built from the checksum of the samples, so that it does not depend on the compression. Confirmed from the other side: `check` finds Adobe's own JPEG XL samples intact by this rule. |
| The fields are laid out over the whole picture, not tile by tile, and a field of an odd-sized picture is one line longer than the next. | Same arithmetic in writer and reader, written twice independently (the test has a third, list-based version); Adobe's reader agrees on ten synthetic sizes. |
| libjxl wraps every tile in its container format, 50 bytes more than a bare codestream, although nothing asked for it: 16 lossless bits need "level 10" of JPEG XL, and that has to be declared in a box. | Left as it is. Adobe's writer produces the same. |
| Tile size hardly matters for most cameras (R8: 32.03, 32.02 and 32.00 MB at 442, 774 and 1547 pixels) but the Panasonic file goes from 14.05 to 16.03 MB at effort 3 as tiles grow. | Tiles of at most 512 pixels, the same limit as for lossless JPEG. |
| Effort: nearly all of the gain is reached at level 2 or 3 (table in the README). Level 9 takes 87 s where level 3 takes 3, and is not smaller than level 5. With libjxl 0.11 level 1 is a different, faster coder that gains only 4 % on lossless JPEG. | Default 3. |
| The thread sanitizer reported, in two runs of three, two encoders on different threads reading and writing the same block of memory inside libjxl 0.7: a table the library builds on first use. The library is not instrumented, so the sanitizer cannot see whether libjxl guards that itself. | The first call for each kind of picture now codes and decodes a small one alone while the other threads wait, so that first use never happens on two threads. Six runs after that: no report. Output was byte-identical across thread counts before and after. |
| `cmake --build --parallel` without a number lets make start a compiler for every file at once. Building libjxl that way next to another build ran the machine out of memory. | The validator's build script passes the number of processors. |
| With Visual Studio a CMake link test is built in the Debug configuration, which does not link with the release-only static libraries of the release workflow; libjxl would have been dropped there without a word. Found by reading, not by running: there is no Windows machine here. | The test is built as Release with multi-configuration generators (tried with Ninja Multi-Config). The workflows print a warning if a build has no JPEG XL. |

How it was verified (libjxl 0.7.0 from Ubuntu and 0.11.1 built from the
copy in Adobe's SDK):

| Check | Result |
|---|---|
| `--verify` on the 15 samples, both libjxl versions | every sample back |
| Adobe `dng_validate` 1.7.1 | no error on the 15 DNGs; three warnings about values the cameras wrote, as before |
| Stage 1 image from `dng_validate -1` of each JPEG XL DNG against that of the lossless-JPEG DNG | identical, 15 of 15 |
| Ten synthetic frames kept from `test_jxl` (odd sizes, X-Trans, three channels, tiny) through the same reader | identical to the frames written |
| Six DNGs written by `dng_validate -losslessJXL -dng`, read by dngconv | same samples as the files they were made from; `check`: intact |
| `check` on Adobe's 14 DNG 1.7 sample files | 12 intact, 2 not judged (floating point; transparency mask) |
| Size against Adobe's writer (effort 7, larger tiles, about 20 s a file) | ours at effort 3 is 0.3 to 6 % larger (R8 32.03 against 31.95 MB, Panasonic 14.05 against 13.30) |
| Sanitizers | ASan and UBSan: tests clean; `check` and the stored-image reader on 400 damaged copies of four JPEG XL DNGs (two writers): 110 decoded, 290 refused, no finding. TSan: see the findings |
| A build with `-DDNGCONV_JXL=OFF` | tests pass; `-c jxl` is refused before any file is read; `check` still verifies JPEG XL files |
| Other readers | rawpy (LibRaw 0.22.1), darktable 4.6.1 and RawTherapee 5.10 refuse the files; ExifTool 12.76 reads all metadata and does not know one tag (`JXLEffort`) by name |

Decisions:

- **Lossless only.** The point of this converter is that nothing is lost.
  Lossy JPEG XL on a mosaic exists (Adobe's SDK has it) and would be a
  small step from here; it is in the to-do list, not in the program.
- **The default stays lossless JPEG.** Every DNG reader opens it. JPEG XL
  is for people who know their software reads DNG 1.7.
- **libjxl optional.** A converter that fails to build because a codec
  library is missing serves nobody; and the checksum rule means even the
  `check` command does not depend on it.
- **Sixteen bits declared for every tile**, whatever the sensor's depth, as
  Adobe does. Declaring twelve would save the container, and little else.

### 0.11.1 (2026-10-12): the curve on macOS

The unit test of the lossy curve failed on GitHub's macOS runner: the
closed form (Cardano) does not give exactly 0 for a linear value of 0, and
the sign of what is left over depends on the maths library. On Linux it
came out at or below 0 and was clamped; on macOS it was a hair above 0
(Python's maths here gives +5.6e-17 too). Nothing a file holds changed,
since such a value rounds to the stored value 0 anyway, but the ends are
now returned exactly, 0 and 1, on every system. The test that caught it
stays as it is.

### 0.11.0 (2026-10-12): lossy JPEG XL

1. **`--jxl-distance <d>`** (implies `-c jxl`): the raw data stored with
   lossy JPEG XL, prepared the way Adobe's SDK prepares a mosaic for it
   (read from `dng_negative.cpp`, `LossyCompressMosaicJXL` and
   `EncodeImageForCompression`): active area only, made linear, stretched
   over its range, put through the curve whose inverse is
   `y/16 + (15/16) y^3`; `MapPolynomial` in `OpcodeList2` to undo it;
   colour fields of whole tiles, with a margin repeated from the edge;
   tiles labelled grey with the sRGB transfer curve (`lossy_jxl.*`).
2. **`--verify` for lossy data**: what comes back is compared with what was
   coded, by a rule with five parts (see the findings), and the report says
   how close it came.
3. **The reader behind `check`** decodes lossy JPEG XL when asked to
   (`readStoredRawImage(..., acceptLossy)`); `check` itself still judges
   such files by their checksum only.
4. **Defect pixels: the BayerPhase is now written as Adobe's reader takes
   it** (see the findings). This changes lossless files too.
5. `tools/compare_lossy.py`; step `lossy` in `verify.sh` (17 steps); a
   fifteenth test program, `test_lossy`.

Findings:

| Finding | Resolution |
|---|---|
| Adobe's SDK stores only a 2 x 2 mosaic lossily, and not as it is: the active area, linearised, put through a curve with an opcode to undo it, padded so that each colour field is a whole number of tiles. Its comment: without the curve "JPEG XL [would] overly compress in the shadows". | Followed, including the curve and its constants, so that a lossy file of ours is read exactly as one of Adobe's. X-Trans (6 x 6 fields) and monochrome sensors are stored the same way; Adobe's reader reads them (X-Trans tried). Full-colour raws are left lossless. |
| The SDK leaves the brightest and darkest hundred-thousandth of the samples out of the range the curve spans. In the file the SDK wrote from the Sony NEX-5N sample, 161 samples lose up to 56 % of their value: the brightest specks, highlights that had clipped, become grey. A star field would lose its stars' cores. | The whole range is used. |
| The first linear values did not match Adobe's reading of the lossless file of the damaged Canon 400D sample, by up to 14 %: its black levels differ widely between the four positions of the pattern (257 to 9146), and Adobe's reader scales every position by white minus the highest black, not by white minus its own. | Same scaling. Since then the linear values equal those of Adobe's reader for every sample, to one 65535th. |
| Defect pixels (Panasonic, marked 0): Adobe's reader filled 50 of them differently from dngconv, by up to 47 %. Its `FixBadPixelsConstant` takes a pixel for green when row + column + p + p/2 is even, p the BayerPhase: green at the origin for p = 0 and 3, the opposite of the specification's description (0 red, 1 and 2 green, 3 blue), which dngconv had followed. So Adobe's reader filled red and blue defects from their diagonal neighbours, of the other colour. | Lossless files: the phase is written as Adobe's reader takes it (the specification's value with its lowest bit flipped); rawspeed ignores it, LibRaw ignores the opcode. Lossy files: the defects are filled before coding, exactly as Adobe's reader fills them (diagonals for green, two pixels away for red and blue, defective neighbours left out, its rounding). After that, Adobe's reading of the lossless Panasonic file and dngconv's filled values agree everywhere. |
| What is "close enough"? Measured on the 14 mosaic samples at distances 0.1 to 6, in the coded domain: root mean square up to 1.23 x distance %, worst tile 1.26 x, worst 32 x 32 block average 0.32 x, overall mean 0.05 x. | Limits 2.5, 3, 1 (+ 0.1 %) and 0.1 (+ 0.01 %) x distance: at least twice what was seen. |
| Those four measures do not catch subtle layout mistakes once the distance reaches 1: a frame moved by one period, a field moved by one line, the two greens exchanged, red and blue exchanged at 3. Their errors are the size of the coder's. | A fifth test: every colour field must fit its own expected values better than those one pixel, or one period, away in any direction. Coding blurs but does not move. The margins of sound files stay positive up to distance 25 (smallest 0.004), and every one of those faults is caught on every sample at 0.2, 1, 3 and 10. A single tile in the wrong place or upside down is caught up to distance 1, and in 12 of 14 samples at 3. |
| Effort for lossy coding hardly matters: Sony NEX-5N and Panasonic at distance 1, effort 1, 3, 5, 7: 3.84/3.49/3.68/3.69 and 4.86/4.50/4.49/4.54 MB. | Default stays 3 (Adobe uses 7). |
| libjxl 0.11.1 writes files 3 to 4 % smaller than 0.7.0 at the same distance. | Noted. Each version's decoder judges the other's files within coding error, with the same figures to three decimals. |

How it was verified:

| Check | Result |
|---|---|
| `dng_validate` 1.7.1 on the lossy DNGs of the 14 mosaic samples at distances 0.2, 1 and 3 | no error |
| `compare_lossy.py`: stage 2 (linearised, opcodes applied) of each lossy file against that of the lossless JPEG XL file, both read by Adobe's SDK | within the rule's limits for all 14 at the three distances |
| dngconv's decoding plus the polynomial, against Adobe's stage 2 of the same lossy file (five samples) | within 0.006 % of full scale (largest difference over the whole frame) |
| Adobe's rendering of lossy against lossless files, four samples | 45 to 49 dB at 0.2, 37 to 42 at 0.5, 32 to 36 at 1, 29 to 33 at 2; 100 % crops looked at: noise smoothed from 1, faint stars gone at 2 |
| The fault simulation above | all layout faults caught |
| Sanitizers | ASan and UBSan: tests, and five conversions with `--verify -e` at distance 0.5, clean. TSan: the 16 samples at distance 2, four files at once, `--verify`, no report |
| All builds | 15 of 15 tests with LibRaw 0.21.2 and 0.22.0, GCC 13 and Clang 18, libjxl 0.7.0, 0.11.1 and none |

Decisions:

- **Lossy is opt-in and says so in the report.** The converter's default
  remains that nothing is lost.
- **Adobe's preparation, not one of our own.** Readers of DNG 1.7 are
  Adobe's code and code that follows it; a file prepared the same way is
  read the same way. The one deviation, the full range, cannot make a
  reader go wrong: it changes only the polynomial's coefficients.
- **Masked borders are dropped** in lossy files, as Adobe does: their
  values mean nothing once the black level is subtracted.
- **`check` does not decode lossy data**: the checksum of the compressed
  tiles says whether the file is as written, which is the question
  `check` answers. Decoding is what `--verify` does, once, when the file is
  made.

### 0.10.0 (2026-10-12): the archive side

1. **`check --add`** (`-a`): writes the checksums into DNGs that have none.
   The raw image gets `NewRawImageDigest`, or `RawImageDigest` if the file
   declares a DNG version before 1.4; an embedded original without a
   checksum gets `OriginalRawFileDigest`. `-t` keeps the file's date.
   Plain `check` now says how many files could be given a checksum.
2. **Adding to a file in place** (`dng_append.*`): a copy of the first
   directory, with the new fields, goes after the end of the file, and the
   header is pointed to it. Nothing else is written. The new directory is
   forced to disk before the header changes; the file is read back
   afterwards and put back as it was if the result does not verify.
3. **The note of its source in every DNG** (`source_record.*`): size and
   MD5 of the source file and the MD5 of the sidecar stored with it, as a
   line of text in private tag 65300. The reader takes the MD5 on a thread
   of its own while LibRaw decodes. `--no-source-record` leaves it out.
4. **`--update-by content`**: decides by that note instead of by file
   times. A DNG without the note is left as it is and reported.
5. `tools/compare_checksums.py`; steps `content` and `checksums` in
   `verify.sh` (16 steps now); a fourteenth test program, `test_archive`.

Findings:

| Finding | Resolution |
|---|---|
| Six DNGs straight from cameras (DJI Phantom 4 twice, LG Nexus 5X, Leica T, Leica X Vario, Pentax Q-S1) carry no checksum of any kind, and declare DNG 1.4, 1.4, 1.4, 1.3, 1.3 and 1.1. `NewRawImageDigest` came with 1.4. | The checksum follows the file's version: three of the six got `NewRawImageDigest`, three `RawImageDigest`. Adobe's validators (1.5.1 and 1.7.1) recompute either kind ("FindRawImageDigest time" appears in their output only for files that have one) and accept all six. |
| Does the validator really compare, or just read past the tag? | One byte of the checksum falsified in three files (new kind, old kind, JPEG XL): "NewRawImageDigest does not match raw image", "RawImageDigest does not match raw image", "NewRawImageDigest does not match Lossy Compressed image". |
| The checksum of lossy JPEG data (compression 34892) had been taken from the SDK's source in 0.9.0 but never compared with a file, for lack of one. | `dng_validate -proxy` of SDK 1.5.1 writes such files, `-lossyMosaicJXL` and `-proxy` of 1.7.1 lossy JPEG XL ones. Stripped of their checksum and given it again by `check --add`, all four get back what Adobe wrote. |
| A file of odd length (one of the DJI files) needs a byte of padding before the new directory: TIFF wants directories on even offsets. | Padded; the growth is 562 bytes for one DJI file and 563 for the other. |
| Where can the note of the source live? DNG has no tag for it. `OriginalRawFileDigest` is defined as the checksum of the embedded copy. `DNGPrivateData` is taken by Adobe's maker-note format. XMP would mean editing the user's sidecar text before embedding it. TIFF/EP's `ImageHistory` fits the meaning, but ExifTool's `-validate` calls it non-standard in either directory it can go in, the same minor warning an unknown tag gets. | A private tag (65300; ExifTool knows 65000 to 65112 from other programs, so away from those), holding text that says what it is. Adobe's validators say nothing; ExifTool: "[minor] Unknown IFD0 tag 0xff14", and it keeps the tag when it rewrites the file. The README no longer says ExifTool reports no problem at all. |
| The first reader of the note refused the largest possible file size: the guard against overflow was `size > (max - 9) / 10` whatever the digit. | Guard uses the digit at hand. Found by the test that writes and reads back `UINT64_MAX`. |
| What to do, updating by content, with a DNG that has no note? Converting it again is what `--update` would suggest, but such a DNG may be from another program, or may have been edited by one that dropped the tag, and the times that would tell are exactly what the user said not to trust. | Left as it is, with a message; counted separately; exit status 0. `--force` converts it. |
| Taking the MD5 costs a second pass over the source file. | Done on its own thread during decoding: 11.3 s for the 16 samples with and without it (three runs each, differences within the noise). When the file is kept for embedding, the copy in memory is hashed instead. |
| A file named twice on the command line of `check --add` would be written to by two threads. | Files are made distinct by their canonical path first. The append also writes at the offset it computed, not "at the end", so that two identical appends that did meet would write the same bytes to the same place. |
| A disk that fills up while the new directory is written leaves part of it behind. | The file is cut back to its old length; the header was not touched yet. Tried with a file-size limit (`ulimit -f`) that allows none, and 200, of the 466 bytes: the file is identical to the original both times, and the failure is reported. |
| A checksum added later cannot know what the file was before. Of 60 randomly damaged copies of a lossless-JPEG DNG, 25 were given one: the damage was in a part no checksum covers (previews, metadata), or in the raw data in a way that still decodes, which lossless JPEG, having no integrity of its own, often allows. | Nothing to be done, and said in the README: the checksum vouches for the file as it was when it was added. Damage that makes the data undecodable, a file that is cut short and an embedded original that does not unpack are caught, and such files are not written to. |

How it was verified:

| Check | Result |
|---|---|
| `compare_checksums.py` on 25 DNGs written by Adobe's code: its twelve readable DNG 1.7 sample files (two more are floating point or have a transparency mask), six JPEG XL files from `dng_validate -losslessJXL`, three lossless-JPEG files from SDK 1.5.1 (8 bits with and without a table, 12 bits), and the four lossy ones above | the checksum dngconv writes is Adobe's, byte for byte, in all 25; `dng_validate` 1.7.1 accepts every stamped copy |
| The same on dngconv's own files: the 15 samples with embedded originals (in `verify.sh`), and lossless JPEG, JPEG XL and uncompressed variants | raw and original checksums come back as written |
| Six camera-written DNGs given their checksums | `dng_validate` 1.5.1 and 1.7.1: same output as before plus the line that the checksum was computed, no new warning; ExifTool `-a -u -G1`: the same tags plus the one added; ExifTool `-validate` and exiv2: the same remarks as before; rawpy (LibRaw 0.22.1): raw data and developed picture identical; darktable 4.6.1 (three of them): developed picture identical pixel for pixel; byte comparison: the old file is a prefix of the new one except for bytes 4 to 7 |
| Rewritten by ExifTool afterwards (`-Artist=`) | `check`: still intact; the note of the source still there and still matching |
| Interrupted between the two writes (simulated: new directory appended, old header) | reads as the old file, not damaged, not "cut short"; the next `--add` completes |
| Read-only file (as an unprivileged user), full disk, damaged files, a wrong or malformed checksum already present | refused or reported; file byte-identical afterwards |
| The file failing to verify when read back (a test puts a fault-finding reader in the place of the real one, since nothing from outside makes that happen) | reported; file byte-identical afterwards |
| `--update-by content` on real files (`verify.sh`, and by hand): all dates changed; one byte appended under an old date; sidecar added, changed, removed, `--no-sidecar`; a DNG from an earlier version; one made with `--no-source-record` | converted exactly the files that changed, with the reason in the report; DNGs without a note left alone |
| The note against `md5sum` | same digest for the three files compared |
| Sanitizers | ASan and UBSan: tests clean; `check --add` on 420 damaged copies of seven DNGs (four writers; checksums removed first): 204 left untouched, 216 given checksums and then found intact by `check`, none written to in any other way, no finding. TSan: `check --add -p 4` on 26 files and parallel conversion with `--update-by content`, no report |
| All builds | 14 of 14 tests with LibRaw 0.21.2 and 0.22.0, GCC 13 and Clang 18, libjxl 0.7.0, 0.11.1 and none; `test_archive` also as an unprivileged user, for the read-only case |

Decisions:

- **Append, do not rewrite.** Writing the DNG out again would be the
  obvious way to add a tag, and would move every byte of a file this
  program did not make: maker notes with absolute pointers, private
  structures no parser here understands. Appending touches four bytes.
  The price is a dead copy of the directory in the file.
- **In place, not through a temporary copy.** A copy and a rename would be
  as safe, but costs writing the whole file again, and loses hard links
  and whatever attributes the copy does not carry. The order of the two
  writes, with a sync between them, gives the same safety.
- **A checksum that is there is never replaced**, even when it is wrong: a
  wrong one is the evidence that something happened to the file.
- **The note is text.** Fifty-odd bytes more than a binary record, and
  anyone who finds an unknown tag in a file can read what it is and check
  it with `md5sum`.
- **The note covers the sidecar, not the options.** A changed rating
  should bring a new DNG; a different `--preview-size` on the command line
  is a decision about new files, not a change of the old ones. Going by
  dates behaves the same.
- **MD5**, as in DNG's own checksums: the purpose is to notice change, and
  every system has a tool to compute it.
- **`--update` stays with dates by default.** It costs nothing and is what
  the option has always done; reading every source file is something to
  ask for.

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

**`check` has three verdicts, not two.** "Intact" is only said when a
checksum was there and matched. A file without one, or with raw data in a
form this program cannot decode, is "not verified": it is not damaged as far
as anyone can tell, and it would be wrong to count it either way. The exit
status follows damage only.

**Reports in list order, whatever finishes first.** A run over a thousand
files is read afterwards, compared with the previous run, or searched for
the one failure. That only works if the output does not depend on timing,
so each job's messages are held back until every job before it has
reported.

**Four files at once by default.** A file being converted is in memory in
full, and people run this on laptops. The thread count is the number of
cores; the number of files is capped, and `-p` lifts the cap.

**`--update` goes by time, not by content.** Comparing content would mean
decoding every source again, which is what the option is there to avoid.
The cost is that it trusts file times.

**`--keep-times` is opt-in.** A new file with an old date surprises backup
and sync tools that go by time.

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
| Unit tests (15 programs) | CTest, on Linux locally and on macOS and Windows through the GitHub workflows | Encoder against a reference decoder, TIFF layout byte by byte, source parser on hand-built and damaged files, MD5, digests and thumbnail colours, embedded originals, lens corrections, the check with its decoder, the job runner, sidecars, checksums added to files in place and the note of a DNG's source, lossy JPEG XL (curve, opcode bytes, linear values, layout, the verification rule against simulated faults), the program itself on generated DNGs, DNG round trips through LibRaw in both byte orders |
| `--verify` on every sample | LibRaw 0.21.2 and 0.22.0 | Every sample of the frame survives |
| Render comparison | rawpy (LibRaw 0.22.1) | Source and DNG develop to the same 8-bit picture, so levels, matrix, white balance and pattern agree |
| Reference reader | Adobe `dng_validate`, DNG SDK 1.5.1 | Adobe's own code decodes the raw data, recomputes the image digest, reads the previews and both maker-note copies. No errors; three warnings about values the cameras wrote |
| Structure | ExifTool 12.76 `-validate` | No error. Warnings beyond those the source file already has: two minor ones about the private-data copy of the Samsung maker note, and since 0.10.0 a minor one in every file about the private tag with the note of the source |
| Independent decoders | darktable 4.6.1 (rawspeed), RawTherapee 5.10 | The files open elsewhere, with the right colours and orientation, including big-endian and uncompressed ones |
| Thumbnail | Pillow, reading IFD0 as a plain TIFF | A generic TIFF reader finds and shows the thumbnail; looked at next to the camera previews for all 15 files |
| Metadata comparison | ExifTool 12.76, exiv2 0.27.6 | Each maker-note, EXIF and GPS tag has the same value in source and DNG |
| Reversibility | `dngconv extract`, ExifTool 12.76 `-OriginalRawImage`, `cmp` | The embedded copy comes back identical to the camera file, through our reader and through an independent one |
| Lens corrections, geometry | Adobe `dng_validate` (render with opcodes), darktable 4.6.1 (lens module, embedded metadata), OpenCV SIFT matching against the camera's JPEG | Two independent readers apply our opcodes and arrive at the camera's own geometry and framing |
| Lens corrections, colour fringes and brightness | Phase correlation between the sensor's colour planes; noise statistics; brightness of the camera JPEG against the render | The sign and size of the aberration parameters; that Sony's vignetting gain is already in the data |
| The `check` command | LibRaw, Adobe `dng_validate` (as reader and as writer), camera-written DNGs | See the 0.5.1 history: the decoder, both digests and the verdicts agree with independent code |
| Checksums written into existing files (`check --add`) | Adobe's code as the writer of 25 files whose checksums are reproduced; `dng_validate` 1.5.1 and 1.7.1, ExifTool, exiv2, LibRaw and darktable as readers of six camera files before and after | The arithmetic is Adobe's for every kind of raw data dngconv reads, and a file is the same file afterwards; see the 0.10.0 history |
| Lossy JPEG XL | Adobe `dng_validate` 1.7.1 as reader (stage 2 against the lossless file's), libjxl 0.7.0 and 0.11.1 each decoding the other's files, simulated layout faults | The opcode and levels written make Adobe's reader arrive at the original linear values within coding error, and `--verify` tells coding error from misplaced data; see the 0.11.0 history |
| JPEG XL | Adobe `dng_validate` 1.7.1 as reader (stage 1 image compared) and as writer; libjxl 0.7.0 and 0.11.1 | The reference reader gets the same raw data from a JPEG XL DNG as from the lossless-JPEG one, and dngconv reads Adobe's files; see the 0.9.0 history |
| Rendered previews | libjpeg (Pillow, OpenCV), OpenCV SIFT matching against the camera's JPEG | The encoder writes what libjpeg would; the rendered picture has the camera's geometry and, within the spread between cameras, its tones; see the 0.8.0 history |
| Camera profiles | ExifTool (tag values, and as the writer of a reference DNG), Adobe `dng_validate` (parsing, rendering, rewriting) | The embedded profile is the `.dcp` file's, and is developed as such; see the 0.7.0 history |
| Memory and undefined behaviour | GCC 13 ASan and UBSan | Clean on the tests (leak detection on), on full conversions with and without `-e`, on extraction, on truncated and byte-flipped copies of real raw files, and on `extract` and `-i` run over 60 truncated or overwritten copies of a DNG with an embedded original, on `-i` run over 300 damaged copies of the five files with lens data, the damage aimed at the directories the lens parsers read, on `check` run over 300 damaged copies of six DNGs from three writers, and on `check --add` run over 420 damaged copies of seven DNGs from four writers, where every copy also had to come out either untouched or sound |
| Threads | GCC 13 thread sanitizer | Parallel conversion (with the checksum of the source taken on its own thread), checking, adding checksums and extraction, and the unit tests, with LibRaw's own OpenMP threads excluded |
| Second compiler | Clang 18 | No warnings at `-Wall -Wextra -Wpedantic` |

`dng_validate` is not part of this repository and is not needed to build or
test dngconv. To build it, compile every `.cpp` in the SDK's `source` folder
into one program with `qLinux=1 qDNGValidateTarget=1 qDNGUseLibJPEG=1
qDNGThreadSafe=1 qDNGUseStdInt=1 qDNGLittleEndian=1`, link it with the XMP
SDK, libjpeg and zlib, and force-include `<cctype>`, `<cstring>`, `<cstdlib>`
and `<cstdio>` for current compilers. `dng_validate -v file.dng` prints every
tag it reads.

The scripts for the checks that need camera files are in `tools/`, with a
driver (`tools/verify.sh`) that runs them all and a recipe for building the
validator; `tools/README.md` says what each proves. The measurements made
once for a particular question (noise statistics for Sony's vignetting, the
brightness comparison against the JPEGs, the knot-spacing experiments) were
not kept as tools; the history sections describe them closely enough to
redo.

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
- JPEG XL DNGs in Lightroom and Camera Raw (their reading code is what the
  validator runs), and in any other program: none that reads DNG 1.7 was
  at hand besides Adobe's SDK. libjxl on macOS and Windows: the build
  logic for static libraries there was written without a machine to try it
  on.
- Rendered previews in the programs they are meant for: no file browser
  or photo manager on any system has been shown one.
- Camera profiles in Lightroom's and Camera Raw's menus, Adobe's own
  profiles (none at hand), profiles with three illuminants or the DNG 1.7
  tables (accepted and carried, never met in a real file).
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

Without JPEG XL, and against a particular libjxl:

```sh
cmake -S . -B build-nojxl -DDNGCONV_JXL=OFF
PKG_CONFIG_PATH=/path/to/libjxl/lib/pkgconfig cmake -S . -B build-jxl -DDNGCONV_JXL=ON
```

Against a self-built LibRaw, to check a second version:

```sh
cmake -S . -B build22 -DLIBRAW_INCLUDE_DIR=/path/to/LibRaw \
      "-DLIBRAW_LIBRARY=/path/to/LibRaw/lib/libraw_r.a;z"
```

After a change, with sample files at hand:

```sh
tools/verify.sh /path/to/samples        # every check there is a tool for
```

It needs ExifTool, Python with the packages in `tools/requirements.txt`, and
Adobe's validator (`tools/dng-validate/build.sh`); steps whose tools are
missing are skipped.

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
2. builds on three runners. LibRaw, zlib, libjxl and what they need besides
   (Little CMS, JasPer, Highway, Brotli) are compiled from source by vcpkg and linked statically, so the
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

What has been tried: the workflow built and published every release from
0.2.0 to 0.5.0, the last two with zlib as a second dependency, without
needing a change.

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
