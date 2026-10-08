// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "ljpeg92.hpp"

#include <array>
#include <stdexcept>

namespace dngconv {

namespace {

constexpr unsigned kSymbols = 17;  // difference categories SSSS = 0..16
constexpr uint32_t kMaxComponents = 4;

struct HuffmanTable {
    std::array<uint8_t, 17> bits{};       // bits[n] = number of codes of length n
    std::array<uint8_t, kSymbols> values{};  // symbols in order of increasing code length
    unsigned valueCount = 0;
    std::array<uint16_t, kSymbols> code{};
    std::array<uint8_t, kSymbols> size{};
};

// Builds an optimal length-limited (16 bit) Huffman code for the given symbol
// frequencies, following the procedure of T.81 Annex K.2. A pseudo-symbol with
// frequency 1 guarantees that no real symbol receives the all-ones code.
HuffmanTable buildHuffmanTable(const std::array<uint64_t, kSymbols>& histogram) {
    constexpr int kN = kSymbols + 1;
    constexpr int kMaxLen = 32;
    std::array<uint64_t, kN> freq{};
    std::array<int, kN> codeSize{};
    std::array<int, kN> others{};
    for (unsigned i = 0; i < kSymbols; ++i) freq[i] = histogram[i];
    freq[kSymbols] = 1;
    others.fill(-1);

    for (;;) {
        // c1: least frequent symbol (highest index on ties), c2: next one.
        int c1 = -1, c2 = -1;
        uint64_t v = UINT64_MAX;
        for (int i = 0; i < kN; ++i)
            if (freq[i] && freq[i] <= v) {
                v = freq[i];
                c1 = i;
            }
        v = UINT64_MAX;
        for (int i = 0; i < kN; ++i)
            if (freq[i] && freq[i] <= v && i != c1) {
                v = freq[i];
                c2 = i;
            }
        if (c2 < 0) break;

        freq[c1] += freq[c2];
        freq[c2] = 0;
        ++codeSize[c1];
        while (others[c1] >= 0) {
            c1 = others[c1];
            ++codeSize[c1];
        }
        others[c1] = c2;
        ++codeSize[c2];
        while (others[c2] >= 0) {
            c2 = others[c2];
            ++codeSize[c2];
        }
    }

    std::array<int, kMaxLen + 1> bits{};
    for (int i = 0; i < kN; ++i)
        if (codeSize[i]) ++bits[codeSize[i]];

    // Limit code lengths to 16 bits.
    for (int i = kMaxLen; i > 16; --i) {
        while (bits[i] > 0) {
            int j = i - 2;
            while (bits[j] == 0) --j;
            bits[i] -= 2;
            ++bits[i - 1];
            bits[j + 1] += 2;
            --bits[j];
        }
    }
    // Drop the pseudo-symbol: it holds the longest code.
    int longest = 16;
    while (bits[longest] == 0) --longest;
    --bits[longest];

    HuffmanTable t;
    for (int i = 1; i <= 16; ++i) t.bits[i] = static_cast<uint8_t>(bits[i]);
    for (int len = 1; len <= kMaxLen; ++len)
        for (unsigned s = 0; s < kSymbols; ++s)
            if (codeSize[s] == len) t.values[t.valueCount++] = static_cast<uint8_t>(s);

    // Canonical code assignment.
    unsigned code = 0, k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int n = 0; n < t.bits[len]; ++n) {
            const uint8_t sym = t.values[k++];
            t.code[sym] = static_cast<uint16_t>(code++);
            t.size[sym] = static_cast<uint8_t>(len);
        }
        code <<= 1;
    }
    return t;
}

// Difference category: number of bits needed for the magnitude, with the
// special category 16 for the single value 32768 (differences are taken
// modulo 2^16).
inline unsigned category(uint16_t diff) {
    if (diff == 0x8000) return 16;
    int d = static_cast<int16_t>(diff);
    if (d < 0) d = -d;
    unsigned n = 0;
    while (d) {
        ++n;
        d >>= 1;
    }
    return n;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    void put(uint32_t value, unsigned count) {
        // count <= 32, and at most 7 bits are pending, so 64 bits suffice.
        acc_ = (acc_ << count) | (value & ((count == 32) ? 0xffffffffu : ((1u << count) - 1)));
        fill_ += count;
        while (fill_ >= 8) {
            const uint8_t byte = static_cast<uint8_t>(acc_ >> (fill_ - 8));
            out_.push_back(byte);
            if (byte == 0xff) out_.push_back(0x00);  // byte stuffing
            fill_ -= 8;
        }
    }

    void flush() {
        if (fill_ > 0) put((1u << (8 - fill_)) - 1, 8 - fill_);  // pad with ones
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t acc_ = 0;
    unsigned fill_ = 0;
};

void putMarker(std::vector<uint8_t>& out, uint8_t marker) {
    out.push_back(0xff);
    out.push_back(marker);
}

void putU16(std::vector<uint8_t>& out, unsigned v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xff));
}

}  // namespace

