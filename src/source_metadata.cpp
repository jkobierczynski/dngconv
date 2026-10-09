// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "source_metadata.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "tiff_writer.hpp"  // tiffTypeSize

namespace dngconv {

namespace {

// TIFF and EXIF tags this file cares about.
constexpr uint16_t kImageDescription = 270;
constexpr uint16_t kMake = 271;
constexpr uint16_t kModel = 272;
constexpr uint16_t kArtist = 315;
constexpr uint16_t kXmp = 700;
constexpr uint16_t kCopyright = 33432;
constexpr uint16_t kExifIfd = 34665;
constexpr uint16_t kGpsIfd = 34853;
constexpr uint16_t kMakerNote = 37500;
constexpr uint16_t kPanasonicJpgFromRaw = 0x002e;

// Sanity limits. Real values are far smaller; these only stop a damaged file
// from making us allocate absurd amounts.
constexpr unsigned kMaxEntries = 2000;
constexpr uint64_t kMaxFieldBytes = 16ull << 20;
constexpr uint64_t kMaxMakerNoteBytes = 64ull << 20;

// Bounds-checked random access to the file. Every read reports failure
// instead of throwing; callers simply give up on the first failure.
class Source {
public:
    explicit Source(const std::filesystem::path& path) : in_(path, std::ios::binary) {
        if (!in_) return;
        in_.seekg(0, std::ios::end);
        const auto end = in_.tellg();
        if (end > 0) size_ = static_cast<uint64_t>(end);
    }

    uint64_t size() const { return size_; }

    bool read(uint64_t offset, void* dst, size_t count) {
        if (count == 0) return true;
        if (offset > size_ || count > size_ - offset) return false;
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(offset));
        in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(count));
        return static_cast<size_t>(in_.gcount()) == count;
    }

private:
    std::ifstream in_;
    uint64_t size_ = 0;
};

uint16_t get16(const uint8_t* p, bool bigEndian) {
    return bigEndian ? static_cast<uint16_t>((p[0] << 8) | p[1])
                     : static_cast<uint16_t>((p[1] << 8) | p[0]);
}

uint32_t get32(const uint8_t* p, bool bigEndian) {
    return bigEndian ? (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                           (static_cast<uint32_t>(p[2]) << 8) | p[3]
                     : (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
                           (static_cast<uint32_t>(p[1]) << 8) | p[0];
}

// Keeps printable ASCII up to the first NUL and trims blanks at both ends.
std::string cleanText(const uint8_t* data, size_t length) {
    std::string out;
    for (size_t i = 0; i < length && data[i] != 0; ++i)
        if (data[i] >= 0x20 && data[i] < 0x7f) out.push_back(static_cast<char>(data[i]));
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    return out.substr(b, out.find_last_not_of(' ') - b + 1);
}

// ---- TIFF structures --------------------------------------------------------

// A TIFF structure somewhere inside the file: everything in it is addressed
// relative to `base` and must end before `limit`.
struct Tiff {
    Source* src = nullptr;
    uint64_t base = 0;
    uint64_t limit = 0;
    bool bigEndian = false;
    uint32_t firstIfd = 0;
};

struct Entry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    uint8_t value[4] = {0, 0, 0, 0};  // the value itself, or its offset
};

bool openTiff(Source& src, uint64_t base, uint64_t limit, Tiff& tiff) {
    uint8_t header[8];
    if (limit < base || limit - base < 8 || !src.read(base, header, sizeof header)) return false;
    if (header[0] == 'I' && header[1] == 'I')
        tiff.bigEndian = false;
    else if (header[0] == 'M' && header[1] == 'M')
        tiff.bigEndian = true;
    else
        return false;
    // 42 = TIFF; 0x4f52 / 0x5352 = Olympus ORF; 0x55 = Panasonic RW2.
    const uint16_t magic = get16(header + 2, tiff.bigEndian);
    if (magic != 42 && magic != 0x4f52 && magic != 0x5352 && magic != 0x55) return false;
    tiff.src = &src;
    tiff.base = base;
    tiff.limit = limit;
    tiff.firstIfd = get32(header + 4, tiff.bigEndian);
    return true;
}

// Reads `count` bytes at `offset` within the TIFF structure.
bool readAt(const Tiff& tiff, uint64_t offset, void* dst, uint64_t count) {
    const uint64_t span = tiff.limit - tiff.base;
    if (offset > span || count > span - offset) return false;
    return tiff.src->read(tiff.base + offset, dst, static_cast<size_t>(count));
}

bool readIfd(const Tiff& tiff, uint32_t offset, std::vector<Entry>& entries) {
    entries.clear();
    uint8_t countBytes[2];
    if (offset < 8 || !readAt(tiff, offset, countBytes, 2)) return false;
    const unsigned count = get16(countBytes, tiff.bigEndian);
    if (count == 0 || count > kMaxEntries) return false;

    std::vector<uint8_t> raw(static_cast<size_t>(count) * 12);
    if (!readAt(tiff, static_cast<uint64_t>(offset) + 2, raw.data(), raw.size())) return false;
    entries.resize(count);
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t* e = &raw[static_cast<size_t>(i) * 12];
        entries[i].tag = get16(e, tiff.bigEndian);
        entries[i].type = get16(e + 2, tiff.bigEndian);
        entries[i].count = get32(e + 4, tiff.bigEndian);
        std::memcpy(entries[i].value, e + 8, 4);
    }
    return true;
}

