// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "dng_writer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "ljpeg92.hpp"
#include "md5.hpp"
#include "thumbnail.hpp"
#include "tiff_writer.hpp"

namespace dngconv {

namespace {

// TIFF 6.0 / TIFF-EP / EXIF / DNG tag numbers used below.
namespace tag {
constexpr uint16_t NewSubFileType = 254;
constexpr uint16_t ImageWidth = 256;
constexpr uint16_t ImageLength = 257;
constexpr uint16_t BitsPerSample = 258;
constexpr uint16_t Compression = 259;
constexpr uint16_t PhotometricInterpretation = 262;
constexpr uint16_t ImageDescription = 270;
constexpr uint16_t Make = 271;
constexpr uint16_t Model = 272;
constexpr uint16_t StripOffsets = 273;
constexpr uint16_t Orientation = 274;
constexpr uint16_t SamplesPerPixel = 277;
constexpr uint16_t RowsPerStrip = 278;
constexpr uint16_t StripByteCounts = 279;
constexpr uint16_t PlanarConfiguration = 284;
constexpr uint16_t Software = 305;
constexpr uint16_t DateTime = 306;
constexpr uint16_t Artist = 315;
constexpr uint16_t TileWidth = 322;
constexpr uint16_t TileLength = 323;
constexpr uint16_t TileOffsets = 324;
constexpr uint16_t TileByteCounts = 325;
constexpr uint16_t SubIFDs = 330;
constexpr uint16_t YCbCrSubSampling = 530;
constexpr uint16_t YCbCrPositioning = 531;
constexpr uint16_t ReferenceBlackWhite = 532;
constexpr uint16_t CFARepeatPatternDim = 33421;
constexpr uint16_t CFAPattern = 33422;
constexpr uint16_t ExifIFD = 34665;
constexpr uint16_t GPSInfo = 34853;
constexpr uint16_t DNGVersion = 50706;
constexpr uint16_t DNGBackwardVersion = 50707;
constexpr uint16_t UniqueCameraModel = 50708;
constexpr uint16_t CFAPlaneColor = 50710;
constexpr uint16_t CFALayout = 50711;
constexpr uint16_t BlackLevelRepeatDim = 50713;
constexpr uint16_t BlackLevel = 50714;
constexpr uint16_t WhiteLevel = 50717;
constexpr uint16_t DefaultScale = 50718;
constexpr uint16_t DefaultCropOrigin = 50719;
constexpr uint16_t DefaultCropSize = 50720;
constexpr uint16_t ColorMatrix1 = 50721;
constexpr uint16_t AsShotNeutral = 50728;
constexpr uint16_t CameraSerialNumber = 50735;
constexpr uint16_t LensInfo = 50736;
constexpr uint16_t CalibrationIlluminant1 = 50778;
constexpr uint16_t OriginalRawFileName = 50827;
constexpr uint16_t DNGPrivateData = 50740;
constexpr uint16_t RawDataUniqueID = 50781;
constexpr uint16_t ActiveArea = 50829;
constexpr uint16_t OpcodeList1 = 51008;
constexpr uint16_t NewRawImageDigest = 51111;
// EXIF IFD
constexpr uint16_t ExposureTime = 33434;
constexpr uint16_t FNumber = 33437;
constexpr uint16_t ISOSpeedRatings = 34855;
constexpr uint16_t SensitivityType = 34864;
constexpr uint16_t RecommendedExposureIndex = 34866;
constexpr uint16_t ExifVersion = 36864;
constexpr uint16_t MakerNote = 37500;
constexpr uint16_t DateTimeOriginal = 36867;
constexpr uint16_t DateTimeDigitized = 36868;
constexpr uint16_t FocalLength = 37386;
constexpr uint16_t FocalLengthIn35mmFilm = 41989;
constexpr uint16_t BodySerialNumber = 42033;
constexpr uint16_t LensSpecification = 42034;
constexpr uint16_t LensMake = 42035;
constexpr uint16_t LensModel = 42036;
// GPS IFD
constexpr uint16_t GPSVersionID = 0;
constexpr uint16_t GPSLatitudeRef = 1;
constexpr uint16_t GPSLatitude = 2;
constexpr uint16_t GPSLongitudeRef = 3;
constexpr uint16_t GPSLongitude = 4;
constexpr uint16_t GPSAltitudeRef = 5;
constexpr uint16_t GPSAltitude = 6;
}  // namespace tag

constexpr uint16_t kCompressionNone = 1;
constexpr uint16_t kCompressionJpeg = 7;  // lossless JPEG for raw data, DCT JPEG for previews
constexpr uint16_t kPhotometricRgb = 2;
constexpr uint16_t kPhotometricYCbCr = 6;
constexpr uint16_t kPhotometricCfa = 32803;
constexpr uint16_t kPhotometricLinearRaw = 34892;
constexpr uint16_t kIlluminantD65 = 21;

struct JpegInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    unsigned components = 0;
    unsigned subH = 1;  // chroma subsampling relative to luma
    unsigned subV = 1;
};

// Finds the frame header of a baseline or progressive JPEG. Returns false for
// anything that cannot be described as an 8-bit YCbCr TIFF image.
bool parseJpeg(const std::vector<uint8_t>& j, JpegInfo& info) {
    if (j.size() < 4 || j[0] != 0xff || j[1] != 0xd8) return false;
    size_t pos = 2;
    while (pos + 4 <= j.size()) {
        if (j[pos] != 0xff) return false;
        const uint8_t marker = j[pos + 1];
        if (marker == 0xff) {  // fill byte
            ++pos;
            continue;
        }
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) {
            pos += 2;
            continue;
        }
        if (marker == 0xd9 || marker == 0xda) return false;  // no frame header seen
        const size_t len = (static_cast<size_t>(j[pos + 2]) << 8) | j[pos + 3];
        if (len < 2 || pos + 2 + len > j.size()) return false;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (len < 8) return false;
            const uint8_t* p = &j[pos + 4];
            if (p[0] != 8) return false;  // sample precision
            info.height = (static_cast<uint32_t>(p[1]) << 8) | p[2];
            info.width = (static_cast<uint32_t>(p[3]) << 8) | p[4];
            info.components = p[5];
            if (info.components != 3 || len < 8 + 3u * info.components) return false;
            const unsigned yH = p[7] >> 4, yV = p[7] & 15;
            const unsigned cH = p[10] >> 4, cV = p[10] & 15;
            if (!yH || !yV || !cH || !cV || yH % cH || yV % cV) return false;
            info.subH = yH / cH;
            info.subV = yV / cV;
            return info.width > 0 && info.height > 0;
        }
        pos += 2 + len;
    }
    return false;
}

