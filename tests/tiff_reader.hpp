// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Test-only parser for the TIFF files the writer produces, in either byte
// order. Deliberately independent of the writer's code.
#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "test_util.hpp"

namespace tiffreader {

struct Entry {
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t offset = 0;        // file offset of the value; 0 if stored inline
    std::vector<uint8_t> data;  // always little-endian here, whatever the file

    uint32_t u32(size_t index = 0) const {
        uint32_t v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | data.at(index * 4 + i);
        return v;
    }
    uint16_t u16(size_t index = 0) const {
        return static_cast<uint16_t>(data.at(index * 2) | (data.at(index * 2 + 1) << 8));
    }
    /// SHORT or LONG, whichever the entry holds.
    uint32_t number(size_t index = 0) const { return type == 3 ? u16(index) : u32(index); }
    std::string text() const {
        std::string s(data.begin(), data.end());
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
};

using Ifd = std::map<uint16_t, Entry>;

inline bool isBigEndian(const std::string& f) { return f.size() > 1 && f[0] == 'M' && f[1] == 'M'; }

inline uint16_t rd16(const std::string& f, size_t p) {
    const unsigned a = static_cast<uint8_t>(f.at(p)), b = static_cast<uint8_t>(f.at(p + 1));
    return static_cast<uint16_t>(isBigEndian(f) ? (a << 8) | b : (b << 8) | a);
}

inline uint32_t rd32(const std::string& f, size_t p) {
    const uint32_t a = rd16(f, p), b = rd16(f, p + 2);
    return isBigEndian(f) ? (a << 16) | b : (b << 16) | a;
}

inline Ifd parseIfd(const std::string& f, size_t offset, uint32_t* next = nullptr) {
    // bytes per value, and bytes per byte-swapped unit (rationals: two 32-bit halves)
    static const size_t typeSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    static const size_t swapUnit[] = {0, 1, 1, 2, 4, 4, 1, 1, 2, 4, 4, 4, 8};
    Ifd ifd;
    const uint16_t n = rd16(f, offset);
    uint16_t previousTag = 0;
    for (uint16_t i = 0; i < n; ++i) {
        const size_t p = offset + 2 + 12 * i;
        const uint16_t tag = rd16(f, p);
        CHECK(i == 0 || tag > previousTag);  // entries must be sorted
        previousTag = tag;
        Entry e;
        e.type = rd16(f, p + 2);
        e.count = rd32(f, p + 4);
        CHECK(e.type >= 1 && e.type <= 12);
        const size_t kind = e.type <= 12 ? e.type : 0;
        const size_t size = typeSize[kind] * e.count;
        size_t where = p + 8;
        if (size > 4) {
            where = rd32(f, p + 8);
            e.offset = static_cast<uint32_t>(where);
            CHECK(where % 2 == 0);  // values must start on a word boundary
        }
        CHECK(where + size <= f.size());
        if (where + size <= f.size()) e.data.assign(f.begin() + where, f.begin() + where + size);
        if (isBigEndian(f) && swapUnit[kind] > 1)
            for (size_t k = 0; k + swapUnit[kind] <= e.data.size(); k += swapUnit[kind])
                std::reverse(e.data.begin() + k, e.data.begin() + k + swapUnit[kind]);
        ifd[tag] = e;
    }
    if (next) *next = rd32(f, offset + 2 + 12 * n);
    return ifd;
}

inline Ifd parseFirstIfd(const std::string& f) {
    CHECK(f.size() > 8);
    CHECK((f[0] == 'I' && f[1] == 'I') || (f[0] == 'M' && f[1] == 'M'));
    CHECK(rd16(f, 2) == 42);
    return parseIfd(f, rd32(f, 4));
}

template <typename Path>
std::string slurp(const Path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

}  // namespace tiffreader
