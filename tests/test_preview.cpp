// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// MD5, the raw-image digest and the rendered thumbnail.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "dng_writer.hpp"
#include "md5.hpp"
#include "test_util.hpp"
#include "thumbnail.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace fs = std::filesystem;

namespace {

std::string hex(const Md5Digest& d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

std::string hex(const std::vector<uint8_t>& v) {
    Md5Digest d{};
    for (size_t i = 0; i < d.size() && i < v.size(); ++i) d[i] = v[i];
    return hex(d);
}

// ---- frames ---------------------------------------------------------------------

const double kMatrix[3][3] = {{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}};
const double kNeutral[3] = {0.5, 1.0, 0.625};
const double kXyzFromSrgb[3][3] = {{0.4124564, 0.3575761, 0.1804375},
                                   {0.2126729, 0.7151522, 0.0721750},
                                   {0.0193339, 0.1191920, 0.9503041}};

RawImage frame(uint32_t width, uint32_t height, uint32_t spp) {
    RawImage img;
    img.width = width;
    img.height = height;
    img.samplesPerPixel = spp;
    img.colorPlanes = 3;
    img.activeBottom = height;
    img.activeRight = width;
    img.cropWidth = width;
    img.cropHeight = height;
    img.blackLevel.assign(spp, 0.0);
    img.whiteLevel = {4000, 4000, 4000, 4000};
    img.hasColorMatrix = true;
    for (int p = 0; p < 3; ++p)
        for (int k = 0; k < 3; ++k) img.colorMatrix[p][k] = kMatrix[p][k];
    img.hasAsShotNeutral = true;
    img.asShotNeutral = {kNeutral[0], kNeutral[1], kNeutral[2], 1.0};
    img.uniqueCameraModel = "Testmake Model T";
    img.pixels.assign(img.sampleCount(), 0);
    if (spp == 1) {
        img.isCfa = true;
        img.cfaRows = img.cfaCols = 2;
        img.cfaPattern = {0, 1, 1, 2};  // RGGB
    }
    return img;
}

// A fixed pattern, the same on every platform, for digest known-answer tests.
void fillPattern(RawImage& img) {
    size_t i = 0;
    for (uint32_t y = 0; y < img.height; ++y)
        for (uint32_t x = 0; x < img.width; ++x)
            for (uint32_t k = 0; k < img.samplesPerPixel; ++k)
                img.pixels[i++] = static_cast<uint16_t>((x * 31u + y * 17u + k * 101u + (x * y) % 13u) & 0x3fffu);
}

// What the sensor records, per colour plane, for a linear sRGB colour under
// the frame's white balance, as a fraction of full scale. Derived here
// independently of the renderer: XYZ -> camera matrix chained with
// sRGB -> XYZ, every plane scaled so that sRGB white gives equal values, then
// the white balance undone.
void cameraResponse(const double rgb[3], double out[3]) {
    for (int p = 0; p < 3; ++p) {
        double row[3] = {0, 0, 0}, sum = 0;
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) row[c] += kMatrix[p][k] * kXyzFromSrgb[k][c];
            sum += row[c];
        }
        double v = 0;
        for (int c = 0; c < 3; ++c) v += row[c] / sum * rgb[c];
        out[p] = v * kNeutral[p];
    }
}

// Paints an area of a Bayer frame with one colour.
void paint(RawImage& img, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
           const double rgb[3]) {
    double cam[3];
    cameraResponse(rgb, cam);
    for (uint32_t y = top; y < bottom; ++y)
        for (uint32_t x = left; x < right; ++x) {
            const uint32_t ay = y - img.activeTop, ax = x - img.activeLeft;
            double black, value;
            if (img.isCfa) {
                const unsigned plane = img.cfaPattern[(ay % img.cfaRows) * img.cfaCols + ax % img.cfaCols];
                black = img.blackLevel[(ay % img.blackRows) * img.blackCols + ax % img.blackCols];
                value = cam[plane];
                img.pixels[static_cast<size_t>(y) * img.width + x] =
                    static_cast<uint16_t>(std::lround(black + value * (img.whiteLevel[0] - black)));
            } else {
                for (uint32_t k = 0; k < img.samplesPerPixel; ++k) {
                    black = img.blackLevel[k];
                    value = img.colorPlanes == 1 ? rgb[0] : cam[k];
                    img.pixels[(static_cast<size_t>(y) * img.width + x) * img.samplesPerPixel + k] =
                        static_cast<uint16_t>(std::lround(black + value * (img.whiteLevel[k] - black)));
                }
            }
        }
}

