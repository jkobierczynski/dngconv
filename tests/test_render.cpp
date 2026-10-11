// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Rendered previews: the JPEG encoder (its output read back by a decoder
// written for these tests), the renderer's sizes and lens corrections, and
// the writer's choice between the camera's preview and one of its own.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "dng_writer.hpp"
#include "jpeg_decoder.hpp"
#include "jpeg_encoder.hpp"
#include "render.hpp"
#include "test_util.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;

namespace {

// ---- pictures ---------------------------------------------------------------------

struct Rgb {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

// Smooth colour with a few hard edges: what a photograph is to a coder.
Rgb scene(uint32_t w, uint32_t h) {
    Rgb img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 3)};
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const double u = static_cast<double>(x) / std::max(w, 2u), v = static_cast<double>(y) / std::max(h, 2u);
            double r = 0.5 + 0.4 * std::sin(6.0 * u + 1.0) * std::cos(3.0 * v);
            double g = 0.5 + 0.4 * std::cos(4.0 * u * v + 0.5);
            double b = 0.3 + 0.6 * v;
            if (x > w / 2 && y > h / 3 && y < 2 * h / 3) {  // a darker rectangle
                r *= 0.4;
                g *= 0.4;
                b *= 0.4;
            }
            uint8_t* p = &img.pixels[(static_cast<size_t>(y) * w + x) * 3];
            p[0] = static_cast<uint8_t>(std::lround(255 * r));
            p[1] = static_cast<uint8_t>(std::lround(255 * g));
            p[2] = static_cast<uint8_t>(std::lround(255 * b));
        }
    return img;
}

Rgb flat(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    Rgb img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 3)};
    for (size_t i = 0; i < img.pixels.size(); i += 3) {
        img.pixels[i] = r;
        img.pixels[i + 1] = g;
        img.pixels[i + 2] = b;
    }
    return img;
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += (a[i] - b[i]) * (a[i] - b[i]);
    return sum == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 * static_cast<double>(a.size()) / sum);
}

int largestDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return 255;
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

std::vector<uint8_t> encode(const Rgb& img, int quality = 90, unsigned threads = 1) {
    return encodeJpeg(img.pixels.data(), img.width, img.height, quality, threads);
}

// ---- the encoder --------------------------------------------------------------------

