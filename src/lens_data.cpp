// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// None of the makers documents these parameters. What is known about them
// was worked out by others, and this file follows their findings:
//
//   Sony, Fujifilm  Freddie Witherden and Simone Gotti, for darktable's
//                   lens-correction module (src/iop/lens.cc, pull requests
//                   7092 and 10519); tag names from ExifTool.
//   Olympus         the Olympus section of the same darktable module.
//   Panasonic       Raphael Rigo's notes on the DistortionInfo tag
//                   (github.com/trou/panasonic-rw2), as used by ExifTool.
//   Sony SR2 block  the scrambling was first described in Dave Coffin's
//                   dcraw (sony_decrypt).
//
// The code below is written from those descriptions, not copied from them.
#include "lens_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "tiff_source.hpp"
#include "tiff_writer.hpp"  // tiffTypeSize

namespace dngconv {

namespace {

using namespace tiffsource;
using Values = std::vector<double>;

constexpr uint16_t kMake = 271;
constexpr uint16_t kSubIfds = 0x014a;
constexpr size_t kMaxValues = 512;

// ---- decoding TIFF values into numbers -------------------------------------

int32_t signed32(uint32_t v) {
    return v <= 0x7fffffffu ? static_cast<int32_t>(v)
                            : -static_cast<int32_t>(~v) - 1;
}

Values decode(uint16_t type, uint32_t count, const uint8_t* p, bool bigEndian) {
    Values out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        switch (type) {
            case 1:  // BYTE
            case 7:  // UNDEFINED
                out.push_back(p[i]);
                break;
            case 6:  // SBYTE
                out.push_back(static_cast<int8_t>(p[i]));
                break;
            case 3:  // SHORT
                out.push_back(get16(p + 2 * i, bigEndian));
                break;
            case 8:  // SSHORT
                out.push_back(static_cast<int16_t>(get16(p + 2 * i, bigEndian)));
                break;
            case 4:  // LONG
                out.push_back(get32(p + 4 * i, bigEndian));
                break;
            case 9:  // SLONG
                out.push_back(signed32(get32(p + 4 * i, bigEndian)));
                break;
            case 5: {  // RATIONAL
                const double d = get32(p + 8 * i + 4, bigEndian);
                out.push_back(d != 0 ? get32(p + 8 * i, bigEndian) / d : 0.0);
                break;
            }
            case 10: {  // SRATIONAL
                const double d = signed32(get32(p + 8 * i + 4, bigEndian));
                out.push_back(d != 0 ? signed32(get32(p + 8 * i, bigEndian)) / d : 0.0);
                break;
            }
            case 11: {  // FLOAT
                const uint32_t bits = get32(p + 4 * i, bigEndian);
                float f;
                std::memcpy(&f, &bits, 4);
                out.push_back(f);
                break;
            }
            default:
                return {};
        }
    }
    return out;
}

// Values of a tag in a directory read from the file.
Values fileValues(const Tiff& tiff, const std::vector<Entry>& entries, uint16_t tag) {
    const Entry* e = find(entries, tag);
    std::vector<uint8_t> bytes;
    if (!e || e->count > kMaxValues || !loadValue(tiff, *e, kMaxValues * 8, bytes)) return {};
    return decode(e->type, e->count, bytes.data(), tiff.bigEndian);
}

// A piece of a TIFF structure held in memory. Offsets found in it are counted
// from `origin` bytes before its first byte.
struct Block {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool bigEndian = false;
    uint64_t origin = 0;