std::vector<uint8_t> encodeLosslessJpeg(const uint16_t* data, uint32_t width, uint32_t height,
                                        uint32_t components, size_t rowStride,
                                        unsigned precision) {
    if (!data || width == 0 || height == 0 || width > 65535 || height > 65535)
        throw std::invalid_argument("lossless JPEG: invalid dimensions");
    if (components < 1 || components > kMaxComponents)
        throw std::invalid_argument("lossless JPEG: 1 to 4 components supported");
    if (precision < 2 || precision > 16)
        throw std::invalid_argument("lossless JPEG: precision must be 2..16");
    const size_t rowSamples = static_cast<size_t>(width) * components;
    if (rowStride < rowSamples)
        throw std::invalid_argument("lossless JPEG: row stride too small");

    // ---- pass 1: prediction residuals and their statistics ----------------
    std::vector<uint16_t> residuals(rowSamples * height);
    std::array<std::array<uint64_t, kSymbols>, kMaxComponents> histogram{};
    const uint16_t initial = static_cast<uint16_t>(1u << (precision - 1));
    {
        uint16_t* r = residuals.data();
        for (uint32_t y = 0; y < height; ++y) {
            const uint16_t* row = data + static_cast<size_t>(y) * rowStride;
            const uint16_t* above = y ? row - rowStride : nullptr;
            for (uint32_t c = 0; c < components; ++c) {
                // First sample of a row: predicted from the row above, or from
                // the mid-point value on the very first row.
                const uint16_t pred = above ? above[c] : initial;
                r[c] = static_cast<uint16_t>(row[c] - pred);
            }
            for (size_t i = components; i < rowSamples; ++i)
                r[i] = static_cast<uint16_t>(row[i] - row[i - components]);  // predictor 1
            for (size_t i = 0; i < rowSamples; ++i) ++histogram[i % components][category(r[i])];
            r += rowSamples;
        }
    }

    std::array<HuffmanTable, kMaxComponents> tables;
    for (uint32_t c = 0; c < components; ++c) tables[c] = buildHuffmanTable(histogram[c]);

    // ---- headers ------------------------------------------------------------
    std::vector<uint8_t> out;
    out.reserve(residuals.size() + residuals.size() / 2 + 256);
    putMarker(out, 0xd8);  // SOI

    putMarker(out, 0xc3);  // SOF3: lossless, Huffman
    putU16(out, 8 + 3 * components);
    out.push_back(static_cast<uint8_t>(precision));
    putU16(out, height);
    putU16(out, width);
    out.push_back(static_cast<uint8_t>(components));
    for (uint32_t c = 0; c < components; ++c) {
        out.push_back(static_cast<uint8_t>(c));  // component id
        out.push_back(0x11);                     // no subsampling
        out.push_back(0);                        // no quantisation table
    }

    putMarker(out, 0xc4);  // DHT
    unsigned dhtLength = 2;
    for (uint32_t c = 0; c < components; ++c) dhtLength += 17 + tables[c].valueCount;
    putU16(out, dhtLength);
    for (uint32_t c = 0; c < components; ++c) {
        out.push_back(static_cast<uint8_t>(c));  // class 0 (DC), table id c
        for (int len = 1; len <= 16; ++len) out.push_back(tables[c].bits[len]);
        for (unsigned i = 0; i < tables[c].valueCount; ++i) out.push_back(tables[c].values[i]);
    }

    putMarker(out, 0xda);  // SOS
    putU16(out, 6 + 2 * components);
    out.push_back(static_cast<uint8_t>(components));
    for (uint32_t c = 0; c < components; ++c) {
        out.push_back(static_cast<uint8_t>(c));
        out.push_back(static_cast<uint8_t>(c << 4));  // DC table c
    }
    out.push_back(1);  // predictor selection: Ra (left neighbour)
    out.push_back(0);
    out.push_back(0);  // no point transform

    // ---- pass 2: entropy coding ---------------------------------------------
    BitWriter bw(out);
    const uint16_t* r = residuals.data();
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            for (uint32_t c = 0; c < components; ++c) {
                const uint16_t diff = *r++;
                const unsigned ssss = category(diff);
                const HuffmanTable& t = tables[c];
                bw.put(t.code[ssss], t.size[ssss]);
                if (ssss > 0 && ssss < 16) {
                    int d = static_cast<int16_t>(diff);
                    if (d < 0) d -= 1;  // one's complement style for negatives
                    bw.put(static_cast<uint32_t>(d), ssss);
                }
            }
        }
    }
    bw.flush();
    putMarker(out, 0xd9);  // EOI
    return out;
}

}  // namespace dngconv
