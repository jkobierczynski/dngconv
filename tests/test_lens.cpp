// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Lens corrections: the curve arithmetic, the fit to DNG's polynomials, the
// opcode bytes, the maker parsers on hand-built files (sound, damaged and
// random), and the way into a DNG file.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dng_writer.hpp"
#include "lens_data.hpp"
#include "lens_opcodes.hpp"
#include "test_util.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

// ---- a tiny TIFF builder -------------------------------------------------------

struct Field {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    Bytes data;  // already in the byte order of the file
};

void put16(Bytes& b, uint32_t v, bool be) {
    if (be) {
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v));
    } else {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    }
}

void put32(Bytes& b, uint32_t v, bool be) {
    if (be) {
        put16(b, v >> 16, true);
        put16(b, v & 0xffff, true);
    } else {
        put16(b, v & 0xffff, false);
        put16(b, v >> 16, false);
    }
}

Field shorts(uint16_t tag, const std::vector<int>& v, bool be = false, uint16_t type = 8) {
    Field f{tag, type, static_cast<uint32_t>(v.size()), {}};
    for (int x : v) put16(f.data, static_cast<uint32_t>(x) & 0xffff, be);
    return f;
}

Field longs(uint16_t tag, const std::vector<uint32_t>& v, bool be = false, uint16_t type = 4) {
    Field f{tag, type, static_cast<uint32_t>(v.size()), {}};
    for (uint32_t x : v) put32(f.data, x, be);
    return f;
}

// Signed rationals with a fixed denominator.
Field rationals(uint16_t tag, const std::vector<double>& v, int32_t denominator = 1000000) {
    Field f{tag, 10, static_cast<uint32_t>(v.size()), {}};
    for (double x : v) {
        put32(f.data, static_cast<uint32_t>(static_cast<int32_t>(std::lround(x * denominator))), false);
        put32(f.data, static_cast<uint32_t>(denominator), false);
    }
    return f;
}

Field floats(uint16_t tag, const std::vector<float>& v, bool be = false) {
    Field f{tag, 11, static_cast<uint32_t>(v.size()), {}};
    for (float x : v) {
        uint32_t bits;
        std::memcpy(&bits, &x, 4);
        put32(f.data, bits, be);
    }
    return f;
}

Field ascii(uint16_t tag, const std::string& s) {
    Field f{tag, 2, static_cast<uint32_t>(s.size() + 1), Bytes(s.begin(), s.end())};
    f.data.push_back(0);
    return f;
}

Field bytes(uint16_t tag, const Bytes& v, uint16_t type = 7) {
    return Field{tag, type, static_cast<uint32_t>(v.size()), v};
}

// Writes a directory at `at` in `file`, its long values right behind it.
// Offsets stored in it are relative to `origin`.
void placeDirectory(Bytes& file, size_t at, std::vector<Field> fields, bool be = false,
                    size_t origin = 0) {
    std::sort(fields.begin(), fields.end(), [](const Field& a, const Field& b) { return a.tag < b.tag; });
    Bytes dir, values;
    const size_t valuesAt = at + 2 + 12 * fields.size() + 4;
    put16(dir, static_cast<uint32_t>(fields.size()), be);
    for (const Field& f : fields) {
        put16(dir, f.tag, be);
        put16(dir, f.type, be);
        put32(dir, f.count, be);
        if (f.data.size() <= 4) {
            Bytes v = f.data;
            v.resize(4, 0);
            dir.insert(dir.end(), v.begin(), v.end());
        } else {
            put32(dir, static_cast<uint32_t>(valuesAt + values.size() - origin), be);
            values.insert(values.end(), f.data.begin(), f.data.end());
            if (values.size() % 2) values.push_back(0);
        }
    }
    put32(dir, 0, be);
    dir.insert(dir.end(), values.begin(), values.end());
    if (file.size() < at + dir.size()) file.resize(at + dir.size(), 0);
    std::copy(dir.begin(), dir.end(), file.begin() + static_cast<std::ptrdiff_t>(at));
}

Bytes tiffHeader(uint16_t magic = 42, uint32_t firstIfd = 8) {
    Bytes b = {'I', 'I'};
    put16(b, magic, false);
    put32(b, firstIfd, false);
    return b;
}

fs::path writeFile(const fs::path& dir, const std::string& name, const Bytes& data) {
    const fs::path p = dir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return p;
}

// ---- Sony ------------------------------------------------------------------------

const std::vector<int> kSonyDistortion = {16, 11, -2, -31, -66, -119, -180, -253, -331, -417, -503,
                                          -595, -685, -774, -857, -936, -1008};
const std::vector<int> kSonyVignetting = {16, 0, 64, 128, 256, 448, 704, 1024, 1344, 1792, 2496,
                                          3520, 4992, 6784, 8832, 11008, 13184};
std::vector<int> sonyCa() {
    std::vector<int> v = {32};
    for (int i = 0; i < 16; ++i) v.push_back(512 - 80 * i);  // red
    for (int i = 0; i < 16; ++i) v.push_back(1152 + 48 * i);  // blue
    return v;
}

Bytes sonyFile(int vignettingSetting, int caSetting, int distortionSetting) {
    Bytes f = tiffHeader();
    placeDirectory(f, 8, {ascii(271, "SONY"), longs(0x14a, {0x200})});
    placeDirectory(f, 0x200,
                   {shorts(0x7031, {vignettingSetting}, false, 3), shorts(0x7032, kSonyVignetting),
                    shorts(0x7034, {caSetting}, false, 3), shorts(0x7035, sonyCa()),
                    shorts(0x7036, {distortionSetting}, false, 3), shorts(0x7037, kSonyDistortion)});
    return f;
}

