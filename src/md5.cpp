// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "md5.hpp"

#include <cstring>

namespace dngconv {

namespace {

// Per-round shift amounts and the sine-derived constants of RFC 1321.
const uint32_t kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                             5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                             4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                             6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

const uint32_t kSine[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

inline uint32_t rotateLeft(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

}  // namespace

Md5::Md5() {
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
}

void Md5::transform(const uint8_t* block) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = static_cast<uint32_t>(block[4 * i]) | (static_cast<uint32_t>(block[4 * i + 1]) << 8) |
               (static_cast<uint32_t>(block[4 * i + 2]) << 16) |
               (static_cast<uint32_t>(block[4 * i + 3]) << 24);

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const uint32_t next = b + rotateLeft(a + f + kSine[i] + m[g], kShift[i]);
        a = d;
        d = c;
        c = b;
        b = next;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(const void* data, size_t length) {
    if (length == 0) return;  // also covers a null pointer from an empty container
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t fill = static_cast<size_t>(length_ & 63);
    length_ += length;

    if (fill) {
        const size_t take = length < 64 - fill ? length : 64 - fill;
        std::memcpy(buffer_ + fill, p, take);
        p += take;
        length -= take;
        fill += take;
        if (fill < 64) return;
        transform(buffer_);
    }
    for (; length >= 64; p += 64, length -= 64) transform(p);
    if (length) std::memcpy(buffer_, p, length);
}

Md5Digest Md5::finish() {
    const uint64_t bits = length_ * 8;
    static const uint8_t padding[64] = {0x80};
    const size_t fill = static_cast<size_t>(length_ & 63);
    update(padding, fill < 56 ? 56 - fill : 120 - fill);

    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) lengthBytes[i] = static_cast<uint8_t>(bits >> (8 * i));
    update(lengthBytes, 8);

    Md5Digest out;
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) out[4 * i + k] = static_cast<uint8_t>(state_[i] >> (8 * k));
    return out;
}

Md5Digest Md5::of(const void* data, size_t length) {
    Md5 md5;
    md5.update(data, length);
    return md5.finish();
}

}  // namespace dngconv
