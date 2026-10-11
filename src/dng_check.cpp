// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "dng_check.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "dng_digest.hpp"
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
constexpr uint16_t kLinearizationTable = 50712;
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

// Reads the raw image of a DNG as stored. Throws std::runtime_error for
// damage and Unsupported for forms this code does not decode.
void readStoredImage(const Tiff& tiff, const std::vector<Entry>& ifd, unsigned threads, StoredRawImage& image) {
    uint32_t width = 0, height = 0;
    if (!number(tiff, find(ifd, kImageWidth), width) || !number(tiff, find(ifd, kImageLength), height) ||
        width == 0 || height == 0)
        damaged("the raw image has no size");
    const uint32_t planes = numberOr(tiff, ifd, kSamplesPerPixel, 1);
    if (planes == 0 || planes > 8) damaged("the raw image has an impossible number of colour planes");
    if (static_cast<uint64_t>(width) * height * planes > kMaxSamples)
        throw Unsupported{"the image is too large to check"};

    std::vector<uint32_t> bitsPerPlane;
    if (!numbers(tiff, find(ifd, kBitsPerSample), 8, bitsPerPlane)) bitsPerPlane = {1};
    const unsigned bits = bitsPerPlane[0];
    for (uint32_t b : bitsPerPlane)
        if (b != bits) throw Unsupported{"colour planes of different depths"};
    const uint32_t compression = numberOr(tiff, ifd, kCompression, 1);
    if (numberOr(tiff, ifd, kSampleFormat, 1) != 1) throw Unsupported{"floating-point raw data"};
    if (bits < 8 || bits > 16) throw Unsupported{std::to_string(bits) + " bits per sample"};
    if (planes > 1 && numberOr(tiff, ifd, kPlanarConfiguration, 1) != 1)
        throw Unsupported{"colour planes stored separately"};
    if (numberOr(tiff, ifd, kFillOrder, 1) != 1) throw Unsupported{"reversed bit order"};
    switch (compression) {
        case 1:
        case 7:
            break;
        case 8:
        case 32946:
            throw Unsupported{"Deflate compression"};
        case 34892:
            throw Unsupported{"lossy JPEG compression"};
        case 52546:
            throw Unsupported{"JPEG XL compression"};
        default:
            throw Unsupported{"compression type " + std::to_string(compression)};
    }

    // Strips are tiles as wide as the image.
    uint32_t chunkWidth = width, chunkRows = height;
    std::vector<uint32_t> offsets, counts;
    const bool tiled = find(ifd, kTileOffsets) != nullptr;
    if (tiled) {
        if (!number(tiff, find(ifd, kTileWidth), chunkWidth) || !number(tiff, find(ifd, kTileLength), chunkRows) ||
            chunkWidth == 0 || chunkRows == 0)
            damaged("the tile size of the raw image is missing");
        if (!numbers(tiff, find(ifd, kTileOffsets), kMaxChunks, offsets) ||
            !numbers(tiff, find(ifd, kTileByteCounts), kMaxChunks, counts))
            damaged("the tile table of the raw image cannot be read");
    } else {
        chunkRows = std::min(numberOr(tiff, ifd, kRowsPerStrip, height), height);
        if (chunkRows == 0) damaged("the raw image has strips without rows");
        if (!numbers(tiff, find(ifd, kStripOffsets), kMaxChunks, offsets) ||
            !numbers(tiff, find(ifd, kStripByteCounts), kMaxChunks, counts))
            damaged("the strip table of the raw image cannot be read");
    }
    const uint32_t across = (width + chunkWidth - 1) / chunkWidth;
    const uint32_t down = (height + chunkRows - 1) / chunkRows;
    const uint64_t chunks = static_cast<uint64_t>(across) * down;
    if (offsets.size() < chunks || counts.size() < chunks)
        damaged("the raw image lists fewer strips or tiles than it needs");
    if (static_cast<uint64_t>(chunkWidth) * chunkRows * planes > kMaxSamples)
        damaged("the raw image has tiles of an impossible size");

    image.width = width;
    image.height = height;
    image.planes = planes;
    image.bitsPerSample = bits;
    image.samples.assign(static_cast<size_t>(width) * height * planes, 0);

    std::mutex fileMutex;  // the file is one stream; the decoding is what runs in parallel
    parallelFor(static_cast<size_t>(chunks), threads, [&](size_t index) {
        const uint32_t left = static_cast<uint32_t>(index % across) * chunkWidth;
        const uint32_t top = static_cast<uint32_t>(index / across) * chunkRows;
        // A tile always has its full size, padded at the image's edges; the
        // last strip has only the rows that are left.
        const uint32_t rows = tiled ? chunkRows : std::min(chunkRows, height - top);
        const size_t perRow = static_cast<size_t>(chunkWidth) * planes;

        if (counts[index] == 0) damaged("a strip or tile of the raw image is empty");
        if (static_cast<uint64_t>(offsets[index]) + counts[index] > tiff.limit - tiff.base)
            damaged("raw image data lies beyond the end of the file (the file may be cut short)");
        std::vector<uint8_t> raw(counts[index]);
        {
            std::lock_guard<std::mutex> lock(fileMutex);
            if (!readAt(tiff, offsets[index], raw.data(), raw.size()))
                damaged("the raw image data cannot be read from the file");
        }

        std::vector<uint16_t> decoded;
        if (compression == 1) {
            unpackChunk(raw, chunkWidth, rows, planes, bits, tiff.bigEndian, decoded);
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
// to: the values of all entries, and the image data of the raw image and of
// every preview.
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
    return needed;
}

}  // namespace

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