// The SR2 key stream, written the way dcraw's sony_decrypt generates it.
void sr2Scramble(Bytes& data, uint32_t key) {
    uint32_t pad[128];
    unsigned p;
    for (p = 0; p < 4; p++) pad[p] = key = key * 48828125u + 1;
    pad[3] = pad[3] << 1 | (pad[0] ^ pad[2]) >> 31;
    for (p = 4; p < 127; p++) pad[p] = (pad[p - 4] ^ pad[p - 2]) << 1 | (pad[p - 3] ^ pad[p - 1]) >> 31;
    pad[127] = 0;
    for (size_t word = 0; word < data.size() / 4; ++word) {
        ++p;
        pad[(p - 1) & 127] = pad[p & 127] ^ pad[(p + 64) & 127];
        const uint32_t x = pad[(p - 1) & 127];
        for (int b = 0; b < 4; ++b) data[4 * word + static_cast<size_t>(b)] ^= static_cast<uint8_t>(x >> (24 - 8 * b));
    }
}

Bytes sonySr2File(uint32_t key) {
    const size_t blockAt = 0x400;
    // Offsets inside the block count from the start of the file.
    Bytes block;
    Bytes scratch(blockAt, 0);
    placeDirectory(scratch, blockAt,
                   {shorts(0x797c, {257}, false, 3), shorts(0x797d, {11, 0, 96, 320, 672, 1120, 1728, 2432, 3200, 4096, 5184, 6496}),
                    shorts(0x797f, {1}, false, 3),
                    shorts(0x7980, {22, 1268, 1264, 1248, 1220, 1176, 1128, 1052, 896, 728, 588, 520,
                                    -536, -480, -432, -352, -280, -192, -80, 112, 392, 784, 1320}),
                    shorts(0x7981, {0}, false, 3), shorts(0x7982, {11, 16, 5, 0, 0, 0, 5, 16, 38, 72, 126, 203})});
    block.assign(scratch.begin() + static_cast<std::ptrdiff_t>(blockAt), scratch.end());
    while (block.size() % 4) block.push_back(0);
    sr2Scramble(block, key);

    Bytes f = tiffHeader();
    Bytes keyBytes;
    put32(keyBytes, key, false);
    Bytes privateOffset;
    put32(privateOffset, 0x100, false);
    placeDirectory(f, 8, {ascii(271, "SONY"), bytes(0xc634, privateOffset, 1)});
    placeDirectory(f, 0x100, {longs(0x7200, {static_cast<uint32_t>(blockAt)}),
                              longs(0x7201, {static_cast<uint32_t>(block.size())}), bytes(0x7221, keyBytes)});
    f.resize(blockAt, 0);
    f.insert(f.end(), block.begin(), block.end());
    return f;
}

// ---- Panasonic -------------------------------------------------------------------

unsigned panasonicSum(const Bytes& d, size_t first, size_t count, size_t step) {
    unsigned sum = 0;
    for (size_t i = 0; i < count; ++i) sum = (73 * sum + d[first + i * step]) % 0xffef;
    return sum;
}

Bytes panasonicTag(int a, int b, int c, int scale, int flag, int unit) {
    const int words[16] = {0, 0, 74, 619, b, scale, 82, flag, a, 456, 516, c, unit, 956, 0, 0};
    Bytes d;
    for (int w : words) put16(d, static_cast<uint32_t>(w) & 0xffff, false);
    const auto setWord = [&](size_t i, unsigned v) {
        d[2 * i] = static_cast<uint8_t>(v);
        d[2 * i + 1] = static_cast<uint8_t>(v >> 8);
    };
    setWord(1, panasonicSum(d, 4, 12, 1));
    setWord(14, panasonicSum(d, 16, 12, 1));
    setWord(0, panasonicSum(d, 2, 14, 2));
    setWord(15, panasonicSum(d, 3, 14, 2));
    return d;
}

Bytes panasonicFile(const Bytes& tag) {
    Bytes f = tiffHeader(0x55);
    placeDirectory(f, 8, {ascii(271, "Panasonic"), bytes(0x0119, tag)});
    return f;
}

// ---- Fujifilm --------------------------------------------------------------------

Bytes fujiFile(const std::vector<double>& distortion, const std::vector<double>& ca,
               const std::vector<double>& vignetting) {
    const size_t tiffAt = 0x200;
    Bytes f(tiffAt, 0);
    std::memcpy(f.data(), "FUJIFILMCCD-RAW ", 16);
    f[100] = 0;
    f[101] = 0;
    f[102] = static_cast<uint8_t>(tiffAt >> 8);
    f[103] = static_cast<uint8_t>(tiffAt);
    Bytes tiff = tiffHeader();
    placeDirectory(tiff, 8, {longs(0xf000, {0x40}, false, 13)});
    placeDirectory(tiff, 0x40, {rationals(0xf00b, distortion), rationals(0xf00f, ca), rationals(0xf010, vignetting)});
    f.insert(f.end(), tiff.begin(), tiff.end());
    return f;
}

MakerNote fujiNote(int cropMode) {
    MakerNote note;
    note.data = {'F', 'U', 'J', 'I', 'F', 'I', 'L', 'M', 12, 0, 0, 0};
    placeDirectory(note.data, 12, {shorts(0x104d, {cropMode}, false, 3)});
    return note;
}

// ---- Olympus ---------------------------------------------------------------------

MakerNote olympusNote(bool be, const std::vector<float>& distortion, const std::vector<float>& ca) {
    MakerNote note;
    note.data = {'O', 'L', 'Y', 'M', 'P', 'U', 'S', 0, static_cast<uint8_t>(be ? 'M' : 'I'),
                 static_cast<uint8_t>(be ? 'M' : 'I'), 3, 0};
    placeDirectory(note.data, 12, {longs(0x2040, {0x80}, be, 13)}, be);
    placeDirectory(note.data, 0x80, {floats(0x150a, distortion, be), floats(0x150c, ca, be)}, be);
    return note;
}

Bytes olympusFile() {
    Bytes f = tiffHeader(0x4f52);
    placeDirectory(f, 8, {ascii(271, "OLYMPUS IMAGING CORP.")});
    return f;
}

// ---- helpers ---------------------------------------------------------------------

uint32_t be32(const Bytes& b, size_t at) {
    return (static_cast<uint32_t>(b.at(at)) << 24) | (static_cast<uint32_t>(b.at(at + 1)) << 16) |
           (static_cast<uint32_t>(b.at(at + 2)) << 8) | b.at(at + 3);
}

