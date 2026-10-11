// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Test-only decoder for baseline JPEG, written from the standard and sharing
// nothing with the encoder under test: its own zigzag table, its own Huffman
// decoding from the tables in the file, a plain inverse DCT. It reads what a
// baseline encoder may write (8 bits, one scan, one to three components, any
// sampling factors, no restart intervals) and refuses the rest with a reason.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace jpegdecoder {

struct Picture {
    bool ok = false;
    std::string error;
    uint32_t width = 0;
    uint32_t height = 0;
    unsigned components = 0;
    unsigned subH = 1, subV = 1;  // chroma subsampling
    std::vector<uint8_t> rgb;     // 8-bit R, G, B
    // What the file's Huffman tables look like: the longest code in use and
    // the number of symbols, over all tables.
    unsigned longestCode = 0;
    unsigned symbols = 0;
    bool jfif = false;
};

namespace detail {

// Coefficient k of the coded order sits at this position of the 8x8 block
// (ISO/IEC 10918-1, figure A.6).
const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                             12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                             35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                             58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Table {
    bool present = false;
    int minCode[17] = {};
    int maxCode[17] = {};  // -1 where there is no code of that length
    int firstIndex[17] = {};
    std::vector<uint8_t> symbols;
};

struct Bits {
    const std::vector<uint8_t>& d;
    size_t pos;
    uint32_t acc = 0;
    int fill = 0;
    bool failed = false;

    int bit() {
        if (fill == 0) {
            if (pos >= d.size()) {
                failed = true;
                return 0;
            }
            uint8_t byte = d[pos++];
            if (byte == 0xff) {
                // A stuffed zero follows a data byte of 0xff; anything else is a marker.
                if (pos < d.size() && d[pos] == 0) {
                    ++pos;
                } else {
                    failed = true;
                    return 0;
                }
            }
            acc = byte;
            fill = 8;
        }
        --fill;
        return (acc >> fill) & 1;
    }
    int receive(int count) {
        int v = 0;
        for (int i = 0; i < count; ++i) v = (v << 1) | bit();
        return v;
    }
    int symbol(const Table& t) {
        int code = 0;
        for (int length = 1; length <= 16; ++length) {
            code = (code << 1) | bit();
            if (t.maxCode[length] >= 0 && code <= t.maxCode[length] && code >= t.minCode[length])
                return t.symbols[static_cast<size_t>(t.firstIndex[length] + code - t.minCode[length])];
        }
        failed = true;
        return 0;
    }
};

inline int extend(int value, int size) { return size && value < (1 << (size - 1)) ? value - (1 << size) + 1 : value; }

inline void inverseDct(const float* in, float* out) {
    static float c[8][8];
    static bool ready = false;
    if (!ready) {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = static_cast<float>((u == 0 ? std::sqrt(0.125) : 0.5) *
                                             std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
        ready = true;
    }
    float tmp[64];
    for (int v = 0; v < 8; ++v)
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) s += c[u][x] * in[v * 8 + u];
            tmp[v * 8 + x] = s;
        }
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            float s = 0;
            for (int v = 0; v < 8; ++v) s += c[v][y] * tmp[v * 8 + x];
            out[y * 8 + x] = s;
        }
}

}  // namespace detail

