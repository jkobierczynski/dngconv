// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Renders an sRGB picture straight from the raw data: the thumbnail DNG
// keeps in its first image directory, and the preview for files whose camera
// stored none. It is a quick look, not a development: every output pixel is
// an average over whole periods of the colour mosaic instead of a
// demosaiced value (so the picture is at most half as wide as the sensor,
// a third for X-Trans), one colour matrix, the as-shot white balance, a
// fixed tone curve modelled on what cameras do. Exposure is left as shot.
#pragma once

#include <cstdint>
#include <vector>

#include "lens_opcodes.hpp"
#include "raw_image.hpp"

namespace dngconv {

struct RgbImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // 8-bit R, G, B per pixel, row-major
};

struct RenderOptions {
    /// Longest edge of the result, in pixels; 0 = as large as the raw data
    /// allows. A picture is never enlarged.
    uint32_t maxSize = 256;
    /// Lift the mid-tones and deepen the shadows, the way cameras render
    /// their JPEGs. Without it values are only gamma-encoded, which looks
    /// dark and flat.
    bool toneCurve = true;
    /// The lens corrections the DNG will carry, to be applied to the picture
    /// as a DNG reader would (distortion and vignetting; colour fringes are
    /// smaller than a pixel at this size). Null for none.
    const LensOpcodes* lens = nullptr;
    /// Worker threads; 0 = one per CPU core.
    unsigned threads = 1;
};

/// The size renderPicture() would give. False if the frame cannot be
/// rendered (inconsistent, or smaller than one period of its mosaic).
bool renderedSize(const RawImage& image, uint32_t maxSize, uint32_t& width, uint32_t& height);

/// Shows the default-crop area of the image, in sensor orientation (the DNG
/// Orientation tag applies to it as it does to the raw data). Returns an
/// empty image if the frame cannot be rendered.
RgbImage renderPicture(const RawImage& image, const RenderOptions& options = {});

/// The tone curve of the rendering: linear light in (0..1), linear light
/// out. Exposed for the tests.
double renderToneCurve(double linear);

}  // namespace dngconv
