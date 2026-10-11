// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// The check command's machinery: the lossless JPEG decoder (against the
// encoder, against streams built by hand with every predictor, and against
// damaged input), the two raw image digests, reading raw data as stored, and
// the verdicts on sound and damaged DNG files.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "dng_check.hpp"
#include "dng_digest.hpp"
#include "dng_writer.hpp"
#include "ljpeg92.hpp"
#include "test_util.hpp"
#include "tiff_builder.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
using namespace tiffbuilder;

namespace {

using State = DngCheck::State;

std::string hex(const Md5Digest& d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

// ---- a lossless JPEG writer of the test's own -----------------------------------
// Straight from ITU-T T.81: any predictor, optional restart intervals, and one
// fixed Huffman table in which every category has a five-bit code.

struct BitWriter {
    Bytes out;
    uint32_t acc = 0;
    int bits = 0;
    void put(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            acc = (acc << 1) | ((value >> i) & 1);
            if (++bits == 8) flushByte();
        }
    }
    void flushByte() {
        out.push_back(static_cast<uint8_t>(acc));
        if ((acc & 0xff) == 0xff) out.push_back(0);  // byte stuffing
        acc = 0;
        bits = 0;
    }
    void align() {
        while (bits) put(1, 1);
    }
};

Bytes handJpeg(const std::vector<uint16_t>& img, uint32_t w, uint32_t h, uint32_t n, int precision,
               int predictor, uint32_t restartRows = 0) {
    Bytes j = {0xff, 0xd8};
    // One table, used by every component: categories 0..16, five bits each.
    j.insert(j.end(), {0xff, 0xc4, 0, 2 + 1 + 16 + 17, 0x00});
    for (int len = 1; len <= 16; ++len) j.push_back(len == 5 ? 17 : 0);
    for (int s = 0; s <= 16; ++s) j.push_back(static_cast<uint8_t>(s));
    j.insert(j.end(), {0xff, 0xc3, 0, static_cast<uint8_t>(8 + 3 * n), static_cast<uint8_t>(precision),
                       static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h), static_cast<uint8_t>(w >> 8),
                       static_cast<uint8_t>(w), static_cast<uint8_t>(n)});
    for (uint32_t c = 0; c < n; ++c) j.insert(j.end(), {static_cast<uint8_t>(c + 1), 0x11, 0});
    if (restartRows) {
        const uint32_t interval = restartRows * w;
        j.insert(j.end(), {0xff, 0xdd, 0, 4, static_cast<uint8_t>(interval >> 8), static_cast<uint8_t>(interval)});
    }
    j.insert(j.end(), {0xff, 0xda, 0, static_cast<uint8_t>(6 + 2 * n), static_cast<uint8_t>(n)});
    for (uint32_t c = 0; c < n; ++c) j.insert(j.end(), {static_cast<uint8_t>(c + 1), 0x00});
    j.insert(j.end(), {static_cast<uint8_t>(predictor), 0, 0});

    BitWriter bw;
    unsigned restarts = 0;
    const auto at = [&](uint32_t x, uint32_t y, uint32_t c) -> int { return img[(static_cast<size_t>(y) * w + x) * n + c]; };
    for (uint32_t y = 0; y < h; ++y) {
        bool first = y == 0;
        if (restartRows && y > 0 && y % restartRows == 0) {
            bw.align();
            bw.out.push_back(0xff);
            bw.out.push_back(static_cast<uint8_t>(0xd0 + (restarts++ & 7)));
            first = true;
        }
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t c = 0; c < n; ++c) {
                int px;
                if (first)
                    px = x == 0 ? 1 << (precision - 1) : at(x - 1, y, c);
                else if (x == 0)
                    px = at(0, y - 1, c);
                else {
                    const int ra = at(x - 1, y, c), rb = at(x, y - 1, c), rc = at(x - 1, y - 1, c);
                    switch (predictor) {
                        case 1: px = ra; break;
                        case 2: px = rb; break;
                        case 3: px = rc; break;
                        case 4: px = ra + rb - rc; break;
                        case 5: px = ra + ((rb - rc) >> 1); break;
                        case 6: px = rb + ((ra - rc) >> 1); break;
                        default: px = (ra + rb) >> 1; break;
                    }
                }
                // Differences are taken modulo 2^16 and coded as -32767..32768.
                int diff = (at(x, y, c) - px) & 0xffff;
                if (diff > 32768) diff -= 65536;
                int category = 0;
                for (int m = diff < 0 ? -diff : diff; m; m >>= 1) ++category;
                bw.put(static_cast<uint32_t>(category), 5);
                if (category > 0 && category < 16)
                    bw.put(static_cast<uint32_t>(diff < 0 ? diff + (1 << category) - 1 : diff), category);
            }
    }
    bw.align();
    j.insert(j.end(), bw.out.begin(), bw.out.end());
    j.insert(j.end(), {0xff, 0xd9});
    return j;
}