void testEncoder() {
    // The file is what readers expect: JFIF, 8 bits, colour at half
    // resolution, start and end markers, and it decodes to the picture.
    {
        const Rgb img = scene(200, 150);
        const std::vector<uint8_t> jpeg = encode(img);
        CHECK(jpeg.size() > 4 && jpeg[0] == 0xff && jpeg[1] == 0xd8 && jpeg[2] == 0xff && jpeg[3] == 0xe0);
        CHECK(jpeg[jpeg.size() - 2] == 0xff && jpeg[jpeg.size() - 1] == 0xd9);
        const jpegdecoder::Picture p = jpegdecoder::decode(jpeg);
        if (!p.ok) std::fprintf(stderr, "decoder: %s\n", p.error.c_str());
        CHECK(p.ok && p.jfif);
        CHECK(p.width == 200 && p.height == 150 && p.components == 3);
        CHECK(p.subH == 2 && p.subV == 2);
        CHECK(psnr(p.rgb, img.pixels) > 33.0);
        CHECK(jpeg.size() < img.pixels.size() / 5);
    }

    // Every size, including those that do not fill whole 16-pixel units.
    for (const auto& size : {std::pair<uint32_t, uint32_t>{1, 1}, {2, 3}, {7, 5}, {15, 17}, {16, 16},
                             {17, 33}, {100, 75}, {250, 31}}) {
        const Rgb img = scene(size.first, size.second);
        const jpegdecoder::Picture p = jpegdecoder::decode(encode(img, 95));
        CHECK(p.ok && p.width == size.first && p.height == size.second);
        // Tiny pictures are all edge, and the colour planes at half size
        // cost most there.
        CHECK(psnr(p.rgb, img.pixels) > (size.first < 8 ? 15.0 : size.first < 100 ? 22.0 : 35.0));
    }

    // Plain colours come back as they went in.
    for (const auto& c : {std::array<uint8_t, 3>{0, 0, 0}, {255, 255, 255}, {128, 128, 128}, {255, 0, 0},
                          {0, 255, 0}, {0, 0, 255}, {200, 150, 40}}) {
        const Rgb img = flat(40, 24, c[0], c[1], c[2]);
        const jpegdecoder::Picture p = jpegdecoder::decode(encode(img));
        CHECK(p.ok);
        CHECK(largestDifference(p.rgb, img.pixels) <= 3);
    }

    // Higher quality: larger file, smaller error.
    {
        const Rgb img = scene(320, 240);
        size_t lastSize = 0;
        double lastPsnr = 0;
        for (int quality : {10, 40, 75, 90, 100}) {
            const std::vector<uint8_t> jpeg = encode(img, quality);
            const jpegdecoder::Picture p = jpegdecoder::decode(jpeg);
            CHECK(p.ok);
            const double quality_db = psnr(p.rgb, img.pixels);
            CHECK(jpeg.size() > lastSize);
            CHECK(quality_db > lastPsnr);
            lastSize = jpeg.size();
            lastPsnr = quality_db;
        }
        CHECK(lastPsnr > 38.0);
        // Out-of-range settings are brought into range.
        CHECK(encode(img, -5) == encode(img, 1));
        CHECK(encode(img, 500) == encode(img, 100));
    }

    // Noise at the highest quality: every kind of coefficient occurs, values
    // reach the limits of what baseline JPEG can code.
    {
        testutil::Random rnd(7);
        Rgb img{192, 160, std::vector<uint8_t>(192u * 160u * 3u)};
        for (size_t i = 0; i < img.pixels.size(); i += 3) {
            const uint8_t v = rnd.below(4) == 0 ? (rnd.below(2) ? 255 : 0) : static_cast<uint8_t>(rnd.below(256));
            img.pixels[i] = img.pixels[i + 1] = img.pixels[i + 2] = v;  // grey: the test is about brightness
        }
        const jpegdecoder::Picture p = jpegdecoder::decode(encode(img, 100));
        if (!p.ok) std::fprintf(stderr, "decoder: %s\n", p.error.c_str());
        CHECK(p.ok);
        CHECK(psnr(p.rgb, img.pixels) > 40.0);
    }

    // The same bytes whatever the number of threads.
    {
        const Rgb img = scene(333, 222);
        const std::vector<uint8_t> one = encode(img, 90, 1);
        CHECK(encode(img, 90, 4) == one);
        CHECK(encode(img, 90, 0) == one);
    }

    // Refused instead of written wrongly.
    {
        const Rgb img = scene(8, 8);
        for (const auto& size : {std::pair<uint32_t, uint32_t>{0, 8}, {8, 0}, {65536, 8}}) {
            bool threw = false;
            try {
                encodeJpeg(img.pixels.data(), size.first, size.second, 90, 1);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            CHECK(threw);
        }
        bool threw = false;
        try {
            encodeJpeg(nullptr, 8, 8, 90, 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
}

// Huffman codes are limited to 16 bits. A picture whose symbols grow in
// frequency a little faster than the Fibonacci numbers gets a tree that is
// one long chain, with codes of up to 20 bits, from the plain algorithm; the
// encoder has to shorten them and still write a table that decodes.
//
// Each 8x8 block here holds one cosine pattern, strong enough to give
// exactly one coefficient after quantisation at quality 50, so every block
// is coded as one symbol (zeros before the coefficient, and its size) plus
// "end of block". Twenty kinds of block; each kind is used once more than
// the two before it together.
void testLongCodes() {
    // (row, column) of the first ten coefficients in coding order, and the
    // standard's luminance quantiser there, which is what quality 50 uses.
    const int position[10][2] = {{0, 1}, {1, 0}, {2, 0}, {1, 1}, {0, 2}, {0, 3}, {1, 2}, {2, 1}, {3, 0}, {4, 0}};
    const int quantiser[10] = {11, 12, 14, 12, 10, 16, 14, 13, 14, 18};
    const double pi = 3.14159265358979323846;

    std::vector<int> kinds;  // kind of each block, most frequent last
    {
        long a = 1, b = 1;
        for (int kind = 0; kind < 20; ++kind) {
            for (long i = 0; i < a; ++i) kinds.push_back(kind);
            const long next = a + b + 1;
            a = b;
            b = next;
        }
    }
    CHECK(kinds.size() == 35400);
    const uint32_t blocksAcross = 192, blocksDown = 186;  // 35712 blocks; the rest stay flat
    Rgb img = flat(blocksAcross * 8, blocksDown * 8, 128, 128, 128);
    for (size_t i = 0; i < kinds.size(); ++i) {
        const int p = kinds[i] / 2, level = 1 + kinds[i] % 2;
        const int v = position[p][0], u = position[p][1];
        // A cosine of amplitude B gives the coefficient 4 B (5.657 B on the
        // first row or column); aim for `level` times the quantiser.
        const double gain = (u == 0 || v == 0) ? 4.0 * std::sqrt(2.0) : 4.0;
        const double amplitude = level * quantiser[p] / gain;
        const uint32_t bx = static_cast<uint32_t>(i % blocksAcross), by = static_cast<uint32_t>(i / blocksAcross);
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                const double value = 128.0 + amplitude * std::cos((2 * x + 1) * u * pi / 16.0) *
                                                 std::cos((2 * y + 1) * v * pi / 16.0);
                uint8_t* px = &img.pixels[(static_cast<size_t>(by * 8 + y) * img.width + bx * 8 + x) * 3];
                px[0] = px[1] = px[2] = static_cast<uint8_t>(std::lround(value));
            }
    }

    const std::vector<uint8_t> jpeg = encode(img, 50);
    const jpegdecoder::Picture p = jpegdecoder::decode(jpeg);
    if (!p.ok) std::fprintf(stderr, "decoder: %s\n", p.error.c_str());
    CHECK(p.ok);
    // One symbol each for the two DC tables and the colour AC table, and for
    // brightness the twenty kinds plus "end of block".
    CHECK(p.symbols == 24);
    CHECK(p.longestCode == 16);  // the limit was reached, and kept
    // Whole multiples of the quantiser are reproduced exactly, up to rounding.
    CHECK(largestDifference(p.rgb, img.pixels) <= 2);
    // Some eight bits per block: a bit for its DC value, a bit for "end of
    // block", the symbol (short for the common kinds) with its one or two
    // value bits, and its share of the two colour blocks.
    CHECK(jpeg.size() < 35712u * 9u / 8u);
}

// ---- the renderer ---------------------------------------------------------------------

const double kMatrix[3][3] = {{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}};

RawImage frame(uint32_t width, uint32_t height, uint32_t spp = 1) {
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
    // Neutral as the matrix has it for D65, so that equal samples are grey.
    img.hasAsShotNeutral = false;
    img.uniqueCameraModel = "Testmake Model T";
    img.pixels.assign(img.sampleCount(), 0);
    if (spp == 1) {
        img.isCfa = true;
        img.cfaRows = img.cfaCols = 2;
        img.cfaPattern = {0, 1, 1, 2};
    }
    return img;
}

// What each colour plane records for a grey of the given linear level.
void greyResponse(const RawImage& img, double level, double out[3]) {
    static const double xyzFromSrgb[3][3] = {{0.4124564, 0.3575761, 0.1804375},
                                             {0.2126729, 0.7151522, 0.0721750},
                                             {0.0193339, 0.1191920, 0.9503041}};
    double white[3] = {0, 0, 0}, largest = 0;
    for (int p = 0; p < 3; ++p) {
        for (int k = 0; k < 3; ++k)
            for (int c = 0; c < 3; ++c) white[p] += img.colorMatrix[p][k] * xyzFromSrgb[k][c];
        largest = std::max(largest, white[p]);
    }
    for (int p = 0; p < 3; ++p) out[p] = level * white[p] / largest;
}

// Paints a rectangle of the frame grey.
void paintGrey(RawImage& img, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom, double level) {
    double cam[3];
    greyResponse(img, level, cam);
    for (uint32_t y = top; y < bottom; ++y)
        for (uint32_t x = left; x < right; ++x) {
            if (img.isCfa) {
                const uint32_t ay = y - img.activeTop, ax = x - img.activeLeft;
                const unsigned plane = img.cfaPattern[(ay % img.cfaRows) * img.cfaCols + ax % img.cfaCols];
                img.pixels[static_cast<size_t>(y) * img.width + x] = static_cast<uint16_t>(std::lround(cam[plane] * 4000));
            } else {
                for (uint32_t k = 0; k < img.samplesPerPixel; ++k)
                    img.pixels[(static_cast<size_t>(y) * img.width + x) * img.samplesPerPixel + k] =
                        static_cast<uint16_t>(std::lround(cam[k] * 4000));
            }
        }
}

int srgb8(double linear) {
    if (linear <= 0) return 0;
    if (linear >= 1) return 255;
    const double v = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(v * 255.0));
}

int greenAt(const RgbImage& img, uint32_t x, uint32_t y) {
    return img.pixels[(static_cast<size_t>(y) * img.width + x) * 3 + 1];
}

bool isGrey(const RgbImage& img, int tolerance) {
    for (size_t i = 0; i + 2 < img.pixels.size(); i += 3)
        if (std::abs(img.pixels[i] - img.pixels[i + 1]) > tolerance ||
            std::abs(img.pixels[i + 2] - img.pixels[i + 1]) > tolerance)
            return false;
    return !img.pixels.empty();
}

void makeXTrans(RawImage& img) {
    img.cfaRows = img.cfaCols = 6;
    img.cfaPattern = {1, 1, 0, 1, 1, 2, 1, 1, 2, 1, 1, 0, 2, 0, 1, 0, 2, 1,
                      1, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 2, 1, 2, 0, 1};
}

void testRenderer() {
    RenderOptions plain;
    plain.toneCurve = false;

    // ---- how large a picture can get ----
    {
        uint32_t w = 0, h = 0;
        RawImage bayer = frame(640, 480);
        RenderOptions largest = plain;
        largest.maxSize = 0;
        RgbImage r = renderPicture(bayer, largest);
        CHECK(r.width == 320 && r.height == 240);  // one pixel per 2x2 cell
        CHECK(renderedSize(bayer, 0, w, h) && w == 320 && h == 240);
        CHECK(renderedSize(bayer, 5000, w, h) && w == 320 && h == 240);  // never enlarged
        CHECK(renderedSize(bayer, 100, w, h) && w == 100 && h == 75);

        // The size can be told without the pixels (when only the metadata was read).
        RawImage metadataOnly = bayer;
        metadataOnly.pixels.clear();
        CHECK(renderedSize(metadataOnly, 0, w, h) && w == 320 && h == 240);
        CHECK(renderPicture(metadataOnly, largest).pixels.empty());

        // X-Trans repeats every six pixels, but every 3x3 block already
        // holds its three colours in the same numbers.
        RawImage xtrans = frame(612, 408);
        makeXTrans(xtrans);
        CHECK(renderedSize(xtrans, 0, w, h) && w == 204 && h == 136);
        // Full-colour data needs no cells at all.
        RawImage rgb = frame(320, 240, 3);
        CHECK(renderedSize(rgb, 0, w, h) && w == 320 && h == 240);
        // A pattern with one colour per row has no smaller block than itself.
        RawImage stripes = frame(600, 400);
        stripes.cfaRows = 3;
        stripes.cfaCols = 1;
        stripes.cfaPattern = {0, 1, 2};
        CHECK(renderedSize(stripes, 0, w, h) && w == 200 && h == 133);

        // Too small for a single cell: no picture, no crash.
        RawImage tiny = frame(1, 1);
        CHECK(!renderedSize(tiny, 0, w, h));
        CHECK(renderPicture(tiny).pixels.empty());
    }

    // ---- colour stays right at every size and for crops off the cell grid ----
    {
        for (uint32_t size : {0u, 300u, 257u, 100u, 33u}) {
            RawImage bayer = frame(640, 480);
            paintGrey(bayer, 0, 0, 640, 480, 0.18);
            RenderOptions o = plain;
            o.maxSize = size;
            const RgbImage r = renderPicture(bayer, o);
            CHECK(isGrey(r, 1));
            CHECK(!r.pixels.empty() && std::abs(greenAt(r, r.width / 2, r.height / 2) - srgb8(0.18)) <= 1);
        }
        for (uint32_t offset = 0; offset < 6; ++offset) {
            RawImage xtrans = frame(630, 420);
            makeXTrans(xtrans);
            xtrans.cropLeft = offset;
            xtrans.cropTop = (offset * 5) % 6;
            xtrans.cropWidth = 600;
            xtrans.cropHeight = 400;
            paintGrey(xtrans, 0, 0, 630, 420, 0.3);
            RenderOptions o = plain;
            o.maxSize = 0;
            const RgbImage r = renderPicture(xtrans, o);
            CHECK(r.width >= 198 && r.width <= 200);
            CHECK(isGrey(r, 1));
        }
    }

    // ---- a picture is where it should be ----
    {
        // Left half dark, right half bright, at a size that is no whole
        // fraction of the frame: the edge stays in the middle.
        RawImage img = frame(1000, 600);
        paintGrey(img, 0, 0, 500, 600, 0.05);
        paintGrey(img, 500, 0, 1000, 600, 0.6);
        RenderOptions o = plain;
        o.maxSize = 333;
        const RgbImage r = renderPicture(img, o);
        CHECK(r.width == 333 && r.height == 200);
        const int dark = srgb8(0.05), bright = srgb8(0.6);
        CHECK(std::abs(greenAt(r, 160, 100) - dark) <= 1);
        CHECK(std::abs(greenAt(r, 172, 100) - bright) <= 1);
        // The pixel the edge runs through is a mixture, in linear light.
        const int mixed = greenAt(r, 166, 100);
        CHECK(mixed > dark && mixed < bright);
        // Threads change nothing.
        o.threads = 4;
        CHECK(renderPicture(img, o).pixels == r.pixels);
    }

    // ---- lens corrections, as a DNG reader applies the opcodes ----
    {
        // Gain against vignetting: 1 + r^2, so twice as bright in the corner.
        RawImage img = frame(800, 600);
        paintGrey(img, 0, 0, 800, 600, 0.2);
        LensOpcodes lens;
        lens.vignetting = true;
        lens.gain = {1.0, 0, 0, 0, 0};
        RenderOptions o = plain;
        o.maxSize = 200;
        o.lens = &lens;
        const RgbImage r = renderPicture(img, o);
        CHECK(std::abs(greenAt(r, 100, 75) - srgb8(0.2)) <= 2);       // centre: unchanged
        CHECK(std::abs(greenAt(r, 0, 0) - srgb8(0.4)) <= 4);          // corner: r = 1
        CHECK(std::abs(greenAt(r, 199, 75) - srgb8(0.2 * (1 + 0.64))) <= 4);  // right edge: r = 0.8
        o.lens = nullptr;
        CHECK(std::abs(greenAt(renderPicture(img, o), 0, 0) - srgb8(0.2)) <= 1);
    }
    {
        // Warp: every point is fetched from 1.25 times as far from the
        // centre. A bright square of half-width 200 in the frame therefore
        // shows with half-width 160.
        RawImage img = frame(800, 600);
        paintGrey(img, 0, 0, 800, 600, 0.05);
        paintGrey(img, 200, 100, 600, 500, 0.6);
        LensOpcodes lens;
        lens.distortion = true;
        lens.planes = 1;
        lens.warp[0] = {1.25, 0, 0, 0};
        RenderOptions o = plain;
        o.maxSize = 400;  // half scale: the square's half-width is 100, shown as 80
        o.lens = &lens;
        const RgbImage r = renderPicture(img, o);
        const int dark = srgb8(0.05), bright = srgb8(0.6);
        CHECK(std::abs(greenAt(r, 200, 150) - bright) <= 1);
        CHECK(std::abs(greenAt(r, 200 + 76, 150) - bright) <= 1);
        CHECK(std::abs(greenAt(r, 200 + 84, 150) - dark) <= 1);
        CHECK(std::abs(greenAt(r, 200 - 76, 150) - bright) <= 1);
        CHECK(std::abs(greenAt(r, 200 - 84, 150) - dark) <= 1);
        CHECK(std::abs(greenAt(r, 200, 150 + 76) - bright) <= 1);
        CHECK(std::abs(greenAt(r, 200, 150 + 84) - dark) <= 1);
        // Without the correction the square has its own size.
        o.lens = nullptr;
        const RgbImage straight = renderPicture(img, o);
        CHECK(std::abs(greenAt(straight, 200 + 96, 150) - bright) <= 1);
        CHECK(std::abs(greenAt(straight, 200 + 104, 150) - dark) <= 1);

        // With three planes (colour fringes corrected too) green leads.
        lens.planes = 3;
        lens.warp[0] = {1.0, 0, 0, 0};
        lens.warp[1] = {1.25, 0, 0, 0};
        lens.warp[2] = {1.5, 0, 0, 0};
        o.lens = &lens;
        CHECK(renderPicture(img, o).pixels == r.pixels);

        // An optical centre off the middle moves the fixed point.
        lens.planes = 1;
        lens.warp[0] = {1.25, 0, 0, 0};
        lens.centreX = 0.25;  // 200 of 800: the left edge of the square
        const RgbImage shifted = renderPicture(img, o);
        // The square's left edge, at the centre, stays; its right edge moves
        // from 400 to 320 pixels right of it (150 -> ... at half scale: 100 + 160).
        CHECK(std::abs(greenAt(shifted, 100 + 4, 150) - bright) <= 1);
        CHECK(std::abs(greenAt(shifted, 100 - 4, 150) - dark) <= 1);
        CHECK(std::abs(greenAt(shifted, 100 + 156, 150) - bright) <= 1);
        CHECK(std::abs(greenAt(shifted, 100 + 164, 150) - dark) <= 1);
    }
}

// ---- the writer's preview ---------------------------------------------------------------

std::string write(const RawImage& img, const DngWriteOptions& options) {
    std::ostringstream out(std::ios::binary);
    writeDng(img, out, options);
    return out.str();
}

struct StoredPreview {
    bool present = false;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> jpeg;
    std::string application, version;
    bool hasColourSpace = false;
    uint32_t colourSpace = 0;
    uint32_t subIfds = 0;
};

StoredPreview storedPreview(const std::string& file) {
    StoredPreview s;
    const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(file);
    if (!ifd0.count(330)) return s;
    s.subIfds = ifd0.at(330).count;
    for (uint32_t i = 0; i < s.subIfds; ++i) {
        const tiffreader::Ifd sub = tiffreader::parseIfd(file, ifd0.at(330).u32(i));
        if (sub.at(254).u32() != 1) continue;  // the raw image
        s.present = true;
        s.width = sub.at(256).u32();
        s.height = sub.at(257).u32();
        CHECK(sub.at(259).u16() == 7 && sub.at(262).u16() == 6);  // JPEG, YCbCr
        const uint32_t at = sub.at(273).u32(), length = sub.at(279).u32();
        CHECK(at + length <= file.size());
        s.jpeg.assign(file.begin() + at, file.begin() + at + length);
        if (sub.count(50966)) s.application = sub.at(50966).text();
        if (sub.count(50967)) s.version = sub.at(50967).text();
        if (sub.count(50970)) {
            s.hasColourSpace = true;
            s.colourSpace = sub.at(50970).u32();
        }
        CHECK(sub.at(530).u16(0) == 2 && sub.at(530).u16(1) == 2);
    }
    return s;
}

void testWriter() {
    RawImage img = frame(1600, 1200);
    paintGrey(img, 0, 0, 1600, 1200, 0.1);
    paintGrey(img, 400, 300, 1200, 900, 0.5);

    // A camera JPEG of a given size (its content does not matter here).
    const auto cameraJpeg = [](uint32_t w, uint32_t h) { return encode(scene(w, h), 85); };

    // ---- no camera preview: one is rendered ----
    {
        DngWriteOptions options;
        options.software = "dngconv 9.9";
        PreviewPlan plan = planPreview(img, options);
        CHECK(plan.kind == PreviewPlan::Kind::Rendered && plan.width == 800 && plan.height == 600);

        const StoredPreview s = storedPreview(write(img, options));
        CHECK(s.present && s.subIfds == 2);
        CHECK(s.width == 800 && s.height == 600);
        CHECK(s.application == "dngconv" && s.version == "9.9");
        CHECK(s.hasColourSpace && s.colourSpace == 2);  // sRGB
        const jpegdecoder::Picture p = jpegdecoder::decode(s.jpeg);
        CHECK(p.ok && p.width == 800 && p.height == 600);
        // It is the renderer's picture.
        RenderOptions render;
        render.maxSize = 1024;
        const RgbImage expected = renderPicture(img, render);
        CHECK(expected.width == 800);
        CHECK(psnr(p.rgb, expected.pixels) > 38.0);

        // At a chosen size.
        options.previewSize = 300;
        plan = planPreview(img, options);
        CHECK(plan.kind == PreviewPlan::Kind::Rendered && plan.width == 300 && plan.height == 225);
        const StoredPreview small = storedPreview(write(img, options));
        CHECK(small.width == 300 && small.height == 225);
        CHECK(jpegdecoder::decode(small.jpeg).ok);
        CHECK(small.jpeg.size() < s.jpeg.size());

        // A lower quality gives a smaller preview.
        options.previewQuality = 40;
        CHECK(storedPreview(write(img, options)).jpeg.size() < small.jpeg.size());

        // "camera" means the camera's or none.
        options = DngWriteOptions();
        options.preview = DngPreview::Camera;
        CHECK(planPreview(img, options).kind == PreviewPlan::Kind::None);
        const StoredPreview none = storedPreview(write(img, options));
        CHECK(!none.present && none.subIfds == 1);

        options.preview = DngPreview::None;
        CHECK(planPreview(img, options).kind == PreviewPlan::Kind::None);
        CHECK(!storedPreview(write(img, options)).present);
    }

    // ---- a camera preview of a useful size is kept as it is ----
    {
        RawImage withJpeg = img;
        withJpeg.previewJpeg = cameraJpeg(1200, 900);
        DngWriteOptions options;
        const PreviewPlan plan = planPreview(withJpeg, options);
        CHECK(plan.kind == PreviewPlan::Kind::Camera && plan.width == 1200 && plan.height == 900);
        const StoredPreview s = storedPreview(write(withJpeg, options));
        CHECK(s.present && s.width == 1200 && s.height == 900);
        CHECK(s.jpeg == withJpeg.previewJpeg);
        // Nothing is claimed about a picture this program did not make.
        CHECK(s.application.empty() && !s.hasColourSpace);

        // Unless a rendered one is asked for.
        options.preview = DngPreview::Render;
        CHECK(planPreview(withJpeg, options).kind == PreviewPlan::Kind::Rendered);
        const StoredPreview rendered = storedPreview(write(withJpeg, options));
        CHECK(rendered.width == 800 && rendered.application == "dngconv");
        CHECK(rendered.jpeg != withJpeg.previewJpeg);
    }

    // ---- a camera preview that is only a thumbnail gives way ----
    {
        RawImage withThumb = img;
        withThumb.previewJpeg = cameraJpeg(160, 120);
        DngWriteOptions options;
        CHECK(planPreview(withThumb, options).kind == PreviewPlan::Kind::Rendered);
        CHECK(storedPreview(write(withThumb, options)).width == 800);
        // Kept when asked for,
        options.preview = DngPreview::Camera;
        CHECK(storedPreview(write(withThumb, options)).jpeg == withThumb.previewJpeg);
        // and when rendering would not give a larger picture.
        options.preview = DngPreview::Auto;
        options.previewSize = 100;
        const PreviewPlan plan = planPreview(withThumb, options);
        CHECK(plan.kind == PreviewPlan::Kind::Camera && plan.width == 160);

        // Bytes that are not a JPEG are not stored as one.
        withThumb.previewJpeg = {'n', 'o', 't', ' ', 'a', ' ', 'j', 'p', 'e', 'g'};
        options = DngWriteOptions();
        options.preview = DngPreview::Camera;
        CHECK(!storedPreview(write(withThumb, options)).present);
        options.preview = DngPreview::Auto;
        CHECK(storedPreview(write(withThumb, options)).application == "dngconv");
    }

    // ---- the largest size, and a frame too small for any ----
    {
        DngWriteOptions options;
        options.previewSize = 0;
        RawImage large = frame(2600, 1800);
        const PreviewPlan plan = planPreview(large, options);
        CHECK(plan.kind == PreviewPlan::Kind::Rendered && plan.width == 1300 && plan.height == 900);
        const StoredPreview s = storedPreview(write(large, options));
        CHECK(s.width == 1300 && s.height == 900 && jpegdecoder::decode(s.jpeg).ok);

        RawImage tiny = frame(2, 2);
        tiny.cropWidth = tiny.cropHeight = 1;
        CHECK(planPreview(tiny, DngWriteOptions()).kind == PreviewPlan::Kind::None);
        const std::string file = write(tiny, DngWriteOptions());
        CHECK(tiffreader::parseFirstIfd(file).count(330) == 0);  // neither thumbnail nor preview
    }

    // ---- the preview shows the lens corrections the DNG carries ----
    {
        RawImage lensImg = img;
        lensImg.lens.frameWidth = 1600;
        lensImg.lens.frameHeight = 1200;
        lensImg.lens.origin = "test";
        lensImg.lens.distortionEnabled = true;
        // Barrel distortion: points are stored closer to the centre than they belong.
        for (int i = 0; i <= 10; ++i) lensImg.lens.distortion.add(i / 10.0, 1.0 - 0.08 * (i / 10.0) * (i / 10.0));
        DngWriteOptions options;
        const jpegdecoder::Picture corrected = jpegdecoder::decode(storedPreview(write(lensImg, options)).jpeg);
        options.lensCorrections = LensCorrectionMode::None;
        const jpegdecoder::Picture uncorrected = jpegdecoder::decode(storedPreview(write(lensImg, options)).jpeg);
        CHECK(corrected.ok && uncorrected.ok && corrected.rgb.size() == uncorrected.rgb.size());
        CHECK(corrected.rgb != uncorrected.rgb);
        // And without lens data both ways give the same picture.
        const std::string a = write(img, DngWriteOptions());
        CHECK(storedPreview(a).jpeg == storedPreview(write(img, options)).jpeg);
    }
}

}  // namespace

int main() {
    testEncoder();
    testLongCodes();
    testRenderer();
    testWriter();
    return testutil::finish("test_render");
}
