// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Lossless JPEG encoder (ITU-T T.81 process 14, SOF3, Huffman, predictor 1),
// the compression DNG uses for raw data ("LJ92").
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dngconv {

/// Encodes `height` rows of `width * components` interleaved 16-bit samples.
///
/// `data` points at the first sample; consecutive rows are `rowStride`
/// samples apart. `precision` is the sample precision written to the frame
/// header (2..16); every sample must fit in that many bits. Each component
/// gets its own Huffman table, optimised for the data.
///
/// For a Bayer mosaic pass components = 2 and width = half the pixel width:
/// each component is then predicted from the previous pixel of the same
/// colour, which is what makes the scheme effective on CFA data.
std::vector<uint8_t> encodeLosslessJpeg(const uint16_t* data, uint32_t width, uint32_t height,
                                        uint32_t components, size_t rowStride,
                                        unsigned precision);

}  // namespace dngconv
