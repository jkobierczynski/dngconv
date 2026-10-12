// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Writes a RawImage as a Digital Negative (DNG) file.
#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "camera_profile.hpp"

#include "lens_opcodes.hpp"
#include "raw_image.hpp"

namespace dngconv {

enum class DngCompression {
    None,          // uncompressed 16-bit samples, one strip
    LosslessJpeg,  // lossless JPEG in tiles
    // Lossless JPEG XL in tiles, as DNG 1.7 defines it: smaller, but only
    // readers of DNG 1.7 can open the file. Needs a build with libjxl
    // (jxlAvailable()), and one or three samples per pixel.
    JpegXl,
};

enum class DngPreview {
    // The camera's own JPEG when the source has one of a useful size,
    // otherwise a picture rendered from the raw data.
    Auto,
    Camera,  // the camera's JPEG, or no preview
    Render,  // always rendered from the raw data
    None,
};

enum class DngByteOrder {
    // Big-endian when a maker note from a big-endian source is carried over,
    // little-endian otherwise. Some maker notes (Nikon's, for one) contain
    // binary tables that readers decode in the byte order of the file around
    // them, so the DNG has to keep the order the camera used.
    MatchSource,
    Little,
    Big,
};

struct DngWriteOptions {
    DngCompression compression = DngCompression::LosslessJpeg;
    /// Largest tile edge in pixels for compressed output. Must be even,
    /// 64..8192. Frames smaller than this are stored as a single tile.
    uint32_t tileSize = 512;
    /// libjxl's effort for JPEG XL, 1 (fastest) to 9 (smallest).
    int jxlEffort = 3;
    /// JPEG XL distance: 0 for lossless; above 0 (up to 25) the raw data is
    /// stored lossily, prepared as described in lossy_jxl.hpp. Needs an
    /// image lossyJxlSupported() accepts.
    float jxlDistance = 0.0f;
    /// Worker threads for compression; 0 = one per CPU core.
    unsigned threads = 0;
    /// Render a small RGB thumbnail from the raw data and store it in the
    /// first image directory, where file browsers look for it.
    bool embedThumbnail = true;
    /// The JPEG preview stored next to the raw image.
    DngPreview preview = DngPreview::Auto;
    /// Longest edge of a rendered preview, in pixels; 0 = as large as the
    /// renderer can make it (half the sensor's width, a third for X-Trans).
    uint32_t previewSize = 1024;
    /// JPEG quality of a rendered preview, 1..100.
    int previewQuality = 90;
    /// Write NewRawImageDigest and RawDataUniqueID.
    bool digests = true;
    /// Store RawImage::originalFile, when it is not empty, so the source file
    /// can be recovered from the DNG.
    bool embedOriginal = true;
    /// Carry over the camera maker's private metadata block. The rest of the
    /// copied metadata (EXIF, GPS) is written whenever the RawImage has it.
    bool makerNotes = true;
    /// Which of the camera's lens corrections (RawImage::lens) to pass on to
    /// the reader as opcodes.
    LensCorrectionMode lensCorrections = LensCorrectionMode::Auto;
    /// Store RawImage::xmpSidecar, when it is not empty, as the DNG's XMP
    /// metadata, in place of any XMP the source file itself held.
    bool xmpSidecar = true;
    /// Camera profiles to store in the DNG. The first becomes the profile of
    /// the file and takes the place of the decoder's plain colour matrix; the
    /// others are stored next to it for the raw developer to offer
    /// (ExtraCameraProfiles). Each must describe as many colour planes as the
    /// image has. Ignored for monochrome images.
    std::vector<std::shared_ptr<const CameraProfile>> profiles;
    /// Note in the DNG which source file it was made from: the size and MD5
    /// of the file (RawImage::sourceFileDigest, when the reader took it) and
    /// the MD5 of the sidecar stored with it. See source_record.hpp.
    bool sourceRecord = true;
    DngByteOrder byteOrder = DngByteOrder::MatchSource;
    /// Value of the TIFF Software tag.
    std::string software = "dngconv";
};

/// Checks that the image is internally consistent (sizes, pattern lengths,
/// areas inside the frame). Throws std::invalid_argument describing the first
/// problem found.
void validate(const RawImage& image);

/// What writeDng() will store as the preview of an image.
struct PreviewPlan {
    enum class Kind { None, Camera, Rendered } kind = Kind::None;
    uint32_t width = 0;
    uint32_t height = 0;
};
PreviewPlan planPreview(const RawImage& image, const DngWriteOptions& options);

/// Writes the DNG to a stream. Throws on invalid input or write failure.
void writeDng(const RawImage& image, std::ostream& out, const DngWriteOptions& options = {});

/// Writes the DNG to `path` through a temporary file in the same directory,
/// so an interrupted run never leaves a truncated DNG behind.
void writeDng(const RawImage& image, const std::filesystem::path& path,
              const DngWriteOptions& options = {});

}  // namespace dngconv
