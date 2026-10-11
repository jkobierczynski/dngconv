// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "dng_check.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "dng_digest.hpp"
#include "jxl_codec.hpp"
#include "ljpeg92.hpp"
#include "original_raw.hpp"
#include "parallel.hpp"
#include "tiff_source.hpp"
#include "tiff_writer.hpp"  // tiffTypeSize

namespace dngconv {

namespace {

using namespace tiffsource;

constexpr uint16_t kNewSubFileType = 254;
constexpr uint16_t kImageWidth = 256;
constexpr uint16_t kImageLength = 257;
constexpr uint16_t kBitsPerSample = 258;
constexpr uint16_t kCompression = 259;
constexpr uint16_t kFillOrder = 266;
constexpr uint16_t kStripOffsets = 273;
constexpr uint16_t kSamplesPerPixel = 277;
constexpr uint16_t kRowsPerStrip = 278;
constexpr uint16_t kStripByteCounts = 279;
constexpr uint16_t kPlanarConfiguration = 284;
constexpr uint16_t kTileWidth = 322;
constexpr uint16_t kTileLength = 323;
constexpr uint16_t kTileOffsets = 324;
constexpr uint16_t kTileByteCounts = 325;
constexpr uint16_t kSubIfds = 330;
constexpr uint16_t kSampleFormat = 339;
constexpr uint16_t kDngVersion = 50706;
constexpr uint16_t kJpegTables = 347;
constexpr uint16_t kLinearizationTable = 50712;
constexpr uint16_t kRowInterleaveFactor = 50975;
constexpr uint16_t kColumnInterleaveFactor = 52547;
constexpr uint32_t kCompressionLossyJpeg = 34892;
constexpr uint32_t kCompressionJxl = 52546;
constexpr uint16_t kExtraCameraProfiles = 50933;
constexpr uint16_t kRawImageDigest = 50972;
constexpr uint16_t kNewRawImageDigest = 51111;

// Largest image this will hold in memory: 600 million samples, 1.2 GB.
constexpr uint64_t kMaxSamples = 600ull * 1000 * 1000;
constexpr uint32_t kMaxChunks = 1u << 20;

// Something about the file that makes checking impossible without it being
// damaged: a compression or layout this code does not decode.
struct Unsupported {
    std::string why;
};

// All SHORT or LONG values of an entry.
bool numbers(const Tiff& tiff, const Entry* e, uint32_t maxCount, std::vector<uint32_t>& out) {
    out.clear();
    if (!e || e->count == 0 || e->count > maxCount) return false;
    Entry entry = *e;
    if (entry.type == 13) entry.type = 4;
    if (entry.type != 3 && entry.type != 4) return false;
    std::vector<uint8_t> raw;
    if (!loadValue(tiff, entry, static_cast<uint64_t>(maxCount) * 4, raw)) return false;
    out.resize(entry.count);
    for (uint32_t i = 0; i < entry.count; ++i)
        out[i] = entry.type == 3 ? get16(&raw[2 * i], tiff.bigEndian) : get32(&raw[4 * i], tiff.bigEndian);
    return true;
}

// The first SHORT or LONG value of an entry.
bool number(const Tiff& tiff, const Entry* e, uint32_t& out) {
    std::vector<uint32_t> all;
    if (!numbers(tiff, e, 1 << 14, all)) return false;
    out = all[0];
    return true;
}

uint32_t numberOr(const Tiff& tiff, const std::vector<Entry>& ifd, uint16_t tag, uint32_t fallback) {
    uint32_t v = 0;
    return number(tiff, find(ifd, tag), v) ? v : fallback;
}

[[noreturn]] void damaged(const std::string& what) { throw std::runtime_error(what); }

// Unpacks one uncompressed strip or tile of `rows` x `columns` pixels.
void unpackChunk(const std::vector<uint8_t>& raw, uint32_t columns, uint32_t rows, uint32_t planes,
                 unsigned bits, bool bigEndian, std::vector<uint16_t>& out) {
    const size_t perRow = static_cast<size_t>(columns) * planes;
    const size_t rowBytes = (perRow * bits + 7) / 8;
    if (raw.size() < rowBytes * rows) damaged("the raw image data is cut short");
    out.resize(perRow * rows);
    for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t* p = &raw[static_cast<size_t>(y) * rowBytes];
        uint16_t* q = &out[static_cast<size_t>(y) * perRow];
        if (bits == 8) {
            for (size_t i = 0; i < perRow; ++i) q[i] = p[i];
        } else if (bits == 16) {
            for (size_t i = 0; i < perRow; ++i) q[i] = get16(p + 2 * i, bigEndian);
        } else {
            // Other depths are packed, most significant bit first, and every
            // row starts on a byte boundary.
            uint32_t acc = 0;
            unsigned have = 0;
            for (size_t i = 0; i < perRow; ++i) {
                while (have < bits) {
                    acc = (acc << 8) | *p++;
                    have += 8;
                }
                have -= bits;
                q[i] = static_cast<uint16_t>((acc >> have) & ((1u << bits) - 1));
            }
        }
    }
}

// How the raw image lies in the file: its size and the table of strips or
// tiles that hold it.
struct Layout {
    uint32_t width = 0, height = 0, planes = 1;
    unsigned bits = 0;
    uint32_t compression = 1;
    bool tiled = false;
    uint32_t chunkWidth = 0, chunkRows = 0;  // strips are tiles as wide as the image
    uint32_t across = 0, down = 0;
    std::vector<uint32_t> offsets, counts;
    size_t chunks() const { return static_cast<size_t>(across) * down; }
};

// Throws std::runtime_error for damage and Unsupported for sizes beyond what
// this code will hold in memory.
Layout readLayout(const Tiff& tiff, const std::vector<Entry>& ifd) {
    Layout l;
    if (!number(tiff, find(ifd, kImageWidth), l.width) || !number(tiff, find(ifd, kImageLength), l.height) ||
        l.width == 0 || l.height == 0)
        damaged("the raw image has no size");
    l.planes = numberOr(tiff, ifd, kSamplesPerPixel, 1);
    if (l.planes == 0 || l.planes > 8) damaged("the raw image has an impossible number of colour planes");
    if (static_cast<uint64_t>(l.width) * l.height * l.planes > kMaxSamples)
        throw Unsupported{"the image is too large to check"};

    std::vector<uint32_t> bitsPerPlane;
    if (!numbers(tiff, find(ifd, kBitsPerSample), 8, bitsPerPlane)) bitsPerPlane = {1};
    l.bits = bitsPerPlane[0];
    for (uint32_t b : bitsPerPlane)
        if (b != l.bits) throw Unsupported{"colour planes of different depths"};
    l.compression = numberOr(tiff, ifd, kCompression, 1);

    l.chunkWidth = l.width;
    l.chunkRows = l.height;
    l.tiled = find(ifd, kTileOffsets) != nullptr;
    if (l.tiled) {
        if (!number(tiff, find(ifd, kTileWidth), l.chunkWidth) || !number(tiff, find(ifd, kTileLength), l.chunkRows) ||
            l.chunkWidth == 0 || l.chunkRows == 0)
            damaged("the tile size of the raw image is missing");
        if (!numbers(tiff, find(ifd, kTileOffsets), kMaxChunks, l.offsets) ||
            !numbers(tiff, find(ifd, kTileByteCounts), kMaxChunks, l.counts))
            damaged("the tile table of the raw image cannot be read");
    } else {
        l.chunkRows = std::min(numberOr(tiff, ifd, kRowsPerStrip, l.height), l.height);
        if (l.chunkRows == 0) damaged("the raw image has strips without rows");
        if (!numbers(tiff, find(ifd, kStripOffsets), kMaxChunks, l.offsets) ||
            !numbers(tiff, find(ifd, kStripByteCounts), kMaxChunks, l.counts))
            damaged("the strip table of the raw image cannot be read");
    }
    l.across = (l.width + l.chunkWidth - 1) / l.chunkWidth;
    l.down = (l.height + l.chunkRows - 1) / l.chunkRows;
    if (l.offsets.size() < l.chunks() || l.counts.size() < l.chunks())
        damaged("the raw image lists fewer strips or tiles than it needs");
    if (static_cast<uint64_t>(l.chunkWidth) * l.chunkRows * l.planes > kMaxSamples)
        damaged("the raw image has tiles of an impossible size");
    return l;
}

// The bytes of one strip or tile. The file is one stream, so reads take
// turns; whatever is done with the bytes can run in parallel.
std::vector<uint8_t> readChunk(const Tiff& tiff, const Layout& l, size_t index, std::mutex& fileMutex) {
    if (l.counts[index] == 0) damaged("a strip or tile of the raw image is empty");
    if (static_cast<uint64_t>(l.offsets[index]) + l.counts[index] > tiff.limit - tiff.base)
        damaged("raw image data lies beyond the end of the file (the file may be cut short)");
    std::vector<uint8_t> raw(l.counts[index]);
    std::lock_guard<std::mutex> lock(fileMutex);
    if (!readAt(tiff, l.offsets[index], raw.data(), raw.size()))
        damaged("the raw image data cannot be read from the file");
    return raw;
}

// Where field f of an image dimension begins when its rows (or columns) are
// stored field by field: field f holds every `factor`-th line from line f on.
inline uint32_t fieldStart(uint32_t f, uint32_t size, uint32_t factor) {
    return f * (size / factor) + std::min(f, size % factor);
}

// Reads the raw image of a DNG as stored. Throws std::runtime_error for
// damage and Unsupported for forms this code does not decode.
void readStoredImage(const Tiff& tiff, const std::vector<Entry>& ifd, unsigned threads, StoredRawImage& image) {
    const Layout l = readLayout(tiff, ifd);
    const uint32_t width = l.width, height = l.height, planes = l.planes;
    const unsigned bits = l.bits;
    const uint32_t compression = l.compression;
    if (numberOr(tiff, ifd, kSampleFormat, 1) != 1) throw Unsupported{"floating-point raw data"};
    if (bits < 8 || bits > 16) throw Unsupported{std::to_string(bits) + " bits per sample"};
    if (planes > 1 && numberOr(tiff, ifd, kPlanarConfiguration, 1) != 1)
        throw Unsupported{"colour planes stored separately"};
    if (numberOr(tiff, ifd, kFillOrder, 1) != 1) throw Unsupported{"reversed bit order"};
    switch (compression) {
        case 1:
        case 7:
            break;
        case kCompressionJxl:
            if (!jxlAvailable()) throw Unsupported{"JPEG XL compression (this build has no libjxl)"};
            if (planes != 1 && planes != 3) damaged("JPEG XL data with other than one or three colour planes");
            break;
        case 8:
        case 32946:
            throw Unsupported{"Deflate compression"};
        case kCompressionLossyJpeg:
            throw Unsupported{"lossy JPEG compression"};
        default:
            throw Unsupported{"compression type " + std::to_string(compression)};
    }
    const uint32_t rowFactor = numberOr(tiff, ifd, kRowInterleaveFactor, 1);
    const uint32_t columnFactor = numberOr(tiff, ifd, kColumnInterleaveFactor, 1);
    if (rowFactor == 0 || columnFactor == 0 || rowFactor > height || columnFactor > width)
        damaged("the raw image has an impossible interleave factor");

    const uint32_t chunkWidth = l.chunkWidth, chunkRows = l.chunkRows, across = l.across;
    const bool tiled = l.tiled;

    image.width = width;
    image.height = height;
    image.planes = planes;
    image.bitsPerSample = bits;
    image.samples.assign(static_cast<size_t>(width) * height * planes, 0);

    std::mutex fileMutex;
    parallelFor(l.chunks(), threads, [&](size_t index) {
        const uint32_t left = static_cast<uint32_t>(index % across) * chunkWidth;
        const uint32_t top = static_cast<uint32_t>(index / across) * chunkRows;
        // A tile always has its full size, padded at the image's edges; the
        // last strip has only the rows that are left.
        const uint32_t rows = tiled ? chunkRows : std::min(chunkRows, height - top);
        const size_t perRow = static_cast<size_t>(chunkWidth) * planes;
        const std::vector<uint8_t> raw = readChunk(tiff, l, index, fileMutex);

        std::vector<uint16_t> decoded;
        if (compression == 1) {
            unpackChunk(raw, chunkWidth, rows, planes, bits, tiff.bigEndian, decoded);
        } else if (compression == kCompressionJxl) {
            JxlPicture picture;
            try {
                picture = decodeJxl(raw.data(), raw.size(), perRow * rows);
            } catch (const std::exception& e) {
                damaged(std::string("a tile of the raw image does not decode (") + e.what() + ")");
            }
            if (picture.width != chunkWidth || picture.height != rows || picture.channels != planes)
                damaged("a JPEG XL tile of the raw image has the wrong size");
            // Lossy data has no values of its own to compare: what comes out
            // depends on the decoder.
            if (!picture.originalValues) throw Unsupported{"lossy JPEG XL compression"};
            decoded = std::move(picture.samples);
        } else {
            LosslessJpegFrame frame;
            decoded = decodeLosslessJpeg(raw.data(), raw.size(), frame, perRow * rows * 2 + 64);
            // Writers are free in how they cut a tile into JPEG lines and
            // components (a mosaic is usually stored as half as many pixels
            // of two components); the samples come out in the same order.
            if (decoded.size() < perRow * rows) damaged("a tile of the raw image holds too few samples");
        }

        const uint32_t copyRows = std::min(rows, height - top);
        const size_t copy = static_cast<size_t>(std::min(chunkWidth, width - left)) * planes;
        for (uint32_t y = 0; y < copyRows; ++y)
            std::memcpy(&image.samples[(static_cast<size_t>(top + y) * width + left) * planes],
                        &decoded[static_cast<size_t>(y) * perRow], copy * sizeof(uint16_t));
    });

    // Rows and columns stored field by field (DNG 1.7 does this to a mosaic,
    // so that each colour forms a picture of its own) go back to their places.
    if (rowFactor > 1 || columnFactor > 1) {
        std::vector<uint32_t> storedColumn(width);
        for (uint32_t x = 0; x < width; ++x)
            storedColumn[x] = fieldStart(x % columnFactor, width, columnFactor) + x / columnFactor;
        std::vector<uint16_t> ordered(image.samples.size());
        parallelFor(height, threads, [&](size_t y) {
            const uint32_t row = static_cast<uint32_t>(y);
            const uint16_t* from =
                &image.samples[static_cast<size_t>(fieldStart(row % rowFactor, height, rowFactor) + row / rowFactor) * width * planes];
            uint16_t* to = &ordered[y * width * planes];
            for (uint32_t x = 0; x < width; ++x)
                for (uint32_t k = 0; k < planes; ++k) to[static_cast<size_t>(x) * planes + k] = from[static_cast<size_t>(storedColumn[x]) * planes + k];
        });
        image.samples = std::move(ordered);
    }
}

// The checksum DNG defines for raw data stored with JPEG XL or lossy JPEG:
// not of the pixels, which a lossy decoder does not reproduce bit for bit,
// but of the compressed data. It is the MD5 of the MD5s of the strips or
// tiles, in order, followed by that of the shared JPEG tables if there are
// any. Adobe's SDK uses it for JPEG XL whether the data is lossless or not.
Md5Digest compressedDataDigest(const Tiff& tiff, const std::vector<Entry>& ifd, const Layout& l, unsigned threads) {
    std::vector<Md5Digest> parts(l.chunks());
    std::mutex fileMutex;
    parallelFor(l.chunks(), threads, [&](size_t index) {
        const std::vector<uint8_t> raw = readChunk(tiff, l, index, fileMutex);
        parts[index] = Md5::of(raw.data(), raw.size());
    });
    if (const Entry* tables = find(ifd, kJpegTables); tables && l.compression == kCompressionLossyJpeg) {
        std::vector<uint8_t> raw;
        if (!loadValue(tiff, *tables, 1u << 20, raw)) damaged("the JPEG tables of the raw image cannot be read");
        parts.push_back(Md5::of(raw.data(), raw.size()));
    }
    Md5 all;
    for (const Md5Digest& d : parts) all.update(d.data(), d.size());
    return all.finish();
}

// The 16 bytes of a digest tag, or false if the tag is absent or malformed.
bool storedDigest(const Tiff& tiff, const std::vector<Entry>& ifd0, uint16_t tag, Md5Digest& out) {
    const Entry* e = find(ifd0, tag);
    std::vector<uint8_t> raw;
    if (!e || e->count != 16 || (e->type != 1 && e->type != 7) || !loadValue(tiff, *e, 16, raw)) return false;
    std::copy(raw.begin(), raw.end(), out.begin());
    return true;
}

// The directories of a DNG that matter here.
struct Opened {
    Tiff tiff;
    std::vector<Entry> ifd0;
    std::vector<std::vector<Entry>> directories;  // IFD0 and its sub-directories
    const std::vector<Entry>* rawIfd = nullptr;   // the full-resolution image
    bool hasMask = false;
};

void openDng(Source& src, Opened& dng) {
    if (src.size() == 0) throw std::runtime_error("cannot read the file");
    if (!openTiff(src, 0, src.size(), dng.tiff) || !readIfd(dng.tiff, dng.tiff.firstIfd, dng.ifd0) ||
        !find(dng.ifd0, kDngVersion))
        throw std::runtime_error("not a DNG file");
    dng.directories = {dng.ifd0};
    std::vector<uint32_t> subOffsets;
    if (numbers(dng.tiff, find(dng.ifd0, kSubIfds), 64, subOffsets))
        for (uint32_t offset : subOffsets) {
            std::vector<Entry> dir;
            if (readIfd(dng.tiff, offset, dir)) dng.directories.push_back(std::move(dir));
        }
    // The raw image is the directory marked as the full-resolution image.
    for (const auto& dir : dng.directories) {
        const uint32_t kind = numberOr(dng.tiff, dir, kNewSubFileType, 0xffffffffu);
        if (kind == 0 && !dng.rawIfd && find(dir, kImageWidth)) dng.rawIfd = &dir;
        if (kind == 4) dng.hasMask = true;
    }
}

// The size the file must at least have for everything its directories refer
// to: the values of all entries, the image data of the raw image and of
// every preview, and the extra camera profiles.
uint64_t requiredSize(const Opened& dng) {
    const Tiff& tiff = dng.tiff;
    std::vector<std::vector<Entry>> all = dng.directories;
    for (uint16_t pointer : {uint16_t{34665}, uint16_t{34853}}) {  // EXIF and GPS
        uint32_t offset = 0;
        std::vector<Entry> dir;
        if (number(tiff, find(dng.ifd0, pointer), offset) && readIfd(tiff, offset, dir)) all.push_back(std::move(dir));
    }
    uint64_t needed = 0;
    for (const auto& dir : all) {
        for (const Entry& e : dir) {
            const uint64_t size = static_cast<uint64_t>(e.type == 13 ? 4 : tiffTypeSize(e.type)) * e.count;
            if (size > 4) needed = std::max(needed, get32(e.value, tiff.bigEndian) + size);
        }
        // Image data: strips, tiles, or an old-style JPEG preview.
        const uint16_t pairs[3][2] = {{kStripOffsets, kStripByteCounts}, {kTileOffsets, kTileByteCounts}, {513, 514}};
        for (const auto& pair : pairs) {
            std::vector<uint32_t> offsets, counts;
            if (!numbers(tiff, find(dir, pair[0]), kMaxChunks, offsets) ||
                !numbers(tiff, find(dir, pair[1]), kMaxChunks, counts))
                continue;
            for (size_t i = 0; i < offsets.size() && i < counts.size(); ++i)
                needed = std::max(needed, static_cast<uint64_t>(offsets[i]) + counts[i]);
        }
    }
    // Extra camera profiles: each a small directory structure of its own,
    // with offsets counted from its first byte.
    std::vector<uint32_t> profiles;
    if (numbers(tiff, find(dng.ifd0, kExtraCameraProfiles), 4096, profiles))
        for (const uint64_t at : profiles) {
            uint8_t header[8];
            needed = std::max<uint64_t>(needed, at + sizeof header);
            if (!tiff.src->read(at, header, sizeof header)) continue;
            const bool big = header[0] == 'M';
            const uint64_t dirAt = at + get32(header + 4, big);
            uint8_t countBytes[2];
            needed = std::max<uint64_t>(needed, dirAt + 2);
            if (!tiff.src->read(dirAt, countBytes, 2)) continue;
            const unsigned count = get16(countBytes, big);
            if (count > kMaxEntries) continue;
            needed = std::max<uint64_t>(needed, dirAt + 2 + 12ull * count + 4);
            std::vector<uint8_t> raw(12u * count);
            if (!tiff.src->read(dirAt + 2, raw.data(), raw.size())) continue;
            for (unsigned i = 0; i < count; ++i) {
                const uint8_t* e = &raw[12u * i];
                const uint64_t size = static_cast<uint64_t>(tiffTypeSize(get16(e + 2, big))) * get32(e + 4, big);
                if (size > 4) needed = std::max<uint64_t>(needed, at + get32(e + 8, big) + size);
            }
        }
    return needed;
}

}  // namespace

