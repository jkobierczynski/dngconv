// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Decodes a camera raw file into a RawImage, using LibRaw.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "raw_image.hpp"

namespace dngconv {

struct RawReadOptions {
    /// Extract the largest embedded JPEG preview, if the file has one.
    bool loadPreview = true;
    /// Collect the source's EXIF and GPS directories and its maker note.
    bool copyMetadata = true;
    /// Skip pixel decoding; only metadata and geometry are filled in.
    bool metadataOnly = false;
};

struct RawReadResult {
    RawImage image;
    /// Non-fatal problems worth telling the user about.
    std::vector<std::string> warnings;
    /// True when the source file already is a DNG.
    bool sourceIsDng = false;
    /// Number of raw frames in the file (only the first is decoded).
    unsigned frameCount = 1;
};

/// Throws std::runtime_error when the file cannot be decoded or uses a sensor
/// layout this program does not handle yet.
RawReadResult readRaw(const std::filesystem::path& path, const RawReadOptions& options = {});

/// Version of the LibRaw library in use, e.g. "0.21.2-Release".
std::string decoderVersion();

/// Number of camera models the decoder knows.
int supportedCameraCount();

}  // namespace dngconv