    const uint8_t* at(uint64_t offset, uint64_t length) const {
        if (offset < origin) return nullptr;
        const uint64_t i = offset - origin;
        if (i > size || length > size - i) return nullptr;
        return data + i;
    }
};

bool blockDirectory(const Block& block, uint64_t offset, std::vector<Entry>& entries) {
    entries.clear();
    const uint8_t* head = block.at(offset, 2);
    if (!head) return false;
    const unsigned count = get16(head, block.bigEndian);
    if (count == 0 || count > kMaxEntries) return false;
    const uint8_t* raw = block.at(offset + 2, static_cast<uint64_t>(count) * 12);
    if (!raw) return false;
    entries.resize(count);
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t* e = raw + static_cast<size_t>(i) * 12;
        entries[i].tag = get16(e, block.bigEndian);
        entries[i].type = get16(e + 2, block.bigEndian);
        entries[i].count = get32(e + 4, block.bigEndian);
        std::memcpy(entries[i].value, e + 8, 4);
    }
    return true;
}

Values blockValues(const Block& block, const std::vector<Entry>& entries, uint16_t tag) {
    const Entry* e = find(entries, tag);
    if (!e || e->count == 0 || e->count > kMaxValues) return {};
    const size_t unit = tiffTypeSize(e->type);
    if (unit == 0) return {};
    const uint64_t size = static_cast<uint64_t>(unit) * e->count;
    const uint8_t* p = size <= 4 ? e->value : block.at(get32(e->value, block.bigEndian), size);
    if (!p) return {};
    return decode(e->type, e->count, p, block.bigEndian);
}

// The directory a pointer entry (type LONG or IFD, one value) leads to.
bool pointer(const std::vector<Entry>& entries, uint16_t tag, bool bigEndian, uint32_t& target) {
    const Entry* e = find(entries, tag);
    if (!e || e->count != 1 || (e->type != 4 && e->type != 13)) return false;
    target = get32(e->value, bigEndian);
    return true;
}

bool allZero(const Values& v, size_t begin, size_t end) {
    for (size_t i = begin; i < end && i < v.size(); ++i)
        if (v[i] != 0) return false;
    return true;
}

// ---- Sony ---------------------------------------------------------------------

struct SonyTables {
    Values distortion, ca, vignetting;
    // The camera's settings; negative when the file does not say.
    double distortionSetting = -1, caSetting = -1, vignettingSetting = -1;
    bool any() const { return !distortion.empty() || !ca.empty() || !vignetting.empty(); }
};

double firstOr(const Values& v, double fallback) { return v.empty() ? fallback : v[0]; }

// Cameras from 2012 on: plain tags next to the raw image.
void sonyFromRawDirectory(const Tiff& tiff, const std::vector<Entry>& ifd0, SonyTables& t) {
    const Entry* sub = find(ifd0, kSubIfds);
    if (!sub || (sub->type != 4 && sub->type != 13) || sub->count == 0 || sub->count > 8) return;
    Entry asLongs = *sub;
    asLongs.type = 4;
    std::vector<uint8_t> offsets;
    if (!loadValue(tiff, asLongs, 32, offsets)) return;
    for (uint32_t i = 0; i < sub->count; ++i) {
        std::vector<Entry> dir;
        if (!readIfd(tiff, get32(&offsets[4 * i], tiff.bigEndian), dir)) continue;
        t.vignetting = fileValues(tiff, dir, 0x7032);
        t.ca = fileValues(tiff, dir, 0x7035);
        t.distortion = fileValues(tiff, dir, 0x7037);
        if (!t.any()) continue;
        t.vignettingSetting = firstOr(fileValues(tiff, dir, 0x7031), -1);
        t.caSetting = firstOr(fileValues(tiff, dir, 0x7034), -1);
        t.distortionSetting = firstOr(fileValues(tiff, dir, 0x7036), -1);
        return;
    }
}

