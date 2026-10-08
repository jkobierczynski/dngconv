// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Encodes test images with the lossless JPEG encoder and decodes them with an
// independent reference decoder written straight from ITU-T T.81.
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ljpeg92.hpp"
#include "test_util.hpp"

using dngconv::encodeLosslessJpeg;

namespace {

struct Decoded {
    unsigned precision = 0;
    uint32_t width = 0, height = 0, components = 0;
    std::vector<uint16_t> samples;
};

struct DecodeTable {
    int minCode[18]{};
    int maxCode[18]{};
    int valPtr[18]{};
    std::vector<uint8_t> values;
    bool defined = false;
};

class BitReader {
public:
    BitReader(const std::vector<uint8_t>& d, size_t pos) : d_(d), pos_(pos) {}
    int bit() {
        if (count_ == 0) {
            if (pos_ >= d_.size()) throw std::runtime_error("out of data");
            cur_ = d_[pos_++];
            if (cur_ == 0xff) {
                if (pos_ >= d_.size()) throw std::runtime_error("truncated");
                if (d_[pos_] != 0) throw std::runtime_error("marker inside entropy data");
                ++pos_;
            }
            count_ = 8;
        }
        --count_;
        return (cur_ >> count_) & 1;
    }
    int bits(int n) {
        int v = 0;
        while (n--) v = (v << 1) | bit();
        return v;
    }
    size_t position() const { return pos_; }

private:
    const std::vector<uint8_t>& d_;
    size_t pos_;
    unsigned cur_ = 0;
    int count_ = 0;
};

int decodeSymbol(BitReader& br, const DecodeTable& t) {
    int code = br.bit();
    int len = 1;
    while (len <= 16 && code > t.maxCode[len]) {
        code = (code << 1) | br.bit();
        ++len;
    }
    if (len > 16) throw std::runtime_error("bad Huffman code");
    return t.values[t.valPtr[len] + code - t.minCode[len]];
}

Decoded decode(const std::vector<uint8_t>& j) {
    Decoded out;
    DecodeTable tables[4];
    int tableOf[4] = {0, 0, 0, 0};
    int predictor = 0;
    size_t pos = 0;
    auto u16 = [&](size_t p) { return (static_cast<unsigned>(j.at(p)) << 8) | j.at(p + 1); };

    if (j.size() < 4 || j[0] != 0xff || j[1] != 0xd8) throw std::runtime_error("no SOI");
    pos = 2;
    bool scan = false;
    while (!scan) {
        if (j.at(pos) != 0xff) throw std::runtime_error("marker expected");
        const uint8_t m = j.at(pos + 1);
        const unsigned len = u16(pos + 2);
        const size_t body = pos + 4;
        if (m == 0xc3) {
            out.precision = j.at(body);
            out.height = u16(body + 1);
            out.width = u16(body + 3);
            out.components = j.at(body + 5);
            if (len != 8 + 3 * out.components) throw std::runtime_error("bad SOF length");
            for (unsigned c = 0; c < out.components; ++c)
                if (j.at(body + 6 + 3 * c + 1) != 0x11) throw std::runtime_error("subsampled");
        } else if (m == 0xc4) {
            size_t p = body;
            while (p < pos + 2 + len) {
                const unsigned id = j.at(p) & 15;
                if ((j.at(p) >> 4) != 0 || id > 3) throw std::runtime_error("bad DHT");
                ++p;
                unsigned counts[17] = {0};
                unsigned total = 0;
                for (int i = 1; i <= 16; ++i) total += counts[i] = j.at(p++);
                DecodeTable& t = tables[id];
                t.values.assign(j.begin() + p, j.begin() + p + total);
                p += total;
                int code = 0, k = 0;
                for (int i = 1; i <= 16; ++i) {
                    t.valPtr[i] = k;
                    t.minCode[i] = code;
                    code += counts[i];
                    k += counts[i];
                    t.maxCode[i] = counts[i] ? code - 1 : -1;
                    code <<= 1;
                }
                t.defined = true;
            }
            if (p != pos + 2 + len) throw std::runtime_error("bad DHT length");
        } else if (m == 0xda) {
            const unsigned ns = j.at(body);
            if (ns != out.components) throw std::runtime_error("scan is not fully interleaved");
            for (unsigned c = 0; c < ns; ++c) tableOf[c] = j.at(body + 2 + 2 * c) >> 4;
            predictor = j.at(body + 1 + 2 * ns);
            if (j.at(body + 3 + 2 * ns) != 0) throw std::runtime_error("point transform");
            scan = true;
        } else {
            throw std::runtime_error("unexpected marker");
        }
        pos += 2 + len;
    }
    if (predictor != 1) throw std::runtime_error("only predictor 1 expected");
    for (unsigned c = 0; c < out.components; ++c)
        if (!tables[tableOf[c]].defined) throw std::runtime_error("missing table");

    const size_t rowSamples = static_cast<size_t>(out.width) * out.components;
    out.samples.resize(rowSamples * out.height);
    BitReader br(j, pos);
    for (uint32_t y = 0; y < out.height; ++y) {
        uint16_t* row = &out.samples[y * rowSamples];
        for (uint32_t x = 0; x < out.width; ++x) {
            for (uint32_t c = 0; c < out.components; ++c) {
                int pred;
                if (x == 0)
                    pred = y == 0 ? (1 << (out.precision - 1)) : (row - rowSamples)[c];
                else
                    pred = row[(x - 1) * out.components + c];
                const int ssss = decodeSymbol(br, tables[tableOf[c]]);
                int diff;
                if (ssss == 0) {
                    diff = 0;
                } else if (ssss == 16) {
                    diff = 32768;
                } else {
                    diff = br.bits(ssss);
                    if (diff < (1 << (ssss - 1))) diff -= (1 << ssss) - 1;
                }
                row[x * out.components + c] = static_cast<uint16_t>(pred + diff);
            }
        }
    }
    const size_t end = br.position();
    if (end + 2 != j.size() || j[end] != 0xff || j[end + 1] != 0xd9)
        throw std::runtime_error("EOI not where expected");
    return out;
}

enum class Fill { Smooth, Noise, Extremes, Constant };

std::vector<uint16_t> makeImage(uint32_t w, uint32_t h, uint32_t comps, unsigned precision,
                                Fill fill, uint64_t seed) {
    testutil::Random rnd(seed);
    const uint32_t maxValue = (1u << precision) - 1;
    std::vector<uint16_t> v(static_cast<size_t>(w) * h * comps);
    size_t i = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t c = 0; c < comps; ++c) {
                uint32_t value = 0;
                switch (fill) {
                    case Fill::Smooth:
                        value = (x * 7 + y * 3 + c * 211 + rnd.below(17)) % (maxValue + 1);
                        break;
                    case Fill::Noise:
                        value = rnd.next() & maxValue;
                        break;
                    case Fill::Extremes:
                        value = rnd.below(3) == 0 ? 0 : (rnd.below(2) ? maxValue : maxValue / 2 + 1);
                        break;
                    case Fill::Constant:
                        value = maxValue / 3;
                        break;
                }
                v[i++] = static_cast<uint16_t>(value);
            }
    return v;
}

