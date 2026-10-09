// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Renders a small sRGB picture straight from the raw data, for the thumbnail
// DNG keeps in its first image directory. It is a quick look, not a
// development: block averaging instead of demosaicing, one matrix, no tone
// curve beyond the sRGB one.
#pragma once

#include <cstdint>
#include <vector>

#include "raw_image.hpp"

namespace dngconv {

struct RgbImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // 8-bit R, G, B per pixel, row-major
};

struct ThumbnailOptions {
    /// Longest edge of the result, in pixels.
    uint32_t maxSize = 256;
    /// Brighten so that the brightest percent of the picture reaches white,
    /// as raw data without a tone curve usually looks dark.
    bool autoExposure = true;
};

/// Shows the default-crop area of the image, in sensor orientation (the DNG
/// Orientation tag applies to the thumbnail as it does to the raw data).
/// Returns an empty image if the frame is too small to render.
RgbImage renderThumbnail(const RawImage& image, const ThumbnailOptions& options = {});

}  // namespace dngconv
