// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Reads the lens-correction parameters cameras store in their raw files and
// turns them into the maker-neutral curves of lens_correction.hpp.
//
// Supported:
//   Sony ARW       tables in the raw image's directory (2012 and later) or in
//                  the scrambled SR2 block (earlier bodies): distortion,
//                  chromatic aberration, vignetting
//   Fujifilm RAF   tables in the directory in front of the raw data:
//                  distortion, chromatic aberration, vignetting
//   Panasonic RW2  the DistortionInfo tag: distortion
//   Olympus ORF    polynomials in the maker note: distortion, chromatic
//                  aberration
//
// How each maker's numbers are to be read was taken from the work of others,
// credited in lens_data.cpp, and then checked against pictures: see
// DEVELOPMENT.md.
#pragma once

#include <filesystem>

#include "lens_correction.hpp"
#include "raw_image.hpp"

namespace dngconv {

/// Looks for lens-correction parameters in a raw file. `makerNote` is the
/// file's maker note if it has been read (Olympus keeps the parameters there,
/// Fuji a crop-mode flag). `pictureWidth` and `pictureHeight` give the size
/// of the picture area in pixels, for makers that count radii in pixels.
///
/// The reference frame of the result is left for the caller to fill in.
/// Never throws: unknown formats and damaged data give an empty result.
LensCorrection readLensData(const std::filesystem::path& path, const MakerNote& makerNote,
                            double pictureWidth, double pictureHeight);

}  // namespace dngconv
