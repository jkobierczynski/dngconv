// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "render.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "parallel.hpp"

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

// The tone curve, fitted to what cameras do in their own JPEGs (the median
// of thirteen cameras from nine makers, measured against this renderer's
// linear output). Two factors, both rising from black to white:
//
//   lift   x (1 + a) / (1 + a x)      mid-tones about one stop up, highlights
//                                     compressed to make room for them
//   toe    (x^2 + f s^2) / (x^2 + s^2)  shadows below about s held down to the
//                                     fraction f, which gives the contrast
//
// Cameras differ from this by up to a stop either way in the mid-tones.
constexpr double kToneLift = 1.5;
constexpr double kToeStart = 0.025;
constexpr double kToeFloor = 0.3;

inline double srgbFromLinear(double linear) {
    if (!(linear > 0.0)) return 0.0;
    if (linear >= 1.0) return 1.0;
    return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double toneCurve(double x) {
    x = std::clamp(x, 0.0, 1.0);
    const double s2 = kToeStart * kToeStart;
    const auto toe = [&](double v) { return (v * v + kToeFloor * s2) / (v * v + s2); };
    return x * (1.0 + kToneLift) / (1.0 + kToneLift * x) * toe(x) / toe(1.0);
}

// Gamma-encoded output (0..1) for a linear input, with or without the curve.
double encoded(double linear, bool withCurve) {
    linear = std::clamp(linear, 0.0, 1.0);
    return srgbFromLinear(withCurve ? toneCurve(linear) : linear);
}

// Where the picture is taken from and how large it gets.
struct Layout {
    uint32_t x0 = 0, y0 = 0;  // first frame column and row used
    uint32_t bx = 1, by = 1;  // sensor pixels per cell, whole mosaic periods
    uint32_t iw = 0, ih = 0;  // cells
    uint32_t tw = 0, th = 0;  // the result
};

// The smallest block of the mosaic that can stand for one pixel: wherever it
// is laid on its own grid it holds every colour plane, each as often as in
// any other position. Two by two for a Bayer sensor; three by three for
// X-Trans, whose pattern repeats only every six pixels.
void mosaicCell(const RawImage& img, uint32_t& cellW, uint32_t& cellH) {
    cellW = img.cfaCols;
    cellH = img.cfaRows;
    uint32_t bestArea = cellW * cellH;
    for (uint32_t h = 1; h <= img.cfaRows; ++h) {
        if (img.cfaRows % h) continue;
        for (uint32_t w = 1; w <= img.cfaCols; ++w) {
            if (img.cfaCols % w || w * h >= bestArea) continue;
            bool same = true;
            std::array<unsigned, 4> first{};
            for (uint32_t by = 0; by < img.cfaRows && same; by += h)
                for (uint32_t bx = 0; bx < img.cfaCols && same; bx += w) {
                    std::array<unsigned, 4> count{};
                    for (uint32_t y = 0; y < h; ++y)
                        for (uint32_t x = 0; x < w; ++x) {
                            const uint8_t plane = img.cfaPattern[(by + y) * img.cfaCols + bx + x];
                            if (plane < 4) ++count[plane];
                        }
                    if (bx == 0 && by == 0) {
                        first = count;
                        for (uint32_t p = 0; p < std::min<uint32_t>(img.colorPlanes, 4); ++p)
                            if (count[p] == 0) same = false;
                    } else if (count != first) {
                        same = false;
                    }
                }
            if (same) {
                cellW = w;
                cellH = h;
                bestArea = w * h;
            }
        }
    }
}

bool layoutFor(const RawImage& img, uint32_t maxSize, Layout& l, bool needPixels) {
    const uint32_t spp = img.samplesPerPixel;
    if ((needPixels && img.pixels.size() != img.sampleCount()) || img.cropWidth == 0 || img.cropHeight == 0 ||
        spp == 0 || img.blackRows == 0 || img.blackCols == 0 ||
        img.blackLevel.size() != static_cast<size_t>(img.blackRows) * img.blackCols * spp ||
        (img.isCfa && (spp != 1 || img.cfaRows == 0 || img.cfaCols == 0 ||
                       img.cfaPattern.size() != static_cast<size_t>(img.cfaRows) * img.cfaCols)) ||
        !(img.scaleH > 0) || !(img.scaleV > 0))
        return false;

    // The area to show, in frame coordinates.
    const uint32_t cropX0 = img.activeLeft + img.cropLeft, cropY0 = img.activeTop + img.cropTop;
    const uint32_t cropW = img.cropWidth, cropH = img.cropHeight;
    if (cropX0 > img.width || cropY0 > img.height || cropW > img.width - cropX0 ||
        cropH > img.height - cropY0)
        return false;

    // Every pixel of the result must see each colour of the mosaic equally
    // often, so a cell is a whole number of the mosaic's smallest blocks.
    uint32_t periodX = 1, periodY = 1;
    if (img.isCfa) mosaicCell(img, periodX, periodY);
    // Cells start on the grid of those blocks, which begins at the corner of
    // the active area; a crop that starts off the grid loses a pixel or two.
    uint32_t x0 = cropX0, y0 = cropY0, w = cropW, h = cropH;
    const uint32_t skipX = (periodX - (x0 - img.activeLeft) % periodX) % periodX;
    const uint32_t skipY = (periodY - (y0 - img.activeTop) % periodY) % periodY;
    if (w < skipX + periodX || h < skipY + periodY) return false;
    x0 += skipX;
    y0 += skipY;
    w -= skipX;
    h -= skipY;
    const uint32_t cellsX = w / periodX, cellsY = h / periodY;

    const double displayW = w * img.scaleH, displayH = h * img.scaleV;
    double shrink = std::min({1.0, cellsX / displayW, cellsY / displayH});
    if (maxSize) shrink = std::min(shrink, maxSize / std::max(displayW, displayH));
    l.tw = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(displayW * shrink)), 1, cellsX);
    l.th = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(displayH * shrink)), 1, cellsY);

    // The largest cells that still give at least the size wanted; what is
    // left over is taken off both edges alike.
    l.bx = periodX * (cellsX / l.tw);
    l.by = periodY * (cellsY / l.th);
    l.iw = w / l.bx;
    l.ih = h / l.by;
    l.x0 = x0 + (w - l.iw * l.bx) / 2 / periodX * periodX;
    l.y0 = y0 + (h - l.ih * l.by) / 2 / periodY * periodY;
    return true;
}

