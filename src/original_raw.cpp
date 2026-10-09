// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "original_raw.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <zlib.h>

#include "md5.hpp"
#include "parallel.hpp"
#include "tiff_source.hpp"
#include "tiff_writer.hpp"  // tiffTypeSize

namespace dngconv {

namespace {

constexpr size_t kBlockSize = 65536;
constexpr uint16_t kOriginalRawFileName = 50827;
constexpr uint16_t kOriginalRawFileData = 50828;
constexpr uint16_t kOriginalRawFileDigest = 50973;

void put32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    out[at] = static_cast<uint8_t>(v >> 24);
    out[at + 1] = static_cast<uint8_t>(v >> 16);
    out[at + 2] = static_cast<uint8_t>(v >> 8);
    out[at + 3] = static_cast<uint8_t>(v);
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Keeps the last path component of a name taken from a file we do not trust,
// whichever separator it uses, and refuses names that are no names.
std::string bareFileName(const std::string& name) {
    std::string out = name;
    const size_t slash = out.find_last_of("/\\");
    if (slash != std::string::npos) out.erase(0, slash + 1);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](unsigned char ch) { return ch < 0x20 || ch == 0x7f || ch == ':'; }),
              out.end());
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out == "." || out == "..") out.clear();
    return out;
}

}  // namespace

std::vector<uint8_t> readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.u8string());
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) throw std::runtime_error("cannot read " + path.u8string());
    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.seekg(0);
    if (size > 0) in.read(reinterpret_cast<char*>(data.data()), size);
    if (in.gcount() != size) throw std::runtime_error("cannot read " + path.u8string());
    return data;
}

std::vector<uint8_t> packOriginalRaw(const std::vector<uint8_t>& file, unsigned threads) {
    if (file.size() >= std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("original file is too large to embed (4 GiB limit)");

    const size_t blocks = (file.size() + kBlockSize - 1) / kBlockSize;
    std::vector<std::vector<uint8_t>> compressed(blocks);
    parallelFor(blocks, threads, [&](size_t i) {
        const size_t begin = i * kBlockSize;
        const size_t length = std::min(kBlockSize, file.size() - begin);
        uLongf bound = compressBound(static_cast<uLong>(length));
        compressed[i].resize(bound);
        const int rc = compress2(compressed[i].data(), &bound, file.data() + begin,
                                 static_cast<uLong>(length), Z_DEFAULT_COMPRESSION);
        if (rc != Z_OK) throw std::runtime_error("compression of the original file failed");
        compressed[i].resize(bound);
    });

    // Fork header: uncompressed length, then one offset per block plus the
    // end offset, all counted from the start of the fork. An empty file is
    // just a zero length.
    const size_t header = blocks ? 4 * (blocks + 2) : 4;
    size_t total = header;
    for (const auto& c : compressed) total += c.size();
    const size_t trailer = 7 * 4;  // the seven unused items, each a zero
    if (total + trailer >= std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("original file is too large to embed (4 GiB limit)");

    std::vector<uint8_t> out(total + trailer, 0);
    put32(out, 0, static_cast<uint32_t>(file.size()));
    size_t at = header;
    for (size_t i = 0; i < blocks; ++i) {
        put32(out, 4 * (i + 1), static_cast<uint32_t>(at));
        std::memcpy(&out[at], compressed[i].data(), compressed[i].size());
        at += compressed[i].size();
    }
    if (blocks) put32(out, 4 * (blocks + 1), static_cast<uint32_t>(at));
    return out;
}

std::vector<uint8_t> unpackOriginalRaw(const uint8_t* packed, size_t size) {
    if (!packed || size < 4) throw std::runtime_error("embedded original is truncated");
    const uint64_t length = be32(packed);
    std::vector<uint8_t> out;
    if (length == 0) return out;

    const uint64_t blocks = (length + kBlockSize - 1) / kBlockSize;
    const uint64_t header = 4 * (blocks + 2);
    if (header > size) throw std::runtime_error("embedded original is truncated");
    // Every block shrinks by at most a small factor and zlib expands by a few
    // bytes at worst, so a length far beyond what the data could hold is a
    // sign of damage, not of a very compressible file. zlib's best case is
    // about 1000:1.
    if (length > static_cast<uint64_t>(size) * 1100 + kBlockSize)
        throw std::runtime_error("embedded original announces an impossible length");

    out.resize(static_cast<size_t>(length));
    uint64_t begin = be32(packed + 4);
    if (begin < header || begin > size)
        throw std::runtime_error("embedded original has a damaged block table");
    for (uint64_t i = 0; i < blocks; ++i) {
        const uint64_t end = be32(packed + 4 * (i + 2));
        if (end <= begin || end > size)
            throw std::runtime_error("embedded original has a damaged block table");
        const size_t outAt = static_cast<size_t>(i * kBlockSize);
        const size_t expect = std::min<uint64_t>(kBlockSize, length - outAt);
        uLongf got = static_cast<uLongf>(expect);
        const int rc = uncompress(&out[outAt], &got, packed + begin, static_cast<uLong>(end - begin));
        if (rc != Z_OK || got != expect)
            throw std::runtime_error("embedded original does not decompress (block " +
                                     std::to_string(i) + " of " + std::to_string(blocks) + ")");
        begin = end;
    }
    return out;
}

EmbeddedOriginal readEmbeddedOriginal(const std::filesystem::path& dng, bool loadData) {
    using namespace tiffsource;
    EmbeddedOriginal result;

    Source src(dng);
    Tiff tiff;
    std::vector<Entry> ifd0;
    if (!openTiff(src, 0, src.size(), tiff) || !readIfd(tiff, tiff.firstIfd, ifd0)) return result;

    const Entry* data = find(ifd0, kOriginalRawFileData);
    if (!data) return result;
    result.present = true;

    if (const Entry* name = find(ifd0, kOriginalRawFileName)) {
        std::vector<uint8_t> text;
        // ASCII in most files, UTF-8 bytes (type BYTE) in some.
        if (tiffTypeSize(name->type) == 1 && loadValue(tiff, *name, 4096, text)) {
            const size_t length = std::find(text.begin(), text.end(), uint8_t{0}) - text.begin();
            result.fileName = bareFileName(std::string(text.begin(), text.begin() + length));
        }
    }

    if (tiffTypeSize(data->type) != 1 || data->count < 4)
        throw std::runtime_error("embedded original has an unexpected form");
    const uint32_t offset = data->count > 4 ? get32(data->value, tiff.bigEndian) : 0;
    uint8_t lengthBytes[4];
    if (data->count == 4)
        std::memcpy(lengthBytes, data->value, 4);
    else if (!readAt(tiff, offset, lengthBytes, 4))
        throw std::runtime_error("embedded original lies outside the file");
    result.originalSize = be32(lengthBytes);
    if (!loadData) return result;

    if (!loadValue(tiff, *data, std::numeric_limits<uint32_t>::max(), result.packed))
        throw std::runtime_error("embedded original lies outside the file");

    if (const Entry* digest = find(ifd0, kOriginalRawFileDigest)) {
        std::vector<uint8_t> stored;
        if (digest->count == 16 && tiffTypeSize(digest->type) == 1 &&
            loadValue(tiff, *digest, 16, stored)) {
            result.hasDigest = true;
            const Md5Digest actual = Md5::of(result.packed.data(), result.packed.size());
            result.digestMatches = std::equal(actual.begin(), actual.end(), stored.begin());
        }
    }
    return result;
}

}  // namespace dngconv
