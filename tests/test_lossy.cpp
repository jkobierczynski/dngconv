// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Lossy JPEG XL: the curve and the polynomial that undoes it, the linear
// values (black pattern, defect pixels), the stored frame (margin, fields,
// tiles), the rule --verify judges by, and, in a build with libjxl, the
// files the writer makes and what reading them back gives.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "dng_check.hpp"
#include "dng_writer.hpp"
#include "jxl_codec.hpp"
#include "lossy_jxl.hpp"
#include "test_util.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace fs = std::filesystem;

namespace {

RawImage mosaic(uint32_t w, uint32_t h, uint32_t period, uint64_t seed) {
    RawImage img;
    img.width = w;
    img.height = h;
    img.samplesPerPixel = 1;
    img.isCfa = true;
    img.cfaRows = img.cfaCols = period;
    if (period == 2) {
        img.cfaPattern = {0, 1, 1, 2};
    } else {
        // X-Trans
        const uint8_t xtrans[36] = {1, 1, 0, 1, 1, 2, 1, 1, 2, 1, 1, 0, 2, 0, 1, 0, 2, 1,
                                    1, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 2, 1, 2, 0, 1};
        img.cfaPattern.assign(xtrans, xtrans + 36);
    }
    img.activeTop = 2;
    img.activeLeft = 4;
    img.activeBottom = h;
    img.activeRight = w - 2;
    img.cropWidth = img.activeRight - img.activeLeft;
    img.cropHeight = img.activeBottom - img.activeTop;
    img.blackRows = img.blackCols = 2;
    img.blackLevel = {256, 260, 258, 262};
    img.whiteLevel = {4095, 4095, 4095, 4095};
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}, {0, 0, 0}}};
    img.uniqueCameraModel = "Testmake Model T";
    testutil::Random rnd(seed);
    img.pixels.resize(img.sampleCount());
    // Smooth shapes, some edges, some noise: enough structure for the
    // alignment test, and values over the whole range.
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const double u = static_cast<double>(x) / w, v = static_cast<double>(y) / h;
            double value = 300 + 3000 * (0.5 + 0.5 * std::sin(9 * u + 4 * v)) * (u * u + v);
            if ((x / 23 + y / 17) % 5 == 0) value += 400;
            value += rnd.below(40);
            img.pixels[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(std::min(value, 4095.0));
        }
    return img;
}

RawImage monochrome(uint32_t w, uint32_t h) {
    RawImage img = mosaic(w, h, 2, 9);
    img.isCfa = false;
    img.cfaRows = img.cfaCols = 0;
    img.cfaPattern.clear();
    img.colorPlanes = 1;
    img.hasColorMatrix = false;
    img.blackRows = img.blackCols = 1;
    img.blackLevel = {256};
    return img;
}

double readDouble(const std::vector<uint8_t>& b, size_t at) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | b.at(at + i);
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

uint32_t readBig32(const std::vector<uint8_t>& b, size_t at) {
    return (static_cast<uint32_t>(b.at(at)) << 24) | (b.at(at + 1) << 16) | (b.at(at + 2) << 8) | b.at(at + 3);
}