// Averages a w x h picture of `channels` floats down to tw x th: every
// output pixel is the mean of the area it covers (edges count by the
// fraction covered).
std::vector<float> shrinkArea(const std::vector<float>& src, uint32_t w, uint32_t h, uint32_t tw,
                              uint32_t th, unsigned threads) {
    if (w == tw && h == th) return src;
    struct Span {
        uint32_t first;
        std::vector<float> weight;
    };
    const auto spans = [](uint32_t from, uint32_t to) {
        std::vector<Span> out(to);
        const double step = static_cast<double>(from) / to;
        for (uint32_t i = 0; i < to; ++i) {
            const double a = i * step, b = std::min<double>((i + 1) * step, from);
            Span& s = out[i];
            s.first = static_cast<uint32_t>(a);
            for (uint32_t k = s.first; k < from && k < b; ++k)
                s.weight.push_back(static_cast<float>((std::min<double>(k + 1, b) - std::max<double>(k, a)) / step));
            if (s.weight.empty()) s.weight.push_back(1.0f);
        }
        return out;
    };
    const std::vector<Span> sx = spans(w, tw), sy = spans(h, th);

    std::vector<float> horizontal(static_cast<size_t>(tw) * h * 3);
    parallelFor(h, threads, [&](size_t y) {
        const float* in = &src[y * w * 3];
        float* out = &horizontal[y * tw * 3];
        for (uint32_t x = 0; x < tw; ++x) {
            float acc[3] = {0, 0, 0};
            for (size_t k = 0; k < sx[x].weight.size(); ++k)
                for (int c = 0; c < 3; ++c) acc[c] += sx[x].weight[k] * in[(sx[x].first + k) * 3 + static_cast<size_t>(c)];
            for (int c = 0; c < 3; ++c) out[x * 3 + static_cast<uint32_t>(c)] = acc[c];
        }
    });
    std::vector<float> out(static_cast<size_t>(tw) * th * 3, 0.0f);
    parallelFor(th, threads, [&](size_t y) {
        float* dst = &out[y * tw * 3];
        for (size_t k = 0; k < sy[y].weight.size(); ++k) {
            const float* in = &horizontal[(sy[y].first + k) * tw * 3];
            const float weight = sy[y].weight[k];
            for (size_t i = 0; i < static_cast<size_t>(tw) * 3; ++i) dst[i] += weight * in[i];
        }
    });
    return out;
}

