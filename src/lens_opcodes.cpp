// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "lens_opcodes.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "raw_image.hpp"

namespace dngconv {

double RadialCurve::at(double r) const {
    const size_t n = radius.size();
    if (n == 0) return 0.0;
    if (n == 1) return value[0];
    // The segment containing r, or the first or last one for r outside.
    size_t i = static_cast<size_t>(std::upper_bound(radius.begin(), radius.end(), r) - radius.begin());
    i = std::min(std::max<size_t>(i, 1), n - 1);
    const double span = radius[i] - radius[i - 1];
    if (!(span > 0)) return value[i];
    return value[i - 1] + (r - radius[i - 1]) * (value[i] - value[i - 1]) / span;
}

namespace {

constexpr uint32_t kOpcodeWarpRectilinear = 1;
constexpr uint32_t kOpcodeFixVignetteRadial = 3;
constexpr uint32_t kOpcodeVersion = 0x01030000;  // DNG 1.3.0.0
// "Optional": a reader that does not implement the opcode may skip it. The
// raw data is complete without these corrections.
constexpr uint32_t kOpcodeFlags = 1;

constexpr int kSamples = 512;
// Samples outside the picture (the few masked pixels between the picture and
// the edge of the active area) only keep the polynomial from running wild.
constexpr double kOutsideWeight = 0.02;

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void putDouble(std::vector<uint8_t>& out, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(bits >> shift));
}

// Solves the n x n system a * x = b in place (Gaussian elimination with
// partial pivoting). Returns false for a singular matrix.
template <size_t N>
bool solve(std::array<std::array<long double, N>, N>& a, std::array<long double, N>& b) {
    for (size_t col = 0; col < N; ++col) {
        size_t pivot = col;
        for (size_t row = col + 1; row < N; ++row)
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) pivot = row;
        if (std::fabs(a[pivot][col]) < 1e-300L) return false;
        std::swap(a[col], a[pivot]);
        std::swap(b[col], b[pivot]);
        for (size_t row = col + 1; row < N; ++row) {
            const long double f = a[row][col] / a[col][col];
            for (size_t k = col; k < N; ++k) a[row][k] -= f * a[col][k];
            b[row] -= f * b[col];
        }
    }
    for (size_t col = N; col-- > 0;) {
        for (size_t k = col + 1; k < N; ++k) b[col] -= a[col][k] * b[k];
        b[col] /= a[col][col];
    }
    return true;
}

// Weighted least squares for c[0] x^first + c[1] x^(first+1) + ... through
// the points (x, y).
template <size_t N>
bool fitPolynomial(const std::vector<double>& x, const std::vector<double>& y,
                   const std::vector<double>& weight, int first, std::array<double, N>& c) {
    std::array<std::array<long double, N>, N> a{};
    std::array<long double, N> b{};
    for (size_t i = 0; i < x.size(); ++i) {
        std::array<long double, N> basis;
        long double p = std::pow(static_cast<long double>(x[i]), first);
        for (size_t j = 0; j < N; ++j, p *= x[i]) basis[j] = p;
        for (size_t j = 0; j < N; ++j) {
            for (size_t k = 0; k < N; ++k) a[j][k] += weight[i] * basis[j] * basis[k];
            b[j] += weight[i] * basis[j] * y[i];
        }
    }
    if (!solve(a, b)) return false;
    for (size_t j = 0; j < N; ++j) c[j] = static_cast<double>(b[j]);
    return true;
}

}  // namespace

double evaluateWarp(const std::array<double, 4>& k, double r) {
    const double x = r * r;
    return k[0] + x * (k[1] + x * (k[2] + x * k[3]));
}

double evaluateGain(const std::array<double, 5>& k, double r) {
    const double x = r * r;
    return 1.0 + x * (k[0] + x * (k[1] + x * (k[2] + x * (k[3] + x * k[4]))));
}

double frameFillingZoom(const RadialCurve& ratio, double frameWidth, double frameHeight) {
    if (ratio.empty() || !(frameWidth > 0) || !(frameHeight > 0)) return 1.0;
    const double halfW = 0.5 * frameWidth, halfH = 0.5 * frameHeight;
    const double unit = std::hypot(halfW, halfH);

    // Does every point on the edge of the picture, enlarged by z, come from
    // inside the frame? By symmetry one quadrant's edges are enough.
    const auto fits = [&](double z) {
        constexpr int kSteps = 256;
        for (int i = 0; i <= kSteps; ++i) {
            const double t = static_cast<double>(i) / kSteps;
            const double edge[2][2] = {{t * halfW, halfH}, {halfW, t * halfH}};
            for (const auto& p : edge) {
                const double x = z * p[0], y = z * p[1];
                const double f = ratio.at(std::hypot(x, y) / unit);
                if (f * x > halfW * (1 + 1e-12) || f * y > halfH * (1 + 1e-12)) return false;
            }
        }
        return true;
    };

    double lo = 0.5, hi = 2.0;
    if (!fits(lo)) return 1.0;  // not a curve this makes sense for
    if (fits(hi)) return 1.0;
    for (int i = 0; i < 50; ++i) {
        const double mid = 0.5 * (lo + hi);
        (fits(mid) ? lo : hi) = mid;
    }
    return lo;
}

