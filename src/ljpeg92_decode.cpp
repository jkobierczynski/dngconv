// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Lossless JPEG decoder. Written for checking files, which may be damaged:
// every read is bounded and every inconsistency ends in an exception.
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

#include "ljpeg92.hpp"

namespace dngconv {

namespace {

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("lossless JPEG: ") + what);
}

constexpr unsigned kFastBits = 11;

struct HuffmanDecoder {
    bool defined = false;
    // Indexed by the next kFastBits bits: (code length << 8) | symbol, or 0
    // for codes longer than that.
    std::array<uint16_t, 1u << kFastBits> fast{};
    // For the longer codes: the largest code of each length (-1 if none) and
    // the offset that turns a code of that length into an index into values.
    std::array<int32_t, 17> maxCode{};
    std::array<int32_t, 17> valueOffset{};
    std::array<uint8_t, 17> values{};
};

// Builds a decoder from the counts per code length and the symbols, as a DHT
// segment gives them. Symbols are difference categories, 0..16.
void buildDecoder(const uint8_t* counts, const uint8_t* symbols, unsigned total, HuffmanDecoder& h) {
    if (total == 0 || total > 17) fail("Huffman table has an impossible number of codes");
    bool seen[17] = {};
    for (unsigned i = 0; i < total; ++i) {
        if (symbols[i] > 16 || seen[symbols[i]]) fail("Huffman table has an invalid symbol");
        seen[symbols[i]] = true;
        h.values[i] = symbols[i];
    }
    h.fast.fill(0);
    uint32_t code = 0;
    unsigned k = 0;
    for (unsigned len = 1; len <= 16; ++len) {
        h.valueOffset[len] = static_cast<int32_t>(k) - static_cast<int32_t>(code);
        for (unsigned n = 0; n < counts[len - 1]; ++n, ++k, ++code) {
            if (code >= (1u << len)) fail("Huffman table assigns more codes than fit");
            if (len <= kFastBits) {
                const uint32_t first = code << (kFastBits - len);
                const uint32_t span = 1u << (kFastBits - len);
                for (uint32_t j = 0; j < span; ++j)
                    h.fast[first + j] = static_cast<uint16_t>((len << 8) | h.values[k]);
            }
        }
        h.maxCode[len] = counts[len - 1] ? static_cast<int32_t>(code) - 1 : -1;
        code <<= 1;
    }
    h.defined = true;
}

// Reads the entropy-coded segment bit by bit, undoing the byte stuffing and
// stopping in front of any marker.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size, size_t pos) : data_(data), size_(size), pos_(pos) {}

    // The next 16 bits, padded with zeros where the data has ended.
    uint32_t peek16() {
        if (bits_ < 16) fill();
        return bits_ >= 16 ? static_cast<uint32_t>(acc_ >> (bits_ - 16)) & 0xffff
                           : static_cast<uint32_t>(acc_ << (16 - bits_)) & 0xffff;
    }
    void skip(unsigned n) {
        if (n > bits_) fail("the compressed data ends early");
        bits_ -= n;
    }
    uint32_t take(unsigned n) {  // n in 1..16
        if (bits_ < n) fill();
        if (n > bits_) fail("the compressed data ends early");
        bits_ -= n;
        return static_cast<uint32_t>(acc_ >> bits_) & ((1u << n) - 1);
    }
    // At a restart: drop the padding bits and step over the expected marker.
    void restart(unsigned index) {
        bits_ = 0;
        acc_ = 0;
        if (pos_ + 1 >= size_ || data_[pos_] != 0xff || data_[pos_ + 1] != 0xd0 + (index & 7))
            fail("a restart marker is missing");
        pos_ += 2;
    }

private:
    void fill() {
        while (bits_ <= 48 && pos_ < size_) {
            const uint8_t b = data_[pos_];
            if (b == 0xff) {
                if (pos_ + 1 >= size_ || data_[pos_ + 1] != 0x00) return;  // a marker: stop here
                pos_ += 2;
            } else {
                ++pos_;
            }
            acc_ = (acc_ << 8) | b;
            bits_ += 8;
        }
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_;
    uint64_t acc_ = 0;
    unsigned bits_ = 0;
};

