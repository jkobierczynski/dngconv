// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "lossy_jxl.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>

#include "jxl_codec.hpp"
#include "parallel.hpp"

namespace dngconv {

namespace {

constexpr double kS = kLossyCurveSlope;

// The white level the DNG would carry for the image: for a mosaic the lowest
// of the planes', as the writer stores it.
double writtenWhite(const RawImage& img) {
    double w = img.whiteLevel[0];
    if (img.isCfa)
        for (uint32_t p = 1; p < img.colorPlanes; ++p) w = std::min(w, img.whiteLevel[p]);
    return std::clamp(std::round(w), 1.0, 65535.0);
}

// Coordinate in the active area for a coordinate of the stored frame: the
// margin repeats the edge, a whole period of the mosaic at a time, so that
// every stored sample keeps the colour its place in the pattern says.
uint32_t sourceOf(int64_t a, uint32_t size, uint32_t period) {
    if (a < 0) return static_cast<uint32_t>(((a % period) + period) % period);
    if (a >= size) {
        const int64_t base = static_cast<int64_t>(size) - period;
        return static_cast<uint32_t>(base + (a - base) % period);
    }
    return static_cast<uint32_t>(a);
}

// One axis of the stored frame: fields of the active size, each a whole
// number of tiles of at most `maxTile`, a multiple of 8 (the coder's block).
struct Axis {
    uint32_t tile = 0, field = 0, padded = 0, pad = 0;
};

Axis planAxis(uint32_t size, uint32_t factor, uint32_t maxTile) {
    Axis a;
    const uint32_t field = (size + factor - 1) / factor;
    const uint32_t count = (field + maxTile - 1) / maxTile;
    a.tile = ((field + count - 1) / count + 7) / 8 * 8;
    a.field = a.tile * count;
    a.padded = a.field * factor;
    a.pad = (a.padded - size) / 2;
    return a;
}

}  // namespace

double lossyCurveEncode(double x) {
    // The inverse of f(y) = s y + (1 - s) y^3, in closed form (Cardano), as
    // in Adobe's SDK.
    x = std::clamp(x, 0.0, 1.0);
    static const double k0 = std::cbrt(2.0);
    static const double k1 = 108.0 * kS * kS * kS * (1.0 - kS) * (1.0 - kS) * (1.0 - kS);
    const double k2 = 27.0 * x * (1.0 - kS) * (1.0 - kS);
    const double k3 = std::cbrt(k2 + std::sqrt(k1 + k2 * k2));
    const double y = k3 / (3.0 * k0 * (1.0 - kS)) - k0 * kS / k3;
    return std::clamp(y, 0.0, 1.0);
}

double lossyCurveDecode(double y) { return kS * y + (1.0 - kS) * y * y * y; }

bool lossyJxlSupported(const RawImage& img, std::string* why) {
    const auto no = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (img.samplesPerPixel != 1) return no("full-colour raw data (several samples per pixel)");
    const uint32_t aw = img.activeRight - img.activeLeft, ah = img.activeBottom - img.activeTop;
    const uint32_t rows = img.isCfa ? img.cfaRows : 1, cols = img.isCfa ? img.cfaCols : 1;
    if (aw < 2 * cols || ah < 2 * rows) return no("an active area too small for its colour pattern");
    return true;
}

std::vector<uint16_t> linearActiveArea(const RawImage& img, unsigned threads, uint32_t* repaired) {
    const uint32_t aw = img.activeRight - img.activeLeft, ah = img.activeBottom - img.activeTop;
    const double white = writtenWhite(img);
    // Adobe's reader scales every position of the black pattern by the same
    // amount, white minus the highest black, so that all of them saturate
    // at the same value; so does this.
    double maxBlack = 0;
    for (double b : img.blackLevel) maxBlack = std::max(maxBlack, b);
    const double scale = white > maxBlack ? 1.0 / (white - maxBlack) : 0.0;
    std::vector<uint16_t> out(static_cast<size_t>(aw) * ah);
    const auto sample = [&](uint32_t ay, uint32_t ax) {
        return img.pixels[static_cast<size_t>(ay + img.activeTop) * img.width + ax + img.activeLeft];
    };
    // Defective pixels: the camera stored 0 for them. Filled in as Adobe's
    // reader fills them in a lossless DNG (FixBadPixelsConstant): a green
    // pixel from its four diagonal neighbours, a red or blue one from the
    // four of its colour two pixels away, leaving out neighbours that are
    // defective themselves; so the two kinds of file agree on them.
    const bool repair = img.zeroIsBadPixel && img.isCfa && img.cfaRows == 2 && img.cfaCols == 2;
    const auto isGreen = [&](uint32_t ay, uint32_t ax) {
        return img.planeColor[img.cfaPattern[(ay % 2) * 2 + ax % 2]] == kGreen;
    };
    std::vector<uint32_t> fixed(ah, 0);
    parallelFor(ah, threads, [&](size_t row) {
        const uint32_t ay = static_cast<uint32_t>(row);
        const double* blackRow = &img.blackLevel[static_cast<size_t>(ay % img.blackRows) * img.blackCols];
        for (uint32_t ax = 0; ax < aw; ++ax) {
            double value = sample(ay, ax);
            if (repair && value == 0) {
                static const int diagonal[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
                static const int straight[4][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}};
                const int(*offsets)[2] = isGreen(ay, ax) ? diagonal : straight;
                uint32_t total = 0, count = 0;
                for (int k = 0; k < 4; ++k) {
                    const int64_t y = static_cast<int64_t>(ay) + offsets[k][0], x = static_cast<int64_t>(ax) + offsets[k][1];
                    if (y < 0 || x < 0 || y >= ah || x >= aw) continue;
                    const uint16_t v = sample(static_cast<uint32_t>(y), static_cast<uint32_t>(x));
                    if (v != 0) {
                        total += v;
                        ++count;
                    }
                }
                if (count) {
                    value = (total + count / 2) / count;  // Adobe's rounding
                    ++fixed[row];
                }
            }
            const double black = blackRow[ax % img.blackCols];
            const double linear = (value - black) * scale;
            out[static_cast<size_t>(ay) * aw + ax] =
                static_cast<uint16_t>(std::lround(std::clamp(linear, 0.0, 1.0) * 65535.0));
        }
    });
    if (repaired) {
        *repaired = 0;
        for (uint32_t n : fixed) *repaired += n;
    }
    return out;
}

LossyEncoding encodeForLossyJxl(const RawImage& img, uint32_t maxTile, unsigned threads) {
    std::string why;
    if (!lossyJxlSupported(img, &why)) throw std::invalid_argument("lossy JPEG XL: " + why);
    LossyEncoding e;
    e.activeWidth = img.activeRight - img.activeLeft;
    e.activeHeight = img.activeBottom - img.activeTop;
    e.rowFactor = img.isCfa ? img.cfaRows : 1;
    e.columnFactor = img.isCfa ? img.cfaCols : 1;
    const Axis across = planAxis(e.activeWidth, e.columnFactor, maxTile);
    const Axis down = planAxis(e.activeHeight, e.rowFactor, maxTile);
    e.width = across.padded;
    e.height = down.padded;
    e.padLeft = across.pad;
    e.padTop = down.pad;
    e.tileWidth = across.tile;
    e.tileHeight = down.tile;

    const std::vector<uint16_t> linear = linearActiveArea(img, threads, &e.repairedPixels);

    // The range in use, all of it, but never less than 256 steps. (Adobe's
    // SDK leaves out the brightest and darkest hundred-thousandth of the
    // samples, and so turns the brightest specks of a picture, blown
    // highlights included, into grey: 161 samples of the Sony NEX-5N file
    // lose up to 56 % of their value that way. The precision that buys is
    // far below what the coder takes away.)
    {
        uint32_t lower = 65535, upper = 0;
        for (uint16_t v : linear) {
            lower = std::min<uint32_t>(lower, v);
            upper = std::max<uint32_t>(upper, v);
        }
        if (upper < lower + 255) {
            upper = std::min<uint32_t>(65535, lower + 255);
            lower = upper - 255;
        }
        e.lower = lower / 65535.0;
        e.upper = upper / 65535.0;
    }

    // Linear value -> stored value, for all 65536 linear values.
    std::vector<uint16_t> table(65536);
    for (uint32_t v = 0; v < 65536; ++v) {
        const double t = (v / 65535.0 - e.lower) / (e.upper - e.lower);
        table[v] = static_cast<uint16_t>(std::lround(lossyCurveEncode(t) * 65535.0));
    }

    e.samples.resize(static_cast<size_t>(e.width) * e.height);
    parallelFor(e.height, threads, [&](size_t row) {
        const uint32_t ay = sourceOf(static_cast<int64_t>(row) - e.padTop, e.activeHeight, e.rowFactor);
        const uint16_t* source = &linear[static_cast<size_t>(ay) * e.activeWidth];
        uint16_t* dst = &e.samples[row * e.width];
        for (uint32_t x = 0; x < e.width; ++x)
            dst[x] = table[source[sourceOf(static_cast<int64_t>(x) - e.padLeft, e.activeWidth, e.columnFactor)]];
    });
    return e;
}

std::vector<double> LossyEncoding::polynomial() const {
    const double range = upper - lower;
    return {lower, kS * range, 0.0, (1.0 - kS) * range};
}

std::vector<uint8_t> LossyEncoding::opcodeList2() const {
    std::vector<uint8_t> out;
    const auto put32 = [&](uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
    };
    const auto putDouble = [&](double d) {
        uint64_t bits;
        static_assert(sizeof bits == sizeof d, "double is 64 bits");
        std::memcpy(&bits, &d, sizeof bits);
        for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(bits >> shift));
    };
    const std::vector<double> c = polynomial();
    put32(1);           // one opcode in the list
    put32(8);           // MapPolynomial
    put32(0x01030000);  // introduced in DNG 1.3
    put32(0);           // not optional: without it the picture is wrong
    put32(16 + 16 + 4 + 8 * static_cast<uint32_t>(c.size()));
    // The area, in the coordinates of the active area: all of it.
    put32(0);
    put32(0);
    put32(activeHeight);
    put32(activeWidth);
    put32(0);  // first plane
    put32(1);  // planes
    put32(1);  // row pitch
    put32(1);  // column pitch
    put32(static_cast<uint32_t>(c.size() - 1));  // degree
    for (double v : c) putDouble(v);
    return out;
}