unsigned workerCount(unsigned requested, size_t jobs) {
    unsigned n = requested ? requested : std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    return static_cast<unsigned>(std::min<size_t>(n, std::max<size_t>(jobs, 1)));
}

// Runs fn(i) for i in [0, jobs) on a few threads; rethrows the first error.
template <typename Fn>
void parallelFor(size_t jobs, unsigned threads, Fn fn) {
    const unsigned n = workerCount(threads, jobs);
    if (n <= 1) {
        for (size_t i = 0; i < jobs; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::mutex errorMutex;
    auto worker = [&] {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= jobs) return;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (!error) error = std::current_exception();
                next.store(jobs);
                return;
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned t = 0; t < n; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

struct TileLayout {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t components = 1;  // JPEG components per tile
};

// Chooses the tile geometry for a frame.
//
// A mosaic is coded as two interleaved components of half the width, so each
// sample is predicted from its neighbour of the same colour. That needs an
// even tile width.
//
// Tiles are never larger than the frame: some decoders wrap rows at the frame
// width when a tile is wider than the frame, which scrambles small images.
// The one case where both rules collide (an odd frame width smaller than the
// tile size) falls back to plain single-component coding.
TileLayout chooseTileLayout(const RawImage& img, uint32_t tileSize) {
    TileLayout t;
    t.width = std::min(tileSize, img.width);
    t.height = std::min(tileSize, img.height);
    if (img.isCfa)
        t.components = (t.width % 2 == 0) ? 2 : 1;
    else
        t.components = img.samplesPerPixel;
    return t;
}

// Lossless-JPEG tiles. TIFF tiles always have the full tile size, so tiles on
// the right and bottom edges are padded. Padding repeats the last pixels with
// a period of two, which keeps the mosaic phase and so costs almost nothing.
std::vector<std::vector<uint8_t>> compressTiles(const RawImage& img, const TileLayout& tile,
                                                unsigned threads) {
    const uint32_t spp = img.samplesPerPixel;
    const uint32_t tilesAcross = (img.width + tile.width - 1) / tile.width;
    const uint32_t tilesDown = (img.height + tile.height - 1) / tile.height;
    std::vector<std::vector<uint8_t>> tiles(static_cast<size_t>(tilesAcross) * tilesDown);

    const uint32_t jpegWidth = tile.width * spp / tile.components;
    const size_t tileRowSamples = static_cast<size_t>(tile.width) * spp;

    auto clampCoord = [](uint32_t v, uint32_t size) -> uint32_t {
        if (v < size) return v;
        if (size < 2) return 0;
        const uint32_t over = v - size;  // 0 for the first padded position
        return size - 2 + (over & 1);
    };

    parallelFor(tiles.size(), threads, [&](size_t index) {
        const uint32_t tx = static_cast<uint32_t>(index % tilesAcross) * tile.width;
        const uint32_t ty = static_cast<uint32_t>(index / tilesAcross) * tile.height;
        std::vector<uint16_t> buffer(tileRowSamples * tile.height);
        const uint32_t inside = tx + tile.width <= img.width ? tile.width : img.width - tx;
        for (uint32_t y = 0; y < tile.height; ++y) {
            const uint32_t sy = clampCoord(ty + y, img.height);
            const uint16_t* srcRow = &img.pixels[static_cast<size_t>(sy) * img.width * spp];
            uint16_t* dst = &buffer[static_cast<size_t>(y) * tileRowSamples];
            std::copy_n(srcRow + static_cast<size_t>(tx) * spp, static_cast<size_t>(inside) * spp,
                        dst);
            for (uint32_t x = inside; x < tile.width; ++x) {
                const uint32_t sx = clampCoord(tx + x, img.width);
                for (uint32_t k = 0; k < spp; ++k)
                    dst[static_cast<size_t>(x) * spp + k] = srcRow[static_cast<size_t>(sx) * spp + k];
            }
        }
        tiles[index] = encodeLosslessJpeg(buffer.data(), jpegWidth, tile.height, tile.components,
                                          tileRowSamples, 16);
    });
    return tiles;
}

// 16-bit samples as bytes in the byte order of the file.
std::vector<uint8_t> sampleBytes(const std::vector<uint16_t>& pixels, bool bigEndian) {
    std::vector<uint8_t> out(pixels.size() * 2);
    const size_t low = bigEndian ? 1 : 0, high = bigEndian ? 0 : 1;
    for (size_t i = 0; i < pixels.size(); ++i) {
        out[2 * i + low] = static_cast<uint8_t>(pixels[i] & 0xff);
        out[2 * i + high] = static_cast<uint8_t>(pixels[i] >> 8);
    }
    return out;
}

// Bayer phase of the pixel at the frame origin, as DNG opcodes number it:
// 0 = red, 1 = green on a red row, 2 = green on a blue row, 3 = blue.
// Returns -1 when the sensor is not a plain red/green/blue Bayer mosaic.
int bayerPhase(const RawImage& img) {
    if (!img.isCfa || img.cfaRows != 2 || img.cfaCols != 2 || img.colorPlanes != 3) return -1;
    // The stored pattern starts at the active area; shift it to the frame origin.
    auto colourAt = [&](uint32_t row, uint32_t col) {
        const uint32_t r = (row + img.activeTop) & 1, c = (col + img.activeLeft) & 1;
        return img.planeColor[img.cfaPattern[r * 2 + c]];
    };
    const uint8_t c00 = colourAt(0, 0), c01 = colourAt(0, 1);
    const uint8_t c10 = colourAt(1, 0), c11 = colourAt(1, 1);
    if (c00 == kRed && c01 == kGreen && c10 == kGreen && c11 == kBlue) return 0;
    if (c00 == kGreen && c01 == kRed && c10 == kBlue && c11 == kGreen) return 1;
    if (c00 == kGreen && c01 == kBlue && c10 == kRed && c11 == kGreen) return 2;
    if (c00 == kBlue && c01 == kGreen && c10 == kGreen && c11 == kRed) return 3;
    return -1;
}

// An opcode list holding a single FixBadPixelsConstant instruction: "every
// sample equal to `constant` is a defective pixel; interpolate over it".
// Opcode lists are big-endian whatever the byte order of the file.
std::vector<uint8_t> fixBadPixelsConstantOpcode(uint32_t constant, uint32_t phase) {
    std::vector<uint8_t> out;
    auto put = [&](uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
    };
    put(1);           // one opcode in the list
    put(4);           // FixBadPixelsConstant
    put(0x01030000);  // introduced in DNG 1.3
    put(1);           // optional: a reader may skip it and still show the image
    put(8);           // bytes of parameters
    put(constant);
    put(phase);
    return out;
}

// The maker note in the layout Adobe's DNG Converter stores it in
// DNGPrivateData, documented in the DNG specification: "Adobe\0", then a
// block tagged "MakN" holding the note's byte order, the position it had in
// the camera file, and the note itself. Readers use the stored position to
// resolve pointers inside the note wherever the block ends up.
std::vector<uint8_t> adobeMakerNoteBlock(const MakerNote& note) {
    std::vector<uint8_t> out = {'A', 'd', 'o', 'b', 'e', 0, 'M', 'a', 'k', 'N'};
    auto put32 = [&](uint32_t v) {  // big-endian, whatever the file's order
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
    };
    put32(static_cast<uint32_t>(note.data.size() + 6));
    out.push_back(note.bigEndian ? 'M' : 'I');
    out.push_back(note.bigEndian ? 'M' : 'I');
    put32(note.originalOffset);
    out.insert(out.end(), note.data.begin(), note.data.end());
    if (out.size() & 1) out.push_back(0);
    return out;
}

// Writes fields copied from the source into a directory. A field that does not
// hold together (length not matching type and count) is skipped, not fatal.
void copyFields(const std::vector<TiffField>& fields, TiffIfd& ifd, bool overwrite) {
    for (const TiffField& f : fields) {
        if (!overwrite && ifd.has(f.tag)) continue;
        try {
            ifd.setRaw(f.tag, f.type, f.count, f.data);
        } catch (const std::invalid_argument&) {
        }
    }
}

// NewRawImageDigest: a fingerprint of the stored raw samples that a reader can
// recompute to detect damage. The DNG specification defers to Adobe's SDK for
// the definition, which is: cut the frame into tiles of 256 x 256 pixels
// (smaller at the right and bottom edges, and never larger than the frame);
// take the MD5 of each tile's samples as little-endian 16-bit values, one
// colour plane after the other; then take the MD5 of all tile digests in
// row-major order.
Md5Digest newRawImageDigest(const RawImage& img, unsigned threads) {
    const uint32_t spp = img.samplesPerPixel;
    const uint32_t tileW = std::min<uint32_t>(256, img.width);
    const uint32_t tileH = std::min<uint32_t>(256, img.height);
    const uint32_t across = (img.width + tileW - 1) / tileW;
    const uint32_t down = (img.height + tileH - 1) / tileH;
    std::vector<Md5Digest> tileDigests(static_cast<size_t>(across) * down);

    parallelFor(tileDigests.size(), threads, [&](size_t index) {
        const uint32_t left = static_cast<uint32_t>(index % across) * tileW;
        const uint32_t top = static_cast<uint32_t>(index / across) * tileH;
        const uint32_t w = std::min(tileW, img.width - left);
        const uint32_t h = std::min(tileH, img.height - top);
        std::vector<uint8_t> bytes(static_cast<size_t>(w) * h * spp * 2);
        size_t at = 0;
        for (uint32_t plane = 0; plane < spp; ++plane)
            for (uint32_t y = 0; y < h; ++y) {
                const uint16_t* row =
                    &img.pixels[(static_cast<size_t>(top + y) * img.width + left) * spp + plane];
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t v = row[static_cast<size_t>(x) * spp];
                    bytes[at++] = static_cast<uint8_t>(v & 0xff);
                    bytes[at++] = static_cast<uint8_t>(v >> 8);
                }
            }
        tileDigests[index] = Md5::of(bytes.data(), bytes.size());
    });

    Md5 all;
    for (const Md5Digest& d : tileDigests) all.update(d.data(), d.size());
    return all.finish();
}

// RawDataUniqueID: the same for every DNG made from the same exposure, and
// different otherwise. Built from the image digest plus the things that
// change how the data is to be read: camera model, default crop, opcodes.
Md5Digest rawDataUniqueId(const RawImage& img, const Md5Digest& imageDigest,
                          const std::vector<uint8_t>& opcodes) {
    Md5 md5;
    md5.update(imageDigest.data(), imageDigest.size());
    md5.update(img.uniqueCameraModel.data(), img.uniqueCameraModel.size());
    for (uint32_t v : {img.activeTop, img.activeLeft, img.cropLeft, img.cropTop, img.cropWidth,
                       img.cropHeight}) {
        const uint8_t le[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                               static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
        md5.update(le, 4);
    }
    md5.update(opcodes.data(), opcodes.size());
    return md5.finish();
}

std::vector<uint8_t> digestBytes(const Md5Digest& d) { return {d.begin(), d.end()}; }

bool allIntegers(const std::vector<double>& v) {
    return std::all_of(v.begin(), v.end(), [](double x) {
        return x >= 0 && x <= 4294967295.0 && std::floor(x) == x;
    });
}

}  // namespace

void validate(const RawImage& img) {
    auto fail = [](const char* what) { throw std::invalid_argument(std::string("raw image: ") + what); };
    if (img.width == 0 || img.height == 0) fail("empty frame");
    if (img.samplesPerPixel < 1 || img.samplesPerPixel > 4) fail("1 to 4 samples per pixel supported");
    if (img.pixels.size() != img.sampleCount()) fail("pixel buffer does not match the frame size");
    if (img.colorPlanes < 1 || img.colorPlanes > 4) fail("1 to 4 colour planes supported");
    if (img.isCfa) {
        if (img.samplesPerPixel != 1) fail("a mosaic has one sample per pixel");
        if (img.cfaRows < 1 || img.cfaCols < 1 || img.cfaRows > 8 || img.cfaCols > 8)
            fail("colour filter pattern must be between 1x1 and 8x8");
        if (img.cfaPattern.size() != static_cast<size_t>(img.cfaRows) * img.cfaCols)
            fail("colour filter pattern has the wrong length");
        for (uint8_t p : img.cfaPattern)
            if (p >= img.colorPlanes) fail("colour filter pattern refers to a missing plane");
        if (img.colorPlanes < 3) fail("a mosaic needs at least three colour planes");
    } else if (img.colorPlanes != img.samplesPerPixel) {
        fail("linear raw data needs one colour plane per sample");
    }
    if (img.activeBottom > img.height || img.activeRight > img.width ||
        img.activeTop >= img.activeBottom || img.activeLeft >= img.activeRight)
        fail("active area lies outside the frame");
    const uint32_t aw = img.activeRight - img.activeLeft, ah = img.activeBottom - img.activeTop;
    if (img.cropWidth == 0 || img.cropHeight == 0 || img.cropLeft > aw || img.cropTop > ah ||
        img.cropWidth > aw - img.cropLeft || img.cropHeight > ah - img.cropTop)
        fail("default crop lies outside the active area");
    if (img.blackRows < 1 || img.blackCols < 1 || img.blackRows > 8 || img.blackCols > 8)
        fail("black level pattern must be between 1x1 and 8x8");
    if (img.blackLevel.size() !=
        static_cast<size_t>(img.blackRows) * img.blackCols * img.samplesPerPixel)
        fail("black level pattern has the wrong length");
    if (img.orientation < 1 || img.orientation > 8) fail("orientation must be 1..8");
    if (!(img.scaleH > 0) || !(img.scaleV > 0)) fail("pixel scale must be positive");
    if (img.colorPlanes >= 3 && !img.hasColorMatrix) fail("colour images need a colour matrix");
}

void writeDng(const RawImage& img, std::ostream& out, const DngWriteOptions& options) {
    validate(img);
    if (options.compression == DngCompression::LosslessJpeg &&
        (options.tileSize < 64 || options.tileSize > 8192 || (options.tileSize & 1)))
        throw std::invalid_argument("tile size must be an even number between 64 and 8192");

    const MakerNote& note = img.source.makerNote;
    const bool withMakerNote = options.makerNotes && note.data.size() > 4 &&
                               note.data.size() < 0xfffffff0u;
    const bool bigEndian = options.byteOrder == DngByteOrder::Big ||
                           (options.byteOrder == DngByteOrder::MatchSource && withMakerNote &&
                            note.bigEndian);

    TiffWriter tiff;
    TiffIfd& ifd0 = tiff.addIfd();

    JpegInfo preview;
    const bool withPreview =
        options.embedPreview && !img.previewJpeg.empty() && parseJpeg(img.previewJpeg, preview);
    RgbImage thumbnail;
    if (options.embedThumbnail) thumbnail = renderThumbnail(img);
    const bool withThumbnail = !thumbnail.pixels.empty();

    // The layout Adobe's converter uses and file browsers expect:
    //   IFD0       small uncompressed thumbnail, plus all camera metadata
    //   SubIFD 0   the raw image
    //   SubIFD 1   the camera's full-size JPEG preview
    // Without a thumbnail the JPEG preview takes IFD0; with neither, the raw
    // image is IFD0 itself.
    TiffIfd& raw = (withThumbnail || withPreview) ? tiff.addIfd() : ifd0;
    TiffIfd& previewIfd = (withThumbnail && withPreview) ? tiff.addIfd() : ifd0;

    // ---- raw image ----------------------------------------------------------
    const uint32_t spp = img.samplesPerPixel;
    raw.setLong(tag::NewSubFileType, 0);
    raw.setLong(tag::ImageWidth, img.width);
    raw.setLong(tag::ImageLength, img.height);
    raw.setShorts(tag::BitsPerSample, std::vector<uint16_t>(spp, 16));
    raw.setShort(tag::PhotometricInterpretation,
                 img.isCfa ? kPhotometricCfa : kPhotometricLinearRaw);
    raw.setShort(tag::SamplesPerPixel, static_cast<uint16_t>(spp));
    raw.setShort(tag::PlanarConfiguration, 1);

    if (options.compression == DngCompression::LosslessJpeg) {
        const TileLayout tile = chooseTileLayout(img, options.tileSize);
        raw.setShort(tag::Compression, kCompressionJpeg);
        raw.setLong(tag::TileWidth, tile.width);
        raw.setLong(tag::TileLength, tile.height);
        raw.setChunks(tag::TileOffsets, tag::TileByteCounts,
                      compressTiles(img, tile, options.threads));
    } else {
        raw.setShort(tag::Compression, kCompressionNone);
        raw.setLong(tag::RowsPerStrip, img.height);
        std::vector<std::vector<uint8_t>> strip;
        strip.push_back(sampleBytes(img.pixels, bigEndian));
        raw.setChunks(tag::StripOffsets, tag::StripByteCounts, std::move(strip));
    }

    if (img.isCfa) {
        raw.setShorts(tag::CFARepeatPatternDim, {static_cast<uint16_t>(img.cfaRows),
                                                 static_cast<uint16_t>(img.cfaCols)});
        std::vector<uint8_t> pattern;
        for (uint8_t plane : img.cfaPattern) pattern.push_back(img.planeColor[plane]);
        raw.setBytes(tag::CFAPattern, pattern);
        raw.setBytes(tag::CFAPlaneColor,
                     std::vector<uint8_t>(img.planeColor.begin(),
                                          img.planeColor.begin() + img.colorPlanes));
        raw.setShort(tag::CFALayout, 1);  // rectangular
    }

    raw.setShorts(tag::BlackLevelRepeatDim, {static_cast<uint16_t>(img.blackRows),
                                             static_cast<uint16_t>(img.blackCols)});
    if (allIntegers(img.blackLevel)) {
        std::vector<uint32_t> black;
        for (double v : img.blackLevel) black.push_back(static_cast<uint32_t>(v));
        raw.setLongs(tag::BlackLevel, black);
    } else {
        raw.setRationals(tag::BlackLevel, img.blackLevel);
    }
    {
        std::vector<uint32_t> white;
        for (uint32_t k = 0; k < spp; ++k) {
            // For a mosaic the single sample carries every plane; use the
            // lowest saturation point so no channel clips unnoticed.
            double w = img.whiteLevel[k];
            if (img.isCfa)
                for (uint32_t p = 1; p < img.colorPlanes; ++p) w = std::min(w, img.whiteLevel[p]);
            white.push_back(static_cast<uint32_t>(std::clamp(std::round(w), 1.0, 65535.0)));
        }
        raw.setLongs(tag::WhiteLevel, white);
    }
    raw.setRationals(tag::DefaultScale, {img.scaleH, img.scaleV});
    raw.setLongs(tag::DefaultCropOrigin, {img.cropLeft, img.cropTop});
    raw.setLongs(tag::DefaultCropSize, {img.cropWidth, img.cropHeight});
    raw.setLongs(tag::ActiveArea,
                 {img.activeTop, img.activeLeft, img.activeBottom, img.activeRight});

    std::vector<uint8_t> opcodeList1;
    if (img.zeroIsBadPixel) {
        const int phase = bayerPhase(img);
        if (phase >= 0) opcodeList1 = fixBadPixelsConstantOpcode(0, static_cast<uint32_t>(phase));
    }
    const bool hasOpcodes = !opcodeList1.empty();
    if (hasOpcodes) raw.setUndefined(tag::OpcodeList1, opcodeList1);

    // ---- thumbnail --------------------------------------------------------------
    if (withThumbnail) {
        ifd0.setLong(tag::NewSubFileType, 1);  // reduced-resolution image
        ifd0.setLong(tag::ImageWidth, thumbnail.width);
        ifd0.setLong(tag::ImageLength, thumbnail.height);
        ifd0.setShorts(tag::BitsPerSample, {8, 8, 8});
        ifd0.setShort(tag::Compression, kCompressionNone);
        ifd0.setShort(tag::PhotometricInterpretation, kPhotometricRgb);
        ifd0.setShort(tag::SamplesPerPixel, 3);
        ifd0.setLong(tag::RowsPerStrip, thumbnail.height);
        ifd0.setShort(tag::PlanarConfiguration, 1);
        std::vector<std::vector<uint8_t>> strip;
        strip.push_back(std::move(thumbnail.pixels));
        ifd0.setChunks(tag::StripOffsets, tag::StripByteCounts, std::move(strip));
    }

    // ---- JPEG preview -------------------------------------------------------------
    if (withPreview) {
        previewIfd.setLong(tag::NewSubFileType, 1);  // reduced-resolution image
        previewIfd.setLong(tag::ImageWidth, preview.width);
        previewIfd.setLong(tag::ImageLength, preview.height);
        previewIfd.setShorts(tag::BitsPerSample, {8, 8, 8});
        previewIfd.setShort(tag::Compression, kCompressionJpeg);
        previewIfd.setShort(tag::PhotometricInterpretation, kPhotometricYCbCr);
        previewIfd.setShort(tag::SamplesPerPixel, 3);
        previewIfd.setLong(tag::RowsPerStrip, preview.height);
        previewIfd.setShort(tag::PlanarConfiguration, 1);
        previewIfd.setShorts(tag::YCbCrSubSampling, {static_cast<uint16_t>(preview.subH),
                                                     static_cast<uint16_t>(preview.subV)});
        previewIfd.setShort(tag::YCbCrPositioning, 1);
        previewIfd.setRationals(tag::ReferenceBlackWhite, {0, 255, 128, 255, 128, 255});
        std::vector<std::vector<uint8_t>> strip;
        strip.push_back(img.previewJpeg);
        previewIfd.setChunks(tag::StripOffsets, tag::StripByteCounts, std::move(strip));
    }

    if (withThumbnail && withPreview)
        ifd0.setSubIfds(tag::SubIFDs, {&raw, &previewIfd});
    else if (withThumbnail || withPreview)
        ifd0.setSubIfds(tag::SubIFDs, {&raw});

    // ---- fingerprints -------------------------------------------------------------
    if (options.digests) {
        const Md5Digest imageDigest = newRawImageDigest(img, options.threads);
        ifd0.setBytes(tag::NewRawImageDigest, digestBytes(imageDigest));
        ifd0.setBytes(tag::RawDataUniqueID,
                      digestBytes(rawDataUniqueId(img, imageDigest, opcodeList1)));
    }

    // ---- camera and picture description (always in IFD0) ------------------------
    ifd0.setBytes(tag::DNGVersion, {1, 4, 0, 0});
    // Opcode lists arrived with DNG 1.3; everything else here is 1.1 material.
    ifd0.setBytes(tag::DNGBackwardVersion, {1, static_cast<uint8_t>(hasOpcodes ? 3 : 1), 0, 0});
    ifd0.setShort(tag::Orientation, static_cast<uint16_t>(img.orientation));
    if (!img.make.empty()) ifd0.setAscii(tag::Make, img.make);
    if (!img.model.empty()) ifd0.setAscii(tag::Model, img.model);
    ifd0.setAscii(tag::UniqueCameraModel,
                  img.uniqueCameraModel.empty() ? std::string("Unknown camera")
                                                : img.uniqueCameraModel);
    if (!options.software.empty()) ifd0.setAscii(tag::Software, options.software);
    if (!img.dateTime.empty()) ifd0.setAscii(tag::DateTime, img.dateTime);
    if (!img.artist.empty()) ifd0.setAscii(tag::Artist, img.artist);
    if (!img.description.empty()) ifd0.setAscii(tag::ImageDescription, img.description);
    // Copyright, XMP and anything descriptive the decoder did not report.
    copyFields(img.source.ifd0, ifd0, false);
    if (!img.bodySerial.empty()) ifd0.setAscii(tag::CameraSerialNumber, img.bodySerial);
    if (!img.originalFileName.empty())
        ifd0.setAscii(tag::OriginalRawFileName, img.originalFileName);
    // Focal length range and maximum apertures; an unknown aperture is NaN -> 0/0.
    const auto orUnknown = [](double v) { return v > 0 ? v : std::nan(""); };
    const std::vector<double> lensInfo = {img.lensInfo[0], img.lensInfo[1],
                                          orUnknown(img.lensInfo[2]), orUnknown(img.lensInfo[3])};
    const bool hasLensInfo = img.lensInfo[0] > 0 && img.lensInfo[1] > 0;
    if (hasLensInfo) ifd0.setRationals(tag::LensInfo, lensInfo);

    if (img.colorPlanes >= 3) {
        std::vector<double> matrix;
        for (uint32_t p = 0; p < img.colorPlanes; ++p)
            for (int k = 0; k < 3; ++k) matrix.push_back(img.colorMatrix[p][k]);
        ifd0.setSRationals(tag::ColorMatrix1, matrix);
        ifd0.setShort(tag::CalibrationIlluminant1, kIlluminantD65);
        if (img.hasAsShotNeutral)
            ifd0.setRationals(tag::AsShotNeutral,
                              std::vector<double>(img.asShotNeutral.begin(),
                                                  img.asShotNeutral.begin() + img.colorPlanes));
    }

    // ---- EXIF -----------------------------------------------------------------
    TiffIfd exif;  // filled first, attached only if it has content
    if (img.exposureTime > 0) exif.setRational(tag::ExposureTime, img.exposureTime);
    if (img.fNumber > 0) exif.setRational(tag::FNumber, img.fNumber);
    if (img.iso > 0) {
        const double iso = std::round(img.iso);
        if (iso <= 65535) {
            exif.setShort(tag::ISOSpeedRatings, static_cast<uint16_t>(iso));
        } else {
            exif.setShort(tag::ISOSpeedRatings, 65535);
            exif.setShort(tag::SensitivityType, 2);  // recommended exposure index
            exif.setLong(tag::RecommendedExposureIndex,
                         static_cast<uint32_t>(std::min(iso, 4294967295.0)));
        }
    }
    if (!img.dateTime.empty()) {
        exif.setAscii(tag::DateTimeOriginal, img.dateTime);
        exif.setAscii(tag::DateTimeDigitized, img.dateTime);
    }
    if (img.focalLength > 0) exif.setRational(tag::FocalLength, img.focalLength);
    if (img.focalLength35 >= 1 && img.focalLength35 <= 65535)
        exif.setShort(tag::FocalLengthIn35mmFilm,
                      static_cast<uint16_t>(std::round(img.focalLength35)));
    if (!img.bodySerial.empty()) exif.setAscii(tag::BodySerialNumber, img.bodySerial);
    if (hasLensInfo) exif.setRationals(tag::LensSpecification, lensInfo);
    if (!img.lensMake.empty()) exif.setAscii(tag::LensMake, img.lensMake);
    if (!img.lensModel.empty()) exif.setAscii(tag::LensModel, img.lensModel);
    // The camera's own EXIF directory replaces the values above wherever it
    // has them, and adds everything else it holds.
    copyFields(img.source.exif, exif, true);

    if (withMakerNote) {
        // Stored twice, for two kinds of reader. In the EXIF directory the
        // note is put back at the file offset it had in the camera file, so
        // pointers inside it that count from the start of the file stay
        // valid. The private-data copy records that offset explicitly and
        // survives programs that rewrite the DNG and move things around.
        exif.setPinned(tag::MakerNote, 7, static_cast<uint32_t>(note.data.size()), note.data,
                       note.originalOffset);
        ifd0.setBytes(tag::DNGPrivateData, adobeMakerNoteBlock(note));
    }

    if (!exif.empty()) {
        if (!exif.has(tag::ExifVersion)) exif.setUndefined(tag::ExifVersion, {'0', '2', '3', '0'});
        TiffIfd& stored = tiff.addIfd();
        stored = std::move(exif);
        ifd0.setSubIfds(tag::ExifIFD, {&stored});
    }

    // ---- GPS ------------------------------------------------------------------
    if (!img.source.gps.empty()) {
        TiffIfd& gps = tiff.addIfd();
        copyFields(img.source.gps, gps, true);
        ifd0.setSubIfds(tag::GPSInfo, {&gps});
    } else if (img.gps.valid) {
        TiffIfd& gps = tiff.addIfd();
        gps.setBytes(tag::GPSVersionID, {2, 3, 0, 0});
        gps.setAscii(tag::GPSLatitudeRef, std::string(1, img.gps.latitudeRef));
        gps.setRationals(tag::GPSLatitude, {img.gps.latitude[0], img.gps.latitude[1],
                                            img.gps.latitude[2]});
        gps.setAscii(tag::GPSLongitudeRef, std::string(1, img.gps.longitudeRef));
        gps.setRationals(tag::GPSLongitude, {img.gps.longitude[0], img.gps.longitude[1],
                                             img.gps.longitude[2]});
        if (img.gps.hasAltitude) {
            gps.setBytes(tag::GPSAltitudeRef, {static_cast<uint8_t>(img.gps.altitudeBelowSea)});
            gps.setRational(tag::GPSAltitude, img.gps.altitude);
        }
        ifd0.setSubIfds(tag::GPSInfo, {&gps});
    }

    tiff.write(out, bigEndian);
}

void writeDng(const RawImage& img, const std::filesystem::path& path,
              const DngWriteOptions& options) {
    std::filesystem::path tmp = path;
    tmp += ".part";
    try {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create " + tmp.u8string());
        writeDng(img, out, options);
        out.close();
        if (!out) throw std::runtime_error("write error on " + tmp.u8string());
        std::filesystem::rename(tmp, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        throw;
    }
}

}  // namespace dngconv
