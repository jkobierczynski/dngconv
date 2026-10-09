// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Turns a camera's lens-correction curves into the DNG instructions that
// make a reader apply them: WarpRectilinear for distortion and lateral
// chromatic aberration, FixVignetteRadial for vignetting. Both go into
// OpcodeList3, which a reader runs on the demosaiced image.
//
// DNG wants polynomials in the squared radius where the cameras have tables
// or formulas of their own, so the curves are fitted by least squares, and
// the result says how far the fit strays from the camera's curve.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "lens_correction.hpp"

namespace dngconv {

struct RawImage;

enum class LensCorrectionMode {
    Auto,  // the corrections the camera itself was set to apply
    All,   // every correction the file has parameters for
    None,
};

struct LensOpcodes {
    /// Contents of the OpcodeList3 tag; empty when there is nothing to write.
    std::vector<uint8_t> opcodeList3;

    bool distortion = false;
    bool chromaticAberration = false;
    bool vignetting = false;
    bool any() const { return distortion || chromaticAberration || vignetting; }

    /// Factor by which the corrected picture was enlarged to fill the frame.
    double zoom = 1.0;
    /// Largest distance, in pixels inside the picture, between where the
    /// fitted polynomial and the camera's curve send a point.
    double warpErrorPixels = 0.0;
    /// Largest deviation of the fitted gain from the camera's, in percent.
    double gainErrorPercent = 0.0;

    // The fitted parameters, in the form DNG stores them.
    uint32_t planes = 0;                          // 1, or 3 (red, green, blue)
    std::array<std::array<double, 4>, 3> warp{};  // kr0 .. kr3 per plane
    std::array<double, 5> gain{};                 // k0 .. k4
    double centreX = 0.5, centreY = 0.5;          // fractions of the active area

    /// Corrections that were wanted but left out, and why.
    std::vector<std::string> notes;

    /// "distortion, chromatic aberration" - for messages. Empty if none.
    std::string summary() const;
};

/// Fits the curves and builds the opcode list. `activeWidth` x `activeHeight`
/// is the image the opcodes will be applied to; the frame recorded in `lens`
/// is in its coordinates. `colorPlanes` is the number of planes after
/// demosaicing; chromatic aberration needs exactly three (red, green, blue).
LensOpcodes makeLensOpcodes(const LensCorrection& lens, uint32_t activeWidth, uint32_t activeHeight,
                            uint32_t colorPlanes, LensCorrectionMode mode);

/// The same for an image as the reader delivers it. Leaves the corrections
/// out, with a note, for sensors the opcodes cannot describe.
LensOpcodes makeLensOpcodes(const RawImage& image, LensCorrectionMode mode);

// Building blocks, exposed for the tests.

/// Largest factor z for which the corrected picture, enlarged by z, takes
/// nothing from outside a frame of the given size. `ratio` maps a radius in
/// the corrected picture (1 = corner of the frame) to stored / corrected.
double frameFillingZoom(const RadialCurve& ratio, double frameWidth, double frameHeight);

/// Where the Adobe DNG SDK's WarpRectilinear sends a point: the ratio of the
/// source radius to the destination radius r (0..1) for coefficients k.
double evaluateWarp(const std::array<double, 4>& k, double r);
/// The gain FixVignetteRadial applies at radius r (0..1).
double evaluateGain(const std::array<double, 5>& k, double r);

}  // namespace dngconv