// Sony's SR2 block is a TIFF directory XORed with a key stream. The stream
// comes from a 127-word shift register seeded with a key stored next to the
// block's position: four words from a linear congruential generator, the
// rest from the recurrence below, and then each output word is the XOR of
// two register taps 64 apart. Words are applied in big-endian byte order.
void unscrambleSr2(std::vector<uint8_t>& data, uint32_t key) {
    uint32_t pad[128] = {};
    for (int i = 0; i < 4; ++i) pad[i] = key = key * 48828125u + 1u;
    pad[3] = (pad[3] << 1) | ((pad[0] ^ pad[2]) >> 31);
    for (int i = 4; i < 127; ++i)
        pad[i] = ((pad[i - 4] ^ pad[i - 2]) << 1) | ((pad[i - 3] ^ pad[i - 1]) >> 31);
    unsigned p = 127;
    for (size_t at = 0; at + 4 <= data.size(); at += 4) {
        ++p;
        const uint32_t word = pad[p & 127] ^ pad[(p + 64) & 127];
        pad[(p - 1) & 127] = word;
        data[at] ^= static_cast<uint8_t>(word >> 24);
        data[at + 1] ^= static_cast<uint8_t>(word >> 16);
        data[at + 2] ^= static_cast<uint8_t>(word >> 8);
        data[at + 3] ^= static_cast<uint8_t>(word);
    }
}

// Earlier cameras: the same tables, under other tag numbers, in the SR2
// block that IFD0's tag 0xc634 points to.
void sonyFromSr2(const Tiff& tiff, const std::vector<Entry>& ifd0, SonyTables& t) {
    const Entry* priv = find(ifd0, 0xc634);
    if (!priv) return;
    const bool fourBytes = (priv->type == 1 || priv->type == 7) && priv->count == 4;
    if (!fourBytes && !(priv->type == 4 && priv->count == 1)) return;
    std::vector<Entry> sr2;
    if (!readIfd(tiff, get32(priv->value, tiff.bigEndian), sr2)) return;

    const Entry* where = find(sr2, 0x7200);
    const Entry* length = find(sr2, 0x7201);
    const Entry* key = find(sr2, 0x7221);
    if (!where || !length || !key) return;
    const uint32_t offset = get32(where->value, tiff.bigEndian);
    const uint32_t size = get32(length->value, tiff.bigEndian);
    if (size < 14 || size > (4u << 20)) return;
    std::vector<uint8_t> data(size);
    if (!readAt(tiff, offset, data.data(), size)) return;
    unscrambleSr2(data, get32(key->value, tiff.bigEndian));

    const Block block{data.data(), data.size(), tiff.bigEndian, offset};
    std::vector<Entry> dir;
    if (!blockDirectory(block, offset, dir)) return;
    t.vignetting = blockValues(block, dir, 0x797d);
    t.ca = blockValues(block, dir, 0x7980);
    t.distortion = blockValues(block, dir, 0x7982);
    t.vignettingSetting = firstOr(blockValues(block, dir, 0x797c), -1);
    t.caSetting = firstOr(blockValues(block, dir, 0x797f), -1);
    t.distortionSetting = firstOr(blockValues(block, dir, 0x7981), -1);
}

// A Sony table starts with its number of knots. Returns 0 if it makes no
// sense; `perKnot` is 2 for the chromatic aberration table (red, then blue).
size_t sonyKnots(const Values& table, size_t perKnot) {
    if (table.size() < 3) return 0;
    const double n = table[0] / static_cast<double>(perKnot);
    if (n < 2 || n > 16 || n != std::floor(n)) return 0;
    const size_t knots = static_cast<size_t>(n);
    return table.size() >= 1 + perKnot * knots ? knots : 0;
}

