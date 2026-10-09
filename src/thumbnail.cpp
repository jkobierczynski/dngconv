// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "thumbnail.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace dngconv {

namespace {

// Linear sRGB -> XYZ (D65).
const double kXyzFromSrgb[3][3] = {
    {0.4124564, 0.3575761, 0.1804375},
    {0.2126729, 0.7151522, 0.0721750},
    {0.0193339, 0.1191920, 0.9503041},
};

struct ColourTransform {
    unsigned planes = 3;
    std::array<double, 4> whiteBalance{1, 1, 1, 1};  // multiplier per plane
    double rgbFromCamera[3][4] = {};                 // applied after white balance
};

bool invert3x3(const double m[3][3], double inv[3][3]) {
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-12) return false;
    inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return true;
}

// Builds the camera -> sRGB conversion from the DNG colour description.
//
// The colour matrix maps XYZ to camera values. Chained with sRGB -> XYZ it
// tells what the camera records for each sRGB primary. Scaling every plane so
// that sRGB white becomes (1, 1, ...) and inverting gives a matrix that turns
// white-balanced camera values into sRGB; with three planes this is a plain
// inverse, with four (CMYG sensors) a least-squares fit.
ColourTransform colourTransform(const RawImage& img) {
    ColourTransform t;
    t.planes = img.colorPlanes;
    if (t.planes < 3) {
        // Monochrome: the one plane feeds all three channels.
        for (int c = 0; c < 3; ++c) t.rgbFromCamera[c][0] = 1.0;
        return t;
    }
    if (!img.hasColorMatrix) {
        // Nothing known about the colours: pass the first three planes through.
        for (int c = 0; c < 3; ++c) t.rgbFromCamera[c][c] = 1.0;
        return t;
    }

    double camFromRgb[4][3] = {};
    std::array<double, 4> daylight{1, 1, 1, 1};
    for (unsigned p = 0; p < t.planes; ++p) {
        double sum = 0;
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) camFromRgb[p][c] += img.colorMatrix[p][k] * kXyzFromSrgb[k][c];
            sum += camFromRgb[p][c];
        }
        if (std::fabs(sum) < 1e-9) sum = 1.0;
        for (int c = 0; c < 3; ++c) camFromRgb[p][c] /= sum;
        daylight[p] = sum;  // what the camera records for D65 white
    }

    // Normal equations: rgbFromCamera = (A^T A)^-1 A^T, with A = camFromRgb.
    double ata[3][3] = {}, ataInv[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (unsigned p = 0; p < t.planes; ++p) ata[i][j] += camFromRgb[p][i] * camFromRgb[p][j];
    if (invert3x3(ata, ataInv)) {
        for (int c = 0; c < 3; ++c)
            for (unsigned p = 0; p < t.planes; ++p)
                for (int k = 0; k < 3; ++k) t.rgbFromCamera[c][p] += ataInv[c][k] * camFromRgb[p][k];
    } else {
        for (int c = 0; c < 3; ++c) t.rgbFromCamera[c][c] = 1.0;
    }

    // White balance: as shot when known, otherwise daylight.
    std::array<double, 4> neutral = daylight;
    if (img.hasAsShotNeutral) neutral = img.asShotNeutral;
    double largest = 0;
    for (unsigned p = 0; p < t.planes; ++p) largest = std::max(largest, neutral[p]);
    for (unsigned p = 0; p < t.planes; ++p)
        t.whiteBalance[p] = neutral[p] > 1e-6 ? largest / neutral[p] : 1.0;
    return t;
}

// Upper limit for automatic brightening: two stops. More than that turns a
// night shot into grey fog.
constexpr double kMaxGain = 4.0;

inline uint8_t srgbEncode(double linear) {
    if (!(linear > 0.0)) return 0;
    if (linear >= 1.0) return 255;
    const double v = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::lround(v * 255.0));
}

}  // namespace

