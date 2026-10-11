// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "dng_digest.hpp"

#include <algorithm>
#include <vector>

#include "parallel.hpp"

namespace dngconv {

Md5Digest newRawImageDigest(const uint16_t* samples, uint32_t width, uint32_t height,
                            uint32_t planes, unsigned bytesPerSample, unsigned threads) {
    const uint32_t tileW = std::min<uint32_t>(256, width);
    const uint32_t tileH = std::min<uint32_t>(256, height);
    if (tileW == 0 || tileH == 0 || planes == 0) return Md5().finish();
    const uint32_t across = (width + tileW - 1) / tileW;
    const uint32_t down = (height + tileH - 1) / tileH;
    std::vector<Md5Digest> tileDigests(static_cast<size_t>(across) * down);

    parallelFor(tileDigests.size(), threads, [&](size_t index) {
        const uint32_t left = static_cast<uint32_t>(index % across) * tileW;
        const uint32_t top = static_cast<uint32_t>(index / across) * tileH;
        const uint32_t w = std::min(tileW, width - left);
        const uint32_t h = std::min(tileH, height - top);
        std::vector<uint8_t> bytes(static_cast<size_t>(w) * h * planes * bytesPerSample);
        size_t at = 0;
        for (uint32_t plane = 0; plane < planes; ++plane)
            for (uint32_t y = 0; y < h; ++y) {
                const uint16_t* row = samples + (static_cast<size_t>(top + y) * width + left) * planes + plane;
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t v = row[static_cast<size_t>(x) * planes];
                    bytes[at++] = static_cast<uint8_t>(v & 0xff);
                    if (bytesPerSample == 2) bytes[at++] = static_cast<uint8_t>(v >> 8);
                }
            }
        tileDigests[index] = Md5::of(bytes.data(), bytes.size());
    });

    Md5 all;
    for (const Md5Digest& d : tileDigests) all.update(d.data(), d.size());
    return all.finish();
}

Md5Digest rawImageDigest(const uint16_t* samples, uint32_t width, uint32_t height,
                         uint32_t planes) {
    Md5 md5;
    const size_t perRow = static_cast<size_t>(width) * planes;
    std::vector<uint8_t> bytes(perRow * 2);
    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t* row = samples + static_cast<size_t>(y) * perRow;
        for (size_t i = 0; i < perRow; ++i) {
            bytes[2 * i] = static_cast<uint8_t>(row[i] & 0xff);
            bytes[2 * i + 1] = static_cast<uint8_t>(row[i] >> 8);
        }
        md5.update(bytes.data(), bytes.size());
    }
    return md5.finish();
}

}  // namespace dngconv