void readSony(const Tiff& tiff, const std::vector<Entry>& ifd0, LensCorrection& out) {
    SonyTables t;
    sonyFromRawDirectory(tiff, ifd0, t);
    if (!t.any()) sonyFromSr2(tiff, ifd0, t);
    if (!t.any()) return;
    out.origin = "Sony";
    out.fitFrame = true;

    // Knot i of a table with n knots sits at radius (i + 0.5) / (n - 1).
    const auto knot = [](size_t i, size_t n) {
        return (static_cast<double>(i) + 0.5) / static_cast<double>(n - 1);
    };

    // Settings: 0 = off, 255 = the lens supplied no parameters; for
    // vignetting 256 and 511. Anything else is some form of "on".
    const bool distortionKnown = t.distortionSetting != 255;
    const bool caKnown = t.caSetting != 255;
    const bool vignettingKnown = t.vignettingSetting != 511;

    // Distortion: steps of 2^-14 around 1.
    const size_t nd = sonyKnots(t.distortion, 1);
    if (nd && distortionKnown && !allZero(t.distortion, 1, 1 + nd)) {
        for (size_t i = 0; i < nd; ++i)
            out.distortion.add(knot(i, nd), 1.0 + std::ldexp(t.distortion[1 + i], -14));
        out.distortionEnabled = t.distortionSetting != 0;
    }

    // Chromatic aberration: steps of 2^-21, red knots first, then blue. The
    // table shares its knots with the distortion table, which is laid out
    // over the corrected picture; here the curve runs over the stored image.
    const size_t nc = sonyKnots(t.ca, 2);
    if (nc && caKnown && !allZero(t.ca, 1, 1 + 2 * nc)) {
        for (size_t i = 0; i < nc; ++i) {
            double r = knot(i, nc);
            if (nc == nd && distortionKnown) r *= 1.0 + std::ldexp(t.distortion[1 + i], -14);
            out.caRed.add(r, std::ldexp(t.ca[1 + i], -21));
            out.caBlue.add(r, std::ldexp(t.ca[1 + nc + i], -21));
        }
        out.caEnabled = t.caSetting != 0;
    }

    // Vignetting: the table value v stands for a gain of 2^(2^(v/8192 - 1) - 0.5).
    const size_t nv = sonyKnots(t.vignetting, 1);
    if (nv && vignettingKnown && !allZero(t.vignetting, 1, 1 + nv)) {
        for (size_t i = 0; i < nv; ++i) {
            const double v = t.vignetting[1 + i];
            out.vignetting.add(knot(i, nv), std::exp2(std::exp2(v / 8192.0 - 1.0) - 0.5));
        }
        // With shading compensation on, a Sony camera applies this gain to
        // the raw data before storing it; the table then only documents what
        // was done. With it off (256) the data is untouched and the table
        // says what the camera would have done.
        out.vignettingInData = t.vignettingSetting != 256;
        out.vignettingEnabled = false;
    }
}

// ---- Fujifilm -----------------------------------------------------------------

// Fuji's "sports finder" and electronic-shutter crop modes store a raw file
// cut down by 1.25, with tables that still refer to the whole sensor.
double fujiCropFactor(const MakerNote& note) {
    if (note.data.size() < 16 || std::memcmp(note.data.data(), "FUJIFILM", 8) != 0) return 1.0;
    const Block block{note.data.data(), note.data.size(), false, 0};
    std::vector<Entry> dir;
    if (!blockDirectory(block, get32(note.data.data() + 8, false), dir)) return 1.0;
    const Values mode = blockValues(block, dir, 0x104d);
    return !mode.empty() && (mode[0] == 2 || mode[0] == 4) ? 1.25 : 1.0;
}

bool ascending(const Values& v, size_t begin, size_t end) {
    for (size_t i = begin + 1; i < end; ++i)
        if (!(v[i] > v[i - 1])) return false;
    return v[begin] >= 0 && v[end - 1] <= 4;
}

