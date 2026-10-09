// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "tiff_source.hpp"

#include <algorithm>
#include <cstring>

#include "tiff_writer.hpp"  // tiffTypeSize

namespace dngconv {
namespace tiffsource {

Source::Source(const std::filesystem::path& path) : in_(path, std::ios::binary) {
    if (!in_) return;
    in_.seekg(0, std::ios::end);
    const auto end = in_.tellg();
    if (end > 0) size_ = static_cast<uint64_t>(end);
}

bool Source::read(uint64_t offset, void* dst, size_t count) {
    if (count == 0) return true;
    if (offset > size_ || count > size_ - offset) return false;
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset));
    in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(count));
    return static_cast<size_t>(in_.gcount()) == count;
}

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
               uint32_t* offset) {
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

const Entry* find(const std::vector<Entry>& entries, uint16_t tag) {
    for (const Entry& e : entries)
        if (e.tag == tag) return &e;
    return nullptr;
}

}  // namespace tiffsource
}  // namespace dngconv