namespace {

// Which row (column) of the frame a stored row (column) is, fields being
// whole: field f holds lines f, f + n, f + 2n ...
inline uint32_t frameLine(uint32_t stored, uint32_t field, uint32_t factor) {
    return (stored % field) * factor + stored / field;
}
inline uint32_t storedLine(uint32_t line, uint32_t field, uint32_t factor) {
    return (line % factor) * field + line / factor;
}

}  // namespace

std::vector<std::vector<uint8_t>> compressLossyTiles(const LossyEncoding& e, float distance, int effort,
                                                     unsigned threads) {
    const uint32_t fieldW = e.width / e.columnFactor, fieldH = e.height / e.rowFactor;
    const uint32_t across = e.width / e.tileWidth, down = e.height / e.tileHeight;
    std::vector<std::vector<uint8_t>> tiles(static_cast<size_t>(across) * down);
    parallelFor(tiles.size(), threads, [&](size_t index) {
        const uint32_t tx = static_cast<uint32_t>(index % across) * e.tileWidth;
        const uint32_t ty = static_cast<uint32_t>(index / across) * e.tileHeight;
        std::vector<uint16_t> buffer(static_cast<size_t>(e.tileWidth) * e.tileHeight);
        for (uint32_t y = 0; y < e.tileHeight; ++y) {
            const uint16_t* source = &e.samples[static_cast<size_t>(frameLine(ty + y, fieldH, e.rowFactor)) * e.width];
            uint16_t* dst = &buffer[static_cast<size_t>(y) * e.tileWidth];
            for (uint32_t x = 0; x < e.tileWidth; ++x) dst[x] = source[frameLine(tx + x, fieldW, e.columnFactor)];
        }
        tiles[index] = encodeJxlLossy(buffer.data(), e.tileWidth, e.tileHeight, distance, effort);
    });
    return tiles;
}