template <typename Function>
bool throws(Function f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = (argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path()) / "lossy_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // ---- the curve --------------------------------------------------------------------------
    {
        CHECK(lossyCurveEncode(0) == 0 && std::fabs(lossyCurveEncode(1) - 1) < 1e-12);
        CHECK(lossyCurveEncode(-0.5) < 1e-12 && std::fabs(lossyCurveEncode(2) - 1) < 1e-12);
        double previous = -1;
        double worst = 0;
        for (int i = 0; i <= 100000; ++i) {
            const double x = i / 100000.0;
            const double y = lossyCurveEncode(x);
            CHECK(y > previous || (i == 0 && y == 0));
            previous = y;
            worst = std::max(worst, std::fabs(lossyCurveDecode(y) - x));
        }
        CHECK(worst < 1e-12);
        // Values from the formula: f(y) = y/16 + (15/16) y^3.
        CHECK(std::fabs(lossyCurveDecode(0.5) - (0.5 / 16 + 15.0 / 16 * 0.125)) < 1e-15);
        CHECK(std::fabs(lossyCurveEncode(0.5 / 16 + 15.0 / 16 * 0.125) - 0.5) < 1e-12);
        // The shadows get many more codes than in linear coding: the darkest
        // 1 % of the light takes an eighth of the stored range.
        CHECK(lossyCurveEncode(0.01) > 0.125 && lossyCurveEncode(0.01) < 0.135);
    }

    // ---- the polynomial and its opcode ------------------------------------------------------------
    {
        LossyEncoding e;
        e.lower = 0.002;
        e.upper = 0.75;
        e.activeWidth = 4000;
        e.activeHeight = 3000;
        const std::vector<double> c = e.polynomial();
        CHECK(c.size() == 4);
        for (double y : {0.0, 0.1, 0.37, 0.8, 1.0}) {
            const double p = c[0] + c[1] * y + c[2] * y * y + c[3] * y * y * y;
            CHECK(std::fabs(p - (e.lower + (e.upper - e.lower) * lossyCurveDecode(y))) < 1e-15);
        }
        const std::vector<uint8_t> op = e.opcodeList2();
        CHECK(op.size() == 4 + 16 + 36 + 32);
        CHECK(readBig32(op, 0) == 1);           // one opcode
        CHECK(readBig32(op, 4) == 8);           // MapPolynomial
        CHECK(readBig32(op, 8) == 0x01030000);  // DNG 1.3
        CHECK(readBig32(op, 12) == 0);          // not optional
        CHECK(readBig32(op, 16) == 36 + 32);    // bytes that follow
        CHECK(readBig32(op, 20) == 0 && readBig32(op, 24) == 0);
        CHECK(readBig32(op, 28) == 3000 && readBig32(op, 32) == 4000);  // bottom, right
        CHECK(readBig32(op, 36) == 0 && readBig32(op, 40) == 1);        // plane, planes
        CHECK(readBig32(op, 44) == 1 && readBig32(op, 48) == 1);        // pitches
        CHECK(readBig32(op, 52) == 3);                                  // degree
        for (int k = 0; k < 4; ++k) CHECK(readDouble(op, 56 + 8 * k) == c[static_cast<size_t>(k)]);
    }

    // ---- linear values -------------------------------------------------------------------------
    {
        RawImage img = mosaic(40, 30, 2, 1);
        // A black pattern counted from the active area (which starts on an
        // even row and column here, so the frame's phase is the same).
        img.blackLevel = {100, 200, 300, 400};
        img.whiteLevel = {4000, 4100, 4200, 4000};  // the mosaic gets the lowest: 4000
        img.pixels[static_cast<size_t>(img.activeTop) * img.width + img.activeLeft] = 1000;      // black 100
        img.pixels[static_cast<size_t>(img.activeTop) * img.width + img.activeLeft + 1] = 1000;  // black 200
        img.pixels[static_cast<size_t>(img.activeTop + 1) * img.width + img.activeLeft] = 50;    // below its black
        img.pixels[static_cast<size_t>(img.activeTop + 1) * img.width + img.activeLeft + 1] = 4095;  // above white
        const std::vector<uint16_t> lin = linearActiveArea(img, 2);
        const uint32_t aw = img.activeRight - img.activeLeft;
        CHECK(lin.size() == static_cast<size_t>(aw) * (img.activeBottom - img.activeTop));
        // Scaled by white minus the highest black (400), as Adobe's reader does.
        CHECK(lin[0] == std::lround((1000 - 100) / 3600.0 * 65535));
        CHECK(lin[1] == std::lround((1000 - 200) / 3600.0 * 65535));
        CHECK(lin[aw] == 0 && lin[aw + 1] == 65535);

        // Defective pixels, marked 0: green from the diagonals, red and blue
        // from two pixels away; neighbours marked 0 themselves are left out.
        RawImage d = mosaic(40, 30, 2, 2);
        d.blackLevel = {0, 0, 0, 0};
        d.whiteLevel = {65535, 65535, 65535, 65535};
        d.zeroIsBadPixel = true;
        const auto at = [&](uint32_t ay, uint32_t ax) -> uint16_t& {
            return d.pixels[static_cast<size_t>(ay + d.activeTop) * d.width + ax + d.activeLeft];
        };
        // (10, 10) is red (pattern R G / G B from the active area's origin), (10, 11) green.
        at(10, 10) = 0;
        at(8, 10) = 100, at(12, 10) = 300, at(10, 8) = 500, at(10, 12) = 0;
        at(10, 11) = 0;
        at(9, 10) = 1000, at(9, 12) = 2000, at(11, 10) = 3000, at(11, 12) = 4001;
        uint32_t repaired = 0;
        const std::vector<uint16_t> fixed = linearActiveArea(d, 1, &repaired);
        const uint32_t dw = d.activeRight - d.activeLeft;
        CHECK(repaired == 3);  // (10, 10), (10, 11) and the neighbour (10, 12) set to 0
        CHECK(fixed[10 * dw + 10] == (100 + 300 + 500 + 1) / 3);
        CHECK(fixed[10 * dw + 11] == (1000 + 2000 + 3000 + 4001 + 2) / 4);
        d.zeroIsBadPixel = false;  // kept as they are otherwise
        CHECK(linearActiveArea(d, 1)[10 * dw + 10] == 0);
    }

    // ---- the stored frame -----------------------------------------------------------------------
    {
        struct Case {
            uint32_t w, h, period;
            bool mono;
        };
        for (const Case& k : {Case{2001, 1203, 2, false}, Case{1600, 1200, 2, false}, Case{130, 77, 2, false},
                              Case{1205, 803, 6, false}, Case{901, 333, 1, true}}) {
            const RawImage img = k.mono ? monochrome(k.w, k.h) : mosaic(k.w, k.h, k.period, 3);
            CHECK(lossyJxlSupported(img));
            const LossyEncoding e = encodeForLossyJxl(img, kLossyMaxTile, 3);
            const uint32_t aw = img.activeRight - img.activeLeft, ah = img.activeBottom - img.activeTop;
            const uint32_t f = k.mono ? 1 : k.period;
            CHECK(e.activeWidth == aw && e.activeHeight == ah);
            CHECK(e.rowFactor == f && e.columnFactor == f);
            CHECK(e.width % f == 0 && e.height % f == 0);
            CHECK((e.width / f) % e.tileWidth == 0 && (e.height / f) % e.tileHeight == 0);  // fields: whole tiles
            CHECK(e.tileWidth % 8 == 0 && e.tileHeight % 8 == 0);
            CHECK(e.tileWidth <= kLossyMaxTile + 7 && e.tileHeight <= kLossyMaxTile + 7);
            CHECK(e.width >= aw && e.height >= ah && e.padLeft + aw <= e.width && e.padTop + ah <= e.height);
            CHECK(e.width - aw - e.padLeft - e.padLeft <= 1 && e.height - ah - e.padTop - e.padTop <= 1);  // centred
            CHECK(e.samples.size() == static_cast<size_t>(e.width) * e.height);
            CHECK(e.lower >= 0 && e.upper <= 1 && e.upper - e.lower >= 255 / 65535.0 - 1e-12);
            // Inside, the curve of the linear values; in the margin, a copy of
            // a sample of the same place in the pattern.
            const std::vector<uint16_t> lin = linearActiveArea(img, 3);
            const auto coded = [&](uint16_t v) {
                return static_cast<uint16_t>(
                    std::lround(lossyCurveEncode((v / 65535.0 - e.lower) / (e.upper - e.lower)) * 65535.0));
            };
            bool inside = true, margin = true;
            for (uint32_t y = 0; y < e.height; y += 7)
                for (uint32_t x = 0; x < e.width; x += 3) {
                    const int64_t ay = static_cast<int64_t>(y) - e.padTop, ax = static_cast<int64_t>(x) - e.padLeft;
                    const uint16_t s = e.samples[static_cast<size_t>(y) * e.width + x];
                    if (ay >= 0 && ax >= 0 && ay < ah && ax < aw) {
                        inside = inside && s == coded(lin[static_cast<size_t>(ay) * aw + static_cast<size_t>(ax)]);
                    } else {
                        bool found = false;
                        for (uint32_t sy = static_cast<uint32_t>(((ay % f) + f) % f); sy < ah && !found; sy += f)
                            for (uint32_t sx = static_cast<uint32_t>(((ax % f) + f) % f); sx < aw && !found; sx += f)
                                found = (sy < 2 * f || sy >= ah - 2 * f || sx < 2 * f || sx >= aw - 2 * f) &&
                                        coded(lin[static_cast<size_t>(sy) * aw + sx]) == s;
                        margin = margin && found;
                    }
                }
            CHECK(inside);
            CHECK(margin);
        }
        // Not stored lossily: full-colour data, and frames smaller than two periods.
        RawImage colour = monochrome(64, 64);
        colour.samplesPerPixel = 3;
        colour.colorPlanes = 3;
        colour.pixels.resize(colour.sampleCount());
        std::string why;
        CHECK(!lossyJxlSupported(colour, &why) && why.find("full-colour") != std::string::npos);
        CHECK(throws([&] { encodeForLossyJxl(colour, kLossyMaxTile, 1); }));
        RawImage tiny = mosaic(16, 16, 6, 4);
        tiny.activeTop = tiny.activeLeft = 0;
        tiny.activeBottom = tiny.activeRight = 11;
        tiny.cropWidth = tiny.cropHeight = 11;
        CHECK(!lossyJxlSupported(tiny));
        // A picture that is flat gets a range of at least 256 steps.
        RawImage flat = mosaic(64, 64, 2, 5);
        std::fill(flat.pixels.begin(), flat.pixels.end(), 1000);
        const LossyEncoding fe = encodeForLossyJxl(flat, kLossyMaxTile, 1);
        CHECK(std::fabs(fe.upper - fe.lower - 255 / 65535.0) < 1e-12);
    }

    // ---- the rule ------------------------------------------------------------------------------------
    {
        LossyErrors good;
        good.rms = 0.011;
        good.worstTileRms = 0.012;
        good.worstBlockMean = 0.003;
        good.mean = 0.0002;
        good.alignment = 0.2;
        std::string why;
        CHECK(lossyCloseEnough(good, 1.0f, why) && why.empty());
        LossyErrors e = good;
        e.rms = 0.026;
        CHECK(!lossyCloseEnough(e, 1.0f, why) && why.find("(rms)") != std::string::npos);
        CHECK(lossyCloseEnough(e, 2.0f, why));
        e = good;
        e.worstTileRms = 0.031;
        e.worstTile = 7;
        CHECK(!lossyCloseEnough(e, 1.0f, why) && why.find("tile 7") != std::string::npos);
        e = good;
        e.worstBlockMean = 0.0111;
        CHECK(!lossyCloseEnough(e, 1.0f, why) && why.find("local average") != std::string::npos);
        e = good;
        e.alignment = -0.01;
        e.closestMove = "one period right (colour field 2)";
        CHECK(!lossyCloseEnough(e, 1.0f, why) && why.find("one period right") != std::string::npos);
        e = good;
        e.mean = -0.0012;
        CHECK(!lossyCloseEnough(e, 1.0f, why) && why.find("shifted") != std::string::npos);
    }

    // ---- files ---------------------------------------------------------------------------------------
    const fs::path dng = dir / "lossy.dng";
    if (!jxlAvailable()) {
        DngWriteOptions o;
        o.compression = DngCompression::JpegXl;
        o.jxlDistance = 1.0f;
        CHECK(throws([&] { writeDng(mosaic(64, 64, 2, 1), dng, o); }));
        return testutil::finish("test_lossy");
    }
    {
        struct Case {
            const char* name;
            RawImage img;
            DngByteOrder order;
            float distance;
        };
        std::vector<Case> cases;
        cases.push_back({"Bayer", mosaic(1601, 1203, 2, 11), DngByteOrder::Little, 1.0f});
        cases.push_back({"Bayer, big-endian, close", mosaic(901, 650, 2, 12), DngByteOrder::Big, 0.2f});
        cases.push_back({"X-Trans", mosaic(1205, 803, 6, 13), DngByteOrder::Little, 2.0f});
        cases.push_back({"monochrome", monochrome(700, 500), DngByteOrder::Little, 0.5f});
        RawImage defects = mosaic(800, 600, 2, 14);
        defects.zeroIsBadPixel = true;
        for (uint32_t i = 1; i < 40; ++i) defects.pixels[static_cast<size_t>(i * 13 + 20) * 800 + i * 17 + 30] = 0;
        cases.push_back({"defect pixels", defects, DngByteOrder::Little, 1.0f});
        for (Case& c : cases) {
            DngWriteOptions o;
            o.compression = DngCompression::JpegXl;
            o.jxlDistance = c.distance;
            o.byteOrder = c.order;
            o.threads = 3;
            writeDng(c.img, dng, o);
            const LossyEncoding expected = encodeForLossyJxl(c.img, kLossyMaxTile, 3);
            const std::string bytes = tiffreader::slurp(dng);
            const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(bytes);
            const tiffreader::Ifd raw = tiffreader::parseIfd(bytes, ifd0.at(330).u32(0));
            CHECK(ifd0.at(50706).data == std::vector<uint8_t>({1, 7, 1, 0}));
            CHECK(ifd0.at(50707).data == std::vector<uint8_t>({1, 7, static_cast<uint8_t>(c.img.isCfa ? 1 : 0), 0}));
            CHECK(raw.at(259).u16() == 52546);
            CHECK(raw.at(256).u32() == expected.width && raw.at(257).u32() == expected.height);
            CHECK(raw.at(322).u32() == expected.tileWidth && raw.at(323).u32() == expected.tileHeight);
            CHECK(raw.at(324).count == (expected.width / expected.tileWidth) * (expected.height / expected.tileHeight));
            CHECK(raw.at(50714).count == 1 && raw.at(50714).u32() == 0);  // black
            CHECK(raw.at(50713).u16(0) == 1 && raw.at(50713).u16(1) == 1);
            CHECK(raw.at(50717).u32() == 65535);  // white
            CHECK(raw.at(50829).u32(0) == expected.padTop && raw.at(50829).u32(1) == expected.padLeft &&
                  raw.at(50829).u32(2) == expected.padTop + expected.activeHeight &&
                  raw.at(50829).u32(3) == expected.padLeft + expected.activeWidth);
            CHECK(raw.at(50719).u32(0) == c.img.cropLeft && raw.at(50720).u32(0) == c.img.cropWidth);
            CHECK(raw.at(51009).data == expected.opcodeList2());
            CHECK(raw.count(51008) == 0);  // defect pixels filled in, no opcode for them
            float written;
            std::memcpy(&written, raw.at(52553).data.data(), 4);
            CHECK(raw.at(52553).type == 11 && written == c.distance);
            CHECK(raw.at(52554).u32() == 3);
            CHECK(raw.count(50975) == (c.img.isCfa ? 1u : 0u) && raw.count(52547) == (c.img.isCfa ? 1u : 0u));
            if (c.img.isCfa) CHECK(raw.at(50975).u16() == c.img.cfaRows && raw.at(52547).u16() == c.img.cfaCols);

            // The file's own checksum, of the compressed tiles, holds.
            const DngCheck check = checkDng(dng);
            CHECK(check.intact());
            // Lossy data is read only when asked for.
            CHECK(throws([&] { readStoredRawImage(dng); }));
            const StoredRawImage back = readStoredRawImage(dng, 3, true);
            CHECK(back.width == expected.width && back.height == expected.height && back.planes == 1);
            const LossyErrors errors = measureLossyErrors(expected, back.samples);
            std::string why;
            if (!lossyCloseEnough(errors, c.distance, why))
                std::fprintf(stderr, "  %s: %s (%s)\n", c.name, why.c_str(), describe(errors).c_str());
            CHECK(lossyCloseEnough(errors, c.distance, why));
            CHECK(errors.rms > 0 && errors.rms < 0.0125 * c.distance);
            CHECK(errors.alignment > 0);

            // What --verify must catch: the frame a period off, colours exchanged.
            const uint32_t W = expected.width, period = expected.columnFactor;
            if (c.img.isCfa) {
                std::vector<uint16_t> shifted = back.samples;
                for (uint32_t y = 0; y < expected.height; ++y)
                    for (uint32_t x = period; x < W; ++x)
                        shifted[static_cast<size_t>(y) * W + x] = back.samples[static_cast<size_t>(y) * W + x - period];
                CHECK(!lossyCloseEnough(measureLossyErrors(expected, shifted), c.distance, why));
                std::vector<uint16_t> swapped = back.samples;
                for (uint32_t y = 0; y + 1 < expected.height; y += 2)
                    for (uint32_t x = 0; x + 1 < W; x += 2)
                        std::swap(swapped[static_cast<size_t>(y) * W + x], swapped[static_cast<size_t>(y + 1) * W + x + 1]);
                CHECK(!lossyCloseEnough(measureLossyErrors(expected, swapped), c.distance, why));
            }
            std::vector<uint16_t> wrongSize(back.samples.begin(), back.samples.end() - 1);
            CHECK(!lossyCloseEnough(measureLossyErrors(expected, wrongSize), c.distance, why));
        }

        // Refused: a distance out of range, an image that cannot be stored lossily.
        DngWriteOptions o;
        o.compression = DngCompression::JpegXl;
        for (float bad : {-1.0f, 25.5f, std::nanf("")}) {
            o.jxlDistance = bad;
            CHECK(throws([&] { writeDng(mosaic(64, 64, 2, 1), dng, o); }));
        }
        o.jxlDistance = 1.0f;
        RawImage colour = monochrome(64, 64);
        colour.samplesPerPixel = 3;
        colour.colorPlanes = 3;
        colour.hasColorMatrix = true;
        colour.blackLevel = {0, 0, 0};
        colour.pixels.resize(colour.sampleCount());
        CHECK(throws([&] { writeDng(colour, dng, o); }));
        // The codec on its own.
        std::vector<uint16_t> picture(64 * 48, 1234);
        CHECK(throws([&] { encodeJxlLossy(picture.data(), 64, 48, 0.0f, 3); }));
        CHECK(throws([&] { encodeJxlLossy(picture.data(), 0, 48, 1.0f, 3); }));
        const std::vector<uint8_t> coded = encodeJxlLossy(picture.data(), 64, 48, 1.0f, 3);
        const JxlPicture decoded = decodeJxl(coded.data(), coded.size(), picture.size());
        CHECK(!decoded.originalValues && decoded.width == 64 && decoded.height == 48 && decoded.channels == 1);
        int worst = 0;
        for (size_t i = 0; i < picture.size(); ++i) worst = std::max(worst, std::abs(decoded.samples[i] - picture[i]));
        CHECK(worst < 200);  // a flat picture comes back flat, or nearly
    }

    fs::remove_all(dir, ec);
    return testutil::finish("test_lossy");
}