void readFuji(Source& src, const uint8_t* head, const MakerNote& note, LensCorrection& out) {
    // The RAF header names the block holding the raw data; in files from
    // X-Trans cameras that block starts with a small TIFF structure.
    const uint64_t offset = get32(head + 100, true);
    Tiff tiff;
    std::vector<Entry> top, dir;
    uint32_t fujiIfd = 0;
    if (!openTiff(src, offset, src.size(), tiff) || !readIfd(tiff, tiff.firstIfd, top) ||
        !pointer(top, 0xf000, tiff.bigEndian, fujiIfd) || !readIfd(tiff, fujiIfd, dir))
        return;

    const Values distortion = fileValues(tiff, dir, 0xf00b);
    const Values ca = fileValues(tiff, dir, 0xf00f);
    const Values vignetting = fileValues(tiff, dir, 0xf010);
    const double crop = fujiCropFactor(note);

    // Two layouts exist. Each table is: one value of unknown meaning, the
    // knot radii, then the values at those knots.
    //   23 / 31 / 23 values: 11 knots; the aberration table leaves out the
    //                        knot at radius 0 and so has 10
    //   19 / 29 / 19 values: 9 knots throughout
    // A knot at radius 0 without correction is added where the table does
    // not start there.

    // Distortion: percent by which a point lies further out in the stored
    // image, given over the radius in the stored image.
    if (distortion.size() == 23 || distortion.size() == 19) {
        const size_t n = distortion.size() == 23 ? 11 : 9;
        if (ascending(distortion, 1, 1 + n) && !allZero(distortion, 1 + n, 1 + 2 * n)) {
            if (distortion[1] > 0) out.distortion.add(0.0, 1.0);
            for (size_t i = 0; i < n; ++i) {
                const double ratio = 1.0 + distortion[1 + n + i] / 100.0;
                if (ratio <= 0) {
                    out.distortion = {};
                    break;
                }
                out.distortion.add(crop * distortion[1 + i] / ratio, ratio);
            }
        }
    }

    // Chromatic aberration: red values, then blue.
    if (ca.size() == 31 || ca.size() == 29) {
        const size_t n = ca.size() == 31 ? 10 : 9;
        if (ascending(ca, 1, 1 + n) && !allZero(ca, 1 + n, 1 + 3 * n)) {
            if (ca[1] > 0) {
                out.caRed.add(0.0, 0.0);
                out.caBlue.add(0.0, 0.0);
            }
            for (size_t i = 0; i < n; ++i) {
                out.caRed.add(crop * ca[1 + i], ca[1 + n + i]);
                out.caBlue.add(crop * ca[1 + i], ca[1 + 2 * n + i]);
            }
        }
    }

    // Vignetting: percent of the light that arrives.
    if (vignetting.size() == 23 || vignetting.size() == 19) {
        const size_t n = vignetting.size() == 23 ? 11 : 9;
        bool usable = ascending(vignetting, 1, 1 + n);
        bool flat = true;
        for (size_t i = 0; i < n && usable; ++i) {
            if (vignetting[1 + n + i] <= 0) usable = false;
            if (vignetting[1 + n + i] != 100) flat = false;
        }
        if (usable && !flat) {
            if (vignetting[1] > 0) out.vignetting.add(0.0, 1.0);
            for (size_t i = 0; i < n; ++i)
                out.vignetting.add(crop * vignetting[1 + i], 100.0 / vignetting[1 + n + i]);
        }
    }

    if (out.empty()) return;
    out.origin = "Fujifilm";
    out.fitFrame = true;
    // Fuji bodies have no switch for these; the camera always applies them.
    out.distortionEnabled = out.caEnabled = out.vignettingEnabled = true;
}

// ---- Panasonic ----------------------------------------------------------------

// The checksum Panasonic protects its DistortionInfo tag with.
unsigned panasonicChecksum(const uint8_t* data, size_t count, size_t stride) {
    unsigned sum = 0;
    for (size_t i = 0; i < count; ++i) sum = (73 * sum + data[i * stride]) % 0xffef;
    return sum;
}

