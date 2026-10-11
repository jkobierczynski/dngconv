// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Lossless JPEG (ITU-T T.81 process 14, SOF3, Huffman), the compression DNG
// uses for raw data ("LJ92"): an encoder that always uses predictor 1, and a
// decoder for whatever a DNG writer may have chosen.
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

struct LosslessJpegFrame {
    uint32_t width = 0;       // samples per line, per component
    uint32_t height = 0;      // lines
    uint32_t components = 0;  // interleaved components, 1..4
    unsigned precision = 0;   // bits per sample, 2..16
    unsigned predictor = 0;   // 1..7
};

/// Decodes a lossless JPEG stream into width * height * components samples,
/// components interleaved, line after line; `frame` receives the dimensions.
///
/// Handles every predictor, restart intervals that end on line boundaries,
/// and up to four components in one interleaved scan. Throws
/// std::runtime_error describing the problem for anything malformed,
/// truncated or outside that (other JPEG processes, a point transform,
/// subsampled components), and for frames with more than `maxSamples`
/// samples. Safe on arbitrary input.
std::vector<uint16_t> decodeLosslessJpeg(const uint8_t* data, size_t size,
                                         LosslessJpegFrame& frame, size_t maxSamples);

}  // namespace dngconv