// Loads an entry's value in the source's byte order. `offset` receives the
// position of an out-of-line value (0 for a value stored in the entry).
bool loadValue(const Tiff& tiff, const Entry& e, uint64_t maxBytes, std::vector<uint8_t>& out,
               uint32_t* offset = nullptr) {
    const size_t unit = tiffTypeSize(e.type);
    if (unit == 0 || e.count == 0) return false;
    const uint64_t size = static_cast<uint64_t>(unit) * e.count;
    if (size > maxBytes) return false;
    out.resize(static_cast<size_t>(size));
    if (offset) *offset = 0;
    if (size <= 4) {
        std::memcpy(out.data(), e.value, static_cast<size_t>(size));
        return true;
    }
    const uint32_t where = get32(e.value, tiff.bigEndian);
    if (offset) *offset = where;
    return readAt(tiff, where, out.data(), size);
}

// Turns a value from big-endian into little-endian, element by element.
void toLittleEndian(uint16_t type, std::vector<uint8_t>& data) {
    size_t unit = tiffTypeSize(type);
    if (type == 5 || type == 10) unit = 4;  // rationals are pairs of 32-bit values
    if (unit < 2) return;
    for (size_t i = 0; i + unit <= data.size(); i += unit)
        std::reverse(data.begin() + i, data.begin() + i + unit);
}

