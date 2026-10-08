// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Reads metadata straight from a raw file's own TIFF structures, without
// interpreting it:
//
//  * the maker and model strings exactly as the camera wrote them
//    ("NIKON CORPORATION" / "NIKON D3S"). The decoder library normalises
//    these ("Nikon" / "D3S"), which suits UniqueCameraModel but not the TIFF
//    Make and Model tags that other software matches lens profiles on;
//  * the complete EXIF and GPS directories;
//  * the maker note, as an opaque block with the facts a reader needs to
//    decode it elsewhere (byte order and original position).
#pragma once

#include <filesystem>
#include <string>

#include "raw_image.hpp"

namespace dngconv {

struct SourceMetadataResult {
    std::string make;
    std::string model;
    SourceMetadata metadata;
    bool identityFound() const { return !make.empty() && !model.empty(); }
};

/// Understands TIFF-based raw formats (most of them, including Olympus ORF
/// and Panasonic RW2), Canon CR3 and Fuji RAF. Returns an empty result for
/// anything else and whatever could be read on a damaged file; never throws.
SourceMetadataResult readSourceMetadata(const std::filesystem::path& path);

}  // namespace dngconv