double beDouble(const Bytes& b, size_t at) {
    uint64_t bits = 0;
    for (size_t i = 0; i < 8; ++i) bits = (bits << 8) | b.at(at + i);
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

// A correction whose frame is the whole image, so camera and DNG radii agree.
LensCorrection wholeFrame(double width, double height) {
    LensCorrection lens;
    lens.frameWidth = width;
    lens.frameHeight = height;
    return lens;
}

template <typename F>
RadialCurve sampled(F f, double upTo = 1.3, int steps = 260) {
    RadialCurve c;
    for (int i = 0; i <= steps; ++i) c.add(upTo * i / steps, f(upTo * i / steps));
    return c;
}

bool plausible(const LensCorrection& c) {
    const auto within = [](const RadialCurve& k, double lo, double hi) {
        if (k.radius.size() != k.value.size()) return false;
        for (size_t i = 0; i < k.radius.size(); ++i) {
            if (!std::isfinite(k.radius[i]) || !std::isfinite(k.value[i])) return false;
            if (k.value[i] < lo || k.value[i] > hi) return false;
            if (i > 0 && !(k.radius[i] > k.radius[i - 1])) return false;
        }
        return true;
    };
    return within(c.distortion, 0.5, 1.5) && within(c.caRed, -0.02, 0.02) &&
           within(c.caBlue, -0.02, 0.02) && within(c.vignetting, 0.5, 16.0);
}

RawImage smallImage() {
    RawImage img;
    img.width = 640;
    img.height = 480;
    img.samplesPerPixel = 1;
    img.isCfa = true;
    img.cfaRows = img.cfaCols = 2;
    img.cfaPattern = {0, 1, 1, 2};
    img.activeBottom = img.height;
    img.activeRight = img.width;
    img.cropLeft = img.cropTop = 2;
    img.cropWidth = img.width - 4;
    img.cropHeight = img.height - 4;
    img.blackLevel = {0};
    img.whiteLevel = {4095, 4095, 4095, 4095};
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}, {0, 0, 0}}};
    img.uniqueCameraModel = "Testmake Model T";
    img.pixels.resize(img.sampleCount());
    for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<uint16_t>((i * 37) & 0xfff);
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path();
    const MakerNote noNote;

    // ---- curves ------------------------------------------------------------------
    {
        RadialCurve c;
        CHECK(c.empty());
        c.add(0.2, 1.0);
        c.add(0.4, 2.0);
        c.add(1.0, 5.0);
        CHECK_NEAR(c.at(0.2), 1.0, 1e-12);
        CHECK_NEAR(c.at(0.3), 1.5, 1e-12);
        CHECK_NEAR(c.at(0.7), 3.5, 1e-12);
        CHECK_NEAR(c.at(1.0), 5.0, 1e-12);
        // Beyond the ends the nearest segment continues.
        CHECK_NEAR(c.at(0.0), 0.0, 1e-12);
        CHECK_NEAR(c.at(1.2), 6.0, 1e-12);
        RadialCurve one;
        one.add(0.5, 7.0);
        CHECK_NEAR(one.at(0.1), 7.0, 1e-12);
    }

    // ---- zoom that fills the frame ---------------------------------------------
    {
        RadialCurve identity = sampled([](double) { return 1.0; });
        CHECK_NEAR(frameFillingZoom(identity, 6000, 4000), 1.0, 1e-9);
        RadialCurve shrink = sampled([](double) { return 0.8; });
        CHECK_NEAR(frameFillingZoom(shrink, 6000, 4000), 1.25, 1e-9);

        // Barrel correction: the ratio falls with the radius, so the point of
        // the frame's edge nearest to the centre decides.
        const auto barrel = [](double r) { return 1.0 - 0.06 * r * r; };
        const double z = frameFillingZoom(sampled(barrel, 1.5, 3000), 6000, 4000);
        const double unit = std::hypot(3000.0, 2000.0);
        CHECK(z > 1.0);
        CHECK_NEAR(z * barrel(z * 2000 / unit), 1.0, 1e-5);        // short side just reaches the edge
        CHECK(z * barrel(z * 3000 / unit) < 1.0);                   // long side and corner stay inside
        CHECK(z * barrel(z) < 1.0);

        // Pincushion correction: the corner decides.
        const auto pincushion = [](double r) { return 1.0 + 0.04 * r * r; };
        const double zp = frameFillingZoom(sampled(pincushion, 1.5, 3000), 6000, 4000);
        CHECK(zp < 1.0);
        CHECK_NEAR(zp * pincushion(zp), 1.0, 1e-5);

        CHECK_NEAR(frameFillingZoom(RadialCurve{}, 6000, 4000), 1.0, 1e-12);
        CHECK_NEAR(frameFillingZoom(identity, 0, 4000), 1.0, 1e-12);
    }

    // ---- the fit: curves that are polynomials come back exactly -------------------
    {
        LensCorrection lens = wholeFrame(6000, 4000);
        lens.distortion = sampled([](double r) { return 0.98 + 0.03 * r * r - 0.01 * r * r * r * r; }, 1.3, 2600);
        lens.vignetting = sampled([](double r) { return 1.0 + 0.5 * r * r + 0.2 * r * r * r * r; }, 1.3, 2600);
        lens.distortionEnabled = lens.vignettingEnabled = true;
        const LensOpcodes o = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto);
        CHECK(o.distortion && o.vignetting && !o.chromaticAberration);
        CHECK(o.planes == 1);
        CHECK_NEAR(o.warp[0][0], 0.98, 1e-6);
        CHECK_NEAR(o.warp[0][1], 0.03, 1e-5);
        CHECK_NEAR(o.warp[0][2], -0.01, 1e-5);
        CHECK_NEAR(o.warp[0][3], 0.0, 1e-5);
        CHECK_NEAR(o.gain[0], 0.5, 1e-4);
        CHECK_NEAR(o.gain[1], 0.2, 1e-3);
        CHECK(o.warpErrorPixels < 0.01);
        CHECK(o.gainErrorPercent < 0.01);
        CHECK_NEAR(o.centreX, 0.5, 1e-12);
        CHECK_NEAR(o.centreY, 0.5, 1e-12);
        CHECK_NEAR(o.zoom, 1.0, 1e-12);
        CHECK_NEAR(evaluateWarp(o.warp[0], 0.5), 0.98 + 0.03 * 0.25 - 0.01 * 0.0625, 1e-6);
        CHECK_NEAR(evaluateGain(o.gain, 0.5), 1.0 + 0.5 * 0.25 + 0.2 * 0.0625, 1e-5);

        // The opcode list: vignetting first, then the warp; big-endian.
        const Bytes& b = o.opcodeList3;
        CHECK(b.size() == 4 + (16 + 56) + (16 + 4 + 48 + 16));
        CHECK(be32(b, 0) == 2);
        CHECK(be32(b, 4) == 3);            // FixVignetteRadial
        CHECK(be32(b, 8) == 0x01030000);   // DNG 1.3
        CHECK(be32(b, 12) == 1);           // optional
        CHECK(be32(b, 16) == 56);
        for (size_t i = 0; i < 5; ++i) CHECK(beDouble(b, 20 + 8 * i) == o.gain[i]);
        CHECK(beDouble(b, 60) == 0.5 && beDouble(b, 68) == 0.5);
        CHECK(be32(b, 76) == 1);           // WarpRectilinear
        CHECK(be32(b, 80) == 0x01030000);
        CHECK(be32(b, 84) == 1);
        CHECK(be32(b, 88) == 4 + 48 + 16);
        CHECK(be32(b, 92) == 1);           // planes
        for (size_t i = 0; i < 4; ++i) CHECK(beDouble(b, 96 + 8 * i) == o.warp[0][i]);
        CHECK(beDouble(b, 128) == 0.0 && beDouble(b, 136) == 0.0);  // tangential terms
        CHECK(beDouble(b, 144) == 0.5 && beDouble(b, 152) == 0.5);
    }

    // ---- a frame smaller than the image, and off centre ---------------------------
    {
        LensCorrection lens = wholeFrame(5000, 3000);
        lens.frameLeft = 300;
        lens.frameTop = 400;
        lens.distortion = sampled([](double r) { return 0.97 + 0.02 * r * r; }, 2.0, 4000);
        lens.distortionEnabled = true;
        const LensOpcodes o = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto);
        CHECK(o.distortion);
        CHECK_NEAR(o.centreX, 2800.0 / 6000.0, 1e-12);
        CHECK_NEAR(o.centreY, 1900.0 / 4000.0, 1e-12);
        // DNG counts radii to the farthest corner of the image, the camera to
        // the corner of its picture: the r^2 coefficient scales by the square.
        const double t = std::hypot(3200.0, 2100.0) / std::hypot(2500.0, 1500.0);
        CHECK_NEAR(o.warp[0][0], 0.97, 1e-6);
        CHECK_NEAR(o.warp[0][1], 0.02 * t * t, 1e-5);
        CHECK(o.warpErrorPixels < 0.01);

        // A frame whose centre is outside the image is not usable.
        lens.frameLeft = 7000;
        CHECK(!makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto).any());
    }

    // ---- chromatic aberration -------------------------------------------------------
    {
        LensCorrection lens = wholeFrame(6000, 4000);
        lens.distortion = sampled([](double r) { return 0.98 + 0.03 * r * r; });
        lens.caRed = sampled([](double) { return 0.0005; });
        lens.caBlue = sampled([](double) { return -0.0003; });
        lens.distortionEnabled = lens.caEnabled = true;
        LensOpcodes o = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto);
        CHECK(o.distortion && o.chromaticAberration && o.planes == 3);
        for (size_t i = 0; i < 4; ++i) {
            CHECK_NEAR(o.warp[0][i], o.warp[1][i] * 1.0005, 1e-7);
            CHECK_NEAR(o.warp[2][i], o.warp[1][i] * 0.9997, 1e-7);
        }
        CHECK(o.opcodeList3.size() == 4 + 16 + 4 + 3 * 48 + 16);
        CHECK(be32(o.opcodeList3, 20) == 3);
        CHECK(o.summary() == "distortion, chromatic aberration");

        // Aberration alone leaves green where it is.
        lens.distortionEnabled = false;
        o = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto);
        CHECK(!o.distortion && o.chromaticAberration && o.planes == 3);
        CHECK_NEAR(o.warp[1][0], 1.0, 1e-9);
        CHECK_NEAR(o.warp[1][1], 0.0, 1e-9);
        CHECK_NEAR(o.warp[0][0], 1.0005, 1e-9);
        CHECK_NEAR(o.warp[2][0], 0.9997, 1e-9);

        // The aberration curve runs over the stored radius: with a ratio of
        // 0.5 a point at radius r looks the curve up at r / 2.
        LensCorrection half = wholeFrame(6000, 4000);
        half.distortion = sampled([](double) { return 0.5; });
        half.caRed = sampled([](double r) { return 0.001 * r * r; });
        half.caBlue = sampled([](double) { return 0.0; });
        half.distortionEnabled = half.caEnabled = true;
        o = makeLensOpcodes(half, 6000, 4000, 3, LensCorrectionMode::Auto);
        CHECK_NEAR(evaluateWarp(o.warp[0], 0.8) / evaluateWarp(o.warp[1], 0.8), 1.0 + 0.001 * 0.16, 1e-7);

        // Not three colours: aberration cannot be expressed, distortion can.
        lens.distortionEnabled = true;
        o = makeLensOpcodes(lens, 6000, 4000, 4, LensCorrectionMode::Auto);
        CHECK(o.distortion && !o.chromaticAberration && o.planes == 1);
        CHECK(o.notes.size() == 1);
    }

    // ---- which corrections are written ----------------------------------------------
    {
        LensCorrection lens = wholeFrame(6000, 4000);
        lens.distortion = sampled([](double r) { return 1.0 - 0.02 * r * r; });
        lens.caRed = sampled([](double) { return 0.0002; });
        lens.caBlue = sampled([](double) { return 0.0002; });
        lens.vignetting = sampled([](double r) { return 1.0 + 0.3 * r * r; });
        CHECK(!makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto).any());
        CHECK(makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto).opcodeList3.empty());
        LensOpcodes all = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::All);
        CHECK(all.distortion && all.chromaticAberration && all.vignetting);
        CHECK(all.summary() == "distortion, chromatic aberration, vignetting");
        lens.distortionEnabled = lens.caEnabled = lens.vignettingEnabled = true;
        CHECK(!makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::None).any());
        // A gain the camera has already applied is never written again.
        lens.vignettingInData = true;
        all = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::All);
        CHECK(all.distortion && all.chromaticAberration && !all.vignetting);
        CHECK(be32(all.opcodeList3, 0) == 1);
        CHECK(!makeLensOpcodes(LensCorrection{}, 6000, 4000, 3, LensCorrectionMode::All).any());

        // Filling the frame: the constant term carries the zoom.
        lens.fitFrame = true;
        const LensOpcodes fitted = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto);
        CHECK(fitted.zoom > 1.0);
        CHECK_NEAR(fitted.warp[1][0], fitted.zoom, 1e-4);
        // ... and the short side of the picture then comes from the edge of the frame.
        const double edge = 2000.0 / std::hypot(3000.0, 2000.0);
        CHECK_NEAR(evaluateWarp(fitted.warp[1], edge), 1.0, 1e-4);
    }

    // ---- curves DNG cannot hold are left out -------------------------------------------
    {
        LensCorrection lens = wholeFrame(6000, 4000);
        // Folds back on itself: r * ratio(r) falls beyond r = 0.6.
        lens.distortion = sampled([](double r) { return r < 0.6 ? 1.0 : 1.0 - 4.0 * (r - 0.6); });
        lens.vignetting = sampled([](double r) { return 1.0 + 0.3 * r * r; });
        lens.distortionEnabled = lens.vignettingEnabled = true;
        const LensOpcodes o = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto);
        CHECK(!o.distortion && o.planes == 0);
        CHECK(o.vignetting);
        CHECK(o.notes.size() == 1);
        CHECK(be32(o.opcodeList3, 0) == 1);
    }

    // ---- Sony: plain tags -----------------------------------------------------------------
    {
        const fs::path file = writeFile(dir, "lens_sony.arw", sonyFile(257, 1, 17));
        LensCorrection lens = readLensData(file, noNote, 6000, 4000);
        CHECK(lens.origin == "Sony");
        CHECK(lens.fitFrame);
        CHECK(lens.distortion.radius.size() == 16);
        CHECK_NEAR(lens.distortion.radius[0], 0.5 / 15, 1e-12);
        CHECK_NEAR(lens.distortion.radius[15], 15.5 / 15, 1e-12);
        CHECK_NEAR(lens.distortion.value[0], 1.0 + 11.0 / 16384, 1e-12);
        CHECK_NEAR(lens.distortion.value[15], 1.0 - 1008.0 / 16384, 1e-12);
        CHECK(lens.caRed.radius.size() == 16 && lens.caBlue.radius.size() == 16);
        CHECK_NEAR(lens.caRed.value[0], 512.0 / 2097152, 1e-15);
        CHECK_NEAR(lens.caBlue.value[15], (1152.0 + 48 * 15) / 2097152, 1e-15);
        // The aberration knots are moved to where the distortion puts them.
        CHECK_NEAR(lens.caRed.radius[15], 15.5 / 15 * (1.0 - 1008.0 / 16384), 1e-12);
        CHECK(lens.vignetting.radius.size() == 16);
        CHECK_NEAR(lens.vignetting.value[0], 1.0, 1e-12);
        CHECK_NEAR(lens.vignetting.value[15], std::pow(2.0, std::pow(2.0, 13184.0 / 8192 - 1) - 0.5), 1e-12);
        CHECK(lens.distortionEnabled && lens.caEnabled);
        // Shading compensation on: the camera has applied the gain already.
        CHECK(lens.vignettingInData && !lens.vignettingEnabled);

        // On real proportions the polynomial follows the table closely.
        lens.frameWidth = 6000;
        lens.frameHeight = 4000;
        lens.frameLeft = lens.frameTop = 12;
        const LensOpcodes o = makeLensOpcodes(lens, 6024, 4024, 3, LensCorrectionMode::Auto);
        CHECK(o.distortion && o.chromaticAberration && !o.vignetting);
        CHECK(o.warpErrorPixels < 1.0);
        CHECK_NEAR(o.zoom, 1.0263, 0.0005);

        // Everything switched off in the camera.
        lens = readLensData(writeFile(dir, "lens_sony_off.arw", sonyFile(256, 0, 0)), noNote, 6000, 4000);
        CHECK(lens.hasDistortion() && lens.hasCa() && lens.hasVignetting());
        CHECK(!lens.distortionEnabled && !lens.caEnabled && !lens.vignettingEnabled && !lens.vignettingInData);
        lens.frameWidth = 6000;
        lens.frameHeight = 4000;
        CHECK(!makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::Auto).any());
        const LensOpcodes all = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::All);
        CHECK(all.distortion && all.chromaticAberration && all.vignetting);
        CHECK(all.gainErrorPercent < 2.0);

        // "No parameters available": the tables mean nothing.
        lens = readLensData(writeFile(dir, "lens_sony_none.arw", sonyFile(511, 255, 255)), noNote, 6000, 4000);
        CHECK(lens.empty());

        // A table that announces more knots than it holds is not used; the
        // others are.
        Bytes broken = tiffHeader();
        std::vector<int> lying = kSonyDistortion;
        lying[0] = 30;
        placeDirectory(broken, 8, {ascii(271, "SONY"), longs(0x14a, {0x200})});
        placeDirectory(broken, 0x200, {shorts(0x7032, kSonyVignetting), shorts(0x7035, sonyCa()), shorts(0x7037, lying)});
        lens = readLensData(writeFile(dir, "lens_sony_broken.arw", broken), noNote, 6000, 4000);
        CHECK(!lens.hasDistortion() && lens.hasCa() && lens.hasVignetting());
        // Without the setting tags nothing says the gain is not in the data
        // already, so it is taken to be.
        CHECK(lens.caEnabled && lens.vignettingInData);
    }

    // ---- Sony: the SR2 block ------------------------------------------------------------
    {
        for (uint32_t key : {0x44332211u, 1u, 0xffffffffu}) {
            const LensCorrection lens =
                readLensData(writeFile(dir, "lens_sony_sr2.arw", sonySr2File(key)), noNote, 4912, 3264);
            CHECK(lens.origin == "Sony");
            CHECK(lens.distortion.radius.size() == 11);
            CHECK_NEAR(lens.distortion.radius[10], 10.5 / 10, 1e-12);
            CHECK_NEAR(lens.distortion.value[10], 1.0 + 203.0 / 16384, 1e-12);
            CHECK_NEAR(lens.caRed.value[0], 1268.0 / 2097152, 1e-15);
            CHECK_NEAR(lens.caBlue.value[10], 1320.0 / 2097152, 1e-15);
            CHECK(lens.vignetting.radius.size() == 11);
            CHECK(!lens.distortionEnabled);  // setting 0
            CHECK(lens.caEnabled);
            CHECK(lens.vignettingInData);
        }
        // A wrong key leaves noise, which must not be mistaken for tables.
        Bytes f = sonySr2File(0x44332211u);
        f[0x100 + 2 + 12 * 2 + 8] ^= 0x55;  // the key's first byte
        CHECK(readLensData(writeFile(dir, "lens_sony_sr2_badkey.arw", f), noNote, 4912, 3264).empty());
    }

    // ---- Panasonic ------------------------------------------------------------------------
    {
        const Bytes tag = panasonicTag(3821, -431, -138, -32, 0xf001, 2870);
        // The checksum routine of the test agrees with a real camera's tag.
        const Bytes real = {0xdc, 0x05, 0xea, 0x4e, 0x4a, 0x00, 0x6b, 0x02, 0x51, 0xfe, 0xe0,
                            0xff, 0x52, 0x00, 0x01, 0xf0, 0xed, 0x0e, 0xc8, 0x01, 0x04, 0x02,
                            0x76, 0xff, 0x36, 0x0b, 0xbc, 0x03, 0xf9, 0x9f, 0x04, 0x7e};
        CHECK(tag == real);

        const double halfDiagonal = 0.5 * std::hypot(4592.0, 3448.0);
        const LensCorrection lens =
            readLensData(writeFile(dir, "lens_panasonic.rw2", panasonicFile(tag)), noNote, 4592, 3448);
        CHECK(lens.origin == "Panasonic");
        CHECK(lens.hasDistortion() && !lens.hasCa() && !lens.hasVignetting());
        CHECK(lens.distortionEnabled && !lens.fitFrame);
        // undistorted = s (r + a r^3 + b r^5 + c r^7), radii in units of 2870 pixels.
        const double a = 3821 / 32768.0, b = -431 / 32768.0, c = -138 / 32768.0, s = 1.0 / (1.0 - 32 / 32768.0);
        for (double stored : {0.1, 0.4, 0.7, 0.95}) {
            const double undistorted = s * (stored + a * std::pow(stored, 3) + b * std::pow(stored, 5) + c * std::pow(stored, 7));
            const double radius = undistorted * 2870 / halfDiagonal;  // as the curve counts it
            CHECK_NEAR(lens.distortion.at(radius), stored / undistorted, 2e-5);
        }
        CHECK_NEAR(lens.distortion.value[0], 1.0 / s, 1e-12);

        // A unit far from the half diagonal is not believed; the half diagonal is used.
        const LensCorrection odd = readLensData(
            writeFile(dir, "lens_panasonic_unit.rw2", panasonicFile(panasonicTag(3821, -431, -138, -32, 1, 500))),
            noNote, 4592, 3448);
        CHECK(odd.hasDistortion());
        CHECK_NEAR(odd.distortion.radius[64], s * (1 + a + b + c), 1e-9);

        // Switched off, or a checksum that does not match: nothing.
        CHECK(readLensData(writeFile(dir, "lens_panasonic_off.rw2", panasonicFile(panasonicTag(3821, -431, -138, -32, 0, 2870))),
                           noNote, 4592, 3448).empty());
        for (size_t i = 4; i < 28; ++i) {
            Bytes damaged = tag;
            damaged[i] ^= 0x10;
            CHECK(readLensData(writeFile(dir, "lens_panasonic_bad.rw2", panasonicFile(damaged)), noNote, 4592, 3448).empty());
        }
        CHECK(readLensData(writeFile(dir, "lens_panasonic_zero.rw2", panasonicFile(panasonicTag(0, 0, 0, 0, 1, 2870))),
                           noNote, 4592, 3448).empty());
    }

    // ---- Fujifilm -------------------------------------------------------------------------
    {
        // Eleven knots; the aberration table leaves out the one at radius 0.
        std::vector<double> d = {267.4545}, c = {294.2}, v = {267.4545};
        for (int i = 0; i <= 10; ++i) d.push_back(i / 10.0);
        const std::vector<double> percent = {0, 0.114, 0.249, 0.427, 0.667, 0.976, 1.352, 1.818, 2.379, 3.042, 3.837};
        d.insert(d.end(), percent.begin(), percent.end());
        for (int i = 1; i <= 10; ++i) c.push_back(i / 10.0);
        for (int i = 1; i <= 10; ++i) c.push_back(0.00001 * i);    // red
        for (int i = 1; i <= 10; ++i) c.push_back(-0.00002 * i);   // blue
        for (int i = 0; i <= 10; ++i) v.push_back(i / 10.0);
        const std::vector<double> light = {100, 99.74, 99.5, 99.3, 99.14, 98.82, 98.11, 97.67, 97.2, 96.64, 93.72};
        v.insert(v.end(), light.begin(), light.end());

        const fs::path file = writeFile(dir, "lens_fuji.raf", fujiFile(d, c, v));
        LensCorrection lens = readLensData(file, noNote, 4896, 3264);
        CHECK(lens.origin == "Fujifilm");
        CHECK(lens.fitFrame);
        CHECK(lens.distortionEnabled && lens.caEnabled && lens.vignettingEnabled && !lens.vignettingInData);
        CHECK(lens.distortion.radius.size() == 11);
        // The table runs over the stored radius; the curve over the corrected one.
        CHECK_NEAR(lens.distortion.value[10], 1.03837, 1e-9);
        CHECK_NEAR(lens.distortion.radius[10], 1.0 / 1.03837, 1e-9);
        CHECK_NEAR(lens.distortion.radius[5], 0.5 / 1.00976, 1e-9);
        CHECK(lens.caRed.radius.size() == 11);  // knot at 0 added
        CHECK_NEAR(lens.caRed.value[0], 0.0, 1e-15);
        CHECK_NEAR(lens.caRed.at(0.5), 0.00005, 1e-9);
        CHECK_NEAR(lens.caBlue.at(1.0), -0.0002, 1e-9);
        CHECK_NEAR(lens.vignetting.at(1.0), 100 / 93.72, 1e-9);
        CHECK_NEAR(lens.vignetting.at(0.0), 1.0, 1e-12);

        lens.frameWidth = 4896;
        lens.frameHeight = 3264;
        lens.frameLeft = 28;
        lens.frameTop = 12;
        const LensOpcodes o = makeLensOpcodes(lens, 4952, 3288, 3, LensCorrectionMode::Auto);
        CHECK(o.distortion && o.chromaticAberration && o.vignetting);
        CHECK(o.warpErrorPixels < 1.0);
        CHECK(o.gainErrorPercent < 1.0);
        CHECK_NEAR(o.zoom, 0.963, 0.001);

        // The cut-down modes: radii grow by 1.25.
        const LensCorrection cropped = readLensData(file, fujiNote(2), 3888, 2592);
        CHECK_NEAR(cropped.distortion.radius[10], 1.25 / 1.03837, 1e-9);
        CHECK_NEAR(cropped.vignetting.radius[10], 1.25, 1e-9);
        CHECK_NEAR(readLensData(file, fujiNote(0), 4896, 3264).vignetting.radius[10], 1.0, 1e-9);

        // Nine knots throughout.
        std::vector<double> d9 = {300}, c9 = {300}, v9 = {300};
        for (int i = 0; i < 9; ++i) d9.push_back(i / 8.0);
        for (int i = 0; i < 9; ++i) d9.push_back(-0.3 * i);
        for (int i = 0; i < 9; ++i) c9.push_back(i / 8.0);
        for (int i = 0; i < 9; ++i) c9.push_back(0.00001 * i);
        for (int i = 0; i < 9; ++i) c9.push_back(0.00003 * i);
        c9.push_back(0);
        for (int i = 0; i < 9; ++i) v9.push_back(i / 8.0);
        for (int i = 0; i < 9; ++i) v9.push_back(100.0 - 4.0 * i);
        CHECK(d9.size() == 19 && c9.size() == 29 && v9.size() == 19);
        const LensCorrection nine = readLensData(writeFile(dir, "lens_fuji9.raf", fujiFile(d9, c9, v9)), noNote, 6240, 4160);
        CHECK(nine.distortion.radius.size() == 9);
        CHECK_NEAR(nine.distortion.value[8], 1.0 - 0.024, 1e-9);
        CHECK(nine.caRed.radius.size() == 9);
        CHECK_NEAR(nine.caBlue.value[8], 0.00024, 1e-9);
        CHECK_NEAR(nine.vignetting.value[8], 100.0 / 68.0, 1e-9);

        // Tables of a length nobody has seen are left alone.
        std::vector<double> strange(20, 0.5);
        CHECK(readLensData(writeFile(dir, "lens_fuji_strange.raf", fujiFile(strange, strange, strange)), noNote, 4896, 3264).empty());
        // Knots out of order.
        std::vector<double> shuffled = d;
        std::swap(shuffled[3], shuffled[7]);
        CHECK(!readLensData(writeFile(dir, "lens_fuji_shuffled.raf", fujiFile(shuffled, c, v)), noNote, 4896, 3264).hasDistortion());
    }

    // ---- Olympus --------------------------------------------------------------------------
    {
        const fs::path file = writeFile(dir, "lens_olympus.orf", olympusFile());
        const std::vector<float> dist = {0.0215045f, 0.00459385f, 0.0010916f, 0.974609375f};
        const std::vector<float> ca = {0.000120878f, -6.96182e-05f, 4.00543e-05f, 9.56059e-05f, 0.000133038f, -0.00010252f};
        for (bool be : {false, true}) {
            const LensCorrection lens = readLensData(file, olympusNote(be, dist, ca), 4608, 3456);
            CHECK(lens.origin == "Olympus");
            CHECK(lens.hasDistortion() && lens.hasCa() && !lens.hasVignetting());
            CHECK(lens.distortionEnabled && lens.caEnabled && !lens.fitFrame);
            const double s = dist[3], q2 = s * s;
            CHECK_NEAR(lens.distortion.at(1.0), s * (1 + q2 * (dist[0] + q2 * (dist[1] + q2 * dist[2]))), 1e-9);
            CHECK_NEAR(lens.distortion.at(0.0), s, 1e-9);
            CHECK_NEAR(lens.caRed.at(1.0), ca[0] + ca[1] + ca[2], 1e-9);
            CHECK_NEAR(lens.caBlue.at(0.5), ca[3] + 0.25 * ca[4] + 0.0625 * ca[5], 1e-9);
        }
        // "0 0 0 1" means no distortion correction; aberration can still be there.
        const LensCorrection caOnly = readLensData(file, olympusNote(false, {0, 0, 0, 1}, ca), 4608, 3456);
        CHECK(!caOnly.hasDistortion() && caOnly.hasCa());
        CHECK(readLensData(file, olympusNote(false, {0, 0, 0, 1}, {0, 0, 0, 0, 0, 0}), 4608, 3456).empty());
        // No maker note, or somebody else's.
        CHECK(readLensData(file, noNote, 4608, 3456).empty());
        CHECK(readLensData(file, fujiNote(0), 4608, 3456).empty());
        // Coefficients far outside what a lens needs are refused.
        CHECK(readLensData(file, olympusNote(false, {5.0f, 0, 0, 1}, {0, 0, 0, 0, 0, 0}), 4608, 3456).empty());
    }

    // ---- files that are something else ------------------------------------------------------
    {
        CHECK(readLensData(dir / "lens_no_such_file.arw", noNote, 6000, 4000).empty());
        CHECK(readLensData(writeFile(dir, "lens_empty.arw", {}), noNote, 6000, 4000).empty());
        CHECK(readLensData(writeFile(dir, "lens_text.arw", Bytes(500, 'x')), noNote, 6000, 4000).empty());
        Bytes canon = tiffHeader();
        placeDirectory(canon, 8, {ascii(271, "Canon")});
        CHECK(readLensData(writeFile(dir, "lens_canon.cr2", canon), noNote, 6000, 4000).empty());
    }

    // ---- damaged files: no crash, and nothing implausible comes out ----------------------
    {
        const std::vector<double> flat(23, 0.5);
        std::vector<double> d = {267.4545}, c = {294.2};
        for (int i = 0; i <= 10; ++i) d.push_back(i / 10.0);
        for (int i = 0; i <= 10; ++i) d.push_back(0.3 * i);
        for (int i = 1; i <= 10; ++i) c.push_back(i / 10.0);
        for (int i = 0; i < 20; ++i) c.push_back(0.00001 * i);
        const std::vector<Bytes> files = {sonyFile(257, 1, 17), sonySr2File(0x44332211u),
                                          panasonicFile(panasonicTag(3821, -431, -138, -32, 1, 2870)),
                                          fujiFile(d, c, d), olympusFile()};
        const MakerNote note = olympusNote(false, {0.02f, 0.004f, 0.001f, 0.97f}, {1e-4f, 0, 0, 1e-4f, 0, 0});
        testutil::Random rnd(2026);
        int survived = 0;
        for (int round = 0; round < 1500; ++round) {
            Bytes f = files[rnd.below(static_cast<uint32_t>(files.size()))];
            const uint32_t kind = rnd.below(3);
            if (kind == 0) {
                f.resize(rnd.below(static_cast<uint32_t>(f.size())));
            } else {
                const uint32_t changes = 1 + rnd.below(kind == 1 ? 3 : 40);
                for (uint32_t i = 0; i < changes; ++i) f[rnd.below(static_cast<uint32_t>(f.size()))] = static_cast<uint8_t>(rnd.next());
            }
            MakerNote n = note;
            if (rnd.below(2)) {
                for (uint32_t i = 0; i < 1 + rnd.below(6); ++i)
                    n.data[rnd.below(static_cast<uint32_t>(n.data.size()))] = static_cast<uint8_t>(rnd.next());
                if (rnd.below(4) == 0) n.data.resize(rnd.below(static_cast<uint32_t>(n.data.size())));
            }
            LensCorrection lens = readLensData(writeFile(dir, "lens_fuzz.bin", f), n, 6000, 4000);
            CHECK(plausible(lens));
            if (!lens.empty()) ++survived;
            // Whatever came out must also go through the fit without trouble.
            lens.frameWidth = 6000;
            lens.frameHeight = 4000;
            const LensOpcodes o = makeLensOpcodes(lens, 6000, 4000, 3, LensCorrectionMode::All);
            for (uint32_t p = 0; p < o.planes; ++p)
                for (double k : o.warp[p]) CHECK(std::isfinite(k));
            for (double k : o.gain) CHECK(std::isfinite(k));
        }
        CHECK(survived > 100);  // the test does exercise the parsers
    }

    // ---- into a DNG ----------------------------------------------------------------------------
    {
        RawImage img = smallImage();
        img.lens.frameLeft = 0;
        img.lens.frameTop = 0;
        img.lens.frameWidth = img.width;
        img.lens.frameHeight = img.height;
        img.lens.distortion = sampled([](double r) { return 1.0 - 0.03 * r * r; });
        img.lens.caRed = sampled([](double) { return 0.0004; });
        img.lens.caBlue = sampled([](double) { return -0.0004; });
        img.lens.vignetting = sampled([](double r) { return 1.0 + 0.4 * r * r; });
        img.lens.distortionEnabled = img.lens.caEnabled = img.lens.vignettingEnabled = true;
        const LensOpcodes expected = makeLensOpcodes(img, LensCorrectionMode::Auto);
        CHECK(expected.distortion && expected.chromaticAberration && expected.vignetting);

        Bytes uniqueWith, uniqueWithout;
        for (DngByteOrder order : {DngByteOrder::Little, DngByteOrder::Big}) {
            DngWriteOptions options;
            options.byteOrder = order;
            const fs::path file = dir / "lens_roundtrip.dng";
            writeDng(img, file, options);
            const std::string data = tiffreader::slurp(file);
            const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(data);
            const tiffreader::Ifd raw = tiffreader::parseIfd(data, ifd0.at(330).u32());
            CHECK(raw.count(51022) == 1);
            if (raw.count(51022)) {
                CHECK(raw.at(51022).type == 7);
                // Opcode lists are big-endian in files of either byte order.
                CHECK(raw.at(51022).data == expected.opcodeList3);
            }
            CHECK(ifd0.at(50707).data == (Bytes{1, 3, 0, 0}));  // DNGBackwardVersion
            uniqueWith = ifd0.at(50781).data;

            options.lensCorrections = LensCorrectionMode::None;
            writeDng(img, file, options);
            const std::string plain = tiffreader::slurp(file);
            const tiffreader::Ifd plain0 = tiffreader::parseFirstIfd(plain);
            CHECK(tiffreader::parseIfd(plain, plain0.at(330).u32()).count(51022) == 0);
            CHECK(plain0.at(50707).data == (Bytes{1, 1, 0, 0}));
            uniqueWithout = plain0.at(50781).data;
            // The corrections change how the file is rendered, so its identity differs.
            CHECK(uniqueWith != uniqueWithout);
        }

        // Non-square pixels: the opcodes cannot describe that; a note instead.
        img.scaleH = 2.0;
        const LensOpcodes skew = makeLensOpcodes(img, LensCorrectionMode::Auto);
        CHECK(!skew.any() && skew.notes.size() == 1);
        img.scaleH = 1.0;
        // Colours other than red, green, blue: distortion only.
        img.planeColor = {kCyan, kMagenta, kYellow, kGreen};
        const LensOpcodes cmy = makeLensOpcodes(img, LensCorrectionMode::Auto);
        CHECK(cmy.distortion && !cmy.chromaticAberration);
    }

    return testutil::finish("test_lens");
}
