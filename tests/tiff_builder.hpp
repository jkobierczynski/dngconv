// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Test-only builder for small TIFF structures: the tests assemble camera
// files, maker notes and odd DNGs with it, byte by byte, so that the parsers
// are tried on input this program's own writer did not produce.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace tiffbuilder {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;


struct Field {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    Bytes data;  // already in the byte order of the file
};

inline void put16(Bytes& b, uint32_t v, bool be) {
    if (be) {
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v));
    } else {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    }
}

inline void put32(Bytes& b, uint32_t v, bool be) {
    if (be) {
        put16(b, v >> 16, true);
        put16(b, v & 0xffff, true);
    } else {
        put16(b, v & 0xffff, false);
        put16(b, v >> 16, false);
    }
}

inline Field shorts(uint16_t tag, const std::vector<int>& v, bool be = false, uint16_t type = 8) {
    Field f{tag, type, static_cast<uint32_t>(v.size()), {}};
    for (int x : v) put16(f.data, static_cast<uint32_t>(x) & 0xffff, be);
    return f;
}

inline Field longs(uint16_t tag, const std::vector<uint32_t>& v, bool be = false, uint16_t type = 4) {
    Field f{tag, type, static_cast<uint32_t>(v.size()), {}};
    for (uint32_t x : v) put32(f.data, x, be);
    return f;
}

// Signed rationals with a fixed denominator.
inline Field rationals(uint16_t tag, const std::vector<double>& v, int32_t denominator = 1000000) {
    Field f{tag, 10, static_cast<uint32_t>(v.size()), {}};
    for (double x : v) {
        put32(f.data, static_cast<uint32_t>(static_cast<int32_t>(std::lround(x * denominator))), false);
        put32(f.data, static_cast<uint32_t>(denominator), false);
    }
    return f;
}

inline Field floats(uint16_t tag, const std::vector<float>& v, bool be = false) {
    Field f{tag, 11, static_cast<uint32_t>(v.size()), {}};
    for (float x : v) {
        uint32_t bits;
        std::memcpy(&bits, &x, 4);
        put32(f.data, bits, be);
    }
    return f;
}

inline Field ascii(uint16_t tag, const std::string& s) {
    Field f{tag, 2, static_cast<uint32_t>(s.size() + 1), Bytes(s.begin(), s.end())};
    f.data.push_back(0);
    return f;
}

inline Field bytes(uint16_t tag, const Bytes& v, uint16_t type = 7) {
    return Field{tag, type, static_cast<uint32_t>(v.size()), v};
}

// Writes a directory at `at` in `file`, its long values right behind it.
// Offsets stored in it are relative to `origin`.
inline void placeDirectory(Bytes& file, size_t at, std::vector<Field> fields, bool be = false,
                    size_t origin = 0) {
    std::sort(fields.begin(), fields.end(), [](const Field& a, const Field& b) { return a.tag < b.tag; });
    Bytes dir, values;
    const size_t valuesAt = at + 2 + 12 * fields.size() + 4;
    put16(dir, static_cast<uint32_t>(fields.size()), be);
    for (const Field& f : fields) {
        put16(dir, f.tag, be);
        put16(dir, f.type, be);
        put32(dir, f.count, be);
        if (f.data.size() <= 4) {
            Bytes v = f.data;
            v.resize(4, 0);
            dir.insert(dir.end(), v.begin(), v.end());
        } else {
            put32(dir, static_cast<uint32_t>(valuesAt + values.size() - origin), be);
            values.insert(values.end(), f.data.begin(), f.data.end());
            if (values.size() % 2) values.push_back(0);
        }
    }
    put32(dir, 0, be);
    dir.insert(dir.end(), values.begin(), values.end());
    if (file.size() < at + dir.size()) file.resize(at + dir.size(), 0);
    std::copy(dir.begin(), dir.end(), file.begin() + static_cast<std::ptrdiff_t>(at));
}

inline Bytes tiffHeader(uint16_t magic = 42, uint32_t firstIfd = 8) {
    Bytes b = {'I', 'I'};
    put16(b, magic, false);
    put32(b, firstIfd, false);
    return b;
}

inline fs::path writeFile(const fs::path& dir, const std::string& name, const Bytes& data) {
    const fs::path p = dir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return p;
}

}  // namespace tiffbuilder
