// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "jpeg_encoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include "parallel.hpp"

namespace dngconv {

namespace {

// The example quantisation tables of the JPEG standard (Annex K.1), in
// natural (row by row) order. They define what "quality 50" means.
const uint8_t kLuminance[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const uint8_t kChrominance[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Position in the 8x8 block of the coefficient coded k-th: the zigzag path
// from the top-left corner along the anti-diagonals.
std::array<uint8_t, 64> zigzagOrder() {
    std::array<uint8_t, 64> order{};
    int k = 0;
    for (int sum = 0; sum < 15; ++sum) {
        // Odd diagonals run down-left, even ones up-right.
        for (int i = 0; i <= sum; ++i) {
            const int row = (sum % 2) ? i : sum - i;
            const int col = sum - row;
            if (row < 8 && col < 8) order[static_cast<size_t>(k++)] = static_cast<uint8_t>(row * 8 + col);
        }
    }
    return order;
}

std::array<uint8_t, 64> scaledTable(const uint8_t* base, int quality) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    std::array<uint8_t, 64> table{};
    for (size_t i = 0; i < 64; ++i)
        table[i] = static_cast<uint8_t>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
    return table;
}

// The 8x8 forward DCT as a matrix: out = C * in * C^T.
struct Dct {
    float c[8][8];
    Dct() {
        const double pi = 3.14159265358979323846;
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = static_cast<float>(0.5 * (u == 0 ? std::sqrt(0.5) : 1.0) *
                                             std::cos((2 * x + 1) * u * pi / 16.0));
    }
    void forward(const float* in, float* out) const {
        float tmp[64];
        for (int y = 0; y < 8; ++y)  // rows
            for (int u = 0; u < 8; ++u) {
                float s = 0;
                for (int x = 0; x < 8; ++x) s += c[u][x] * in[y * 8 + x];
                tmp[y * 8 + u] = s;
            }
        for (int u = 0; u < 8; ++u)  // columns
            for (int v = 0; v < 8; ++v) {
                float s = 0;
                for (int y = 0; y < 8; ++y) s += c[v][y] * tmp[y * 8 + u];
                out[v * 8 + u] = s;
            }
    }
};

// A Huffman table in the form JPEG stores it, plus the codes for encoding.
struct HuffmanTable {
    uint8_t counts[17] = {};       // codes of each length 1..16
    std::vector<uint8_t> symbols;  // in order of increasing code length
    uint16_t code[256] = {};
    uint8_t length[256] = {};
};

// Builds the best table for the symbol frequencies given, within JPEG's limit
// of 16 bits per code (the procedure of the standard's Annex K.2).
HuffmanTable buildTable(const uint32_t* frequency) {
    // One extra symbol that is never coded keeps the all-ones code unused.
    long freq[257];
    int codeSize[257] = {};
    int others[257];
    for (int i = 0; i < 256; ++i) freq[i] = frequency[i];
    freq[256] = 1;
    std::fill(std::begin(others), std::end(others), -1);

    for (;;) {
        // The two least frequent trees; ties go to the larger symbol value.
        int c1 = -1, c2 = -1;
        long v = 1000000000L;
        for (int i = 0; i <= 256; ++i)
            if (freq[i] && freq[i] <= v) {
                v = freq[i];
                c1 = i;
            }
        v = 1000000000L;
        for (int i = 0; i <= 256; ++i)
            if (freq[i] && freq[i] <= v && i != c1) {
                v = freq[i];
                c2 = i;
            }
        if (c2 < 0) break;  // one tree left
        freq[c1] += freq[c2];
        freq[c2] = 0;
        // Every symbol of both trees moves one level down.
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

    int bits[64] = {};
    for (int i = 0; i <= 256; ++i)
        if (codeSize[i]) ++bits[std::min(codeSize[i], 63)];
    // Shorten codes longer than 16 bits: take a pair from the longest length,
    // move one of them up a level and hang the pair under a shorter code.
    for (int i = 63; i > 16; --i) {
        while (bits[i] > 0) {
            int j = i - 2;
            while (j > 0 && bits[j] == 0) --j;
            bits[i] -= 2;
            ++bits[i - 1];
            bits[j + 1] += 2;
            --bits[j];
        }
    }
    // Drop the code of the extra symbol: it is the longest one.
    int longest = 16;
    while (longest > 0 && bits[longest] == 0) --longest;
    if (longest > 0) --bits[longest];

    HuffmanTable table;
    for (int i = 1; i <= 16; ++i) table.counts[i] = static_cast<uint8_t>(bits[i]);
    for (int size = 1; size < 64; ++size)
        for (int i = 0; i < 256; ++i)
            if (codeSize[i] == size) table.symbols.push_back(static_cast<uint8_t>(i));

    // Canonical codes: counting up within a length, doubling between lengths.
    unsigned next = 0;
    size_t k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int n = 0; n < table.counts[len] && k < table.symbols.size(); ++n, ++k) {
            table.code[table.symbols[k]] = static_cast<uint16_t>(next++);
            table.length[table.symbols[k]] = static_cast<uint8_t>(len);
        }
        next <<= 1;
    }
    return table;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void put(unsigned value, unsigned count) {
        acc_ = (acc_ << count) | (value & ((1u << count) - 1));
        fill_ += count;
        while (fill_ >= 8) {
            const uint8_t byte = static_cast<uint8_t>(acc_ >> (fill_ - 8));
            out_.push_back(byte);
            if (byte == 0xff) out_.push_back(0);  // not a marker
            fill_ -= 8;
        }
    }
    void finish() {
        if (fill_ > 0) put(0x7f, 8 - fill_);  // pad with ones
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t acc_ = 0;
    unsigned fill_ = 0;
};

// Number of bits needed for the magnitude of a coefficient.
inline unsigned category(int v) {
    unsigned a = static_cast<unsigned>(v < 0 ? -v : v), n = 0;
    while (a) {
        ++n;
        a >>= 1;
    }
    return n;
}

// Walks one block the way the entropy coder does. `emit(table symbol,
// extra bits, number of extra bits, isDc)` is called for every code.
template <typename Emit>
void codeBlock(const int16_t* block, int& previousDc, Emit emit) {
    const int diff = block[0] - previousDc;
    previousDc = block[0];
    unsigned size = category(diff);
    emit(size, static_cast<unsigned>(diff < 0 ? diff - 1 : diff), size, true);

    int run = 0;
    for (int k = 1; k < 64; ++k) {
        const int v = block[k];
        if (v == 0) {
            ++run;
            continue;
        }
        while (run > 15) {
            emit(0xf0, 0, 0, false);  // sixteen zeros
            run -= 16;
        }
        size = category(v);
        emit(static_cast<unsigned>(run) << 4 | size, static_cast<unsigned>(v < 0 ? v - 1 : v), size, false);
        run = 0;
    }
    if (run > 0) emit(0x00, 0, 0, false);  // end of block
}

void putMarker(std::vector<uint8_t>& out, uint8_t marker, const std::vector<uint8_t>& payload) {
    out.push_back(0xff);
    out.push_back(marker);
    const size_t length = payload.size() + 2;
    out.push_back(static_cast<uint8_t>(length >> 8));
    out.push_back(static_cast<uint8_t>(length));
    out.insert(out.end(), payload.begin(), payload.end());
}

}  // namespace

std::vector<uint8_t> encodeJpeg(const uint8_t* rgb, uint32_t width, uint32_t height, int quality,
                                unsigned threads) {
    if (!rgb || width == 0 || height == 0 || width > 65535 || height > 65535)
        throw std::invalid_argument("JPEG: picture must be 1 to 65535 pixels wide and high");

    static const std::array<uint8_t, 64> zigzag = zigzagOrder();
    static const Dct dct;
    const std::array<uint8_t, 64> quant[2] = {scaledTable(kLuminance, quality),
                                              scaledTable(kChrominance, quality)};

    // ---- transform ------------------------------------------------------------
    // One unit is 16x16 pixels: four brightness blocks and one block for each
    // colour-difference plane. Coefficients are kept in coding order.
    const uint32_t unitsAcross = (width + 15) / 16, unitsDown = (height + 15) / 16;
    const size_t blocksPerUnit = 6;
    std::vector<int16_t> coefficients(static_cast<size_t>(unitsAcross) * unitsDown * blocksPerUnit * 64);

    parallelFor(unitsDown, threads, [&](size_t unitRow) {
        float y[16 * 16], cb[16 * 16], cr[16 * 16];
        float in[64], out[64];
        for (uint32_t ux = 0; ux < unitsAcross; ++ux) {
            for (uint32_t dy = 0; dy < 16; ++dy) {
                // Past the edge the last row and column are repeated.
                const uint32_t sy = std::min<uint32_t>(static_cast<uint32_t>(unitRow) * 16 + dy, height - 1);
                for (uint32_t dx = 0; dx < 16; ++dx) {
                    const uint32_t sx = std::min<uint32_t>(ux * 16 + dx, width - 1);
                    const uint8_t* p = rgb + (static_cast<size_t>(sy) * width + sx) * 3;
                    const float r = p[0], g = p[1], b = p[2];
                    y[dy * 16 + dx] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    cb[dy * 16 + dx] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    cr[dy * 16 + dx] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            }
            int16_t* unit = &coefficients[(unitRow * unitsAcross + ux) * blocksPerUnit * 64];
            const auto store = [&](const std::array<uint8_t, 64>& q, int16_t* dst) {
                dct.forward(in, out);
                for (size_t k = 0; k < 64; ++k) {
                    const size_t at = zigzag[k];
                    // The range baseline JPEG has codes for.
                    dst[k] = static_cast<int16_t>(
                        std::clamp<long>(std::lround(out[at] / static_cast<float>(q[at])), k ? -1023 : -1024, 1023));
                }
            };
            for (int b = 0; b < 4; ++b) {
                const int ox = (b & 1) * 8, oy = (b >> 1) * 8;
                for (int j = 0; j < 8; ++j)
                    for (int i = 0; i < 8; ++i) in[j * 8 + i] = y[(oy + j) * 16 + ox + i];
                store(quant[0], unit + b * 64);
            }
            for (int plane = 0; plane < 2; ++plane) {
                const float* c = plane == 0 ? cb : cr;
                for (int j = 0; j < 8; ++j)
                    for (int i = 0; i < 8; ++i)
                        in[j * 8 + i] = 0.25f * (c[(2 * j) * 16 + 2 * i] + c[(2 * j) * 16 + 2 * i + 1] +
                                                 c[(2 * j + 1) * 16 + 2 * i] + c[(2 * j + 1) * 16 + 2 * i + 1]);
                store(quant[1], unit + (4 + plane) * 64);
            }
        }
    });

    // ---- Huffman tables made for this picture ----------------------------------
    // Table 0 codes brightness, table 1 both colour planes.
    const size_t units = static_cast<size_t>(unitsAcross) * unitsDown;
    uint32_t dcFrequency[2][256] = {}, acFrequency[2][256] = {};
    {
        int previous[3] = {0, 0, 0};
        for (size_t u = 0; u < units; ++u)
            for (size_t b = 0; b < blocksPerUnit; ++b) {
                const int component = b < 4 ? 0 : static_cast<int>(b) - 3;
                const int t = component ? 1 : 0;
                codeBlock(&coefficients[(u * blocksPerUnit + b) * 64], previous[component],
                          [&](unsigned symbol, unsigned, unsigned, bool isDc) {
                              ++(isDc ? dcFrequency : acFrequency)[t][symbol];
                          });
            }
    }
    const HuffmanTable dcTable[2] = {buildTable(dcFrequency[0]), buildTable(dcFrequency[1])};
    const HuffmanTable acTable[2] = {buildTable(acFrequency[0]), buildTable(acFrequency[1])};

    // ---- the file ---------------------------------------------------------------
    std::vector<uint8_t> out;
    out.reserve(coefficients.size() / 8 + 1024);
    out.push_back(0xff);
    out.push_back(0xd8);  // start of image
    putMarker(out, 0xe0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    for (uint8_t t = 0; t < 2; ++t) {
        std::vector<uint8_t> dqt = {t};
        for (size_t k = 0; k < 64; ++k) dqt.push_back(quant[t][zigzag[k]]);
        putMarker(out, 0xdb, dqt);
    }
    putMarker(out, 0xc0,
              {8, static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height),
               static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width), 3,
               1, 0x22, 0,    // brightness: two samples each way per unit
               2, 0x11, 1,    // colour difference, blue
               3, 0x11, 1});  // colour difference, red
    const auto putTable = [&](uint8_t id, const HuffmanTable& table) {
        std::vector<uint8_t> dht = {id};
        for (int i = 1; i <= 16; ++i) dht.push_back(table.counts[i]);
        dht.insert(dht.end(), table.symbols.begin(), table.symbols.end());
        putMarker(out, 0xc4, dht);
    };
    putTable(0x00, dcTable[0]);
    putTable(0x10, acTable[0]);
    putTable(0x01, dcTable[1]);
    putTable(0x11, acTable[1]);
    putMarker(out, 0xda, {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0});

    BitWriter bits(out);
    int previous[3] = {0, 0, 0};
    for (size_t u = 0; u < units; ++u)
        for (size_t b = 0; b < blocksPerUnit; ++b) {
            const int component = b < 4 ? 0 : static_cast<int>(b) - 3;
            const int t = component ? 1 : 0;
            codeBlock(&coefficients[(u * blocksPerUnit + b) * 64], previous[component],
                      [&](unsigned symbol, unsigned extra, unsigned count, bool isDc) {
                          const HuffmanTable& table = isDc ? dcTable[t] : acTable[t];
                          bits.put(table.code[symbol], table.length[symbol]);
                          if (count) bits.put(extra, count);
                      });
        }
    bits.finish();
    out.push_back(0xff);
    out.push_back(0xd9);  // end of image
    return out;
}

}  // namespace dngconv