void readPanasonic(const Tiff& tiff, const std::vector<Entry>& ifd0, double pictureWidth,
                   double pictureHeight, LensCorrection& out) {
    const Entry* e = find(ifd0, 0x0119);
    std::vector<uint8_t> raw;
    if (!e || e->count != 32 || tiffTypeSize(e->type) != 1 || !loadValue(tiff, *e, 32, raw)) return;

    // Sixteen little-endian words. Four of them are checksums over the
    // others; a camera ignores the tag when they do not match, and so do we.
    const uint8_t* d = raw.data();
    int16_t w[16];
    for (int i = 0; i < 16; ++i) w[i] = static_cast<int16_t>(get16(d + 2 * i, false));
    const auto word = [&](int i) { return static_cast<unsigned>(get16(d + 2 * i, false)); };
    if (panasonicChecksum(d + 4, 12, 1) != word(1) || panasonicChecksum(d + 16, 12, 1) != word(14) ||
        panasonicChecksum(d + 2, 14, 2) != word(0) || panasonicChecksum(d + 3, 14, 2) != word(15))
        return;

    // Word 7: correction on or off.
    if ((w[7] & 0x0f) != 1) return;

    // undistorted = scale * (r + a r^3 + b r^5 + c r^7), r being the radius
    // in the stored image. Radii are in units of word 12 pixels, which is
    // half the diagonal of the picture.
    const double a = w[8] / 32768.0;
    const double b = w[4] / 32768.0;
    const double c = w[11] / 32768.0;
    const double denominator = 1.0 + w[5] / 32768.0;
    if (denominator < 0.5) return;
    const double scale = 1.0 / denominator;
    if (a == 0 && b == 0 && c == 0) return;

    const double halfDiagonal = 0.5 * std::hypot(pictureWidth, pictureHeight);
    double unit = 1.0;
    if (halfDiagonal > 0 && w[12] > 0.9 * halfDiagonal && w[12] < 1.5 * halfDiagonal)
        unit = w[12] / halfDiagonal;

    // The curve is wanted the other way round, over the undistorted radius.
    double previous = -1.0;
    for (int i = 0; i <= 96; ++i) {
        const double r = i / 64.0;
        const double r2 = r * r;
        const double poly = 1.0 + r2 * (a + r2 * (b + r2 * c));
        const double undistorted = scale * r * poly;
        if (poly <= 0 || undistorted <= previous) break;  // no longer invertible
        out.distortion.add(undistorted * unit, 1.0 / (scale * poly));
        previous = undistorted;
    }
    if (out.distortion.radius.size() < 32) {
        out.distortion = {};
        return;
    }
    out.origin = "Panasonic";
    out.distortionEnabled = true;
}

// ---- Olympus ------------------------------------------------------------------

void readOlympus(const MakerNote& note, LensCorrection& out) {
    // "OLYMPUS\0II" + version, or "OM SYSTEM\0\0\0II" + version; offsets in the
    // note count from its first byte.
    const std::vector<uint8_t>& n = note.data;
    size_t header = 0;
    if (n.size() > 12 && std::memcmp(n.data(), "OLYMPUS\0", 8) == 0)
        header = 12;
    else if (n.size() > 16 && std::memcmp(n.data(), "OM SYSTEM\0", 10) == 0)
        header = 16;
    else
        return;
    const uint8_t* order = n.data() + header - 4;
    if (!((order[0] == 'I' && order[1] == 'I') || (order[0] == 'M' && order[1] == 'M'))) return;
    const Block block{n.data(), n.size(), order[0] == 'M', 0};

    std::vector<Entry> top, processing;
    uint32_t target = 0;
    if (!blockDirectory(block, header, top) || !pointer(top, 0x2040, block.bigEndian, target) ||
        !blockDirectory(block, target, processing))
        return;

    // Distortion: three coefficients and the radius, in the stored image,
    // that the corner of the corrected picture comes from.
    //   stored = corrected * s * (1 + k2 q^2 + k4 q^4 + k6 q^6),  q = corrected * s
    const Values dist = blockValues(block, processing, 0x150a);
    bool hasDistortion = dist.size() == 4 && !allZero(dist, 0, 3) && dist[3] > 0.5 && dist[3] < 1.5;
    const auto ratio = [&](double r) {
        if (!hasDistortion) return 1.0;
        const double q2 = (r * dist[3]) * (r * dist[3]);
        return dist[3] * (1.0 + q2 * (dist[0] + q2 * (dist[1] + q2 * dist[2])));
    };
    if (hasDistortion) {
        for (int i = 0; i <= 80; ++i) {
            const double r = i / 64.0;
            if (ratio(r) <= 0) {
                hasDistortion = false;
                out.distortion = {};
                break;
            }
            out.distortion.add(r, ratio(r));
        }
    }

    // Chromatic aberration, over the radius in the stored image:
    //   red = green * (1 + c0 + c2 r^2 + c4 r^4), and three more for blue.
    const Values ca = blockValues(block, processing, 0x150c);
    if (ca.size() == 6 && !allZero(ca, 0, 6)) {
        for (int i = 0; i <= 80; ++i) {
            const double r = i / 64.0;
            const double r2 = r * r;
            out.caRed.add(r, ca[0] + r2 * (ca[1] + r2 * ca[2]));
            out.caBlue.add(r, ca[3] + r2 * (ca[4] + r2 * ca[5]));
        }
    }

    if (out.empty()) return;
    out.origin = "Olympus";
    // The body's "distortion correction" menu setting is not what decides
    // this: Micro Four Thirds lenses rely on the correction, and the camera's
    // JPEG has it applied whatever that setting says.
    out.distortionEnabled = out.caEnabled = true;
}