std::vector<uint16_t> testImage(uint32_t w, uint32_t h, uint32_t n, int precision, uint64_t seed, bool smooth) {
    testutil::Random rnd(seed);
    const uint32_t limit = 1u << precision;
    std::vector<uint16_t> img(static_cast<size_t>(w) * h * n);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t c = 0; c < n; ++c) {
                const uint32_t v = smooth ? (x * 9 + y * 5 + c * 100 + rnd.below(7)) % limit : rnd.next() % limit;
                img[(static_cast<size_t>(y) * w + x) * n + c] = static_cast<uint16_t>(v);
            }
    return img;
}

bool decodeThrows(const Bytes& j, size_t maxSamples = 1 << 22) {
    try {
        LosslessJpegFrame f;
        decodeLosslessJpeg(j.data(), j.size(), f, maxSamples);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

// ---- images and DNGs ------------------------------------------------------------------

RawImage mosaic(uint32_t w, uint32_t h, uint64_t seed) {
    RawImage img;
    img.width = w;
    img.height = h;
    img.samplesPerPixel = 1;
    img.isCfa = true;
    img.cfaRows = img.cfaCols = 2;
    img.cfaPattern = {0, 1, 1, 2};
    img.activeBottom = h;
    img.activeRight = w;
    img.cropWidth = w;
    img.cropHeight = h;
    img.blackLevel = {0};
    img.whiteLevel = {4095, 4095, 4095, 4095};
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}, {0, 0, 0}}};
    img.uniqueCameraModel = "Testmake Model T";
    testutil::Random rnd(seed);
    img.pixels.resize(img.sampleCount());
    for (size_t i = 0; i < img.pixels.size(); ++i)
        img.pixels[i] = static_cast<uint16_t>(((i % w) * 3 + (i / w) * 5 + rnd.below(64)) & 0xfff);
    return img;
}

RawImage fullColour(uint32_t w, uint32_t h) {
    RawImage img = mosaic(w, h, 3);
    img.isCfa = false;
    img.samplesPerPixel = 3;
    img.cfaRows = img.cfaCols = 0;
    img.cfaPattern.clear();
    img.blackLevel = {0, 0, 0};
    img.pixels.resize(img.sampleCount());
    for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<uint16_t>((i * 31) & 0x3fff);
    return img;
}

Bytes readAll(const fs::path& p) {
    const std::string s = tiffreader::slurp(p);
    return Bytes(s.begin(), s.end());
}

Field digestTag(uint16_t tag, const Md5Digest& d) { return bytes(tag, Bytes(d.begin(), d.end()), 1); }

