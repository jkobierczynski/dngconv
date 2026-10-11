// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// A baseline JPEG encoder for the previews dngconv renders itself: 8-bit
// YCbCr with the colour planes at half resolution (4:2:0), the layout every
// JPEG reader knows. The Huffman tables are built for each picture instead
// of taken from the standard's examples, which makes files a little smaller
// and the encoder independent of any table but the two quantisation ones.
//
// This is the ordinary, lossy JPEG. The lossless kind that stores the raw
// data is in ljpeg92.hpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dngconv {

/// Encodes `width` x `height` pixels of 8-bit R, G, B (row-major) as a JFIF
/// file. `quality` is the usual 1..100 scale. Throws std::invalid_argument
/// for an empty picture or one larger than JPEG's 65535 pixels per side.
std::vector<uint8_t> encodeJpeg(const uint8_t* rgb, uint32_t width, uint32_t height, int quality = 90,
                                unsigned threads = 1);

}  // namespace dngconv
