// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// A small classic-TIFF writer: enough structure for DNG (nested IFDs, strips
// or tiles, either byte order), nothing more.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dngconv {

enum class TiffType : uint16_t {
    Byte = 1,
    Ascii = 2,
    Short = 3,
    Long = 4,
    Rational = 5,
    SByte = 6,
    Undefined = 7,
    SShort = 8,
    SLong = 9,
    SRational = 10,
    Float = 11,
    Double = 12,
};

/// Size in bytes of one value of a TIFF field type (1..12); 0 if unknown.
size_t tiffTypeSize(uint16_t type);

/// Best rational approximation of a non-negative value with a 32-bit
/// numerator and denominator. Exposed for testing.
void toRational(double value, uint32_t& numerator, uint32_t& denominator);

class TiffIfd {
public:
    void setAscii(uint16_t tag, const std::string& text);
    void setBytes(uint16_t tag, const std::vector<uint8_t>& values);
    void setUndefined(uint16_t tag, const std::vector<uint8_t>& values);
    void setShorts(uint16_t tag, const std::vector<uint16_t>& values);
    void setShort(uint16_t tag, uint16_t value) { setShorts(tag, {value}); }
    void setLongs(uint16_t tag, const std::vector<uint32_t>& values);
    void setLong(uint16_t tag, uint32_t value) { setLongs(tag, {value}); }
    /// Unsigned rationals. A NaN is stored as 0/0, the TIFF way to say "unknown".
    void setRationals(uint16_t tag, const std::vector<double>& values);
    void setRational(uint16_t tag, double value) { setRationals(tag, {value}); }
    void setSRationals(uint16_t tag, const std::vector<double>& values);

    /// Any field type, with the value already encoded little-endian. Throws
    /// std::invalid_argument if the data length does not match type and count.
    void setRaw(uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> data);

    /// Like setRaw, but asks for the value to be stored at a given file
    /// offset. Camera maker notes need this: many contain pointers that are
    /// only valid while the note sits where the camera put it. The request is
    /// honoured when possible; valueOffset() tells after TiffWriter::write().
    void setPinned(uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> data,
                   uint64_t preferredOffset);

    bool has(uint16_t tag) const;

    /// File offset of an out-of-line value, valid after TiffWriter::write();
    /// 0 for inline values and unknown tags.
    uint64_t valueOffset(uint16_t tag) const;

    /// Tag whose value is the file offset of one or more child IFDs.
    void setSubIfds(uint16_t tag, const std::vector<TiffIfd*>& children);

    /// Image data: one chunk per strip or tile. Writes the offsets tag and the
    /// byte-counts tag.
    void setChunks(uint16_t offsetsTag, uint16_t byteCountsTag,
                   std::vector<std::vector<uint8_t>> chunks);

    bool empty() const { return entries_.empty(); }

private:
    friend class TiffWriter;

    struct Entry {
        TiffType type = TiffType::Byte;
        uint32_t count = 0;
        std::vector<uint8_t> data;          // little-endian payload
        std::vector<TiffIfd*> children;     // payload = offsets of these IFDs
        bool chunkOffsets = false;          // payload = offsets of chunks_
        bool pinned = false;                // value wants a fixed file offset
        uint64_t pinOffset = 0;
        bool pinGranted = false;            // layout result
        uint64_t valueOffset = 0;           // layout result
    };

    void set(uint16_t tag, TiffType type, uint32_t count, std::vector<uint8_t> data);

    std::map<uint16_t, Entry> entries_;
    std::vector<std::vector<uint8_t>> chunks_;
    std::vector<uint64_t> chunkOffsets_;  // layout result
    uint64_t offset_ = 0;                 // layout result
    bool placed_ = false;
};

class TiffWriter {
public:
    /// Creates an IFD owned by the writer. The first one created is IFD0.
    TiffIfd& addIfd();

    /// Lays the file out and writes it, little-endian ("II") by default or
    /// big-endian ("MM"). Throws std::runtime_error on failure or if the file
    /// would exceed the 4 GiB limit of classic TIFF.
    ///
    /// Field values are always handed to TiffIfd little-endian; the writer
    /// re-encodes them. Image data chunks are written untouched, so 16-bit
    /// samples in them must already be in the byte order asked for here.
    void write(std::ostream& out, bool bigEndian = false);

private:
    std::vector<std::unique_ptr<TiffIfd>> ifds_;
};

}  // namespace dngconv
