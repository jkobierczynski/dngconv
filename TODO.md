# To do

Open work on dngconv, most useful first within each section. Tick an item
when it is done and move the story of how it went to the history in
`DEVELOPMENT.md`; this file only lists what is left.

Last updated: 2026-10-11, at version 0.8.0.

## Needs someone with the software or an account

These cannot be done from a Linux machine without Adobe software, and they
close the largest gaps in the verification.

- [ ] **Convert camera files on macOS and Windows.** The unit tests pass
      there; no real file has gone through the released binaries.
- [ ] **Open the DNGs in Lightroom or Camera Raw.** Adobe's validator accepts
      them; what remains to be seen is colour, the thumbnail and preview in
      the library, and what the applications make of the lens opcodes
      (whether they report a built-in profile, and how the result compares
      with their own profile for the same lens).
- [ ] **Camera profiles in Lightroom and Camera Raw.** Convert with
      `--profile` and two or three of Adobe's own profiles: do they all show
      in the profile menu, is the first the default, and how does the
      picture compare with an Adobe-made DNG of the same file (the
      `BaselineExposure` question below)?
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
- [ ] **Rendered previews: what the first version left open.**
      - Full-resolution previews, which need demosaicing (Bayer first;
        X-Trans is a larger piece of work).
      - Rendering with the embedded camera profile's matrices and tone
        curve instead of the decoder's matrix and the built-in curve.
      - Per-camera brightness: the built-in curve is the median of thirteen
        cameras, and single cameras are up to a stop away from it.
      - Sharpening for small sizes, and a quality setting on the command
        line (the writer has one, fixed at 90).
      - Look at them in file browsers and photo managers on the three
        systems.
- [ ] **A second calibration illuminant** (`ColorMatrix2`, standard light A)
      for files converted without `--profile`. LibRaw's table has only D65;
      the data would have to come from elsewhere.
- [ ] **Camera profiles: what the first version left open.**
      - `BaselineExposure` per camera, which Adobe's profiles are tuned for.
        Could be taken from a profile-specific table, or from an Adobe-made
        DNG of the same camera given as a template.
      - A table of camera names that differ between LibRaw and Adobe, so
        that `--profile-any-camera` is needed less often.
      - Rendering the thumbnail (and later the preview) with the embedded
        profile's matrices instead of the decoder's.
      - Choosing the default profile by name (`--default-profile "Camera
        Standard"`) instead of by order.
- [ ] **Multi-frame files** beyond their first frame (pixel shift, dual
      exposure), **Fuji Super CCD** with the diagonal layout, and
      **floating-point raws**.
- [ ] **JPEG XL compression** (DNG 1.7).
- [ ] **A library interface**, so other programs can use the reader and the
      writer without the command line.

## Smaller items

- [ ] `check`: decode Deflate-compressed and floating-point raw data, and
      compare lossy-JPEG raw data with its own digest (`RawJPEGImageDigest`),
      so that fewer files end up "not verified".
- [ ] `check --add`: write the checksums into a DNG that has none, so that
      camera-written DNGs can be watched from then on.
- [ ] Tile sizes that divide the frame evenly, so that no padding is needed
      at all (it is cheap since 0.5.1, not free).
- [ ] Embed a camera's `.THM` sidecar with the original (forks 5 to 8 of
      `OriginalRawFileData` exist for that).
- [ ] Stream the original instead of holding the file and its packed copy in
      memory.
- [ ] A `--byte-order` switch.
- [ ] `--update` by content (a checksum of the source stored in the DNG)
      for archives whose file times cannot be trusted.
- [ ] Choose the number of files converted at once from the memory that is
      free, instead of the fixed four.
- [ ] Detect an input that already is a DNG before decoding it.
- [ ] Sigma X3F: document or automate building LibRaw with X3F support.

## Verification and tooling

- [ ] Run `tools/verify.sh` on macOS and Windows (Git Bash or WSL); it has
      only been run on Linux.
- [ ] A way to compare the raw samples a DNG stores with what LibRaw reads
      from it, as was done by hand for the `check` command (it needs a small
      program that dumps `readStoredRawImage`).
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
