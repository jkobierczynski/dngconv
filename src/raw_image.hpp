// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// In-memory model of a decoded raw frame plus the metadata a DNG needs.
// The reader fills it, the writer consumes it; neither knows about the other.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "lens_correction.hpp"

namespace dngconv {

/// Colour codes used by the TIFF/EP CFAPattern and DNG CFAPlaneColor tags.
enum CfaColor : uint8_t {
    kRed = 0,
    kGreen = 1,
    kBlue = 2,
    kCyan = 3,
    kMagenta = 4,
    kYellow = 5,
    kWhite = 6,
};

struct GpsInfo {
    bool valid = false;
    std::array<double, 3> latitude{};  // degrees, minutes, seconds
    std::array<double, 3> longitude{};
    char latitudeRef = 0;   // 'N' or 'S'
    char longitudeRef = 0;  // 'E' or 'W'
    bool hasAltitude = false;
    double altitude = 0.0;         // metres, absolute value
    bool altitudeBelowSea = false; // GPSAltitudeRef = 1
};

/// One TIFF directory entry copied from the source file. `data` holds the
/// value in little-endian byte order, whatever the source used.
struct TiffField {
    uint16_t tag = 0;
    uint16_t type = 0;   // TIFF field type, 1..12
    uint32_t count = 0;
    std::vector<uint8_t> data;
};

/// The camera maker's private metadata block, kept byte for byte.
struct MakerNote {
    std::vector<uint8_t> data;
    // Byte order of the TIFF structure the note was found in. Notes without
    // a byte-order mark of their own inherit it.
    bool bigEndian = false;
    // Where the note started, counted from the start of that TIFF structure.
    // Pointers inside many maker notes are relative to the same origin.
    uint32_t originalOffset = 0;
    bool empty() const { return data.empty(); }
};

/// Metadata carried over verbatim from the source file, as opposed to the
/// handful of values the raw decoder interprets (exposure, lens, date ...).
/// Where both exist, these win: they are what the camera actually wrote.
struct SourceMetadata {
    std::vector<TiffField> ifd0;  // descriptive tags only: copyright, XMP ...
    std::vector<TiffField> exif;  // the whole EXIF directory, minus pointers
    std::vector<TiffField> gps;   // the whole GPS directory
    MakerNote makerNote;
};

struct RawImage {
    // ---- pixel data -------------------------------------------------------
    // The complete sensor frame as the camera stored it, including masked
    // (optical black) borders. Row-major, samplesPerPixel interleaved values
    // per pixel, 16 bits each, not black-subtracted and not white-balanced.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t samplesPerPixel = 1;
    std::vector<uint16_t> pixels;

    // ---- colour filter array ----------------------------------------------
    // isCfa: one sample per pixel behind a mosaic (Bayer, X-Trans, ...).
    // Otherwise the data is "linear raw": full colour (3 or 4 samples) or
    // monochrome (1 sample, colorPlanes == 1).
    bool isCfa = false;
    uint32_t cfaRows = 0;
    uint32_t cfaCols = 0;
    // Plane index (0 .. colorPlanes-1) for each cell of the repeating pattern,
    // row-major. The pattern origin is the top-left pixel of the active area.
    std::vector<uint8_t> cfaPattern;

    // Number of colour planes and the colour each plane stands for.
    uint32_t colorPlanes = 3;
    std::array<uint8_t, 4> planeColor{kRed, kGreen, kBlue, kGreen};

    // ---- geometry -----------------------------------------------------------
    // Active (light-sensitive) area inside the frame.
    uint32_t activeTop = 0;
    uint32_t activeLeft = 0;
    uint32_t activeBottom = 0;  // exclusive
    uint32_t activeRight = 0;   // exclusive
    // Default crop, relative to the active area.
    uint32_t cropLeft = 0;
    uint32_t cropTop = 0;
    uint32_t cropWidth = 0;
    uint32_t cropHeight = 0;
    // Non-square pixel correction.
    double scaleH = 1.0;
    double scaleV = 1.0;
    // TIFF orientation (1..8).
    int orientation = 1;

    // ---- levels -------------------------------------------------------------
    // Black level pattern: blackRows x blackCols x samplesPerPixel values,
    // origin at the top-left pixel of the active area.
    uint32_t blackRows = 1;
    uint32_t blackCols = 1;
    std::vector<double> blackLevel;
    // Saturation level per sample.
    std::array<double, 4> whiteLevel{65535, 65535, 65535, 65535};
    // The camera marks defective pixels by storing 0 for them (Panasonic and a
    // few others do). The samples are kept as they are; the DNG gets an
    // instruction telling readers to interpolate over them.
    bool zeroIsBadPixel = false;

    // ---- lens corrections ----------------------------------------------------
    // What the camera recorded about straightening and evening out the
    // picture, if anything; see lens_correction.hpp.
    LensCorrection lens;

    // ---- colour -------------------------------------------------------------
    // XYZ (D65) -> camera native, colorPlanes rows of 3.
    bool hasColorMatrix = false;
    std::array<std::array<double, 3>, 4> colorMatrix{};
    // Camera-native values of a neutral object under the shot's white balance.
    bool hasAsShotNeutral = false;
    std::array<double, 4> asShotNeutral{1, 1, 1, 1};

    // ---- descriptive metadata ----------------------------------------------
    std::string make;
    std::string model;
    std::string uniqueCameraModel;
    std::string artist;
    std::string description;
    std::string bodySerial;
    std::string lensMake;
    std::string lensModel;
    std::string dateTime;  // "YYYY:MM:DD HH:MM:SS", empty if unknown
    std::string originalFileName;
    double exposureTime = 0.0;  // seconds, 0 if unknown
    double fNumber = 0.0;
    double iso = 0.0;
    double focalLength = 0.0;    // mm
    double focalLength35 = 0.0;  // mm, 35 mm equivalent
    // min focal, max focal, max aperture at min focal, max aperture at max focal
    std::array<double, 4> lensInfo{};
    GpsInfo gps;

    // ---- metadata copied from the source --------------------------------------
    SourceMetadata source;

    // ---- the source file itself ----------------------------------------------
    // Every byte of the file the image was decoded from, when it is to be
    // embedded in the DNG so that the conversion can be undone. Empty otherwise.
    std::vector<uint8_t> originalFile;

    // ---- embedded preview ---------------------------------------------------
    // A complete JPEG stream taken unchanged from the source file, or empty.
    std::vector<uint8_t> previewJpeg;

    size_t sampleCount() const {
        return static_cast<size_t>(width) * height * samplesPerPixel;
    }
};

}  // namespace dngconv
