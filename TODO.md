# To do

Open work on dngconv, most useful first within each section. Tick an item
when it is done and move the story of how it went to the history in
`DEVELOPMENT.md`; this file only lists what is left.

Last updated: 2026-10-09, at version 0.5.0.

## Needs someone with the software or an account

These cannot be done from a Linux machine without Adobe software, and they
close the largest gaps in the verification.

- [ ] **Tag a release that includes zlib.** 0.4.0 was committed without a
      tag, so the release workflow has not yet built the packages with the
      second dependency. Running `release.yml` by hand ("Run workflow")
      builds them without publishing anything.
- [ ] **Convert camera files on macOS and Windows.** The unit tests pass
      there; no real file has gone through the released binaries.
- [ ] **Open the DNGs in Lightroom or Camera Raw.** Adobe's validator accepts
      them; what remains to be seen is colour, the thumbnail and preview in
      the library, and what the applications make of the lens opcodes
      (whether they report a built-in profile, and how the result compares
      with their own profile for the same lens).
- [ ] **Embedded originals and Adobe's DNG Converter**, both directions:
      extract one of ours with "Extract originals", and run `dngconv extract`
      on a DNG that Adobe's converter embedded an original in.
- [ ] **Unicode file names on Windows**, for conversion and for `extract`.
- [ ] **A Samsung DNG made by Adobe's converter**, to see how it stores the
      maker note in `DNGPrivateData`. ExifTool misreads our copy of it.

## Features

- [ ] **Lens corrections: what the first version left open.**
      - Sample files for the paths no real file has exercised: a Fuji body
        with the nine-knot tables (X-Trans IV and later), a Fuji file from a
        cropped shooting mode, a Sony file with shading compensation off, a
        Sony full-frame body in APS-C crop mode.
      - Fuji: confirm the vignetting gain on a lens that needs more than the
        7 % of the sample, and find out why the measured blue fringes are
        about half the size the table gives.
      - Other makers. Whether Nikon Z, Canon RF, Samsung, Pentax or Leica
        files hold parameters that can be read has not been looked into.
      - Vignetting for Panasonic and Olympus, whose "shading compensation"
        may be applied to the raw data by the camera, as Sony's is.
      - An Adobe-made DNG of one of the sample files, to compare its opcodes
        with ours.
- [ ] **Maker data outside the maker note.** Sony `SR2Private`, which Adobe
      stores as its own block in `DNGPrivateData`; the Canon CR3
      timed-metadata track (`CTMD`); the tags of the RAF and RW2 containers;
      the Samsung preview directory.
- [ ] **A JPEG preview for files that carry none**, and re-rendering previews
      at a chosen size. Needs a JPEG encoder.
- [ ] **A second calibration illuminant** (`ColorMatrix2`, standard light A).
      LibRaw's table has only D65; the data would have to come from elsewhere.
- [ ] **Multi-frame files** beyond their first frame (pixel shift, dual
      exposure), **Fuji Super CCD** with the diagonal layout, and
      **floating-point raws**.
- [ ] **JPEG XL compression** (DNG 1.7).
- [ ] **A library interface**, so other programs can use the reader and the
      writer without the command line.

## Smaller items

- [ ] Convert several files in parallel (today only the tiles of one file
      are compressed in parallel).
- [ ] Keep file timestamps, for DNGs and for extracted originals.
- [ ] Pick up an XMP sidecar next to the source file.
- [ ] Embed a camera's `.THM` sidecar with the original (forks 5 to 8 of
      `OriginalRawFileData` exist for that).
- [ ] Stream the original instead of holding the file and its packed copy in
      memory.
- [ ] A `--byte-order` switch.
- [ ] Detect an input that already is a DNG before decoding it.
- [ ] Sigma X3F: document or automate building LibRaw with X3F support.

## Verification and tooling

- [ ] **Bring the comparison scripts into the repository** as a `tools/`
      folder (render comparison, metadata comparison, and the lens checks:
      matching a render against the camera's JPEG, measuring colour fringes
      on the sensor planes), with a recipe for building `dng_validate`, so
      the checks in `DEVELOPMENT.md` can be repeated by anyone with sample
      files.
- [ ] Find sample files for the code paths no real file has exercised:
      four-colour sensors (CMYG, RGBE), monochrome sensors, non-square pixels,
      a source file with XMP.
- [ ] Check the licences of the sample files, or replace them with files
      that may be redistributed, so a regression set can live next to the
      code.
- [ ] An Intel Mac build in the release workflow (`x64-osx-release` on an
      Intel runner).
- [ ] Sign and notarise the macOS binary, sign the Windows one.

## Questions to settle

- [ ] **Is the private-data copy of the maker note needed?** Adobe's SDK
      reads both copies. Dropping it would silence an exiv2 message and save
      space; keeping it protects the note when a program rewrites the DNG.
      Kept for now.
- [ ] **`MakerNoteSafety`.** Not written, which means "unsafe to preserve".
      Cautious for notes with absolute pointers, but it tells a rewriting
      program to drop the EXIF copy.
- [ ] **White level policy.** The camera's own level is used when it is lower
      than the format maximum. Needs more files with blown highlights to see
      whether that is what people expect.
- [ ] **Should lens opcodes be mandatory, as Adobe's converter writes
      them?** They are flagged optional now, which lets a reader skip them.
- [ ] **Name.** `dngconv` is a working name.
- [ ] **Licence.** GPL-3.0-or-later was chosen without discussion. A library
      interface would raise the question again.
