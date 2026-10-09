// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Careful reading of TIFF structures from files that may be damaged or
// hostile: every access is bounds-checked and reports failure instead of
// throwing. Shared by the source-metadata parser (camera raw files) and the
// reader for embedded originals (DNG files).
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace dngconv {
namespace tiffsource {

// Upper bound on directory entries; real directories have a few dozen.
constexpr unsigned kMaxEntries = 2000;

// Bounds-checked random access to a file.
class Source {
public:
    explicit Source(const std::filesystem::path& path);
    uint64_t size() const { return size_; }
    bool read(uint64_t offset, void* dst, size_t count);

private:
    std::ifstream in_;
    uint64_t size_ = 0;
};

uint16_t get16(const uint8_t* p, bool bigEndian);
uint32_t get32(const uint8_t* p, bool bigEndian);

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

/// Accepts plain TIFF and the Olympus ORF and Panasonic RW2 variants.
bool openTiff(Source& src, uint64_t base, uint64_t limit, Tiff& tiff);

/// Reads `count` bytes at `offset` within the TIFF structure.
bool readAt(const Tiff& tiff, uint64_t offset, void* dst, uint64_t count);

bool readIfd(const Tiff& tiff, uint32_t offset, std::vector<Entry>& entries);

/// Loads an entry's value in the source's byte order. `offset` receives the
/// position of an out-of-line value (0 for a value stored in the entry).
bool loadValue(const Tiff& tiff, const Entry& e, uint64_t maxBytes, std::vector<uint8_t>& out,
               uint32_t* offset = nullptr);

/// Turns a value from big-endian into little-endian, element by element.
void toLittleEndian(uint16_t type, std::vector<uint8_t>& data);

const Entry* find(const std::vector<Entry>& entries, uint16_t tag);

}  // namespace tiffsource
}  // namespace dngconv