namespace {

// For each move and each field (colour of the mosaic), the squared error
// against the moved values and, over the same pixels, against the values in
// place. Field by field, because a fault may concern one field only (one
// colour stored a line off, two colours exchanged).
void measureAlignment(const LossyEncoding& e, const std::vector<uint16_t>& stored, LossyErrors& r) {
    struct Move {
        int dy, dx;
        std::string name;
    };
    const int fr = static_cast<int>(e.rowFactor), fc = static_cast<int>(e.columnFactor);
    std::vector<Move> moves = {{0, 1, "one pixel right"}, {0, -1, "one pixel left"}, {1, 0, "one pixel down"},
                               {-1, 0, "one pixel up"}, {1, 1, "one pixel diagonally"}, {-1, -1, "one pixel diagonally back"},
                               {1, -1, "one pixel down and left"}, {-1, 1, "one pixel up and right"}};
    if (fc > 1) {
        moves.push_back({0, fc, "one period right"});
        moves.push_back({0, -fc, "one period left"});
    }
    if (fr > 1) {
        moves.push_back({fr, 0, "one period down"});
        moves.push_back({-fr, 0, "one period up"});
    }
    const int y0 = static_cast<int>(e.padTop), y1 = y0 + static_cast<int>(e.activeHeight);
    const int x0 = static_cast<int>(e.padLeft), x1 = x0 + static_cast<int>(e.activeWidth);
    const size_t fields = static_cast<size_t>(fr) * fc;
    std::vector<double> own(moves.size() * fields, 0.0), moved(moves.size() * fields, 0.0);
    parallelFor(moves.size(), 0, [&](size_t m) {
        const int dy = moves[m].dy, dx = moves[m].dx;
        double* a = &own[m * fields];
        double* b = &moved[m * fields];
        std::vector<double> rowA(fields), rowB(fields);
        for (int y = std::max(y0, y0 - dy); y < std::min(y1, y1 - dy); ++y) {
            const uint16_t* s = &stored[static_cast<size_t>(y) * e.width];
            const uint16_t* x = &e.samples[static_cast<size_t>(y) * e.width];
            const uint16_t* xm = &e.samples[static_cast<size_t>(y + dy) * e.width + dx];
            const size_t fieldRow = static_cast<size_t>(y % fr) * fc;
            std::fill(rowA.begin(), rowA.end(), 0.0);
            std::fill(rowB.begin(), rowB.end(), 0.0);
            for (int c = std::max(x0, x0 - dx); c < std::min(x1, x1 - dx); ++c) {
                const double d0 = static_cast<double>(s[c]) - x[c], d1 = static_cast<double>(s[c]) - xm[c];
                const size_t f = fieldRow + static_cast<size_t>(c % fc);
                rowA[f] += d0 * d0;
                rowB[f] += d1 * d1;
            }
            for (size_t f = 0; f < fields; ++f) {
                a[f] += rowA[f];
                b[f] += rowB[f];
            }
        }
    });
    r.alignment = 1e9;
    for (size_t m = 0; m < moves.size(); ++m)
        for (size_t f = 0; f < fields; ++f) {
            const double o = own[m * fields + f], v = moved[m * fields + f];
            // Values that are equal everywhere (a flat picture) cannot be told apart.
            if (v == o) continue;
            const double margin = o > 0 ? (v - o) / o : 1e9;
            if (margin < r.alignment) {
                r.alignment = margin;
                r.closestMove = moves[m].name;
                if (fields > 1) r.closestMove += " (colour field " + std::to_string(f + 1) + ")";
            }
        }
}

}  // namespace

