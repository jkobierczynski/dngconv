// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "source_record.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "sidecar.hpp"
#include "tiff_source.hpp"

namespace dngconv {

namespace {

const char kPrefix[] = "dngconv source record 1: size ";
const char kDigestLabel[] = ", MD5 ";
const char kSidecarLabel[] = ", sidecar MD5 ";
constexpr uint16_t kDngVersion = 50706;

std::string hex(const Md5Digest& d) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : d) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

// Reads 32 hexadecimal digits at `at`, and moves past them.
bool readDigest(const std::string& text, size_t& at, Md5Digest& out) {
    if (text.size() - at < 32) return false;
    for (size_t i = 0; i < 16; ++i) {
        unsigned value = 0;
        for (size_t k = 0; k < 2; ++k) {
            const char ch = text[at + 2 * i + k];
            unsigned digit;
            if (ch >= '0' && ch <= '9')
                digit = static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                digit = static_cast<unsigned>(ch - 'a') + 10;
            else
                return false;
            value = value * 16 + digit;
        }
        out[i] = static_cast<uint8_t>(value);
    }
    at += 32;
    return true;
}

bool skip(const std::string& text, size_t& at, const char* label) {
    const size_t length = std::strlen(label);
    if (text.compare(at, length, label) != 0) return false;
    at += length;
    return true;
}

}  // namespace

std::string packSourceRecord(const SourceRecord& record) {
    std::string out = kPrefix + std::to_string(record.fileSize) + kDigestLabel + hex(record.fileDigest);
    if (record.hasSidecar) out += kSidecarLabel + hex(record.sidecarDigest);
    return out;
}

SourceRecord unpackSourceRecord(const char* data, size_t size) {
    SourceRecord none, record;
    if (!data) return none;
    std::string text(data, size);
    while (!text.empty() && text.back() == '\0') text.pop_back();

    size_t at = 0;
    if (!skip(text, at, kPrefix)) return none;
    const size_t digits = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        const uint64_t digit = static_cast<uint64_t>(text[at] - '0');
        if (record.fileSize > (UINT64_MAX - digit) / 10) return none;  // more than 64 bits hold
        record.fileSize = record.fileSize * 10 + digit;
        ++at;
    }
    if (at == digits) return none;
    if (!skip(text, at, kDigestLabel) || !readDigest(text, at, record.fileDigest)) return none;
    if (at != text.size()) {
        if (!skip(text, at, kSidecarLabel) || !readDigest(text, at, record.sidecarDigest) || at != text.size())
            return none;
        record.hasSidecar = true;
    }
    record.present = true;
    return record;
}

SourceRecord readSourceRecord(const std::filesystem::path& dng) {
    using namespace tiffsource;
    try {
        Source src(dng);
        Tiff tiff;
        std::vector<Entry> ifd0;
        if (src.size() == 0 || !openTiff(src, 0, src.size(), tiff) || !readIfd(tiff, tiff.firstIfd, ifd0) ||
            !find(ifd0, kDngVersion))
            return {};
        const Entry* e = find(ifd0, kSourceRecordTag);
        std::vector<uint8_t> raw;
        if (!e || e->type != 2 || !loadValue(tiff, *e, 256, raw)) return {};
        return unpackSourceRecord(reinterpret_cast<const char*>(raw.data()), raw.size());
    } catch (const std::exception&) {
        return {};
    }
}

SourceComparison compareWithSource(const std::filesystem::path& dng, const std::filesystem::path& source,
                                   bool withSidecar) {
    SourceComparison result;
    const SourceRecord record = readSourceRecord(dng);
    if (!record.present) return result;

    // The size settles most changes without reading the file.
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(source, ec);
    if (ec) throw std::runtime_error("cannot read '" + source.u8string() + "': " + ec.message());
    bool sourceChanged = size != record.fileSize;
    if (!sourceChanged) {
        result.fileDigest = digestOfFile(source, result.fileSize);
        result.digestKnown = true;
        sourceChanged = result.fileDigest != record.fileDigest;
    }

    std::vector<uint8_t> sidecar;
    if (withSidecar)
        if (const std::filesystem::path found = findXmpSidecar(source); !found.empty())
            sidecar = readXmpSidecar(found);
    const char* sidecarNote = nullptr;
    if (record.hasSidecar && sidecar.empty())
        sidecarNote = "sidecar no longer there";
    else if (!record.hasSidecar && !sidecar.empty())
        sidecarNote = "sidecar added";
    else if (record.hasSidecar && Md5::of(sidecar.data(), sidecar.size()) != record.sidecarDigest)
        sidecarNote = "sidecar changed";

    if (!sourceChanged && !sidecarNote) {
        result.state = SourceComparison::State::UpToDate;
        return result;
    }
    result.state = SourceComparison::State::Changed;
    if (sourceChanged) result.what = "source changed";
    if (sidecarNote) result.what += std::string(result.what.empty() ? "" : ", ") + sidecarNote;
    return result;
}

Md5Digest digestOfFile(const std::filesystem::path& path, uint64_t& size) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open '" + path.u8string() + "'");
    Md5 md5;
    std::vector<char> buffer(1 << 20);
    size = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        md5.update(buffer.data(), static_cast<size_t>(got));
        size += static_cast<uint64_t>(got);
    }
    if (in.bad() || !in.eof()) throw std::runtime_error("read error on '" + path.u8string() + "'");
    return md5.finish();
}

}  // namespace dngconv