inline Picture decode(const std::vector<uint8_t>& d) {
    using namespace detail;
    Picture p;
    const auto fail = [&](const std::string& why) {
        p.error = why;
        return p;
    };
    if (d.size() < 4 || d[0] != 0xff || d[1] != 0xd8) return fail("no start-of-image marker");

    uint16_t quant[4][64] = {};
    bool quantPresent[4] = {};
    Table dc[4], ac[4];
    struct Component {
        int id = 0, h = 1, v = 1, q = 0, dcTable = 0, acTable = 0;
        std::vector<float> plane;  // decoded samples, padded to whole units
        uint32_t stride = 0;
    };
    std::vector<Component> comps;
    size_t pos = 2;
    size_t scanStart = 0;

    while (!scanStart) {
        if (pos + 4 > d.size() || d[pos] != 0xff) return fail("marker expected");
        const uint8_t marker = d[pos + 1];
        const size_t length = (static_cast<size_t>(d[pos + 2]) << 8) | d[pos + 3];
        if (length < 2 || pos + 2 + length > d.size()) return fail("segment runs past the end");
        const uint8_t* s = &d[pos + 4];
        const size_t n = length - 2;
        switch (marker) {
            case 0xe0:
                p.jfif = n >= 5 && std::string(s, s + 4) == "JFIF" && s[4] == 0;
                break;
            case 0xdb:
                for (size_t at = 0; at < n;) {
                    const int precision = s[at] >> 4, id = s[at] & 15;
                    if (precision != 0 || id > 3 || at + 65 > n) return fail("quantisation table not 8-bit");
                    for (int k = 0; k < 64; ++k) quant[id][kZigzag[k]] = s[at + 1 + static_cast<size_t>(k)];
                    quantPresent[id] = true;
                    at += 65;
                }
                break;
            case 0xc0: {
                if (n < 6 || s[0] != 8) return fail("not 8 bits per sample");
                p.height = (static_cast<uint32_t>(s[1]) << 8) | s[2];
                p.width = (static_cast<uint32_t>(s[3]) << 8) | s[4];
                p.components = s[5];
                if ((p.components != 1 && p.components != 3) || n < 6 + 3u * p.components || !p.width || !p.height)
                    return fail("unsupported frame");
                for (unsigned i = 0; i < p.components; ++i) {
                    Component c;
                    c.id = s[6 + 3 * i];
                    c.h = s[7 + 3 * i] >> 4;
                    c.v = s[7 + 3 * i] & 15;
                    c.q = s[8 + 3 * i];
                    if (c.h < 1 || c.h > 2 || c.v < 1 || c.v > 2 || c.q > 3) return fail("unsupported sampling");
                    comps.push_back(c);
                }
                break;
            }
            case 0xc4:
                for (size_t at = 0; at < n;) {
                    if (at + 17 > n) return fail("Huffman table cut short");
                    const int kind = s[at] >> 4, id = s[at] & 15;
                    if (kind > 1 || id > 3) return fail("bad Huffman table id");
                    Table& t = (kind ? ac : dc)[id];
                    t = Table();
                    t.present = true;
                    size_t total = 0;
                    int code = 0;
                    for (int len = 1; len <= 16; ++len) {
                        const int count = s[at + static_cast<size_t>(len)];
                        t.firstIndex[len] = static_cast<int>(total);
                        t.minCode[len] = code;
                        t.maxCode[len] = count ? code + count - 1 : -1;
                        // Codes of a length count up from where the shorter ones
                        // left off; they must fit, and none may be all ones.
                        if (count && code + count >= (1 << len))
                            return fail("Huffman table has more codes than fit, or one of all ones");
                        if (count) p.longestCode = std::max<unsigned>(p.longestCode, static_cast<unsigned>(len));
                        code = (code + count) << 1;
                        total += static_cast<size_t>(count);
                    }
                    if (at + 17 + total > n) return fail("Huffman table cut short");
                    t.symbols.assign(s + at + 17, s + at + 17 + total);
                    p.symbols += static_cast<unsigned>(total);
                    at += 17 + total;
                }
                break;
            case 0xdd:
                if (n >= 2 && (s[0] || s[1])) return fail("restart intervals not supported");
                break;
            case 0xda: {
                if (comps.empty() || n < 1 || s[0] != comps.size() || n < 1 + 2 * comps.size() + 3)
                    return fail("unsupported scan");
                for (size_t i = 0; i < comps.size(); ++i) {
                    if (s[1 + 2 * i] != comps[i].id) return fail("scan components out of order");
                    comps[i].dcTable = s[2 + 2 * i] >> 4;
                    comps[i].acTable = s[2 + 2 * i] & 15;
                }
                scanStart = pos + 2 + length;
                break;
            }
            case 0xc1: case 0xc2: case 0xc3: case 0xc9: case 0xca:
                return fail("not a baseline JPEG");
            default:
                break;
        }
        pos += 2 + length;
    }
    if (comps.empty()) return fail("no frame header");

    int hMax = 1, vMax = 1;
    for (const Component& c : comps) {
        hMax = std::max(hMax, c.h);
        vMax = std::max(vMax, c.v);
        if (!quantPresent[c.q] || !dc[c.dcTable].present || !ac[c.acTable].present)
            return fail("a table the scan refers to is missing");
    }
    if (comps.size() == 3) {
        p.subH = static_cast<unsigned>(hMax / comps[1].h);
        p.subV = static_cast<unsigned>(vMax / comps[1].v);
    }
    const uint32_t unitW = 8u * static_cast<uint32_t>(hMax), unitH = 8u * static_cast<uint32_t>(vMax);
    const uint32_t unitsAcross = (p.width + unitW - 1) / unitW, unitsDown = (p.height + unitH - 1) / unitH;
    for (Component& c : comps) {
        c.stride = unitsAcross * 8u * static_cast<uint32_t>(c.h);
        c.plane.assign(static_cast<size_t>(c.stride) * unitsDown * 8u * static_cast<uint32_t>(c.v), 0.0f);
    }

    Bits bits{d, scanStart};
    std::vector<int> previous(comps.size(), 0);
    for (uint32_t uy = 0; uy < unitsDown; ++uy)
        for (uint32_t ux = 0; ux < unitsAcross; ++ux)
            for (size_t ci = 0; ci < comps.size(); ++ci) {
                Component& c = comps[ci];
                for (int by = 0; by < c.v; ++by)
                    for (int bx = 0; bx < c.h; ++bx) {
                        float coefficient[64] = {};
                        const int size = bits.symbol(dc[c.dcTable]);
                        if (size > 11) return fail("DC category out of range");
                        previous[ci] += extend(bits.receive(size), size);
                        coefficient[0] = static_cast<float>(previous[ci] * quant[c.q][0]);
                        for (int k = 1; k < 64;) {
                            const int rs = bits.symbol(ac[c.acTable]);
                            const int run = rs >> 4, s = rs & 15;
                            if (s == 0) {
                                if (run == 15) {
                                    k += 16;
                                    continue;
                                }
                                break;  // end of block
                            }
                            k += run;
                            if (k > 63) return fail("coefficient index out of range");
                            const int at = kZigzag[k];
                            coefficient[at] = static_cast<float>(extend(bits.receive(s), s) * quant[c.q][at]);
                            ++k;
                        }
                        if (bits.failed) return fail("entropy-coded data ends early or holds a marker");
                        float block[64];
                        inverseDct(coefficient, block);
                        const uint32_t x0 = (ux * static_cast<uint32_t>(c.h) + static_cast<uint32_t>(bx)) * 8;
                        const uint32_t y0 = (uy * static_cast<uint32_t>(c.v) + static_cast<uint32_t>(by)) * 8;
                        for (uint32_t y = 0; y < 8; ++y)
                            for (uint32_t x = 0; x < 8; ++x)
                                c.plane[static_cast<size_t>(y0 + y) * c.stride + x0 + x] = block[y * 8 + x] + 128.0f;
                    }
            }

    // What follows the data must be the end-of-image marker (after padding bits).
    size_t end = bits.pos;
    if (end + 2 > d.size() || d[end] != 0xff || d[end + 1] != 0xd9) return fail("no end-of-image marker after the data");
    if (end + 2 != d.size()) return fail("bytes after the end-of-image marker");

    p.rgb.resize(static_cast<size_t>(p.width) * p.height * 3);
    const auto clamp8 = [](float v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    for (uint32_t y = 0; y < p.height; ++y)
        for (uint32_t x = 0; x < p.width; ++x) {
            float v[3] = {0, 128, 128};
            for (size_t ci = 0; ci < comps.size(); ++ci) {
                const Component& c = comps[ci];
                const uint32_t sx = x * static_cast<uint32_t>(c.h) / static_cast<uint32_t>(hMax);
                const uint32_t sy = y * static_cast<uint32_t>(c.v) / static_cast<uint32_t>(vMax);
                v[ci] = c.plane[static_cast<size_t>(sy) * c.stride + sx];
            }
            uint8_t* out = &p.rgb[(static_cast<size_t>(y) * p.width + x) * 3];
            if (comps.size() == 1) {
                out[0] = out[1] = out[2] = clamp8(v[0]);
            } else {
                out[0] = clamp8(v[0] + 1.402f * (v[2] - 128.0f));
                out[1] = clamp8(v[0] - 0.344136f * (v[1] - 128.0f) - 0.714136f * (v[2] - 128.0f));
                out[2] = clamp8(v[0] + 1.772f * (v[1] - 128.0f));
            }
        }
    p.ok = true;
    return p;
}

}  // namespace jpegdecoder