inline unsigned decodeSymbol(BitReader& in, const HuffmanDecoder& h) {
    const uint32_t next = in.peek16();
    const uint16_t entry = h.fast[next >> (16 - kFastBits)];
    if (entry) {
        in.skip(entry >> 8);
        return entry & 0xff;
    }
    for (unsigned len = kFastBits + 1; len <= 16; ++len) {
        const int32_t code = static_cast<int32_t>(next >> (16 - len));
        if (code <= h.maxCode[len]) {
            in.skip(len);
            return h.values[static_cast<size_t>(code + h.valueOffset[len])];
        }
    }
    fail("the compressed data holds an invalid code");
}

// The difference a category stands for, given its extra bits.
inline int32_t decodeDifference(BitReader& in, const HuffmanDecoder& h) {
    const unsigned category = decodeSymbol(in, h);
    if (category == 0) return 0;
    if (category == 16) return 32768;
    const int32_t extra = static_cast<int32_t>(in.take(category));
    return extra < (1 << (category - 1)) ? extra - (1 << category) + 1 : extra;
}

uint32_t be16(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 8) | p[1]; }

}  // namespace

std::vector<uint16_t> decodeLosslessJpeg(const uint8_t* data, size_t size,
                                         LosslessJpegFrame& frame, size_t maxSamples) {
    if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) fail("no start-of-image marker");

    std::array<HuffmanDecoder, 4> tables;
    std::array<uint8_t, 4> componentId{};
    std::array<uint8_t, 4> tableOf{};
    bool haveFrame = false;
    uint32_t restartInterval = 0;
    unsigned pointTransform = 0;
    frame = LosslessJpegFrame{};

    size_t pos = 2;
    for (;;) {
        // Every segment starts with 0xff and a marker byte; fill bytes may precede it.
        if (pos + 4 > size) fail("the header ends early");
        if (data[pos] != 0xff) fail("a marker is missing");
        while (pos < size && data[pos] == 0xff) ++pos;
        if (pos + 2 > size) fail("the header ends early");
        const uint8_t marker = data[pos++];
        if (marker == 0xd9) fail("the image ends before any data");
        const size_t length = be16(data + pos);
        if (length < 2 || length > size - pos) fail("a segment reaches beyond the data");
        const uint8_t* seg = data + pos + 2;
        const size_t segSize = length - 2;
        pos += length;

        if (marker == 0xc4) {  // Huffman tables, possibly several
            size_t at = 0;
            while (at < segSize) {
                if (segSize - at < 17) fail("a Huffman table is cut short");
                const uint8_t info = seg[at];
                if ((info >> 4) != 0 || (info & 15) > 3) fail("a Huffman table has an invalid id");
                unsigned total = 0;
                for (unsigned i = 0; i < 16; ++i) total += seg[at + 1 + i];
                if (segSize - at - 17 < total) fail("a Huffman table is cut short");
                buildDecoder(seg + at + 1, seg + at + 17, total, tables[info & 15]);
                at += 17 + total;
            }
        } else if (marker == 0xc3) {  // start of frame, lossless
            if (haveFrame) fail("more than one frame");
            if (segSize < 6) fail("the frame header is cut short");
            frame.precision = seg[0];
            frame.height = be16(seg + 1);
            frame.width = be16(seg + 3);
            frame.components = seg[5];
            if (frame.precision < 2 || frame.precision > 16) fail("unsupported sample precision");
            if (frame.width == 0 || frame.height == 0) fail("the frame has no size");
            if (frame.components < 1 || frame.components > 4) fail("unsupported number of components");
            if (segSize < 6 + 3 * static_cast<size_t>(frame.components)) fail("the frame header is cut short");
            for (uint32_t c = 0; c < frame.components; ++c) {
                componentId[c] = seg[6 + 3 * c];
                if (seg[7 + 3 * c] != 0x11) fail("subsampled components are not supported");
            }
            const uint64_t samples =
                static_cast<uint64_t>(frame.width) * frame.height * frame.components;
            if (samples > maxSamples) fail("the frame is larger than the image it belongs to");
            haveFrame = true;
        } else if (marker == 0xdd) {  // restart interval
            if (segSize < 2) fail("the restart interval is cut short");
            restartInterval = be16(seg);
        } else if (marker == 0xda) {  // start of scan: the data follows
            if (!haveFrame) fail("the scan comes before the frame header");
            if (segSize < 1 || seg[0] != frame.components ||
                segSize < 4 + 2 * static_cast<size_t>(frame.components))
                fail("only one scan with all components interleaved is supported");
            for (uint32_t c = 0; c < frame.components; ++c) {
                if (seg[1 + 2 * c] != componentId[c]) fail("the scan names an unknown component");
                tableOf[c] = seg[2 + 2 * c] >> 4;
                if (tableOf[c] > 3 || !tables[tableOf[c]].defined) fail("the scan uses an undefined Huffman table");
            }
            const uint8_t* tail = seg + 1 + 2 * frame.components;
            frame.predictor = tail[0];
            pointTransform = tail[2] & 15;
            if (frame.predictor < 1 || frame.predictor > 7) fail("invalid predictor");
            if (pointTransform != 0) fail("a point transform is not supported");
            break;
        } else if ((marker >= 0xc0 && marker <= 0xcf && marker != 0xc8) || marker == 0xf7) {
            fail("not a Huffman-coded lossless JPEG");
        }
        // Anything else (application segments, comments) is skipped.
    }

    if (restartInterval % frame.width != 0)
        fail("restart intervals that do not end on a line are not supported");
    const uint32_t restartLines = restartInterval / frame.width;

    const uint32_t width = frame.width, height = frame.height, n = frame.components;
    const size_t stride = static_cast<size_t>(width) * n;
    std::vector<uint16_t> out(stride * height);
    BitReader in(data, size, pos);
    const int32_t start = 1 << (frame.precision - 1);
    unsigned restarts = 0;

    for (uint32_t y = 0; y < height; ++y) {
        bool firstLine = y == 0;
        if (restartLines && y > 0 && y % restartLines == 0) {
            in.restart(restarts++);
            firstLine = true;
        }
        uint16_t* row = &out[static_cast<size_t>(y) * stride];
        const uint16_t* above = firstLine ? nullptr : row - stride;

        // First sample of the line: a fixed value on a first line, else the
        // sample above.
        for (uint32_t c = 0; c < n; ++c) {
            const int32_t prediction = above ? above[c] : start;
            row[c] = static_cast<uint16_t>(prediction + decodeDifference(in, tables[tableOf[c]]));
        }
        const HuffmanDecoder* table[4] = {&tables[tableOf[0]], &tables[tableOf[1]],
                                          &tables[tableOf[2]], &tables[tableOf[3]]};
        if (firstLine || frame.predictor == 1) {
            // Along the line: each sample from the one before it.
            for (uint32_t x = 1; x < width; ++x) {
                uint16_t* p = row + static_cast<size_t>(x) * n;
                for (uint32_t c = 0; c < n; ++c)
                    p[c] = static_cast<uint16_t>((p - n)[c] + decodeDifference(in, *table[c]));
            }
            continue;
        }
        for (uint32_t x = 1; x < width; ++x) {
            uint16_t* p = row + static_cast<size_t>(x) * n;
            const uint16_t* q = above + static_cast<size_t>(x) * n;
            for (uint32_t c = 0; c < n; ++c) {
                const int32_t ra = (p - n)[c], rb = q[c], rc = (q - n)[c];
                int32_t prediction;
                switch (frame.predictor) {
                    case 2: prediction = rb; break;
                    case 3: prediction = rc; break;
                    case 4: prediction = ra + rb - rc; break;
                    case 5: prediction = ra + ((rb - rc) >> 1); break;
                    case 6: prediction = rb + ((ra - rc) >> 1); break;
                    default: prediction = (ra + rb) >> 1; break;
                }
                p[c] = static_cast<uint16_t>(prediction + decodeDifference(in, *table[c]));
            }
        }
    }
    return out;
}

}  // namespace dngconv
