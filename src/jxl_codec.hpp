// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// JPEG XL through libjxl, for the raw data of DNG 1.7 files: lossless
// encoding of 16-bit samples, and decoding for the check. libjxl is an
// optional dependency; in a build without it the two coding functions throw
// and jxlAvailable() says so beforehand.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dngconv {

/// Whether this build can encode and decode JPEG XL.
bool jxlAvailable();

/// Version of libjxl in use, "0.11.1"; empty in a build without it.
std::string jxlVersion();

/// Encodes `width` x `height` pixels of `channels` (1 or 3) 16-bit samples,
/// interleaved, as JPEG XL data that decodes to exactly the same values.
/// `effort` is libjxl's 1 (fast) to 9 (small). The result is a codestream
/// in the small container libjxl puts around data that needs level 10 of
/// the format, which sixteen lossless bits do; Adobe's writer stores the
/// same. Throws std::runtime_error on failure or in a build without libjxl.
std::vector<uint8_t> encodeJxlLossless(const uint16_t* samples, uint32_t width, uint32_t height,
                                       uint32_t channels, int effort);

struct JxlPicture {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    unsigned bitsPerSample = 0;
    /// Stored in its own colour encoding, as lossless data is. Pictures coded
    /// the lossy way decode to slightly different values from one version of
    /// the decoder to the next.
    bool originalValues = false;
    std::vector<uint16_t> samples;  // interleaved
};

/// Decodes a JPEG XL codestream or container to 16-bit samples. Throws
/// std::runtime_error for data that does not decode, for pictures of more
/// than `maxSamples` samples or other than 1 or 3 channels, and in a build
/// without libjxl.
JxlPicture decodeJxl(const uint8_t* data, size_t size, size_t maxSamples);

}  // namespace dngconv