LossyErrors measureLossyErrors(const LossyEncoding& e, const std::vector<uint16_t>& stored) {
    LossyErrors r;
    if (stored.size() != e.samples.size()) {
        r.rms = r.worstTileRms = r.p999 = r.max = 1.0;
        return r;
    }
    const uint32_t fieldW = e.width / e.columnFactor, fieldH = e.height / e.rowFactor;
    const uint32_t across = e.width / e.tileWidth, down = e.height / e.tileHeight;
    std::vector<double> tileSum(static_cast<size_t>(across) * down, 0.0);
    constexpr uint32_t kBlock = 32;
    const uint32_t blocksAcross = (e.width + kBlock - 1) / kBlock, blocksDown = (e.height + kBlock - 1) / kBlock;
    std::vector<double> blockSum(static_cast<size_t>(blocksAcross) * blocksDown, 0.0);
    std::vector<uint32_t> blockCount(blockSum.size(), 0);
    std::vector<uint64_t> tileCount(tileSum.size(), 0);
    std::vector<uint64_t> histogram(65536, 0);
    double sum = 0, sumSquares = 0;
    uint64_t count = 0;
    for (uint32_t y = e.padTop; y < e.padTop + e.activeHeight; ++y) {
        const uint32_t sy = storedLine(y, fieldH, e.rowFactor);
        const size_t tileRow = static_cast<size_t>(sy / e.tileHeight) * across;
        const size_t blockRow = static_cast<size_t>(sy / kBlock) * blocksAcross;
        for (uint32_t x = e.padLeft; x < e.padLeft + e.activeWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * e.width + x;
            const int difference = static_cast<int>(stored[i]) - static_cast<int>(e.samples[i]);
            const double d = difference;
            const uint32_t sx = storedLine(x, fieldW, e.columnFactor);
            const size_t tile = tileRow + sx / e.tileWidth;
            const size_t block = blockRow + sx / kBlock;
            blockSum[block] += d;
            ++blockCount[block];
            tileSum[tile] += d * d;
            ++tileCount[tile];
            sum += d;
            sumSquares += d * d;
            ++count;
            ++histogram[static_cast<size_t>(std::abs(difference))];
        }
    }
    constexpr double unit = 1.0 / 65535.0;
    r.mean = count ? sum / count * unit : 0;
    r.rms = count ? std::sqrt(sumSquares / count) * unit : 0;
    for (size_t t = 0; t < tileSum.size(); ++t) {
        if (!tileCount[t]) continue;
        const double tileRms = std::sqrt(tileSum[t] / tileCount[t]) * unit;
        if (tileRms > r.worstTileRms) {
            r.worstTileRms = tileRms;
            r.worstTile = static_cast<uint32_t>(t);
        }
    }
    for (size_t b = 0; b < blockSum.size(); ++b)
        if (blockCount[b] >= kBlock * kBlock / 4)  // blocks mostly in the margin say little
            r.worstBlockMean = std::max(r.worstBlockMean, std::fabs(blockSum[b] / blockCount[b]) * unit);
    measureAlignment(e, stored, r);
    const uint64_t below = count - count / 1000;  // the 99.9th percentile
    uint64_t seen = 0;
    for (size_t v = 0; v < histogram.size(); ++v) {
        if (!histogram[v]) continue;
        if (seen < below && seen + histogram[v] >= below) r.p999 = v * unit;
        seen += histogram[v];
        r.max = v * unit;
    }
    return r;
}