// Applies the lens corrections the way a DNG reader applies the opcodes:
// first the gain against vignetting, laid out over the picture as stored,
// then the warp that straightens it. Radii are counted from the optical
// centre, in units of the distance to the farthest corner of the active
// area; `scaleX`/`scaleY` and `offsetX`/`offsetY` turn a pixel of the picture
// into a position in the active area.
void applyLens(std::vector<float>& picture, uint32_t w, uint32_t h, const LensOpcodes& lens,
               double activeW, double activeH, double offsetX, double offsetY, double scaleX,
               double scaleY, unsigned threads) {
    const double cx = lens.centreX * activeW, cy = lens.centreY * activeH;
    const double unit = std::hypot(std::max(cx, activeW - cx), std::max(cy, activeH - cy));
    if (!(unit > 0)) return;

    if (lens.vignetting) {
        parallelFor(h, threads, [&](size_t y) {
            const double ay = offsetY + (static_cast<double>(y) + 0.5) * scaleY - cy;
            for (uint32_t x = 0; x < w; ++x) {
                const double ax = offsetX + (x + 0.5) * scaleX - cx;
                const float gain = static_cast<float>(evaluateGain(lens.gain, std::hypot(ax, ay) / unit));
                float* p = &picture[(y * w + x) * 3];
                for (int c = 0; c < 3; ++c) p[c] *= gain;
            }
        });
    }
    if (!lens.distortion || lens.planes == 0) return;

    // Green stands for all three planes.
    const std::array<double, 4>& k = lens.warp[lens.planes == 3 ? 1 : 0];
    const std::vector<float> source = picture;
    parallelFor(h, threads, [&](size_t y) {
        const double ay = offsetY + (static_cast<double>(y) + 0.5) * scaleY - cy;
        for (uint32_t x = 0; x < w; ++x) {
            const double ax = offsetX + (x + 0.5) * scaleX - cx;
            const double ratio = evaluateWarp(k, std::hypot(ax, ay) / unit);
            // Where this pixel comes from, back in picture coordinates.
            const double sx = (cx + ax * ratio - offsetX) / scaleX - 0.5;
            const double sy = (cy + ay * ratio - offsetY) / scaleY - 0.5;
            const double fx = std::clamp(sx, 0.0, static_cast<double>(w - 1));
            const double fy = std::clamp(sy, 0.0, static_cast<double>(h - 1));
            const uint32_t ix = std::min<uint32_t>(static_cast<uint32_t>(fx), w > 1 ? w - 2 : 0);
            const uint32_t iy = std::min<uint32_t>(static_cast<uint32_t>(fy), h > 1 ? h - 2 : 0);
            const float tx = static_cast<float>(fx - ix), ty = static_cast<float>(fy - iy);
            const uint32_t ix1 = std::min(ix + 1, w - 1), iy1 = std::min(iy + 1, h - 1);
            const float* p00 = &source[(static_cast<size_t>(iy) * w + ix) * 3];
            const float* p01 = &source[(static_cast<size_t>(iy) * w + ix1) * 3];
            const float* p10 = &source[(static_cast<size_t>(iy1) * w + ix) * 3];
            const float* p11 = &source[(static_cast<size_t>(iy1) * w + ix1) * 3];
            float* dst = &picture[(y * w + x) * 3];
            for (int c = 0; c < 3; ++c)
                dst[c] = (1 - ty) * ((1 - tx) * p00[c] + tx * p01[c]) + ty * ((1 - tx) * p10[c] + tx * p11[c]);
        }
    });
}

}  // namespace

double renderToneCurve(double linear) { return toneCurve(linear); }

bool renderedSize(const RawImage& img, uint32_t maxSize, uint32_t& width, uint32_t& height) {
    Layout l;
    if (!layoutFor(img, maxSize, l, false)) return false;
    width = l.tw;
    height = l.th;
    return true;
}

