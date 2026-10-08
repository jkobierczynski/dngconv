// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "raw_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <numeric>
#include <stdexcept>

#include <libraw/libraw.h>

#include "source_metadata.hpp"

#if !LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0, 21)
#error "dngconv needs LibRaw 0.21 or newer"
#endif

namespace dngconv {

namespace {

std::string trimmed(const char* s, size_t maxLen) {
    std::string out(s, strnlen(s, maxLen));
    const char* ws = " \t\r\n";
    const size_t b = out.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const size_t e = out.find_last_not_of(ws);
    return out.substr(b, e - b + 1);
}

template <size_t N>
std::string trimmed(const char (&s)[N]) {
    return trimmed(s, N);
}

bool startsWithNoCase(const std::string& text, const std::string& prefix) {
    if (prefix.size() > text.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

int openFile(LibRaw& lr, const std::filesystem::path& path) {
#if defined(_WIN32) && defined(LIBRAW_WIN32_UNICODEPATHS)
    return lr.open_file(path.wstring().c_str());
#else
    return lr.open_file(path.string().c_str());
#endif
}

// LibRaw's "flip" value -> TIFF Orientation.
int tiffOrientation(int flip) {
    static const int map[8] = {1, 2, 4, 3, 5, 8, 6, 7};
    return (flip >= 0 && flip < 8) ? map[flip] : 1;
}

int cfaColorCode(char c) {
    switch (c) {
        case 'R': return kRed;
        case 'G': return kGreen;
        case 'B': return kBlue;
        case 'C': return kCyan;
        case 'M': return kMagenta;
        case 'Y': return kYellow;
        case 'W': return kWhite;
        default: return -1;
    }
}

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

// XYZ (D65) -> linear sRGB.
const double kSrgbFromXyz[3][3] = {
    {3.2404542, -1.5371385, -0.4985314},
    {-0.9692660, 1.8760108, 0.0415560},
    {0.0556434, -0.2040259, 1.0572252},
};

struct DataErrorContext {
    bool reported = false;
};

// The offset parameter is int in LibRaw 0.21 and 64-bit from 0.22 on; the
// template lets the compiler pick whichever signature the library declares.
template <typename Offset>
void dataErrorCallback(void* context, const char* /*file*/, Offset /*offset*/) {
    if (context) static_cast<DataErrorContext*>(context)->reported = true;
}

std::string formatTimestamp(time_t t) {
    if (t <= 0) return {};
    std::tm tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0) return {};
#else
    if (!localtime_r(&t, &tm)) return {};
#endif
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d:%02d:%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void fillColour(const libraw_data_t& d, RawImage& img, std::vector<std::string>& warnings) {
    const auto& c = d.color;
    if (img.colorPlanes < 3) return;  // monochrome needs no colour description

    // 1. The matrix LibRaw knows for this camera (XYZ D65 -> camera).
    bool have = false;
    for (unsigned p = 0; p < 3 && !have; ++p)
        for (int k = 0; k < 3; ++k)
            if (c.cam_xyz[p][k] != 0.0f) have = true;
    if (have) {
        for (unsigned p = 0; p < img.colorPlanes; ++p)
            for (int k = 0; k < 3; ++k) img.colorMatrix[p][k] = c.cam_xyz[p][k];
        img.hasColorMatrix = true;
    }

    // 2. Otherwise derive one from the camera -> sRGB matrix, undoing the
    //    row normalisation dcraw-style decoders apply (stored in pre_mul).
    if (!img.hasColorMatrix && img.colorPlanes == 3) {
        double rgbCam[3][3], camRgb[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) rgbCam[i][j] = c.rgb_cam[i][j];
        if (invert3x3(rgbCam, camRgb)) {
            for (int i = 0; i < 3; ++i) {
                const double scale = c.pre_mul[i] > 0 ? 1.0 / c.pre_mul[i] : 1.0;
                for (int k = 0; k < 3; ++k) {
                    double v = 0;
                    for (int j = 0; j < 3; ++j) v += camRgb[i][j] * kSrgbFromXyz[j][k];
                    img.colorMatrix[i][k] = v * scale;
                }
            }
            img.hasColorMatrix = true;
            warnings.push_back(
                "no calibrated colour matrix for this camera; derived one from the decoder's "
                "built-in camera-to-sRGB matrix");
        }
    }

    // 3. Last resort: treat the camera as an sRGB device.
    if (!img.hasColorMatrix) {
        for (unsigned p = 0; p < img.colorPlanes; ++p)
            for (int k = 0; k < 3; ++k) img.colorMatrix[p][k] = p < 3 ? kSrgbFromXyz[p][k] : 0.0;
        img.hasColorMatrix = true;
        warnings.push_back("no colour matrix available; colours will only be approximate");
    }

    // White balance as shot.
    double mul[4];
    for (unsigned p = 0; p < 4; ++p) mul[p] = c.cam_mul[p];
    auto usable = [&](const double* m) {
        for (unsigned p = 0; p < img.colorPlanes; ++p)
            if (!(m[p] > 0.0) || !std::isfinite(m[p])) return false;
        return true;
    };
    if (!usable(mul)) {
        for (unsigned p = 0; p < 4; ++p) mul[p] = c.pre_mul[p];
        if (usable(mul))
            warnings.push_back("no as-shot white balance in the file; using a daylight default");
    }
    if (usable(mul)) {
        double smallest = mul[0];
        for (unsigned p = 1; p < img.colorPlanes; ++p) smallest = std::min(smallest, mul[p]);
        for (unsigned p = 0; p < img.colorPlanes; ++p) img.asShotNeutral[p] = smallest / mul[p];
        img.hasAsShotNeutral = true;
    }
}

}  // namespace

std::string decoderVersion() { return LibRaw::version(); }

int supportedCameraCount() { return LibRaw::cameraCount(); }

RawReadResult readRaw(const std::filesystem::path& path, const RawReadOptions& options) {
    RawReadResult result;
    RawImage& img = result.image;
    auto& warnings = result.warnings;

    // LibRaw objects are large; keep them off the stack.
    auto lrOwner = std::make_unique<LibRaw>();
    LibRaw& lr = *lrOwner;
    DataErrorContext dataError;
    lr.set_dataerror_handler(dataErrorCallback, &dataError);

    int rc = openFile(lr, path);
    if (rc != LIBRAW_SUCCESS)
        throw std::runtime_error(std::string("cannot open as raw: ") + libraw_strerror(rc));

    const libraw_data_t& d = lr.imgdata;
    const auto& s = d.sizes;
    const auto& c = d.color;

    result.sourceIsDng = d.idata.dng_version != 0;
    result.frameCount = d.idata.raw_count ? d.idata.raw_count : 1;
    if (result.frameCount > 1)
        warnings.push_back("file holds " + std::to_string(result.frameCount) +
                           " raw frames; only the first one is converted");

    if (!options.metadataOnly) {
        rc = lr.unpack();
        if (rc != LIBRAW_SUCCESS)
            throw std::runtime_error(std::string("cannot decode raw data: ") +
                                     libraw_strerror(rc));
        if (dataError.reported)
            warnings.push_back("the decoder reported damaged raw data; the image may be "
                               "incomplete");
    }

    const auto& internal = lr.get_internal_data_pointer()->internal_output_params;
    if (internal.fuji_width != 0)
        throw std::runtime_error("Fuji Super CCD sensors (diagonal layout) are not supported yet");
    img.zeroIsBadPixel = internal.zero_is_bad != 0;
    if (s.raw_width == 0 || s.raw_height == 0)
        throw std::runtime_error("file reports an empty raw frame");

    // ---- pixels -----------------------------------------------------------
    img.width = s.raw_width;
    img.height = s.raw_height;
    const int colors = d.idata.colors;
    const unsigned filters = d.idata.filters;
    const auto& rd = d.rawdata;

    if (options.metadataOnly) {
        // Without decoding we do not know which buffer the decoder will fill;
        // a mosaic is by far the common case.
        img.samplesPerPixel = filters ? 1 : static_cast<uint32_t>(std::clamp(colors, 1, 4));
    } else if (rd.raw_image) {
        img.samplesPerPixel = 1;
        const size_t pitch = s.raw_pitch ? s.raw_pitch / sizeof(uint16_t) : s.raw_width;
        if (pitch < img.width) throw std::runtime_error("unexpected raw buffer layout");
        img.pixels.resize(img.sampleCount());
        for (uint32_t y = 0; y < img.height; ++y)
            std::memcpy(&img.pixels[static_cast<size_t>(y) * img.width],
                        rd.raw_image + static_cast<size_t>(y) * pitch,
                        static_cast<size_t>(img.width) * sizeof(uint16_t));
    } else if (rd.color3_image || rd.color4_image) {
        const unsigned srcSamples = rd.color3_image ? 3 : 4;
        img.samplesPerPixel = static_cast<uint32_t>(std::clamp<int>(colors, 1, srcSamples));
        const uint16_t* src = rd.color3_image ? &rd.color3_image[0][0] : &rd.color4_image[0][0];
        const size_t pitch = s.raw_pitch ? s.raw_pitch / sizeof(uint16_t)
                                         : static_cast<size_t>(s.raw_width) * srcSamples;
        if (pitch < static_cast<size_t>(img.width) * srcSamples)
            throw std::runtime_error("unexpected raw buffer layout");
        img.pixels.resize(img.sampleCount());
        uint16_t* dst = img.pixels.data();
        for (uint32_t y = 0; y < img.height; ++y) {
            const uint16_t* row = src + static_cast<size_t>(y) * pitch;
            for (uint32_t x = 0; x < img.width; ++x)
                for (uint32_t k = 0; k < img.samplesPerPixel; ++k)
                    *dst++ = row[static_cast<size_t>(x) * srcSamples + k];
        }
    } else if (rd.float_image || rd.float3_image || rd.float4_image) {
        throw std::runtime_error("floating-point raw data is not supported yet");
    } else {
        throw std::runtime_error("decoder returned no raw data");
    }

    // ---- geometry -----------------------------------------------------------
    img.activeTop = std::min<uint32_t>(s.top_margin, img.height);
    img.activeLeft = std::min<uint32_t>(s.left_margin, img.width);
    img.activeBottom = std::min<uint32_t>(img.activeTop + s.height, img.height);
    img.activeRight = std::min<uint32_t>(img.activeLeft + s.width, img.width);
    if (img.activeBottom <= img.activeTop || img.activeRight <= img.activeLeft) {
        img.activeTop = img.activeLeft = 0;
        img.activeBottom = img.height;
        img.activeRight = img.width;
        warnings.push_back("file reports no usable image area; using the whole frame");
    }
    const uint32_t activeW = img.activeRight - img.activeLeft;
    const uint32_t activeH = img.activeBottom - img.activeTop;

    // Default crop: what the camera itself considers the picture, when known.
    img.cropLeft = img.cropTop = 0;
    img.cropWidth = activeW;
    img.cropHeight = activeH;
    {
        const auto& ic = s.raw_inset_crops[0];
        if (ic.cwidth > 0 && ic.cheight > 0 && ic.cwidth <= activeW && ic.cheight <= activeH) {
            // 0xffff means "centred"; otherwise the origin is in frame coordinates.
            int64_t left = ic.cleft == 0xffff
                               ? static_cast<int64_t>(img.activeLeft) + (activeW - ic.cwidth) / 2
                               : ic.cleft;
            int64_t top = ic.ctop == 0xffff
                              ? static_cast<int64_t>(img.activeTop) + (activeH - ic.cheight) / 2
                              : ic.ctop;
            left = std::clamp<int64_t>(left - img.activeLeft, 0, activeW - ic.cwidth);
            top = std::clamp<int64_t>(top - img.activeTop, 0, activeH - ic.cheight);
            img.cropLeft = static_cast<uint32_t>(left);
            img.cropTop = static_cast<uint32_t>(top);
            img.cropWidth = ic.cwidth;
            img.cropHeight = ic.cheight;
        }
    }

    if (s.pixel_aspect > 0.0 && std::fabs(s.pixel_aspect - 1.0) > 1e-3) {
        if (s.pixel_aspect > 1.0)
            img.scaleH = s.pixel_aspect;
        else
            img.scaleV = 1.0 / s.pixel_aspect;
    }
    img.orientation = tiffOrientation(s.flip);

    // ---- colour filter array ----------------------------------------------
    img.isCfa = img.samplesPerPixel == 1 && filters != 0;
    if (img.isCfa) {
        if (filters == 1)
            throw std::runtime_error("16x16 colour filter patterns are not supported yet");
        if (colors != 3 && colors != 4)
            throw std::runtime_error("unexpected number of colours in the filter array");

        // LibRaw numbers filter colours 0..3 in the order of idata.cdesc. A
        // three-colour sensor still uses index 3 for its second green.
        int planeOf[4] = {0, 1, 2, 3};
        if (colors == 3) {
            planeOf[3] = -1;
            for (int p = 0; p < 3; ++p)
                if (d.idata.cdesc[p] == d.idata.cdesc[3]) planeOf[3] = p;
            if (planeOf[3] < 0) planeOf[3] = 1;
        }
        img.colorPlanes = static_cast<uint32_t>(colors);
        for (int p = 0; p < colors; ++p) {
            const int code = cfaColorCode(d.idata.cdesc[p]);
            if (code < 0)
                throw std::runtime_error(std::string("colour filter '") + d.idata.cdesc[p] +
                                         "' has no DNG equivalent");
            img.planeColor[p] = static_cast<uint8_t>(code);
        }

        if (filters == 9) {  // Fuji X-Trans
            img.cfaRows = img.cfaCols = 6;
        } else {
            img.cfaCols = 2;
            img.cfaRows = 8;
            for (uint32_t rows : {2u, 4u}) {
                bool repeats = true;
                for (uint32_t r = 0; r < 8 && repeats; ++r)
                    for (uint32_t col = 0; col < 2; ++col)
                        if (lr.COLOR(r, col) != lr.COLOR(r % rows, col)) repeats = false;
                if (repeats) {
                    img.cfaRows = rows;
                    break;
                }
            }
        }
        img.cfaPattern.resize(static_cast<size_t>(img.cfaRows) * img.cfaCols);
        for (uint32_t r = 0; r < img.cfaRows; ++r)
            for (uint32_t col = 0; col < img.cfaCols; ++col) {
                const int idx = lr.COLOR(r, col);
                if (idx < 0 || idx > 3) throw std::runtime_error("unreadable colour filter layout");
                img.cfaPattern[r * img.cfaCols + col] = static_cast<uint8_t>(planeOf[idx]);
            }
    } else {
        img.colorPlanes = img.samplesPerPixel;
        if (img.colorPlanes >= 3) {
            for (size_t p = 0; p < img.planeColor.size() && p < img.colorPlanes; ++p) {
                const int code = cfaColorCode(d.idata.cdesc[p]);
                img.planeColor[p] = static_cast<uint8_t>(code < 0 ? static_cast<int>(p) : code);
            }
        }
    }

    // ---- black and white levels ---------------------------------------------
    {
        const unsigned patRows = c.cblack[4], patCols = c.cblack[5];
        const bool hasPattern = patRows > 0 && patCols > 0 &&
                                6 + static_cast<size_t>(patRows) * patCols <= LIBRAW_CBLACK_SIZE;
        auto patternAt = [&](uint32_t r, uint32_t col) -> double {
            return hasPattern ? c.cblack[6 + (r % patRows) * patCols + (col % patCols)] : 0.0;
        };

        if (img.isCfa) {
            uint32_t rows = img.cfaRows, cols = img.cfaCols;
            if (hasPattern) {
                rows = std::lcm(rows, patRows);
                cols = std::lcm(cols, patCols);
            }
            if (rows > 8 || cols > 8) {
                // Beyond what DNG readers accept: fall back to the CFA period.
                rows = img.cfaRows;
                cols = img.cfaCols;
                warnings.push_back("black level pattern simplified");
            }
            img.blackRows = rows;
            img.blackCols = cols;
            img.blackLevel.resize(static_cast<size_t>(rows) * cols);
            for (uint32_t r = 0; r < rows; ++r)
                for (uint32_t col = 0; col < cols; ++col)
                    img.blackLevel[r * cols + col] =
                        static_cast<double>(c.black) + c.cblack[lr.COLOR(r, col)] +
                        patternAt(r, col);
        } else {
            double patternMean = 0.0;
            if (hasPattern) {
                for (unsigned i = 0; i < patRows * patCols; ++i) patternMean += c.cblack[6 + i];
                patternMean /= patRows * patCols;
            }
            img.blackRows = img.blackCols = 1;
            img.blackLevel.resize(img.samplesPerPixel);
            for (uint32_t k = 0; k < img.samplesPerPixel; ++k)
                img.blackLevel[k] = static_cast<double>(c.black) + c.cblack[k] + patternMean;
        }
        // Collapse a uniform pattern to a single value per sample.
        if (img.isCfa && std::all_of(img.blackLevel.begin(), img.blackLevel.end(),
                                     [&](double v) { return v == img.blackLevel[0]; })) {
            img.blackRows = img.blackCols = 1;
            img.blackLevel.resize(1);
        }

        const double maxBlack = *std::max_element(img.blackLevel.begin(), img.blackLevel.end());
        const double formatMax = c.maximum > 0 ? static_cast<double>(c.maximum) : 65535.0;
        for (unsigned p = 0; p < 4; ++p) {
            // Prefer the saturation point the camera recorded, when it is
            // tighter than the format maximum.
            const double cameraMax = static_cast<double>(c.linear_max[p]);
            double white = (cameraMax > maxBlack && cameraMax < formatMax) ? cameraMax : formatMax;
            if (white <= maxBlack) white = 65535.0;
            img.whiteLevel[p] = std::min(white, 65535.0);
        }
        if (formatMax <= maxBlack)
            warnings.push_back("black level is above the white level in the source metadata; "
                               "levels may be wrong");
    }

    // ---- colour ---------------------------------------------------------------
    fillColour(d, img, warnings);

    // ---- descriptive metadata -------------------------------------------------
    img.make = trimmed(d.idata.make);
    img.model = trimmed(d.idata.model);
    if (img.make.empty() && img.model.empty()) img.model = "Unknown camera";
    img.uniqueCameraModel = (img.make.empty() || startsWithNoCase(img.model, img.make))
                                ? img.model
                                : img.make + " " + img.model;
    // Make and Model keep the camera's own spelling when we can read it; the
    // normalised names above are only the fallback. The same pass collects
    // the EXIF and GPS directories and the maker note for copying.
    {
        SourceMetadataResult original = readSourceMetadata(path);
        if (original.identityFound()) {
            img.make = original.make;
            img.model = original.model;
        }
        if (options.copyMetadata) img.source = std::move(original.metadata);
    }
    img.artist = trimmed(d.other.artist);
    img.description = trimmed(d.other.desc);
    img.bodySerial = trimmed(d.shootinginfo.BodySerial);
    if (std::none_of(img.bodySerial.begin(), img.bodySerial.end(),
                     [](unsigned char ch) { return std::isalnum(ch) != 0; }))
        img.bodySerial.clear();  // placeholders such as "."
    img.lensMake = trimmed(d.lens.LensMake);
    img.lensModel = trimmed(d.lens.Lens);
    img.dateTime = formatTimestamp(d.other.timestamp);
    img.originalFileName = path.filename().u8string();

    auto positive = [](double v) { return std::isfinite(v) && v > 0.0 ? v : 0.0; };
    img.exposureTime = positive(d.other.shutter);
    img.fNumber = positive(d.other.aperture);
    img.iso = positive(d.other.iso_speed);
    img.focalLength = positive(d.other.focal_len);
    img.focalLength35 = positive(d.lens.FocalLengthIn35mmFormat);
    img.lensInfo = {positive(d.lens.MinFocal), positive(d.lens.MaxFocal),
                    positive(d.lens.MaxAp4MinFocal), positive(d.lens.MaxAp4MaxFocal)};

    {
        const auto& g = d.other.parsed_gps;
        const bool refsOk = (g.latref == 'N' || g.latref == 'S') &&
                            (g.longref == 'E' || g.longref == 'W');
        if (g.gpsparsed && refsOk) {
            img.gps.valid = true;
            for (int i = 0; i < 3; ++i) {
                img.gps.latitude[i] = positive(g.latitude[i]);
                img.gps.longitude[i] = positive(g.longitude[i]);
            }
            img.gps.latitudeRef = g.latref;
            img.gps.longitudeRef = g.longref;
            if (std::isfinite(g.altitude) && g.altitude != 0.0f) {
                img.gps.hasAltitude = true;
                img.gps.altitude = std::fabs(g.altitude);
                img.gps.altitudeBelowSea = g.altref == 1;
            }
        }
    }

    // ---- embedded preview ---------------------------------------------------
    if (options.loadPreview && !options.metadataOnly) {
        if (lr.unpack_thumb() == LIBRAW_SUCCESS &&
            d.thumbnail.tformat == LIBRAW_THUMBNAIL_JPEG && d.thumbnail.thumb &&
            d.thumbnail.tlength > 4) {
            const auto* p = reinterpret_cast<const uint8_t*>(d.thumbnail.thumb);
            img.previewJpeg.assign(p, p + d.thumbnail.tlength);
        }
    }

    return result;
}

}  // namespace dngconv