bool contains(std::initializer_list<uint16_t> tags, uint16_t tag) {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

// Copies every entry of a directory for which `wanted(tag)` holds.
template <typename Predicate>
void copyDirectory(const Tiff& tiff, const std::vector<Entry>& entries, Predicate wanted,
                   std::vector<TiffField>& out) {
    for (const Entry& e : entries) {
        if (!wanted(e.tag)) continue;
        TiffField field;
        if (!loadValue(tiff, e, kMaxFieldBytes, field.data)) continue;
        if (tiff.bigEndian) toLittleEndian(e.type, field.data);
        field.tag = e.tag;
        field.type = e.type;
        field.count = e.count;
        out.push_back(std::move(field));
    }
}

const Entry* find(const std::vector<Entry>& entries, uint16_t tag) {
    for (const Entry& e : entries)
        if (e.tag == tag) return &e;
    return nullptr;
}

// Follows a pointer tag (EXIF or GPS directory) to the directory it names.
bool followPointer(const Tiff& tiff, const std::vector<Entry>& entries, uint16_t tag,
                   std::vector<Entry>& target) {
    const Entry* e = find(entries, tag);
    if (!e || e->count != 1 || (e->type != 4 && e->type != 13)) return false;
    return readIfd(tiff, get32(e->value, tiff.bigEndian), target);
}

void copyExif(const Tiff& tiff, const std::vector<Entry>& entries, SourceMetadata& meta) {
    copyDirectory(
        tiff, entries,
        [](uint16_t tag) {
            // Left out: the maker note (handled separately); the pointer to
            // the interoperability directory, which describes a JPEG; padding;
            // and a few fields whose inner layout depends on the byte order
            // of the file they sit in, so they cannot be moved safely.
            return !contains({kMakerNote, 40965 /* InteropIFD */, 59932 /* Padding */,
                              59933 /* OffsetSchema */, 41730 /* CFAPattern */,
                              34856 /* OECF */, 37388, 41484 /* SpatialFrequencyResponse */,
                              41995 /* DeviceSettingDescription */},
                             tag);
        },
        meta.exif);

    // LensSpecification: cameras write 0/1 for an aperture they do not know,
    // where the EXIF standard asks for 0/0. Same meaning, valid form.
    for (TiffField& f : meta.exif) {
        if (f.tag != 42034 || f.type != 5) continue;
        for (size_t i = 0; i + 8 <= f.data.size(); i += 8) {
            const bool zeroNumerator = !(f.data[i] | f.data[i + 1] | f.data[i + 2] | f.data[i + 3]);
            if (zeroNumerator) std::fill(f.data.begin() + i + 4, f.data.begin() + i + 8, uint8_t{0});
        }
    }

    if (const Entry* e = find(entries, kMakerNote)) {
        std::vector<uint8_t> note;
        uint32_t offset = 0;
        if (tiffTypeSize(e->type) == 1 && loadValue(tiff, *e, kMaxMakerNoteBytes, note, &offset) &&
            offset != 0) {
            meta.makerNote.data = std::move(note);
            meta.makerNote.bigEndian = tiff.bigEndian;
            meta.makerNote.originalOffset = offset;
        }
    }
}

void copyGps(const Tiff& tiff, const std::vector<Entry>& entries, SourceMetadata& meta) {
    std::vector<TiffField> gps;
    copyDirectory(tiff, entries, [](uint16_t) { return true; }, gps);
    // A directory holding nothing but its version number says nothing.
    const bool hasPosition =
        std::any_of(gps.begin(), gps.end(), [](const TiffField& f) { return f.tag != 0; });
    if (hasPosition) meta.gps = std::move(gps);
}

void copyDescriptive(const Tiff& tiff, const std::vector<Entry>& ifd0, SourceMetadata& meta) {
    copyDirectory(
        tiff, ifd0,
        [](uint16_t tag) { return contains({kImageDescription, kArtist, kCopyright, kXmp}, tag); },
        meta.ifd0);
}

void readIdentity(const Tiff& tiff, const std::vector<Entry>& ifd0, SourceMetadataResult& result) {
    for (uint16_t tag : {kMake, kModel}) {
        const Entry* e = find(ifd0, tag);
        std::vector<uint8_t> text;
        if (!e || e->type != 2 || !loadValue(tiff, *e, 512, text)) continue;
        (tag == kMake ? result.make : result.model) = cleanText(text.data(), text.size());
    }
}

void readTiffTree(Source& src, uint64_t base, uint64_t limit, SourceMetadataResult& result,
                  bool wantIdentity, bool followEmbeddedJpeg);

// Finds the EXIF segment of a JPEG stored in [begin, end) and reads the TIFF
// structure inside it.
void readJpegExif(Source& src, uint64_t begin, uint64_t end, SourceMetadataResult& result) {
    uint8_t marker[4];
    if (end < begin || end - begin < 4 || !src.read(begin, marker, 2) || marker[0] != 0xff ||
        marker[1] != 0xd8)
        return;
    uint64_t pos = begin + 2;
    for (int guard = 0; guard < 64 && pos + 4 <= end; ++guard) {
        if (!src.read(pos, marker, 4) || marker[0] != 0xff) return;
        if (marker[1] == 0xda || marker[1] == 0xd9) return;  // image data: no EXIF found
        const uint64_t length = get16(marker + 2, true);
        if (length < 2 || pos + 2 + length > end) return;
        uint8_t id[6];
        if (marker[1] == 0xe1 && length >= 16 && src.read(pos + 4, id, 6) &&
            std::memcmp(id, "Exif\0\0", 6) == 0) {
            readTiffTree(src, pos + 10, pos + 2 + length, result, false, false);
            return;
        }
        pos += 2 + length;
    }
}

// Adds what `extra` has and `meta` lacks.
void mergeMissing(SourceMetadata& meta, SourceMetadata&& extra) {
    auto addMissing = [](std::vector<TiffField>& into, std::vector<TiffField>& from) {
        for (TiffField& f : from) {
            const bool present = std::any_of(into.begin(), into.end(), [&](const TiffField& have) {
                return have.tag == f.tag;
            });
            if (!present) into.push_back(std::move(f));
        }
    };
    addMissing(meta.ifd0, extra.ifd0);
    addMissing(meta.exif, extra.exif);
    if (meta.gps.empty()) meta.gps = std::move(extra.gps);
    if (meta.makerNote.empty()) meta.makerNote = std::move(extra.makerNote);
}

// The usual arrangement: IFD0 with pointers to the EXIF and GPS directories,
// and the maker note inside the EXIF directory.
void readTiffTree(Source& src, uint64_t base, uint64_t limit, SourceMetadataResult& result,
                  bool wantIdentity, bool followEmbeddedJpeg) {
    Tiff tiff;
    std::vector<Entry> ifd0;
    if (!openTiff(src, base, limit, tiff) || !readIfd(tiff, tiff.firstIfd, ifd0)) return;

    if (wantIdentity) readIdentity(tiff, ifd0, result);
    copyDescriptive(tiff, ifd0, result.metadata);

    std::vector<Entry> sub;
    if (followPointer(tiff, ifd0, kExifIfd, sub)) copyExif(tiff, sub, result.metadata);
    if (followPointer(tiff, ifd0, kGpsIfd, sub)) copyGps(tiff, sub, result.metadata);

    // Panasonic RW2 keeps only a short EXIF directory of its own; the full
    // one, maker note included, sits in the JPEG preview stored in tag 0x2e.
    if (followEmbeddedJpeg) {
        const Entry* jpeg = find(ifd0, kPanasonicJpgFromRaw);
        if (jpeg && tiffTypeSize(jpeg->type) == 1 && jpeg->count > 4) {
            const uint64_t begin = base + get32(jpeg->value, tiff.bigEndian);
            if (begin < limit && jpeg->count <= limit - begin) {
                SourceMetadataResult fromJpeg;
                readJpegExif(src, begin, begin + jpeg->count, fromJpeg);
                mergeMissing(result.metadata, std::move(fromJpeg.metadata));
            }
        }
    }
}

// ---- Canon CR3 --------------------------------------------------------------

struct Box {
    uint64_t payload = 0;  // offset of the box contents
    uint64_t end = 0;      // offset just past the box
    char type[4] = {0, 0, 0, 0};
};

// Reads one ISO base-media box header at `offset`, inside [offset, limit).
bool readBox(Source& src, uint64_t offset, uint64_t limit, Box& box) {
    uint8_t h[16];
    if (limit < 8 || offset > limit - 8 || !src.read(offset, h, 8)) return false;
    uint64_t size = get32(h, true);
    uint64_t headerSize = 8;
    std::memcpy(box.type, h + 4, 4);
    if (size == 1) {
        if (!src.read(offset + 8, h + 8, 8)) return false;
        size = (static_cast<uint64_t>(get32(h + 8, true)) << 32) | get32(h + 12, true);
        headerSize = 16;
    } else if (size == 0) {
        size = limit - offset;
    }
    if (size < headerSize || size > limit - offset) return false;
    box.payload = offset + headerSize;
    box.end = offset + size;
    return true;
}

// Finds the first child box of the given type in [begin, end).
bool findBox(Source& src, uint64_t begin, uint64_t end, const char* type, Box& found) {
    uint64_t pos = begin;
    for (int guard = 0; guard < 256 && pos < end; ++guard) {
        Box box;
        if (!readBox(src, pos, end, box)) return false;
        if (std::memcmp(box.type, type, 4) == 0) {
            found = box;
            return true;
        }
        pos = box.end;
    }
    return false;
}

// A CR3 keeps its metadata in four separate TIFF blocks inside
// moov / uuid(Canon): CMT1 = IFD0, CMT2 = EXIF, CMT3 = maker note, CMT4 = GPS.
void readCr3(Source& src, SourceMetadataResult& result) {
    static const uint8_t canonUuid[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                                          0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
    Box moov;
    if (!findBox(src, 0, src.size(), "moov", moov)) return;

    Box canon;
    bool found = false;
    uint64_t pos = moov.payload;
    for (int guard = 0; guard < 256 && pos < moov.end && !found; ++guard) {
        Box box;
        if (!readBox(src, pos, moov.end, box)) return;
        uint8_t uuid[16];
        if (std::memcmp(box.type, "uuid", 4) == 0 && src.read(box.payload, uuid, 16) &&
            std::memcmp(uuid, canonUuid, 16) == 0) {
            canon = box;
            found = true;
        }
        pos = box.end;
    }
    if (!found) return;
    const uint64_t begin = canon.payload + 16, end = canon.end;

    Box cmt;
    Tiff tiff;
    std::vector<Entry> entries;
    if (findBox(src, begin, end, "CMT1", cmt) && openTiff(src, cmt.payload, cmt.end, tiff) &&
        readIfd(tiff, tiff.firstIfd, entries)) {
        readIdentity(tiff, entries, result);
        copyDescriptive(tiff, entries, result.metadata);
    }
    if (findBox(src, begin, end, "CMT2", cmt) && openTiff(src, cmt.payload, cmt.end, tiff) &&
        readIfd(tiff, tiff.firstIfd, entries))
        copyExif(tiff, entries, result.metadata);
    if (findBox(src, begin, end, "CMT4", cmt) && openTiff(src, cmt.payload, cmt.end, tiff) &&
        readIfd(tiff, tiff.firstIfd, entries))
        copyGps(tiff, entries, result.metadata);

    // The maker note is the block's directory and everything after it. Its
    // pointers count from the start of the block, so the note's own position
    // in that frame of reference is the directory offset (normally 8).
    if (result.metadata.makerNote.empty() && findBox(src, begin, end, "CMT3", cmt) &&
        openTiff(src, cmt.payload, cmt.end, tiff)) {
        const uint64_t blockSize = cmt.end - cmt.payload;
        if (tiff.firstIfd >= 8 && tiff.firstIfd < blockSize &&
            blockSize - tiff.firstIfd <= kMaxMakerNoteBytes) {
            std::vector<uint8_t> note(static_cast<size_t>(blockSize - tiff.firstIfd));
            if (src.read(cmt.payload + tiff.firstIfd, note.data(), note.size())) {
                result.metadata.makerNote.data = std::move(note);
                result.metadata.makerNote.bigEndian = tiff.bigEndian;
                result.metadata.makerNote.originalOffset = tiff.firstIfd;
            }
        }
    }
}

// ---- Fuji RAF ---------------------------------------------------------------

// A RAF has no TIFF metadata of its own; the camera's EXIF lives in the JPEG
// preview embedded near the start of the file.
void readRaf(Source& src, const uint8_t* head, SourceMetadataResult& result) {
    // Fixed header: magic(16) version(4) camera id(8) model(32) ...
    result.make = "FUJIFILM";
    result.model = cleanText(head + 28, 32);

    uint8_t dir[8];
    if (!src.read(84, dir, sizeof dir)) return;
    const uint64_t jpeg = get32(dir, true), jpegLength = get32(dir + 4, true);
    if (jpeg == 0 || jpeg > src.size() || jpegLength > src.size() - jpeg) return;
    readJpegExif(src, jpeg, jpeg + jpegLength, result);
}

}  // namespace

SourceMetadataResult readSourceMetadata(const std::filesystem::path& path) {
    SourceMetadataResult result;
    try {
        Source src(path);
        uint8_t head[64] = {0};
        if (src.size() < sizeof head || !src.read(0, head, sizeof head)) return {};

        if (std::memcmp(head, "FUJIFILMCCD-RAW ", 16) == 0)
            readRaf(src, head, result);
        else if (std::memcmp(head + 4, "ftyp", 4) == 0)
            readCr3(src, result);
        else
            readTiffTree(src, 0, src.size(), result, true, true);
    } catch (...) {
        return {};
    }
    if (!result.identityFound()) {
        result.make.clear();
        result.model.clear();
    }
    return result;
}

}  // namespace dngconv