bool lossyCloseEnough(const LossyErrors& errors, float distance, std::string& why) {
    const double d = static_cast<double>(distance);
    const double rmsLimit = 0.025 * d, tileLimit = 0.03 * d, blockLimit = 0.01 * d + 0.001,
                 meanLimit = 0.001 * d + 0.0001;
    char text[240];
    if (errors.rms > rmsLimit) {
        std::snprintf(text, sizeof text,
                      "the stored values are %.3f %% off (rms), more than coding at distance %g explains (%.3f %%)",
                      errors.rms * 100, d, rmsLimit * 100);
    } else if (errors.worstTileRms > tileLimit) {
        std::snprintf(text, sizeof text,
                      "tile %u is %.3f %% off (rms), more than coding at distance %g explains (%.3f %%)",
                      errors.worstTile, errors.worstTileRms * 100, d, tileLimit * 100);
    } else if (errors.worstBlockMean > blockLimit) {
        std::snprintf(text, sizeof text,
                      "a local average is %.3f %% off, more than coding at distance %g explains (%.3f %%)",
                      errors.worstBlockMean * 100, d, blockLimit * 100);
    } else if (errors.alignment <= 0) {
        std::snprintf(text, sizeof text,
                      "the stored values fit their place no better than %s from it: they are not where they belong",
                      errors.closestMove.c_str());
    } else if (std::fabs(errors.mean) > meanLimit) {
        std::snprintf(text, sizeof text, "the stored values are shifted by %.4f %% on average (at most %.4f %%)",
                      errors.mean * 100, meanLimit * 100);
    } else {
        return true;
    }
    why = text;
    return false;
}

std::string describe(const LossyErrors& e) {
    char text[160];
    std::snprintf(text, sizeof text, "rms %.3f %%, worst tile %.3f %%, 99.9 %% within %.2f %%", e.rms * 100,
                  e.worstTileRms * 100, e.p999 * 100);
    return text;
}

}  // namespace dngconv
