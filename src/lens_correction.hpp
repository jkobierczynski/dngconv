// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Lens corrections as a camera describes them, in a form that does not
// depend on the maker.
//
// Mirrorless cameras store, with every raw file, how their own JPEG engine
// straightens the picture: how far each point has to be moved to undo the
// lens's distortion, how much larger or smaller the red and blue images are
// than the green one (lateral chromatic aberration), and how much the
// corners have to be brightened (vignetting). All three depend only on the
// distance from the optical centre, so each is a curve over the radius.
//
// Radii are measured from the centre of the picture in units of `radius`
// pixels, which the reader sets to half the diagonal of the picture area: a
// radius of 1 is the corner of the picture.
#pragma once

#include <string>
#include <vector>

namespace dngconv {

/// A function of the radius given by samples. Between samples it is linear;
/// beyond the first and last sample it continues along the nearest segment.
struct RadialCurve {
    std::vector<double> radius;  // ascending
    std::vector<double> value;

    bool empty() const { return radius.empty(); }
    void add(double r, double v) {
        radius.push_back(r);
        value.push_back(v);
    }
    double at(double r) const;
};

struct LensCorrection {
    // ---- reference frame, filled in by the raw reader ----------------------
    // The picture area the camera's curves refer to, in pixels of the active
    // area. Its centre is the optical centre.
    double frameLeft = 0, frameTop = 0, frameWidth = 0, frameHeight = 0;

    // ---- geometry -----------------------------------------------------------
    // Distortion: a point at radius r of the corrected picture is found at
    // radius r * distortion.at(r) of the stored image.
    RadialCurve distortion;
    // The camera then enlarges or shrinks the corrected picture until it just
    // fills the frame without showing anything from outside it (Sony, Fuji).
    // Other makers build that step into the curve.
    bool fitFrame = false;
    // Lateral chromatic aberration: in the stored image, what the green
    // plane shows at radius r the red plane shows at r * (1 + caRed.at(r)),
    // and likewise for blue.
    RadialCurve caRed, caBlue;

    // ---- light fall-off ------------------------------------------------------
    // Vignetting: the factor to multiply a pixel at radius r of the stored
    // image with.
    RadialCurve vignetting;

    // ---- what the camera was set to do --------------------------------------
    // Cameras that let the user switch a correction off still record its
    // parameters. These say whether the camera's own JPEG had it applied.
    bool distortionEnabled = false;
    bool caEnabled = false;
    bool vignettingEnabled = false;
    // Some cameras multiply the raw data itself with the vignetting gain
    // (Sony, when its shading compensation is on). The curve then describes
    // something that has happened already and must not be applied again.
    bool vignettingInData = false;

    /// Where the parameters were found, for messages ("Sony", "Fujifilm" ...).
    std::string origin;

    bool hasDistortion() const { return !distortion.empty(); }
    bool hasCa() const { return !caRed.empty() && !caBlue.empty(); }
    bool hasVignetting() const { return !vignetting.empty(); }
    bool empty() const { return !hasDistortion() && !hasCa() && !hasVignetting(); }
};

}  // namespace dngconv