bool isDngFile(const std::filesystem::path& path) {
    try {
        Source src(path);
        Opened dng;
        openDng(src, dng);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

StoredRawImage readStoredRawImage(const std::filesystem::path& path, unsigned threads) {
    Source src(path);
    Opened dng;
    openDng(src, dng);
    if (!dng.rawIfd) throw std::runtime_error("the file has no raw image directory");
    StoredRawImage image;
    try {
        readStoredImage(dng.tiff, *dng.rawIfd, threads, image);
    } catch (const Unsupported& u) {
        throw std::runtime_error("cannot decode: " + u.why);
    }
    return image;
}

DngCheck checkDng(const std::filesystem::path& path, unsigned threads) {
    DngCheck result;
    Source src(path);
    Opened dng;
    openDng(src, dng);
    const Tiff& tiff = dng.tiff;
    const std::vector<Entry>& ifd0 = dng.ifd0;
    const std::vector<Entry>* rawIfd = dng.rawIfd;
    const bool hasMask = dng.hasMask;

    // ---- is the whole file there? ---------------------------------------------------
    if (const uint64_t needed = requiredSize(dng); needed > src.size()) {
        result.truncated = true;
        const uint64_t missing = needed - src.size();
        result.truncatedDetail = "the file is cut short: " + std::to_string(missing) +
                                 (missing == 1 ? " byte is" : " bytes are") + " missing at its end";
    }

    Md5Digest newDigest{}, oldDigest{};
    const bool hasNew = storedDigest(tiff, ifd0, kNewRawImageDigest, newDigest);
    const bool hasOld = storedDigest(tiff, ifd0, kRawImageDigest, oldDigest);

    if (!rawIfd) {
        result.rawImage = DngCheck::State::Damaged;
        result.rawDetail = "the file has no raw image directory";
    } else {
        try {
            const uint32_t compression = numberOr(tiff, *rawIfd, kCompression, 1);
            if (compression == kCompressionJxl || compression == kCompressionLossyJpeg) {
                // The checksum is of the compressed data; nothing needs decoding.
                const Layout l = readLayout(tiff, *rawIfd);
                result.width = l.width;
                result.height = l.height;
                result.planes = l.planes;
                result.bitsPerSample = l.bits;
                if (!hasNew && !hasOld) {
                    // Without a checksum, at least see whether it decodes.
                    if (compression == kCompressionJxl && jxlAvailable()) {
                        try {
                            StoredRawImage image;
                            readStoredImage(tiff, *rawIfd, threads, image);
                        } catch (const Unsupported&) {
                        }
                    }
                    result.rawImage = DngCheck::State::Unverified;
                    result.rawDetail = "the file carries no checksum of its raw data";
                } else if (hasMask) {
                    result.rawImage = DngCheck::State::Unverified;
                    result.rawDetail = "cannot check: the image has a transparency mask";
                } else {
                    const Md5Digest found = compressedDataDigest(tiff, *rawIfd, l, threads);
                    const bool ok = found == (hasNew ? newDigest : oldDigest);
                    result.rawImage = ok ? DngCheck::State::Intact : DngCheck::State::Damaged;
                    result.rawDetail = ok ? (hasNew ? "NewRawImageDigest" : "RawImageDigest")
                                          : "the raw image data does not match its checksum";
                }
            } else {
            StoredRawImage image;
            readStoredImage(tiff, *rawIfd, threads, image);
            result.width = image.width;
            result.height = image.height;
            result.planes = image.planes;
            result.bitsPerSample = image.bitsPerSample;

            // Data of up to 8 bits is hashed as bytes by the newer digest; so
            // is deeper data that a table of at most 256 entries expands.
            unsigned bytesPerSample = image.bitsPerSample <= 8 ? 1 : 2;
            if (const Entry* table = find(*rawIfd, kLinearizationTable); table && table->count <= 256)
                bytesPerSample = 1;

            if (!hasNew && !hasOld) {
                result.rawImage = DngCheck::State::Unverified;
                result.rawDetail = "the file carries no checksum of its raw data";
            } else if (hasNew && hasMask) {
                result.rawImage = DngCheck::State::Unverified;
                result.rawDetail = "cannot check: the image has a transparency mask";
            } else {
                bool ok = true;
                if (hasNew)
                    ok = newRawImageDigest(image.samples.data(), image.width, image.height, image.planes,
                                           bytesPerSample, threads) == newDigest;
                if (ok && hasOld)
                    ok = rawImageDigest(image.samples.data(), image.width, image.height, image.planes) == oldDigest;
                result.rawImage = ok ? DngCheck::State::Intact : DngCheck::State::Damaged;
                result.rawDetail = ok ? (hasNew ? "NewRawImageDigest" : "RawImageDigest")
                                      : "the raw image data does not match its checksum";
            }
            }
        } catch (const Unsupported& u) {
            result.rawImage = DngCheck::State::Unverified;
            result.rawDetail = "cannot check: " + u.why;
        } catch (const std::exception& e) {
            result.rawImage = DngCheck::State::Damaged;
            result.rawDetail = e.what();
        }
    }

    // ---- the embedded original ---------------------------------------------------
    try {
        const EmbeddedOriginal original = readEmbeddedOriginal(path, true);
        if (original.present) {
            if (original.hasDigest && !original.digestMatches) {
                result.original = DngCheck::State::Damaged;
                result.originalDetail = "the embedded original does not match its checksum";
            } else {
                // Unpack it too: the checksum covers the packed form, and a
                // file without one can only be judged by whether it unpacks.
                const std::vector<uint8_t> bytes = unpackOriginalRaw(original.packed.data(), original.packed.size());
                (void)bytes;
                result.original = original.hasDigest ? DngCheck::State::Intact : DngCheck::State::Unverified;
                result.originalDetail = original.hasDigest ? "OriginalRawFileDigest"
                                                           : "unpacks, but the file carries no checksum of it";
            }
        }
    } catch (const std::exception& e) {
        result.original = DngCheck::State::Damaged;
        result.originalDetail = e.what();
    }
    return result;
}

}  // namespace dngconv