RgbImage renderThumbnail(const RawImage& img, const ThumbnailOptions& options) {
    RgbImage out;
    const uint32_t spp = img.samplesPerPixel;
    if (img.pixels.size() != img.sampleCount() || img.cropWidth == 0 || img.cropHeight == 0 ||
        options.maxSize == 0 || spp == 0 || img.blackRows == 0 || img.blackCols == 0 ||
        img.blackLevel.size() != static_cast<size_t>(img.blackRows) * img.blackCols * spp ||
        (img.isCfa && (spp != 1 || img.cfaPattern.size() !=
                                       static_cast<size_t>(img.cfaRows) * img.cfaCols)))
        return out;

    // The area to show, in frame coordinates.
    const uint32_t x0 = img.activeLeft + img.cropLeft, y0 = img.activeTop + img.cropTop;
    const uint32_t w = img.cropWidth, h = img.cropHeight;
    if (x0 + w > img.width || y0 + h > img.height) return out;

    // Every thumbnail pixel must see each colour of the mosaic at least once,
    // so a cell is never smaller than one period of the pattern.
    const uint32_t periodX = img.isCfa ? img.cfaCols : 1, periodY = img.isCfa ? img.cfaRows : 1;
    if (periodX == 0 || periodY == 0 || w < periodX || h < periodY) return out;

    const double displayW = w * img.scaleH, displayH = h * img.scaleV;
    const double shrink = std::min(1.0, options.maxSize / std::max(displayW, displayH));
    const uint32_t tw = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(displayW * shrink)), 1, w / periodX);
    const uint32_t th = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(displayH * shrink)), 1, h / periodY);

    const ColourTransform colour = colourTransform(img);
    const unsigned planes = std::min<unsigned>(colour.planes, 4);

    // ---- block averages of black-subtracted samples, per colour plane --------
    std::vector<double> sum(static_cast<size_t>(tw) * th * planes, 0.0);
    std::vector<uint32_t> count(static_cast<size_t>(tw) * th * planes, 0);
    std::vector<uint32_t> cellOfColumn(w);
    for (uint32_t x = 0; x < w; ++x)
        cellOfColumn[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * tw / w);

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t fy = y0 + y;
        const uint32_t ty = static_cast<uint32_t>(static_cast<uint64_t>(y) * th / h);
        const uint16_t* row = &img.pixels[(static_cast<size_t>(fy) * img.width + x0) * spp];
        const uint32_t ay = fy - img.activeTop;  // pattern origin is the active area
        const double* blackRow = &img.blackLevel[static_cast<size_t>(ay % img.blackRows) * img.blackCols * spp];
        const uint8_t* cfaRow = img.isCfa ? &img.cfaPattern[static_cast<size_t>(ay % img.cfaRows) * img.cfaCols] : nullptr;
        double* sumRow = &sum[static_cast<size_t>(ty) * tw * planes];
        uint32_t* countRow = &count[static_cast<size_t>(ty) * tw * planes];
        const uint32_t ax0 = x0 - img.activeLeft;

        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t ax = ax0 + x;
            const size_t cell = static_cast<size_t>(cellOfColumn[x]) * planes;
            const double* black = &blackRow[static_cast<size_t>(ax % img.blackCols) * spp];
            if (img.isCfa) {
                const unsigned p = cfaRow[ax % img.cfaCols];
                if (p < planes) {
                    sumRow[cell + p] += row[x] - black[0];
                    ++countRow[cell + p];
                }
            } else {
                for (unsigned p = 0; p < planes && p < spp; ++p) {
                    sumRow[cell + p] += row[static_cast<size_t>(x) * spp + p] - black[p];
                    ++countRow[cell + p];
                }
            }
        }
    }

    // ---- camera values -> linear sRGB ------------------------------------------
    double maxBlack = 0;
    for (double b : img.blackLevel) maxBlack = std::max(maxBlack, b);
    std::array<double, 4> range{};
    double commonWhite = img.whiteLevel[0];
    for (unsigned p = 1; p < planes; ++p) commonWhite = std::min(commonWhite, img.whiteLevel[p]);
    for (unsigned p = 0; p < planes; ++p) {
        // A mosaic has one white level for all planes (the writer stores the lowest).
        const double white = img.isCfa ? commonWhite : img.whiteLevel[p];
        range[p] = std::max(1.0, white - maxBlack);
    }

    std::vector<float> linear(static_cast<size_t>(tw) * th * 3);
    for (size_t i = 0; i < static_cast<size_t>(tw) * th; ++i) {
        double cam[4] = {0, 0, 0, 0};
        for (unsigned p = 0; p < planes; ++p) {
            const double mean = count[i * planes + p] ? sum[i * planes + p] / count[i * planes + p] : 0.0;
            // Clip after white balance: a saturated sensor then renders as white.
            cam[p] = std::clamp(mean / range[p] * colour.whiteBalance[p], 0.0, 1.0);
        }
        for (int c = 0; c < 3; ++c) {
            double v = 0;
            for (unsigned p = 0; p < planes; ++p) v += colour.rgbFromCamera[c][p] * cam[p];
            linear[i * 3 + c] = static_cast<float>(std::clamp(v, 0.0, 1.0));
        }
    }

    // ---- exposure ---------------------------------------------------------------
    double gain = 1.0;
    if (options.autoExposure && tw * th >= 16) {
        std::vector<float> brightest(static_cast<size_t>(tw) * th);
        for (size_t i = 0; i < brightest.size(); ++i)
            brightest[i] = std::max({linear[i * 3], linear[i * 3 + 1], linear[i * 3 + 2]});
        const size_t k = brightest.size() - 1 - brightest.size() / 100;
        std::nth_element(brightest.begin(), brightest.begin() + k, brightest.end());
        const double level = brightest[k];
        // Never darken, and do not turn a black frame into noise.
        if (level > 1e-4) gain = std::clamp(1.0 / level, 1.0, kMaxGain);
    }

    out.width = tw;
    out.height = th;
    out.pixels.resize(linear.size());
    for (size_t i = 0; i < linear.size(); ++i) out.pixels[i] = srgbEncode(linear[i] * gain);
    return out;
}

}  // namespace dngconv
