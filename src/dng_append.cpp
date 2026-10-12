// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "dng_append.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#include <wchar.h>
#else
#include <unistd.h>
#endif

#include "tiff_source.hpp"

namespace dngconv {

namespace {

using tiffsource::get16;
using tiffsource::get32;

void put16(uint8_t* p, uint16_t v, bool bigEndian) {
    p[bigEndian ? 0 : 1] = static_cast<uint8_t>(v >> 8);
    p[bigEndian ? 1 : 0] = static_cast<uint8_t>(v);
}

void put32(uint8_t* p, uint32_t v, bool bigEndian) {
    for (int i = 0; i < 4; ++i) p[bigEndian ? 3 - i : i] = static_cast<uint8_t>(v >> (8 * i));
}

// A file open for reading and writing, with positions beyond 2 GiB and a way
// to make sure what was written has reached the disk.
class File {
public:
    explicit File(const std::filesystem::path& path) {
#if defined(_WIN32)
        f_ = _wfopen(path.c_str(), L"r+b");
#else
        f_ = std::fopen(path.c_str(), "r+b");
#endif
        if (!f_)
            throw std::runtime_error("the file cannot be opened for writing: " + std::generic_category().message(errno));
    }
    ~File() {
        if (f_) std::fclose(f_);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    uint64_t size() {
        seekEnd();
#if defined(_WIN32)
        const long long at = _ftelli64(f_);
#else
        const off_t at = ftello(f_);
#endif
        if (at < 0) fail("the size of the file cannot be told");
        return static_cast<uint64_t>(at);
    }
    void seek(uint64_t offset) {
#if defined(_WIN32)
        const int rc = _fseeki64(f_, static_cast<long long>(offset), SEEK_SET);
#else
        const int rc = fseeko(f_, static_cast<off_t>(offset), SEEK_SET);
#endif
        if (rc != 0) fail("the file cannot be read");
    }
    void seekEnd() {
        if (std::fseek(f_, 0, SEEK_END) != 0) fail("the file cannot be read");
    }
    void read(void* dst, size_t count) {
        if (count && std::fread(dst, 1, count, f_) != count) fail("the file cannot be read");
    }
    void write(const void* src, size_t count) {
        if (count && std::fwrite(src, 1, count, f_) != count) fail("the file could not be written");
    }
    // Hands everything written so far to the disk, not just to the system.
    void toDisk() {
        if (std::fflush(f_) != 0) fail("the file could not be written");
#if defined(_WIN32)
        if (_commit(_fileno(f_)) != 0) fail("the file could not be written");
#else
        if (fsync(fileno(f_)) != 0) fail("the file could not be written");
#endif
    }
    void close() {
        std::FILE* f = f_;
        f_ = nullptr;
        if (std::fclose(f) != 0) fail("the file could not be written");
    }

private:
    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(errno ? std::string(what) + ": " + std::generic_category().message(errno)
                                       : std::string(what));
    }
    std::FILE* f_ = nullptr;
};

}  // namespace

AppendReceipt appendToFirstIfd(const std::filesystem::path& path, const std::vector<TiffField>& fields) {
    for (const TiffField& f : fields) {
        if (f.type != 1 && f.type != 2 && f.type != 7)
            throw std::invalid_argument("appendToFirstIfd: only byte-sized field types are supported");
        if (f.count == 0 || f.data.size() != f.count)
            throw std::invalid_argument("appendToFirstIfd: a field's count does not match its data");
    }
    std::vector<TiffField> added = fields;
    std::stable_sort(added.begin(), added.end(), [](const TiffField& a, const TiffField& b) { return a.tag < b.tag; });
    for (size_t i = 1; i < added.size(); ++i)
        if (added[i].tag == added[i - 1].tag) throw std::invalid_argument("appendToFirstIfd: a tag is given twice");

    auto opened = std::make_unique<File>(path);
    File& file = *opened;
    AppendReceipt receipt;
    receipt.oldSize = file.size();

    // ---- the directory as it is ---------------------------------------------------
    uint8_t header[8];
    if (receipt.oldSize < 8) throw std::runtime_error("not a TIFF file");
    file.seek(0);
    file.read(header, sizeof header);
    if (header[0] == 'I' && header[1] == 'I')
        receipt.bigEndian = false;
    else if (header[0] == 'M' && header[1] == 'M')
        receipt.bigEndian = true;
    else
        throw std::runtime_error("not a TIFF file");
    const bool big = receipt.bigEndian;
    if (get16(header + 2, big) != 42) throw std::runtime_error("not a TIFF file");
    receipt.oldFirstIfd = get32(header + 4, big);
    if (receipt.oldFirstIfd < 8 || static_cast<uint64_t>(receipt.oldFirstIfd) + 2 > receipt.oldSize)
        throw std::runtime_error("the first directory lies outside the file");

    uint8_t countBytes[2];
    file.seek(receipt.oldFirstIfd);
    file.read(countBytes, 2);
    const unsigned oldCount = get16(countBytes, big);
    if (oldCount == 0 || oldCount > tiffsource::kMaxEntries ||
        static_cast<uint64_t>(receipt.oldFirstIfd) + 2 + 12ull * oldCount + 4 > receipt.oldSize)
        throw std::runtime_error("the first directory cannot be read");
    std::vector<uint8_t> oldEntries(12u * oldCount + 4);  // the entries and the link to the next directory
    file.read(oldEntries.data(), oldEntries.size());
    for (unsigned i = 0; i < oldCount; ++i) {
        const uint16_t tag = get16(&oldEntries[12u * i], big);
        for (const TiffField& f : added)
            if (f.tag == tag)
                throw std::invalid_argument("appendToFirstIfd: the directory already has tag " + std::to_string(tag));
    }
    if (oldCount + added.size() > 0xffff) throw std::runtime_error("the first directory is full");

    // ---- the new one, to go at the end of the file -----------------------------------
    // Directories start on an even offset; so do values.
    const uint64_t newIfd = receipt.oldSize + (receipt.oldSize & 1);
    const size_t newCount = oldCount + added.size();
    const size_t directoryBytes = 2 + 12 * newCount + 4;
    std::vector<uint8_t> block(receipt.oldSize & 1, 0);
    const size_t start = block.size();
    block.resize(start + directoryBytes, 0);
    size_t valuesAt = block.size();  // within the block
    std::vector<uint8_t> values;

    put16(&block[start], static_cast<uint16_t>(newCount), big);
    size_t out = start + 2;
    size_t nextAdded = 0;
    const auto emitAdded = [&](const TiffField& f) {
        uint8_t* e = &block[out];
        put16(e, f.tag, big);
        put16(e + 2, f.type, big);
        put32(e + 4, f.count, big);
        if (f.data.size() <= 4) {
            std::memcpy(e + 8, f.data.data(), f.data.size());
        } else {
            const uint64_t at = receipt.oldSize + valuesAt + values.size();
            if (at + f.data.size() > 0xffffffffull)
                throw std::runtime_error("the file would grow beyond 4 GiB, more than a TIFF file can address");
            put32(e + 8, static_cast<uint32_t>(at), big);
            values.insert(values.end(), f.data.begin(), f.data.end());
            if (values.size() & 1) values.push_back(0);
        }
        out += 12;
    };
    // The old entries keep their order; the new ones go in where their tag
    // numbers belong.
    for (unsigned i = 0; i < oldCount; ++i) {
        const uint16_t tag = get16(&oldEntries[12u * i], big);
        while (nextAdded < added.size() && added[nextAdded].tag < tag) emitAdded(added[nextAdded++]);
        std::memcpy(&block[out], &oldEntries[12u * i], 12);
        out += 12;
    }
    while (nextAdded < added.size()) emitAdded(added[nextAdded++]);
    std::memcpy(&block[out], &oldEntries[12u * oldCount], 4);  // the link to the next directory, as it was
    block.insert(block.end(), values.begin(), values.end());
    if (newIfd + directoryBytes > 0xffffffffull || receipt.oldSize + block.size() > 0xffffffffull)
        throw std::runtime_error("the file would grow beyond 4 GiB, more than a TIFF file can address");

    // ---- write: first the new directory, to disk; then the pointer to it -----------------
    try {
        file.seek(receipt.oldSize);
        file.write(block.data(), block.size());
        file.toDisk();
    } catch (...) {
        // A full disk, most likely. The header still points to the old
        // directory; take away whatever part of the new one got written.
        opened.reset();
        std::error_code ignored;
        std::filesystem::resize_file(path, receipt.oldSize, ignored);
        throw;
    }
    receipt.newSize = receipt.oldSize + block.size();

    try {
        uint8_t pointer[4];
        put32(pointer, static_cast<uint32_t>(newIfd), big);
        file.seek(4);
        file.write(pointer, 4);
        file.toDisk();
        file.close();
    } catch (...) {
        // Whether the four bytes arrived is not known; the old state is the
        // one that can be restored for certain.
        opened.reset();
        try {
            undoAppend(path, receipt);
        } catch (...) {
        }
        throw;
    }
    return receipt;
}

void undoAppend(const std::filesystem::path& path, const AppendReceipt& receipt) {
    {
        File file(path);
        uint8_t pointer[4];
        put32(pointer, receipt.oldFirstIfd, receipt.bigEndian);
        file.seek(4);
        file.write(pointer, 4);
        file.toDisk();
        file.close();
    }
    std::error_code ec;
    std::filesystem::resize_file(path, receipt.oldSize, ec);
    if (ec) throw std::runtime_error("cannot shorten the file again: " + ec.message());
}

}  // namespace dngconv