void roundTrip(uint32_t w, uint32_t h, uint32_t comps, unsigned precision, Fill fill,
               uint64_t seed, size_t extraStride = 0) {
    const std::vector<uint16_t> tight = makeImage(w, h, comps, precision, fill, seed);
    const size_t rowSamples = static_cast<size_t>(w) * comps;
    const size_t stride = rowSamples + extraStride;
    std::vector<uint16_t> padded(stride * h, 0xABCD);
    for (uint32_t y = 0; y < h; ++y)
        for (size_t i = 0; i < rowSamples; ++i) padded[y * stride + i] = tight[y * rowSamples + i];

    const std::vector<uint8_t> jpeg =
        encodeLosslessJpeg(padded.data(), w, h, comps, stride, precision);
    try {
        const Decoded d = decode(jpeg);
        CHECK(d.precision == precision);
        CHECK(d.width == w);
        CHECK(d.height == h);
        CHECK(d.components == comps);
        CHECK(d.samples == tight);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "decode error (%ux%u c%u p%u): %s\n", w, h, comps, precision, e.what());
        CHECK(false);
    }
}

}  // namespace

int main() {
    uint64_t seed = 1;
    for (Fill fill : {Fill::Smooth, Fill::Noise, Fill::Extremes, Fill::Constant}) {
        for (unsigned precision : {8u, 12u, 14u, 16u}) {
            for (uint32_t comps = 1; comps <= 4; ++comps) {
                roundTrip(64, 48, comps, precision, fill, seed++);
                roundTrip(1, 1, comps, precision, fill, seed++);
                roundTrip(37, 5, comps, precision, fill, seed++, 9);
                roundTrip(3, 91, comps, precision, fill, seed++);
            }
        }
    }
    // A tile-sized mosaic block, as the DNG writer produces it.
    roundTrip(256, 512, 2, 16, Fill::Smooth, seed++);
    roundTrip(256, 512, 2, 16, Fill::Noise, seed++);

    // Smooth data must actually compress.
    {
        const auto img = makeImage(256, 256, 2, 12, Fill::Smooth, 99);
        const auto jpeg = encodeLosslessJpeg(img.data(), 256, 256, 2, 512, 12);
        CHECK(jpeg.size() < img.size() * 2 * 6 / 10);
    }

    // Invalid arguments are rejected.
    {
        std::vector<uint16_t> one(16, 0);
        bool threw = false;
        try {
            encodeLosslessJpeg(one.data(), 4, 4, 5, 20, 12);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            encodeLosslessJpeg(one.data(), 4, 2, 2, 4, 12);  // stride smaller than a row
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
    return testutil::finish("test_ljpeg92");
}
