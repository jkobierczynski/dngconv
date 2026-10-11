// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// The fingerprints DNG defines over the stored raw samples. A reader can
// recompute them to tell whether the raw data is still what the writer put
// there. The DNG specification defers to Adobe's SDK for both definitions.
#pragma once

#include <cstdint>

#include "md5.hpp"

namespace dngconv {

/// NewRawImageDigest (DNG 1.4). The frame is cut into tiles of 256 x 256
/// pixels (smaller at the right and bottom edges, and never larger than the
/// frame); each tile's samples are hashed one colour plane after the other;
/// the result is the MD5 of all tile digests in row-major order.
///
/// `samples` holds width * height * planes values, interleaved per pixel.
/// `bytesPerSample` is 2 for data stored with 9 to 16 bits (hashed as
/// little-endian 16-bit values) and 1 for data stored with up to 8 bits.
Md5Digest newRawImageDigest(const uint16_t* samples, uint32_t width, uint32_t height,
                            uint32_t planes, unsigned bytesPerSample, unsigned threads);

/// RawImageDigest, the older scheme (DNG 1.1): the MD5 of all samples in
/// row-scan order, interleaved as stored, each as a little-endian 16-bit
/// value whatever the stored depth.
Md5Digest rawImageDigest(const uint16_t* samples, uint32_t width, uint32_t height,
                         uint32_t planes);

}  // namespace dngconv