std::string LensOpcodes::summary() const {
    std::string out;
    const auto add = [&](bool on, const char* name) {
        if (!on) return;
        if (!out.empty()) out += ", ";
        out += name;
    };
    add(distortion, "distortion");
    add(chromaticAberration, "chromatic aberration");
    add(vignetting, "vignetting");
    return out;
}

LensOpcodes makeLensOpcodes(const LensCorrection& lens, uint32_t activeWidth, uint32_t activeHeight,
                            uint32_t colorPlanes, LensCorrectionMode mode) {
    LensOpcodes out;
    if (mode == LensCorrectionMode::None || lens.empty()) return out;
    if (activeWidth == 0 || activeHeight == 0 || !(lens.frameWidth > 0) || !(lens.frameHeight > 0))
        return out;

    const bool all = mode == LensCorrectionMode::All;
    const bool wantDistortion = lens.hasDistortion() && (all || lens.distortionEnabled);
    bool wantCa = lens.hasCa() && (all || lens.caEnabled);
    const bool wantVignetting =
        lens.hasVignetting() && !lens.vignettingInData && (all || lens.vignettingEnabled);
    if (wantCa && colorPlanes != 3) {
        out.notes.push_back("chromatic aberration left out: the sensor does not have three colours");
        wantCa = false;
    }

    // Geometry. The cameras measure radii from the centre of the picture in
    // units of its half diagonal; DNG measures them from a centre given as a
    // fraction of the image, in units of the distance to the farthest corner.
    const double centreX = lens.frameLeft + 0.5 * lens.frameWidth;
    const double centreY = lens.frameTop + 0.5 * lens.frameHeight;
    if (centreX <= 0 || centreY <= 0 || centreX >= activeWidth || centreY >= activeHeight) return out;
    out.centreX = centreX / activeWidth;
    out.centreY = centreY / activeHeight;
    const double cameraUnit = 0.5 * std::hypot(lens.frameWidth, lens.frameHeight);
    const double dngUnit = std::hypot(std::max(centreX, activeWidth - centreX),
                                      std::max(centreY, activeHeight - centreY));
    const double toCamera = dngUnit / cameraUnit;  // DNG radius -> camera radius
    const double pictureEdge = cameraUnit / dngUnit;  // DNG radius of the picture's corner

    std::vector<double> radius(kSamples), x(kSamples), inside(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        radius[i] = (i + 0.5) / kSamples;
        x[i] = radius[i] * radius[i];
        inside[i] = radius[i] <= pictureEdge ? 1.0 : 0.0;
    }

    // ---- distortion and chromatic aberration: WarpRectilinear ----------------
    if (wantDistortion || wantCa) {
        if (wantDistortion && lens.fitFrame)
            out.zoom = frameFillingZoom(lens.distortion, lens.frameWidth, lens.frameHeight);
        const double zoom = out.zoom;
        // Stored radius over corrected radius for green, at a camera radius.
        const auto green = [&](double r) {
            return wantDistortion ? zoom * lens.distortion.at(zoom * r) : 1.0;
        };

        const uint32_t planes = wantCa ? 3 : 1;
        bool ok = true;
        double worst = 0.0;
        std::vector<double> y(kSamples), weight(kSamples);
        for (uint32_t plane = 0; plane < planes && ok; ++plane) {
            for (int i = 0; i < kSamples; ++i) {
                const double r = radius[i] * toCamera;
                double f = green(r);
                if (wantCa && plane != 1) {
                    // The aberration curves run over the radius in the stored
                    // image, which is where the green plane takes this point from.
                    const RadialCurve& ca = plane == 0 ? lens.caRed : lens.caBlue;
                    f *= 1.0 + ca.at(r * f);
                }
                y[i] = f;
                // Weighting by r^2 turns the error of the ratio into the
                // error of the position, which is what matters.
                weight[i] = x[i] * (inside[i] > 0 ? 1.0 : kOutsideWeight);
            }
            std::array<double, 4> k{};
            ok = fitPolynomial<4>(x, y, weight, 0, k);
            // A reader inverts r -> r * P(r^2); it has to keep growing.
            double previous = 0.0;
            for (int i = 0; i < kSamples && ok; ++i) {
                const double p = evaluateWarp(k, radius[i]);
                const double mapped = radius[i] * p;
                if (!std::isfinite(p) || p <= 0 || mapped <= previous) ok = false;
                previous = mapped;
                if (inside[i] > 0)
                    worst = std::max(worst, std::fabs(p - y[i]) * radius[i] * dngUnit);
            }
            out.warp[plane] = k;
        }
        if (ok) {
            out.planes = planes;
            out.distortion = wantDistortion;
            out.chromaticAberration = wantCa;
            out.warpErrorPixels = worst;
        } else {
            out.zoom = 1.0;
            out.notes.push_back("distortion left out: the camera's curve cannot be expressed in DNG");
        }
    }

    // ---- vignetting: FixVignetteRadial -----------------------------------------
    if (wantVignetting) {
        std::vector<double> y(kSamples), weight(kSamples);
        for (int i = 0; i < kSamples; ++i) {
            const double g = lens.vignetting.at(radius[i] * toCamera);
            // Fit (gain - 1) and judge the error relative to the gain.
            y[i] = g - 1.0;
            weight[i] = (inside[i] > 0 ? 1.0 : kOutsideWeight) / (g * g);
        }
        std::array<double, 5> k{};
        bool ok = fitPolynomial<5>(x, y, weight, 1, k);
        double worst = 0.0;
        for (int i = 0; i < kSamples && ok; ++i) {
            const double g = evaluateGain(k, radius[i]);
            if (!std::isfinite(g) || g <= 0.1) ok = false;
            if (inside[i] > 0) worst = std::max(worst, std::fabs(g - (y[i] + 1.0)) / (y[i] + 1.0));
        }
        if (ok) ok = evaluateGain(k, 0.0) > 0.1 && evaluateGain(k, 1.0) > 0.1;
        if (ok) {
            out.gain = k;
            out.vignetting = true;
            out.gainErrorPercent = 100.0 * worst;
        } else {
            out.notes.push_back("vignetting left out: the camera's curve cannot be expressed in DNG");
        }
    }

    if (!out.any()) return out;

    // ---- the opcode list ---------------------------------------------------------
    // Vignetting first: its curve is laid out over the stored image, so the
    // gain has to be applied before the warp moves anything.
    std::vector<uint8_t>& list = out.opcodeList3;
    put32(list, (out.vignetting ? 1u : 0u) + (out.planes ? 1u : 0u));
    if (out.vignetting) {
        put32(list, kOpcodeFixVignetteRadial);
        put32(list, kOpcodeVersion);
        put32(list, kOpcodeFlags);
        put32(list, 7 * 8);
        for (double k : out.gain) putDouble(list, k);
        putDouble(list, out.centreX);
        putDouble(list, out.centreY);
    }
    if (out.planes) {
        put32(list, kOpcodeWarpRectilinear);
        put32(list, kOpcodeVersion);
        put32(list, kOpcodeFlags);
        put32(list, 4 + out.planes * 6 * 8 + 2 * 8);
        put32(list, out.planes);
        for (uint32_t plane = 0; plane < out.planes; ++plane) {
            for (double k : out.warp[plane]) putDouble(list, k);
            putDouble(list, 0.0);  // no tangential terms
            putDouble(list, 0.0);
        }
        putDouble(list, out.centreX);
        putDouble(list, out.centreY);
    }
    return out;
}

LensOpcodes makeLensOpcodes(const RawImage& image, LensCorrectionMode mode) {
    LensOpcodes out;
    if (mode == LensCorrectionMode::None || image.lens.empty()) return out;
    if (image.scaleH != image.scaleV) {
        out.notes.push_back("lens corrections left out: the pixels are not square");
        return out;
    }
    if (image.activeRight <= image.activeLeft || image.activeBottom <= image.activeTop) return out;
    // Three planes in the order red, green, blue is what the curves assume.
    uint32_t planes = image.colorPlanes;
    if (planes == 3 && !(image.planeColor[0] == kRed && image.planeColor[1] == kGreen &&
                         image.planeColor[2] == kBlue))
        planes = 0;
    return makeLensOpcodes(image.lens, image.activeRight - image.activeLeft,
                           image.activeBottom - image.activeTop, planes, mode);
}

}  // namespace dngconv