int srgb8(double linear) {
    if (linear <= 0) return 0;
    if (linear >= 1) return 255;
    const double v = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(v * 255.0));
}

// Checks that every thumbnail pixel is close to the expected colour.
bool uniform(const RgbImage& t, int r, int g, int b, int tolerance) {
    if (t.pixels.empty()) return false;
    for (size_t i = 0; i + 2 < t.pixels.size(); i += 3)
        if (std::abs(t.pixels[i] - r) > tolerance || std::abs(t.pixels[i + 1] - g) > tolerance ||
            std::abs(t.pixels[i + 2] - b) > tolerance) {
            std::fprintf(stderr, "pixel %zu is (%d, %d, %d), expected (%d, %d, %d)\n", i / 3,
                         t.pixels[i], t.pixels[i + 1], t.pixels[i + 2], r, g, b);
            return false;
        }
    return true;
}

// The image digest, computed the plain way: no threads, no shared code with
// the writer beyond MD5 itself.
std::string plainDigest(const RawImage& img) {
    const uint32_t tw = std::min<uint32_t>(256, img.width), th = std::min<uint32_t>(256, img.height);
    Md5 all;
    for (uint32_t top = 0; top < img.height; top += th)
        for (uint32_t left = 0; left < img.width; left += tw) {
            Md5 tile;
            for (uint32_t k = 0; k < img.samplesPerPixel; ++k)
                for (uint32_t y = top; y < std::min(top + th, img.height); ++y)
                    for (uint32_t x = left; x < std::min(left + tw, img.width); ++x) {
                        const uint16_t v = img.pixels[(static_cast<size_t>(y) * img.width + x) * img.samplesPerPixel + k];
                        const uint8_t le[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
                        tile.update(le, 2);
                    }
            const Md5Digest d = tile.finish();
            all.update(d.data(), d.size());
        }
    return hex(all.finish());
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path();
    const bool keepFiles = argc > 2;  // for checking the files with another validator

    // ---- MD5: the test suite of RFC 1321 -------------------------------------------
    {
        const std::pair<const char*, const char*> vectors[] = {
            {"", "d41d8cd98f00b204e9800998ecf8427e"},
            {"a", "0cc175b9c0f1b6a831c399e269772661"},
            {"abc", "900150983cd24fb0d6963f7d28e17f72"},
            {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
            {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
            {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
             "d174ab98d277d9f5a5611c2c9f419d9f"},
            {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
             "57edf4a22be3c955ac49da2e2107b67a"},
        };
        for (const auto& [text, expected] : vectors) {
            const std::string s(text);
            CHECK(hex(Md5::of(s.data(), s.size())) == expected);
        }

        // An empty update, even with a null pointer, changes nothing.
        Md5 withEmpty;
        withEmpty.update(nullptr, 0);
        withEmpty.update("abc", 3);
        withEmpty.update(nullptr, 0);
        CHECK(hex(withEmpty.finish()) == "900150983cd24fb0d6963f7d28e17f72");

        // Feeding the data in pieces gives the same result, at every length
        // around the block and padding boundaries.
        testutil::Random rnd(11);
        std::vector<uint8_t> data(300);
        for (uint8_t& b : data) b = static_cast<uint8_t>(rnd.next());
        for (size_t length : {0u, 1u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 121u, 128u, 300u}) {
            const std::string whole = hex(Md5::of(data.data(), length));
            Md5 bytewise;
            for (size_t i = 0; i < length; ++i) bytewise.update(&data[i], 1);
            CHECK(hex(bytewise.finish()) == whole);
            Md5 chunks;
            for (size_t at = 0; at < length;) {
                const size_t n = std::min<size_t>(1 + rnd.below(90), length - at);
                chunks.update(&data[at], n);
                at += n;
            }
            CHECK(hex(chunks.finish()) == whole);
        }
    }

    // ---- NewRawImageDigest ------------------------------------------------------------
    // The expected values were confirmed with Adobe's dng_validate (DNG SDK
    // 1.5.1), which recomputes the digest and reports a mismatch: it accepted
    // the files this test writes. They pin the algorithm to the reference.
    {
        struct DigestCase {
            const char* name;
            uint32_t width, height, spp;
            const char* expected;
        };
        const DigestCase digestCases[] = {
            {"mosaic-odd", 1037, 771, 1, "3f865ce7e472538e1f0bc2774b059188"},
            {"rgb", 333, 201, 3, "a8365e966a0d1bee02e1c409e252b539"},
            {"smaller-than-a-tile", 120, 90, 1, "5c11dddcaf36010dfec3d3af0f6784ff"},
            {"exactly-one-tile", 256, 256, 1, "bb3b4e6b73f9a2e8c3bad8548c8e8f52"},
            {"one-pixel-more", 257, 513, 1, "e5c0f628eade9b407e759b4f17bece71"},
        };
        for (const DigestCase& dc : digestCases) {
            RawImage img = frame(dc.width, dc.height, dc.spp);
            fillPattern(img);
            const fs::path file = dir / (std::string("dngconv_test_digest_") + dc.name + ".dng");
            writeDng(img, file);
            const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(tiffreader::slurp(file));
            const std::string stored = hex(ifd0.at(51111).data);
            CHECK(stored == plainDigest(img));
            if (stored != dc.expected)
                std::fprintf(stderr, "digest %s: got %s, expected %s\n", dc.name, stored.c_str(), dc.expected);
            CHECK(stored == dc.expected);
            if (!keepFiles) {
                std::error_code ec;
                fs::remove(file, ec);
            }
        }
    }

    // ---- thumbnail: size ---------------------------------------------------------------
    {
        const double grey[3] = {0.2, 0.2, 0.2};
        RawImage img = frame(1037, 771, 1);
        paint(img, 0, 0, img.width, img.height, grey);
        img.activeTop = 6;
        img.activeLeft = 10;
        img.activeBottom = 762;
        img.activeRight = 1030;
        img.cropLeft = 4;
        img.cropTop = 2;
        img.cropWidth = 1008;
        img.cropHeight = 752;
        RgbImage t = renderThumbnail(img);
        CHECK(t.width == 256 && t.height == 191);
        CHECK(t.pixels.size() == 256u * 191u * 3u);

        ThumbnailOptions small;
        small.maxSize = 64;
        t = renderThumbnail(img, small);
        CHECK(t.width == 64 && t.height == 48);

        img.scaleH = 2.0;  // pixels twice as wide as high
        t = renderThumbnail(img);
        CHECK(t.width == 256 && t.height == 95);
        img.scaleH = 1.0;

        RawImage portrait = frame(600, 900, 1);
        t = renderThumbnail(portrait);
        CHECK(t.width == 171 && t.height == 256);

        // Smaller than the limit: not enlarged, and never finer than the mosaic.
        RawImage tiny = frame(8, 6, 1);
        t = renderThumbnail(tiny);
        CHECK(t.width == 4 && t.height == 3);
        RawImage tinyRgb = frame(8, 6, 3);
        t = renderThumbnail(tinyRgb);
        CHECK(t.width == 8 && t.height == 6);

        // Inconsistent input gives no picture rather than a crash.
        RawImage broken = img;
        broken.pixels.pop_back();
        CHECK(renderThumbnail(broken).pixels.empty());
        broken = img;
        broken.cfaPattern.pop_back();
        CHECK(renderThumbnail(broken).pixels.empty());
        broken = img;
        broken.cropWidth = 5000;
        CHECK(renderThumbnail(broken).pixels.empty());
    }

    // ---- thumbnail: colour ----------------------------------------------------------------
    {
        ThumbnailOptions plain;
        plain.autoExposure = false;

        // A neutral subject comes out neutral, at the right level.
        const double grey[3] = {0.18, 0.18, 0.18};
        RawImage img = frame(640, 480, 1);
        paint(img, 0, 0, img.width, img.height, grey);
        CHECK(uniform(renderThumbnail(img, plain), srgb8(0.18), srgb8(0.18), srgb8(0.18), 2));

        // So does a coloured one.
        const double orange[3] = {0.6, 0.3, 0.1};
        paint(img, 0, 0, img.width, img.height, orange);
        CHECK(uniform(renderThumbnail(img, plain), srgb8(0.6), srgb8(0.3), srgb8(0.1), 3));

        // Black stays black; a saturated sensor renders white, not tinted.
        std::fill(img.pixels.begin(), img.pixels.end(), uint16_t{0});
        CHECK(uniform(renderThumbnail(img, plain), 0, 0, 0, 0));
        std::fill(img.pixels.begin(), img.pixels.end(), uint16_t{4000});
        CHECK(uniform(renderThumbnail(img, plain), 255, 255, 255, 0));

        // A black level that differs from cell to cell is taken out per cell.
        RawImage patterned = frame(640, 480, 1);
        patterned.blackRows = patterned.blackCols = 2;
        patterned.blackLevel = {200, 240, 280, 320};
        paint(patterned, 0, 0, patterned.width, patterned.height, grey);
        // Levels are scaled against the highest black, so allow a little more room.
        CHECK(uniform(renderThumbnail(patterned, plain), srgb8(0.18), srgb8(0.18), srgb8(0.18), 6));

        // Only the default crop is shown: a saturated border must not appear.
        RawImage bordered = frame(700, 500, 1);
        std::fill(bordered.pixels.begin(), bordered.pixels.end(), uint16_t{4000});
        bordered.activeTop = 10;
        bordered.activeLeft = 20;
        bordered.activeBottom = 490;
        bordered.activeRight = 680;
        bordered.cropLeft = 8;
        bordered.cropTop = 6;
        bordered.cropWidth = 640;
        bordered.cropHeight = 460;
        paint(bordered, 28, 16, 28 + 640, 16 + 460, grey);
        CHECK(uniform(renderThumbnail(bordered, plain), srgb8(0.18), srgb8(0.18), srgb8(0.18), 2));

        // Other mosaic phases and a 6x6 pattern.
        for (const std::vector<uint8_t>& pattern :
             {std::vector<uint8_t>{1, 0, 2, 1}, std::vector<uint8_t>{2, 1, 1, 0},
              std::vector<uint8_t>{1, 2, 0, 1}}) {
            RawImage other = frame(640, 480, 1);
            other.cfaPattern = pattern;
            paint(other, 0, 0, other.width, other.height, orange);
            CHECK(uniform(renderThumbnail(other, plain), srgb8(0.6), srgb8(0.3), srgb8(0.1), 3));
        }
        RawImage xtrans = frame(612, 408, 1);
        xtrans.cfaRows = xtrans.cfaCols = 6;
        xtrans.cfaPattern = {1, 1, 0, 1, 1, 2, 1, 1, 2, 1, 1, 0, 2, 0, 1, 0, 2, 1,
                             1, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 2, 1, 2, 0, 1};
        paint(xtrans, 0, 0, xtrans.width, xtrans.height, orange);
        CHECK(uniform(renderThumbnail(xtrans, plain), srgb8(0.6), srgb8(0.3), srgb8(0.1), 3));

        // Full-colour and monochrome data.
        RawImage rgb = frame(320, 240, 3);
        paint(rgb, 0, 0, rgb.width, rgb.height, orange);
        CHECK(uniform(renderThumbnail(rgb, plain), srgb8(0.6), srgb8(0.3), srgb8(0.1), 3));
        RawImage mono = frame(320, 240, 1);
        mono.isCfa = false;
        mono.colorPlanes = 1;
        mono.hasColorMatrix = false;
        mono.hasAsShotNeutral = false;
        paint(mono, 0, 0, mono.width, mono.height, grey);
        CHECK(uniform(renderThumbnail(mono, plain), srgb8(0.18), srgb8(0.18), srgb8(0.18), 2));

        // Without a white balance in the file, daylight is assumed: a subject
        // lit by D65 is then neutral when the sensor shows the matrix's own
        // response to white.
        RawImage daylight = frame(320, 240, 1);
        daylight.hasAsShotNeutral = false;
        double white[3] = {0, 0, 0};
        for (int p = 0; p < 3; ++p)
            for (int k = 0; k < 3; ++k)
                for (int c = 0; c < 3; ++c) white[p] += kMatrix[p][k] * kXyzFromSrgb[k][c];
        const double largest = std::max({white[0], white[1], white[2]});
        for (uint32_t y = 0; y < daylight.height; ++y)
            for (uint32_t x = 0; x < daylight.width; ++x)
                daylight.pixels[static_cast<size_t>(y) * daylight.width + x] = static_cast<uint16_t>(
                    std::lround(0.18 * white[daylight.cfaPattern[(y % 2) * 2 + x % 2]] / largest * 4000));
        CHECK(uniform(renderThumbnail(daylight, plain), srgb8(0.18), srgb8(0.18), srgb8(0.18), 2));
    }

    // ---- thumbnail: exposure ---------------------------------------------------------------
    {
        RawImage img = frame(640, 480, 1);
        const double half[3] = {0.5, 0.5, 0.5};
        paint(img, 0, 0, img.width, img.height, half);
        CHECK(uniform(renderThumbnail(img), 255, 255, 255, 1));  // brightened to white

        const double dim[3] = {0.05, 0.05, 0.05};
        paint(img, 0, 0, img.width, img.height, dim);
        CHECK(uniform(renderThumbnail(img), srgb8(0.2), srgb8(0.2), srgb8(0.2), 3));  // at most two stops

        std::fill(img.pixels.begin(), img.pixels.end(), uint16_t{0});
        CHECK(uniform(renderThumbnail(img), 0, 0, 0, 0));  // a black frame stays black
    }

    return testutil::finish("test_preview");
}