// A minimal DNG holding one raw strip, built by hand.
Bytes handDng(uint32_t w, uint32_t h, uint32_t planes, int bits, const Bytes& strip, bool be,
              uint32_t compression = 1, const std::vector<Field>& extra = {}) {
    Bytes f = {static_cast<uint8_t>(be ? 'M' : 'I'), static_cast<uint8_t>(be ? 'M' : 'I')};
    put16(f, 42, be);
    put32(f, 8, be);
    const uint32_t dataAt = 0x400;
    std::vector<int> depth(planes, bits);
    std::vector<Field> fields = {
        longs(254, {0}, be), longs(256, {w}, be), longs(257, {h}, be), shorts(258, depth, be, 3),
        shorts(259, {static_cast<int>(compression)}, be, 3), shorts(262, {planes == 1 ? 32803 : 34892}, be, 3),
        longs(273, {dataAt}, be), shorts(277, {static_cast<int>(planes)}, be, 3), longs(278, {h}, be),
        longs(279, {static_cast<uint32_t>(strip.size())}, be), bytes(50706, {1, 4, 0, 0}, 1)};
    fields.insert(fields.end(), extra.begin(), extra.end());
    placeDirectory(f, 8, fields, be);
    f.resize(dataAt, 0);
    f.insert(f.end(), strip.begin(), strip.end());
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path();

    // ---- decoder against the encoder -----------------------------------------------------
    {
        struct Case {
            uint32_t w, h, n;
            int precision;
            bool smooth;
        };
        const Case cases[] = {{1, 1, 1, 16, false},  {2, 1, 2, 12, true},   {7, 5, 1, 8, false},
                              {64, 48, 2, 14, true}, {33, 17, 3, 16, false}, {256, 100, 2, 12, true},
                              {40, 30, 4, 10, true}, {300, 2, 1, 16, false}, {5, 200, 2, 2, false}};
        uint64_t seed = 1;
        for (const Case& c : cases) {
            const std::vector<uint16_t> img = testImage(c.w, c.h, c.n, c.precision, seed++, c.smooth);
            const Bytes j = encodeLosslessJpeg(img.data(), c.w, c.h, c.n, static_cast<size_t>(c.w) * c.n,
                                               static_cast<unsigned>(c.precision));
            LosslessJpegFrame f;
            const std::vector<uint16_t> back = decodeLosslessJpeg(j.data(), j.size(), f, img.size());
            CHECK(f.width == c.w && f.height == c.h && f.components == c.n);
            CHECK(f.precision == static_cast<unsigned>(c.precision) && f.predictor == 1);
            CHECK(back == img);
        }
    }

    // ---- decoder against streams built by hand: every predictor, restarts ------------------
    {
        uint64_t seed = 100;
        for (int predictor = 1; predictor <= 7; ++predictor)
            for (uint32_t n = 1; n <= 3; ++n)
                for (int precision : {8, 12, 16}) {
                    const uint32_t w = 37 + static_cast<uint32_t>(predictor), h = 23;
                    // Smooth data, and noise that wraps around 2^16.
                    for (bool smooth : {true, false}) {
                        const std::vector<uint16_t> img = testImage(w, h, n, precision, seed++, smooth);
                        const Bytes j = handJpeg(img, w, h, n, precision, predictor);
                        LosslessJpegFrame f;
                        const std::vector<uint16_t> back = decodeLosslessJpeg(j.data(), j.size(), f, img.size());
                        CHECK(f.predictor == static_cast<unsigned>(predictor));
                        CHECK(back == img);
                    }
                }
        // Restart intervals of one and of several lines.
        for (uint32_t rows : {1u, 4u, 23u})
            for (int predictor : {1, 4, 7}) {
                const std::vector<uint16_t> img = testImage(50, 23, 2, 12, seed++, true);
                const Bytes j = handJpeg(img, 50, 23, 2, 12, predictor, rows);
                LosslessJpegFrame f;
                CHECK(decodeLosslessJpeg(j.data(), j.size(), f, img.size()) == img);
            }
        // The value 32768 as a difference (category 16, no extra bits).
        const std::vector<uint16_t> jump = {0, 32768, 0, 32768, 65535, 32767};
        const Bytes j = handJpeg(jump, 6, 1, 1, 16, 1);
        LosslessJpegFrame f;
        CHECK(decodeLosslessJpeg(j.data(), j.size(), f, 6) == jump);
    }

    // ---- decoder on input that is not what it should be -------------------------------------
    {
        const std::vector<uint16_t> img = testImage(24, 9, 2, 12, 7, true);
        const Bytes good = handJpeg(img, 24, 9, 2, 12, 4, 3);
        CHECK(!decodeThrows(good));
        // Cut short anywhere: an error, never a result and never a crash.
        for (size_t cut = 0; cut + 2 < good.size(); ++cut) CHECK(decodeThrows(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut))));
        CHECK(decodeThrows({}));
        CHECK(decodeThrows(Bytes(100, 0)));
        // More samples than the caller allows.
        CHECK(decodeThrows(good, 24 * 9 * 2 - 1));
        // A baseline JPEG frame header instead of a lossless one.
        Bytes baseline = good;
        for (size_t i = 0; i + 1 < baseline.size(); ++i)
            if (baseline[i] == 0xff && baseline[i + 1] == 0xc3) baseline[i + 1] = 0xc0;
        CHECK(decodeThrows(baseline));
        // A scan that names a Huffman table nobody defined.
        Bytes noTable = good;
        for (size_t i = 0; i + 1 < noTable.size(); ++i)
            if (noTable[i] == 0xff && noTable[i + 1] == 0xda) noTable[i + 6] = 0x10;
        CHECK(decodeThrows(noTable));
        // A missing restart marker.
        Bytes noRestart = good;
        for (size_t i = 0; i + 1 < noRestart.size(); ++i)
            if (noRestart[i] == 0xff && noRestart[i + 1] == 0xd0) noRestart[i + 1] = 0xd3;
        CHECK(decodeThrows(noRestart));
        // Random damage: whatever happens, it must end in a result or an exception.
        testutil::Random rnd(77);
        int threw = 0;
        for (int round = 0; round < 3000; ++round) {
            Bytes b = good;
            const uint32_t changes = 1 + rnd.below(4);
            for (uint32_t i = 0; i < changes; ++i) b[rnd.below(static_cast<uint32_t>(b.size()))] = static_cast<uint8_t>(rnd.next());
            if (decodeThrows(b)) ++threw;
        }
        CHECK(threw > 300);
    }

    // ---- the digests, against values computed independently (Python's hashlib) -------------
    {
        const auto make = [](uint32_t w, uint32_t h, uint32_t p, uint32_t (*f)(uint32_t, uint32_t, uint32_t)) {
            std::vector<uint16_t> img(static_cast<size_t>(w) * h * p);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    for (uint32_t c = 0; c < p; ++c) img[(static_cast<size_t>(y) * w + x) * p + c] = static_cast<uint16_t>(f(x, y, c));
            return img;
        };
        const std::vector<uint16_t> a = make(300, 270, 1, [](uint32_t x, uint32_t y, uint32_t) { return (x * 7 + y * 13) % 4096; });
        CHECK(hex(newRawImageDigest(a.data(), 300, 270, 1, 2, 0)) == "3ecd9755451ca5ed2c569b1ba3c3611d");
        CHECK(hex(rawImageDigest(a.data(), 300, 270, 1)) == "56c70e3787ee1f0b49fbb06f965cc971");
        const std::vector<uint16_t> b = make(70, 50, 3, [](uint32_t x, uint32_t y, uint32_t c) { return (x * 5 + y * 3 + c * 11) % 251; });
        CHECK(hex(newRawImageDigest(b.data(), 70, 50, 3, 1, 0)) == "1b2a873ecd6f26b60df98f5c3362cd10");
        CHECK(hex(newRawImageDigest(b.data(), 70, 50, 3, 2, 0)) == "bb89e1d2c261178cf5583a7076ad8463");
        CHECK(hex(rawImageDigest(b.data(), 70, 50, 3)) == "24b6a7f4662dd9ef395c5ff489897ab1");
        const std::vector<uint16_t> c = make(513, 257, 1, [](uint32_t x, uint32_t y, uint32_t) { return (x ^ y) & 0xff; });
        CHECK(hex(newRawImageDigest(c.data(), 513, 257, 1, 1, 0)) == "0ba3f6f43cd463581eb9ff30ca81bc15");
        CHECK(hex(rawImageDigest(c.data(), 513, 257, 1)) == "2d23c684d7b392e087b6073be2a506e4");
        // The thread count must not matter.
        CHECK(newRawImageDigest(c.data(), 513, 257, 1, 1, 1) == newRawImageDigest(c.data(), 513, 257, 1, 1, 7));
    }

    // ---- raw data as stored, from files built by hand ---------------------------------------
    {
        const uint32_t w = 5, h = 3;
        const std::vector<uint16_t> values = {0, 1, 4095, 2048, 77, 300, 301, 302, 303, 304, 4000, 3999, 5, 6, 7};
        // 16 bits, both byte orders.
        for (bool be : {false, true}) {
            Bytes strip;
            for (uint16_t v : values) put16(strip, v, be);
            const StoredRawImage im = readStoredRawImage(writeFile(dir, "check_hand16.dng", handDng(w, h, 1, 16, strip, be)));
            CHECK(im.width == w && im.height == h && im.planes == 1 && im.bitsPerSample == 16);
            CHECK(im.samples == values);
        }
        // 12 bits, packed with the most significant bit first; every row
        // starts on a byte boundary (five samples are 60 bits, so 8 bytes).
        {
            Bytes strip;
            for (uint32_t y = 0; y < h; ++y) {
                uint64_t acc = 0;
                for (uint32_t x = 0; x < w; ++x) acc = (acc << 12) | values[y * w + x];
                acc <<= 4;
                for (int shift = 56; shift >= 0; shift -= 8) strip.push_back(static_cast<uint8_t>(acc >> shift));
            }
            for (bool be : {false, true}) {
                const StoredRawImage im = readStoredRawImage(writeFile(dir, "check_hand12.dng", handDng(w, h, 1, 12, strip, be)));
                CHECK(im.bitsPerSample == 12);
                CHECK(im.samples == values);
            }
            // One byte short: damage.
            Bytes shortStrip(strip.begin(), strip.end() - 1);
            const DngCheck c = checkDng(writeFile(dir, "check_hand12s.dng", handDng(w, h, 1, 12, shortStrip, false)));
            CHECK(c.rawImage == State::Damaged);
        }
        // 8 bits, three planes; the newer digest then hashes bytes.
        {
            Bytes strip;
            std::vector<uint16_t> rgb;
            for (uint32_t i = 0; i < w * h * 3; ++i) {
                strip.push_back(static_cast<uint8_t>(i * 17));
                rgb.push_back(static_cast<uint8_t>(i * 17));
            }
            const Bytes plain = handDng(w, h, 3, 8, strip, false);
            const StoredRawImage im = readStoredRawImage(writeFile(dir, "check_hand8.dng", plain));
            CHECK(im.planes == 3 && im.bitsPerSample == 8 && im.samples == rgb);
            CHECK(checkDng(dir / "check_hand8.dng").rawImage == State::Unverified);

            const Md5Digest asBytes = newRawImageDigest(rgb.data(), w, h, 3, 1, 0);
            const Md5Digest asWords = newRawImageDigest(rgb.data(), w, h, 3, 2, 0);
            CHECK(asBytes != asWords);
            DngCheck c = checkDng(writeFile(dir, "check_hand8d.dng", handDng(w, h, 3, 8, strip, false, 1, {digestTag(51111, asBytes)})));
            CHECK(c.rawImage == State::Intact && c.intact() && c.rawDetail == "NewRawImageDigest");
            c = checkDng(writeFile(dir, "check_hand8w.dng", handDng(w, h, 3, 8, strip, false, 1, {digestTag(51111, asWords)})));
            CHECK(c.rawImage == State::Damaged && c.damaged());
            // The older digest always hashes 16-bit values.
            c = checkDng(writeFile(dir, "check_hand8o.dng", handDng(w, h, 3, 8, strip, true, 1, {digestTag(50972, rawImageDigest(rgb.data(), w, h, 3))})));
            CHECK(c.rawImage == State::Intact && c.rawDetail == "RawImageDigest");
        }
        // Forms that cannot be decoded here are "not verified", not "damaged".
        {
            Bytes strip(64, 0);
            DngCheck c = checkDng(writeFile(dir, "check_lossy.dng", handDng(8, 8, 1, 8, strip, false, 34892)));
            CHECK(c.rawImage == State::Unverified && !c.damaged() && !c.intact());
            CHECK(c.rawDetail.find("lossy JPEG") != std::string::npos);
            c = checkDng(writeFile(dir, "check_jxl.dng", handDng(8, 8, 1, 8, strip, false, 52546)));
            CHECK(c.rawImage == State::Unverified);
            bool threw = false;
            try {
                readStoredRawImage(dir / "check_jxl.dng");
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
        }
        // Not DNGs at all.
        for (const Bytes& junk : {Bytes{}, Bytes(300, 'x'), tiffHeader()}) {
            bool threw = false;
            try {
                checkDng(writeFile(dir, "check_junk.dng", junk));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
        }
    }

    // ---- files from this program's writer ---------------------------------------------------------
    {
        const fs::path file = dir / "check_written.dng";
        for (DngCompression compression : {DngCompression::LosslessJpeg, DngCompression::None})
            for (DngByteOrder order : {DngByteOrder::Little, DngByteOrder::Big})
                for (int kind = 0; kind < 3; ++kind) {
                    // Several tiles with padding on both edges; a frame smaller
                    // than a tile; full colour.
                    RawImage img = kind == 0 ? mosaic(1100, 700, 5) : kind == 1 ? mosaic(120, 90, 6) : fullColour(600, 530);
                    DngWriteOptions options;
                    options.compression = compression;
                    options.byteOrder = order;
                    writeDng(img, file, options);
                    const DngCheck c = checkDng(file, kind == 0 ? 3 : 1);
                    CHECK(c.rawImage == State::Intact && c.original == State::Absent);
                    CHECK(c.intact() && !c.damaged());
                    CHECK(c.width == img.width && c.height == img.height && c.planes == img.samplesPerPixel);
                    CHECK(readStoredRawImage(file).samples == img.pixels);
                }

        // Without digests there is nothing to verify, but the data must decode.
        RawImage img = mosaic(1100, 700, 9);
        DngWriteOptions plain;
        plain.digests = false;
        writeDng(img, file, plain);
        DngCheck c = checkDng(file);
        CHECK(c.rawImage == State::Unverified && !c.damaged() && !c.intact());

        // With an embedded original.
        img.originalFile = Bytes(200000);
        for (size_t i = 0; i < img.originalFile.size(); ++i) img.originalFile[i] = static_cast<uint8_t>(i * 7 + i / 300);
        img.originalFileName = "shot.raw";
        writeDng(img, file, DngWriteOptions{});
        c = checkDng(file);
        CHECK(c.rawImage == State::Intact && c.original == State::Intact && c.intact());
        const Bytes sound = readAll(file);
        const std::string text(sound.begin(), sound.end());
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(text);
        const tiffreader::Ifd raw = tiffreader::parseIfd(text, ifd0.at(330).u32());

        // Damage inside the image: every tile in turn. All 1100 x 700 pixels
        // are real in the first tile; in edge tiles the stream also codes
        // padding, which no checksum covers.
        const tiffreader::Entry& offsets = raw.at(324);
        const tiffreader::Entry& counts = raw.at(325);
        {
            Bytes b = sound;
            b[offsets.u32(0) + counts.u32(0) / 2] ^= 0x04;
            c = checkDng(writeFile(dir, "check_dmg.dng", b));
            CHECK(c.rawImage == State::Damaged && c.original == State::Intact && c.damaged() && !c.intact());
        }
        // A changed digest.
        {
            Bytes b = sound;
            b[ifd0.at(51111).offset + 5] ^= 0x80;
            c = checkDng(writeFile(dir, "check_dmg.dng", b));
            CHECK(c.rawImage == State::Damaged);
            CHECK(c.rawDetail == "the raw image data does not match its checksum");
        }
        // Damage inside the embedded original: the image is still fine.
        {
            Bytes b = sound;
            b[ifd0.at(50828).offset + 1000] ^= 0x01;
            c = checkDng(writeFile(dir, "check_dmg.dng", b));
            CHECK(c.rawImage == State::Intact && c.original == State::Damaged && c.damaged());
        }
        // The end of the file missing: here that is part of the embedded
        // original, which the writer places last.
        {
            Bytes b(sound.begin(), sound.end() - 100);
            c = checkDng(writeFile(dir, "check_dmg.dng", b));
            CHECK(c.truncated && c.damaged() && !c.intact());
            CHECK(c.truncatedDetail == "the file is cut short: 100 bytes are missing at its end");
            CHECK(!checkDng(file).truncated);
        }
        // A tile table that points past the end of the file.
        {
            Bytes b = sound;
            b.resize(offsets.u32(2));
            bool threw = false;
            try {
                c = checkDng(writeFile(dir, "check_dmg.dng", b));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw || c.damaged());
        }
        // Random damage anywhere: never a crash, and never "intact" for a
        // file whose raw data or original changed.
        testutil::Random rnd(31);
        for (int round = 0; round < 400; ++round) {
            Bytes b = sound;
            const uint32_t kind = rnd.below(3);
            if (kind == 0)
                b.resize(rnd.below(static_cast<uint32_t>(b.size())));
            else
                for (uint32_t i = 0; i < 1 + rnd.below(kind == 1 ? 2 : 50); ++i)
                    b[rnd.below(static_cast<uint32_t>(b.size()))] ^= static_cast<uint8_t>(1 + rnd.below(255));
            try {
                const DngCheck r = checkDng(writeFile(dir, "check_fuzz.dng", b), 2);
                if (r.intact()) {
                    // Then the image and the original really are unchanged.
                    CHECK(readStoredRawImage(dir / "check_fuzz.dng").samples == img.pixels);
                }
            } catch (const std::runtime_error&) {
            }
        }
    }

    // ---- padding below the image costs next to nothing -----------------------------------------
    {
        // 513 rows: the second row of tiles holds one row of picture and 511
        // of padding.
        const fs::path a = dir / "check_pad_a.dng", b = dir / "check_pad_b.dng";
        DngWriteOptions options;
        options.embedThumbnail = false;
        RawImage tall = mosaic(1024, 513, 12);
        RawImage exact = tall;
        exact.height = 512;
        exact.activeBottom = 512;
        exact.cropHeight = 512;
        exact.pixels.resize(exact.sampleCount());
        writeDng(tall, a, options);
        writeDng(exact, b, options);
        CHECK(checkDng(a).intact());
        CHECK(readStoredRawImage(a).samples == tall.pixels);
        const double ratio = static_cast<double>(fs::file_size(a)) / static_cast<double>(fs::file_size(b));
        CHECK(ratio < 1.15);
    }

    return testutil::finish("test_check");
}