// ---- common --------------------------------------------------------------------

bool sane(const RadialCurve& c, double low, double high) {
    if (c.radius.size() != c.value.size()) return false;
    if (c.empty()) return true;
    if (c.radius.size() < 2) return false;
    for (size_t i = 0; i < c.radius.size(); ++i) {
        if (!std::isfinite(c.radius[i]) || !std::isfinite(c.value[i])) return false;
        if (c.value[i] < low || c.value[i] > high) return false;
        if (c.radius[i] < 0 || (i > 0 && !(c.radius[i] > c.radius[i - 1]))) return false;
    }
    return true;
}

// Drops whatever lies outside what a lens could plausibly need, so that a
// misread table can never turn into a wild instruction in the DNG.
void sanitise(LensCorrection& c) {
    if (!sane(c.distortion, 0.5, 1.5)) c.distortion = {};
    if (!sane(c.caRed, -0.02, 0.02) || !sane(c.caBlue, -0.02, 0.02) || c.caRed.empty() != c.caBlue.empty())
        c.caRed = c.caBlue = {};
    if (!sane(c.vignetting, 0.5, 16.0)) c.vignetting = {};
    if (!c.hasDistortion()) c.distortionEnabled = false;
    if (!c.hasCa()) c.caEnabled = false;
    if (!c.hasVignetting()) c.vignettingEnabled = c.vignettingInData = false;
    if (c.empty()) c = LensCorrection{};
}

std::string upperCase(std::string s) {
    for (char& ch : s)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return s;
}

}  // namespace

LensCorrection readLensData(const std::filesystem::path& path, const MakerNote& makerNote,
                            double pictureWidth, double pictureHeight) {
    LensCorrection out;
    try {
        Source src(path);
        uint8_t head[108] = {};
        if (src.size() >= sizeof head && src.read(0, head, sizeof head) &&
            std::memcmp(head, "FUJIFILMCCD-RAW ", 16) == 0) {
            readFuji(src, head, makerNote, out);
        } else {
            Tiff tiff;
            std::vector<Entry> ifd0;
            if (openTiff(src, 0, src.size(), tiff) && readIfd(tiff, tiff.firstIfd, ifd0)) {
                std::string make;
                std::vector<uint8_t> text;
                if (const Entry* e = find(ifd0, kMake); e && e->type == 2 && loadValue(tiff, *e, 256, text))
                    make = upperCase(std::string(text.begin(), std::find(text.begin(), text.end(), uint8_t{0})));
                if (make.rfind("SONY", 0) == 0)
                    readSony(tiff, ifd0, out);
                else if (find(ifd0, 0x0119))  // RW2, whoever's name is on the camera
                    readPanasonic(tiff, ifd0, pictureWidth, pictureHeight, out);
                else if (make.rfind("OLYMPUS", 0) == 0 || make.rfind("OM ", 0) == 0)
                    readOlympus(makerNote, out);
            }
        }
    } catch (...) {
        return LensCorrection{};
    }
    sanitise(out);
    return out;
}

}  // namespace dngconv
