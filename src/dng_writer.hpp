// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Writes a RawImage as a Digital Negative (DNG) file.
#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "lens_opcodes.hpp"
#include "raw_image.hpp"

namespace dngconv {

enum class DngCompression {
    None,          // uncompressed 16-bit samples, one strip
    LosslessJpeg,  // lossless JPEG in tiles
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
    /// Worker threads for compression; 0 = one per CPU core.
    unsigned threads = 0;
    /// Render a small RGB thumbnail from the raw data and store it in the
    /// first image directory, where file browsers look for it.
    bool embedThumbnail = true;
    /// Store the source file's JPEG preview, when the RawImage carries one.
    bool embedPreview = true;
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
    DngByteOrder byteOrder = DngByteOrder::MatchSource;
    /// Value of the TIFF Software tag.
    std::string software = "dngconv";
};

/// Checks that the image is internally consistent (sizes, pattern lengths,
/// areas inside the frame). Throws std::invalid_argument describing the first
/// problem found.
void validate(const RawImage& image);

/// Writes the DNG to a stream. Throws on invalid input or write failure.
void writeDng(const RawImage& image, std::ostream& out, const DngWriteOptions& options = {});

/// Writes the DNG to `path` through a temporary file in the same directory,
/// so an interrupted run never leaves a truncated DNG behind.
void writeDng(const RawImage& image, const std::filesystem::path& path,
              const DngWriteOptions& options = {});

}  // namespace dngconv
