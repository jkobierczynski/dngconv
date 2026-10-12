// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Raw data prepared for lossy JPEG XL, the way Adobe's DNG SDK prepares a
// mosaic for it (DNG 1.7, "lossy mosaic"), and the means to judge the result.
//
// A lossy coder spends its bits where it believes the eye looks. Sensor
// values are linear in light, which puts most of their range in the
// highlights; given as they are, the coder would starve the shadows. So the
// values are first made linear in the DNG sense (black level subtracted,
// scaled to the white level), then stretched over the range they actually
// use (all of it; Adobe trims the extremes, which clips bright specks) and
// put through a curve that spaces the tones about evenly to the eye:
// the inverse of f(y) = s y + (1 - s) y^3 with s = 1/16. The DNG carries an
// opcode (MapPolynomial, in OpcodeList2) with that polynomial, so that a
// reader turns the stored values back into linear ones; its BlackLevel is 0
// and its WhiteLevel 65535.
//
// What is stored is the active area only (masked border pixels mean
// nothing once the black level is taken off), with a margin repeated from
// its edges so that each colour of the mosaic, stored as a field of its own,
// is a whole number of tiles: no tile holds two colours, which a lossy coder
// would smear into each other at the seam.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "raw_image.hpp"

namespace dngconv {

/// The curve: linear (0..1, after the range is stretched) to stored (0..1).
double lossyCurveEncode(double linear);
/// Its inverse, the polynomial the DNG's opcode carries.
double lossyCurveDecode(double stored);
/// The slope of the decoding polynomial at 0.
constexpr double kLossyCurveSlope = 1.0 / 16.0;

/// Largest tile edge the writer uses for lossy JPEG XL.
constexpr uint32_t kLossyMaxTile = 768;

/// Whether an image can be stored lossily: a mosaic (any pattern up to 8 x
/// 8) or a single-channel image. Full-colour raws are not done (yet).
bool lossyJxlSupported(const RawImage& image, std::string* why = nullptr);

struct LossyEncoding {
    // The stored frame: the active area with its margin.
    uint32_t width = 0, height = 0;
    uint32_t padTop = 0, padLeft = 0;            // where the active area starts in it
    uint32_t activeWidth = 0, activeHeight = 0;  // its size
    // Fields and tiles, as for lossless JPEG XL: rows and columns stored
    // field by field, `rowFactor` and `columnFactor` of them; tiles that
    // divide each field exactly.
    uint32_t rowFactor = 1, columnFactor = 1;
    uint32_t tileWidth = 0, tileHeight = 0;
    // The range of linear values the curve spans (fractions of white).
    double lower = 0, upper = 1;
    // The stored values, frame order (not yet field by field).
    std::vector<uint16_t> samples;
    // Defective pixels (a sample of 0 where the camera marks them so) that
    // were filled in from their neighbours before coding, since coding
    // would blur the marks the DNG opcode looks for.
    uint32_t repairedPixels = 0;

    /// The coefficients of the MapPolynomial: stored value -> linear value.
    std::vector<double> polynomial() const;
    /// The OpcodeList2 that carries it (big-endian, as opcode lists are).
    std::vector<uint8_t> opcodeList2() const;
};

/// The linear value of each active-area sample, as a reader of a lossless
/// DNG computes it before any opcode ((sample - black) / (white - highest
/// black), held to 0..1), in units of 1/65535: active area only, row by row.
/// Defective pixels are filled in when the image says the camera marks them.
std::vector<uint16_t> linearActiveArea(const RawImage& image, unsigned threads, uint32_t* repaired = nullptr);

/// Prepares the image. `maxTile` bounds the tile edge. Throws
/// std::invalid_argument if lossyJxlSupported() says no.
LossyEncoding encodeForLossyJxl(const RawImage& image, uint32_t maxTile, unsigned threads);

/// Codes the tiles: field by field, then tile by tile, in DNG order.
std::vector<std::vector<uint8_t>> compressLossyTiles(const LossyEncoding& encoding, float distance, int effort,
                                                     unsigned threads);

/// How far the stored values that come back from a file are from the ones
/// that were coded, over the active area, in units of the stored range
/// (65535 = 1).
struct LossyErrors {
    double rms = 0;           // root mean square
    double mean = 0;          // signed: a shift of everything shows here
    double worstTileRms = 0;  // the worst tile's root mean square
    uint32_t worstTile = 0;   // and which one (DNG order)
    double p999 = 0;          // 99.9th percentile of the absolute error
    double max = 0;
    // The largest error of a local average: over blocks of 32 x 32 stored
    // values (one colour each). Coding at any distance keeps local averages
    // close; a value in the wrong place does not.
    double worstBlockMean = 0;
    // Whether the values are where they belong: the error against the
    // expected values is compared with the error against the same values
    // moved by one pixel, by one period of the mosaic, and with the colours
    // of the mosaic exchanged. Coding blurs and adds noise, but leaves every
    // value closest to its own place; values in the wrong place are closer
    // to one of the others. `alignment` is the smallest relative margin,
    // (moved - own) / own: above 0 when everything is in place.
    double alignment = 1;
    std::string closestMove;  // the move with the smallest margin, e.g. "one period right"
};

/// `stored` is the frame as read back (frame order, padded size).
LossyErrors measureLossyErrors(const LossyEncoding& expected, const std::vector<uint16_t>& stored);

/// The verdict of --verify on a lossy file: whether the errors are those of
/// the coder at that distance, or of something else (a tile in the wrong
/// place, fields mixed up, a broken tile). `why` says what is wrong.
bool lossyCloseEnough(const LossyErrors& errors, float distance, std::string& why);

/// Describes the errors for a report, e.g. "rms 0.21 %, worst tile 0.30 %".
std::string describe(const LossyErrors& errors);

}  // namespace dngconv