RgbImage renderPicture(const RawImage& img, const RenderOptions& options) {
    RgbImage out;
    Layout l;
    if (!layoutFor(img, options.maxSize, l, true)) return out;
    const uint32_t spp = img.samplesPerPixel;

    const ColourTransform colour = colourTransform(img);
    const unsigned planes = std::min<unsigned>(colour.planes, 4);

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

    // ---- cell averages of black-subtracted samples, turned into linear sRGB ------
    std::vector<uint32_t> cellOfColumn(static_cast<size_t>(l.iw) * l.bx);
    for (size_t x = 0; x < cellOfColumn.size(); ++x) cellOfColumn[x] = static_cast<uint32_t>(x / l.bx);

    std::vector<float> cells(static_cast<size_t>(l.iw) * l.ih * 3);
    parallelFor(l.ih, options.threads, [&](size_t cellY) {
        std::vector<double> sum(static_cast<size_t>(l.iw) * planes, 0.0);
        std::vector<uint32_t> count(static_cast<size_t>(l.iw) * planes, 0);
        for (uint32_t dy = 0; dy < l.by; ++dy) {
            const uint32_t fy = l.y0 + static_cast<uint32_t>(cellY) * l.by + dy;
            const uint16_t* row = &img.pixels[(static_cast<size_t>(fy) * img.width + l.x0) * spp];
            const uint32_t ay = fy - img.activeTop;  // pattern origin is the active area
            const double* blackRow = &img.blackLevel[static_cast<size_t>(ay % img.blackRows) * img.blackCols * spp];
            const uint8_t* cfaRow = img.isCfa ? &img.cfaPattern[static_cast<size_t>(ay % img.cfaRows) * img.cfaCols] : nullptr;
            const uint32_t ax0 = l.x0 - img.activeLeft;
            for (size_t x = 0; x < cellOfColumn.size(); ++x) {
                const uint32_t ax = ax0 + static_cast<uint32_t>(x);
                const size_t cell = static_cast<size_t>(cellOfColumn[x]) * planes;
                const double* black = &blackRow[static_cast<size_t>(ax % img.blackCols) * spp];
                if (img.isCfa) {
                    const unsigned p = cfaRow[ax % img.cfaCols];
                    if (p < planes) {
                        sum[cell + p] += row[x] - black[0];
                        ++count[cell + p];
                    }
                } else {
                    for (unsigned p = 0; p < planes && p < spp; ++p) {
                        sum[cell + p] += row[x * spp + p] - black[p];
                        ++count[cell + p];
                    }
                }
            }
        }
        float* dst = &cells[cellY * l.iw * 3];
        for (uint32_t x = 0; x < l.iw; ++x) {
            double cam[4] = {0, 0, 0, 0};
            for (unsigned p = 0; p < planes; ++p) {
                const size_t i = static_cast<size_t>(x) * planes + p;
                const double mean = count[i] ? sum[i] / count[i] : 0.0;
                // Clip after white balance: a saturated sensor then renders as white.
                cam[p] = std::clamp(mean / range[p] * colour.whiteBalance[p], 0.0, 1.0);
            }
            for (int c = 0; c < 3; ++c) {
                double v = 0;
                for (unsigned p = 0; p < planes; ++p) v += colour.rgbFromCamera[c][p] * cam[p];
                dst[x * 3 + static_cast<uint32_t>(c)] = static_cast<float>(std::clamp(v, 0.0, 1.0));
            }
        }
    });

    // ---- to the size asked for --------------------------------------------------
    std::vector<float> linear = shrinkArea(cells, l.iw, l.ih, l.tw, l.th, options.threads);
    const uint32_t tw = l.tw, th = l.th;

    // ---- lens corrections -------------------------------------------------------
    if (options.lens && (options.lens->distortion || options.lens->vignetting)) {
        const double activeW = img.activeRight - img.activeLeft, activeH = img.activeBottom - img.activeTop;
        const double scaleX = static_cast<double>(l.iw) * l.bx / tw, scaleY = static_cast<double>(l.ih) * l.by / th;
        applyLens(linear, tw, th, *options.lens, activeW, activeH,
                  static_cast<double>(l.x0) - img.activeLeft, static_cast<double>(l.y0) - img.activeTop,
                  scaleX, scaleY, options.threads);
    }

    // ---- tone curve and gamma, through a table ------------------------------------
    constexpr int kSteps = 16384;
    std::vector<uint8_t> table(kSteps + 1);
    for (int i = 0; i <= kSteps; ++i)
        table[static_cast<size_t>(i)] = static_cast<uint8_t>(
            std::lround(255.0 * encoded(static_cast<double>(i) / kSteps, options.toneCurve)));

    out.width = tw;
    out.height = th;
    out.pixels.resize(linear.size());
    const float scale = static_cast<float>(kSteps);
    for (size_t i = 0; i < linear.size(); ++i) {
        const float v = linear[i] * scale;
        out.pixels[i] = table[static_cast<size_t>(v <= 0.0f ? 0 : v >= kSteps ? kSteps : static_cast<int>(v + 0.5f))];
    }
    return out;
}

}  // namespace dngconv
