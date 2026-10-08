// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "tiff_writer.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <ostream>
#include <stdexcept>

namespace dngconv {

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
}

uint64_t evenSize(uint64_t n) { return n + (n & 1); }

}  // namespace

void toRational(double value, uint32_t& numerator, uint32_t& denominator) {
    numerator = 0;
    denominator = 1;
    if (!std::isfinite(value) || value <= 0.0) return;

    const double limit = static_cast<double>(std::numeric_limits<uint32_t>::max());
    if (value >= limit) {
        numerator = std::numeric_limits<uint32_t>::max();
        return;
    }

    // Source values are mostly single-precision floats, so anything that
    // agrees to about seven significant digits counts as exact.
    const double tolerance = value * 1.5e-7;

    // 1. Short decimals, the way people write them: 5.6 -> 28/5,
    //    0.8293 -> 8293/10000.
    for (uint32_t scale : {1u, 10u, 100u, 1000u, 10000u}) {
        const double scaled = std::round(value * scale);
        if (scaled < 1.0 || scaled > limit) continue;
        if (std::fabs(scaled / scale - value) <= tolerance) {
            const uint32_t n = static_cast<uint32_t>(scaled);
            const uint32_t g = std::gcd(n, scale);
            numerator = n / g;
            denominator = scale / g;
            return;
        }
    }

    // 2. Continued-fraction expansion: 0.0333333 -> 1/30, 0.000125 -> 1/8000.
    const uint64_t maxDen = 1000000000ull;
    uint64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    double x = value;
    for (int i = 0; i < 40; ++i) {
        const double a = std::floor(x);
        if (a > limit) break;
        const uint64_t ai = static_cast<uint64_t>(a);
        const uint64_t p2 = ai * p1 + p0;
        const uint64_t q2 = ai * q1 + q0;
        if (p2 > std::numeric_limits<uint32_t>::max() || q2 > maxDen) break;
        p0 = p1;
        q0 = q1;
        p1 = p2;
        q1 = q2;
        const double approx = static_cast<double>(p1) / static_cast<double>(q1);
        if (std::fabs(approx - value) <= tolerance) break;
        const double frac = x - a;
        if (frac < 1e-12) break;
        x = 1.0 / frac;
    }
    if (q1 == 0) return;  // cannot happen for finite input, but stay safe
    numerator = static_cast<uint32_t>(p1);
    denominator = static_cast<uint32_t>(q1);
}

void TiffIfd::set(uint16_t tag, TiffType type, uint32_t count, std::vector<uint8_t> data) {
    Entry e;
    e.type = type;
    e.count = count;
    e.data = std::move(data);
    entries_[tag] = std::move(e);
}

void TiffIfd::setAscii(uint16_t tag, const std::string& text) {
    std::vector<uint8_t> d(text.begin(), text.end());
    d.push_back(0);
    const uint32_t n = static_cast<uint32_t>(d.size());
    set(tag, TiffType::Ascii, n, std::move(d));
}

void TiffIfd::setBytes(uint16_t tag, const std::vector<uint8_t>& values) {
    set(tag, TiffType::Byte, static_cast<uint32_t>(values.size()), values);
}

void TiffIfd::setUndefined(uint16_t tag, const std::vector<uint8_t>& values) {
    set(tag, TiffType::Undefined, static_cast<uint32_t>(values.size()), values);
}

void TiffIfd::setShorts(uint16_t tag, const std::vector<uint16_t>& values) {
    std::vector<uint8_t> d;
    d.reserve(values.size() * 2);
    for (uint16_t v : values) put16(d, v);
    set(tag, TiffType::Short, static_cast<uint32_t>(values.size()), std::move(d));
}

void TiffIfd::setLongs(uint16_t tag, const std::vector<uint32_t>& values) {
    std::vector<uint8_t> d;
    d.reserve(values.size() * 4);
    for (uint32_t v : values) put32(d, v);
    set(tag, TiffType::Long, static_cast<uint32_t>(values.size()), std::move(d));
}

void TiffIfd::setRationals(uint16_t tag, const std::vector<double>& values) {
    std::vector<uint8_t> d;
    d.reserve(values.size() * 8);
    for (double v : values) {
        uint32_t n = 0, den = 0;  // NaN means "unknown" and is stored as 0/0
        if (!std::isnan(v)) toRational(v, n, den);
        put32(d, n);
        put32(d, den);
    }
    set(tag, TiffType::Rational, static_cast<uint32_t>(values.size()), std::move(d));
}

void TiffIfd::setSRationals(uint16_t tag, const std::vector<double>& values) {
    std::vector<uint8_t> d;
    d.reserve(values.size() * 8);
    for (double v : values) {
        uint32_t n, den;
        toRational(std::fabs(v), n, den);
        // Keep the numerator inside the signed range by halving both terms.
        while (n > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
            n >>= 1;
            den = den > 1 ? den >> 1 : 1;
        }
        int32_t sn = static_cast<int32_t>(n);
        if (v < 0) sn = -sn;
        put32(d, static_cast<uint32_t>(sn));
        put32(d, den);
    }
    set(tag, TiffType::SRational, static_cast<uint32_t>(values.size()), std::move(d));
}

size_t tiffTypeSize(uint16_t type) {
    static const size_t sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    return type <= 12 ? sizes[type] : 0;
}

void TiffIfd::setRaw(uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> data) {
    const size_t unit = tiffTypeSize(type);
    if (unit == 0) throw std::invalid_argument("TIFF field has an unknown type");
    if (data.size() != unit * static_cast<size_t>(count))
        throw std::invalid_argument("TIFF field data does not match its type and count");
    set(tag, static_cast<TiffType>(type), count, std::move(data));
}

void TiffIfd::setPinned(uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> data,
                        uint64_t preferredOffset) {
    setRaw(tag, type, count, std::move(data));
    Entry& e = entries_[tag];
    e.pinned = true;
    e.pinOffset = preferredOffset;
}

bool TiffIfd::has(uint16_t tag) const { return entries_.count(tag) != 0; }

uint64_t TiffIfd::valueOffset(uint16_t tag) const {
    const auto it = entries_.find(tag);
    return it == entries_.end() ? 0 : it->second.valueOffset;
}

void TiffIfd::setSubIfds(uint16_t tag, const std::vector<TiffIfd*>& children) {
    Entry e;
    e.type = TiffType::Long;
    e.count = static_cast<uint32_t>(children.size());
    e.children = children;
    e.data.assign(children.size() * 4, 0);
    entries_[tag] = std::move(e);
}

void TiffIfd::setChunks(uint16_t offsetsTag, uint16_t byteCountsTag,
                        std::vector<std::vector<uint8_t>> chunks) {
    chunks_ = std::move(chunks);

    Entry offsets;
    offsets.type = TiffType::Long;
    offsets.count = static_cast<uint32_t>(chunks_.size());
    offsets.chunkOffsets = true;
    offsets.data.assign(chunks_.size() * 4, 0);
    entries_[offsetsTag] = std::move(offsets);

    std::vector<uint32_t> counts;
    counts.reserve(chunks_.size());
    for (const auto& c : chunks_) {
        if (c.size() > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("image data chunk exceeds 4 GiB");
        counts.push_back(static_cast<uint32_t>(c.size()));
    }
    setLongs(byteCountsTag, counts);
}

TiffIfd& TiffWriter::addIfd() {
    ifds_.push_back(std::make_unique<TiffIfd>());
    return *ifds_.back();
}

namespace {

// Re-encodes a little-endian field value as big-endian, element by element.
std::vector<uint8_t> swappedValue(TiffType type, const std::vector<uint8_t>& data) {
    size_t unit = tiffTypeSize(static_cast<uint16_t>(type));
    if (type == TiffType::Rational || type == TiffType::SRational) unit = 4;
    std::vector<uint8_t> out = data;
    if (unit >= 2)
        for (size_t i = 0; i + unit <= out.size(); i += unit)
            std::reverse(out.begin() + i, out.begin() + i + unit);
    return out;
}

bool needsSwap(TiffType type) { return tiffTypeSize(static_cast<uint16_t>(type)) >= 2; }

}  // namespace

void TiffWriter::write(std::ostream& out, bool bigEndian) {
    if (ifds_.empty()) throw std::runtime_error("TIFF has no image directory");

    // ---- pinned values ------------------------------------------------------
    // A pinned value asks for a fixed file offset. Honour the request when the
    // offset is usable (past the header, on a word boundary, inside the file,
    // not colliding with another pinned value); otherwise the value is placed
    // like any other.
    struct Span {
        uint64_t begin, end;
    };
    // Size of the file if nothing were pinned. A pinned value must fit inside
    // it: a request far beyond would only pad the file with zeros up to there.
    uint64_t naturalSize = 8;
    for (const auto& ifd : ifds_) {
        naturalSize += evenSize(2 + 12 * static_cast<uint64_t>(ifd->entries_.size()) + 4);
        for (const auto& [tag, e] : ifd->entries_) {
            (void)tag;
            if (e.data.size() > 4) naturalSize += evenSize(e.data.size());
        }
        for (const auto& chunk : ifd->chunks_) naturalSize += evenSize(chunk.size());
    }
    std::vector<Span> fixed;
    for (auto& ifd : ifds_) {
        for (auto& [tag, e] : ifd->entries_) {
            (void)tag;
            e.valueOffset = 0;
            if (!e.pinned) continue;
            e.pinGranted = false;
            if (e.data.size() <= 4 || e.pinOffset < 8 || (e.pinOffset & 1)) continue;
            const Span want{e.pinOffset, e.pinOffset + e.data.size()};
            if (want.end > naturalSize) continue;
            bool collides = false;
            for (const Span& f : fixed)
                if (want.begin < f.end + 1 && f.begin < want.end + 1) collides = true;
            if (collides) continue;
            fixed.push_back(want);
            e.pinGranted = true;
            e.valueOffset = e.pinOffset;
        }
    }
    std::sort(fixed.begin(), fixed.end(),
              [](const Span& a, const Span& b) { return a.begin < b.begin; });

    // ---- layout -----------------------------------------------------------
    // Header, then every IFD followed by its out-of-line values, then all
    // image data, each piece taking the next free word-aligned position and
    // stepping over the pinned spans. Keeping the directories together at the
    // front means a reader finds all metadata without seeking through pixels.
    uint64_t cursor = 8;
    auto allocate = [&](uint64_t size) {
        for (const Span& f : fixed) {
            if (f.end <= cursor) continue;
            if (cursor + size <= f.begin) break;
            cursor = evenSize(f.end);
        }
        const uint64_t at = cursor;
        cursor += evenSize(size);
        return at;
    };

    for (auto& ifd : ifds_) {
        ifd->offset_ = allocate(2 + 12 * static_cast<uint64_t>(ifd->entries_.size()) + 4);
        ifd->placed_ = true;
        for (auto& [tag, e] : ifd->entries_) {
            (void)tag;
            if (e.data.size() > 4 && !(e.pinned && e.pinGranted))
                e.valueOffset = allocate(e.data.size());
        }
    }
    for (auto& ifd : ifds_) {
        ifd->chunkOffsets_.clear();
        for (const auto& chunk : ifd->chunks_) ifd->chunkOffsets_.push_back(allocate(chunk.size()));
    }
    uint64_t fileEnd = cursor;
    for (const Span& f : fixed) fileEnd = std::max(fileEnd, evenSize(f.end));
    if (fileEnd > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("output would exceed the 4 GiB limit of the TIFF container");

    // ---- resolve offsets --------------------------------------------------
    for (auto& ifd : ifds_) {
        for (auto& [tag, e] : ifd->entries_) {
            (void)tag;
            if (!e.children.empty()) {
                e.data.clear();
                for (const TiffIfd* child : e.children) {
                    if (!child || !child->placed_)
                        throw std::runtime_error("sub-IFD is not part of this TIFF");
                    put32(e.data, static_cast<uint32_t>(child->offset_));
                }
            } else if (e.chunkOffsets) {
                e.data.clear();
                for (uint64_t off : ifd->chunkOffsets_)
                    put32(e.data, static_cast<uint32_t>(off));
            }
        }
    }

    // ---- collect the pieces -------------------------------------------------
    // Field values are held little-endian; a big-endian file gets re-encoded
    // copies of the multi-byte ones. Byte-typed values (maker notes, opcode
    // lists, image data) are opaque and go out as they are.
    auto put16o = [&](std::vector<uint8_t>& v, uint16_t x) {
        if (bigEndian) {
            v.push_back(static_cast<uint8_t>(x >> 8));
            v.push_back(static_cast<uint8_t>(x & 0xff));
        } else {
            put16(v, x);
        }
    };
    auto put32o = [&](std::vector<uint8_t>& v, uint32_t x) {
        if (bigEndian) {
            for (int shift = 24; shift >= 0; shift -= 8) v.push_back(static_cast<uint8_t>(x >> shift));
        } else {
            put32(v, x);
        }
    };

    struct Piece {
        uint64_t offset;
        const std::vector<uint8_t>* bytes;
    };
    std::vector<Piece> pieces;
    std::deque<std::vector<uint8_t>> reencoded;  // deque: addresses stay valid

    std::vector<uint8_t> header;
    header.push_back(bigEndian ? 'M' : 'I');
    header.push_back(bigEndian ? 'M' : 'I');
    put16o(header, 42);
    put32o(header, static_cast<uint32_t>(ifds_.front()->offset_));
    pieces.push_back({0, &header});

    std::vector<std::vector<uint8_t>> tables(ifds_.size());
    for (size_t i = 0; i < ifds_.size(); ++i) {
        TiffIfd& ifd = *ifds_[i];
        std::vector<uint8_t>& table = tables[i];
        put16o(table, static_cast<uint16_t>(ifd.entries_.size()));
        for (auto& [tag, e] : ifd.entries_) {
            put16o(table, tag);
            put16o(table, static_cast<uint16_t>(e.type));
            put32o(table, e.count);
            const std::vector<uint8_t>* value = &e.data;
            if (bigEndian && needsSwap(e.type)) {
                reencoded.push_back(swappedValue(e.type, e.data));
                value = &reencoded.back();
            }
            if (value->size() > 4) {
                put32o(table, static_cast<uint32_t>(e.valueOffset));
                pieces.push_back({e.valueOffset, value});
            } else {
                for (size_t k = 0; k < 4; ++k) table.push_back(k < value->size() ? (*value)[k] : 0);
            }
        }
        put32o(table, 0);  // no next IFD: children hang off SubIFDs instead
        pieces.push_back({ifd.offset_, &table});
        for (size_t c = 0; c < ifd.chunks_.size(); ++c)
            pieces.push_back({ifd.chunkOffsets_[c], &ifd.chunks_[c]});
    }
    std::sort(pieces.begin(), pieces.end(),
              [](const Piece& a, const Piece& b) { return a.offset < b.offset; });

    // ---- emit in file order, zero-filling the gaps --------------------------
    static const char zeros[4096] = {0};
    uint64_t written = 0;
    auto padTo = [&](uint64_t offset) {
        while (written < offset) {
            const uint64_t n = std::min<uint64_t>(offset - written, sizeof zeros);
            out.write(zeros, static_cast<std::streamsize>(n));
            written += n;
        }
    };
    for (const Piece& piece : pieces) {
        if (piece.offset < written) throw std::logic_error("TIFF layout overlap");
        padTo(piece.offset);
        out.write(reinterpret_cast<const char*>(piece.bytes->data()),
                  static_cast<std::streamsize>(piece.bytes->size()));
        written += piece.bytes->size();
    }
    padTo(fileEnd);
    if (!out) throw std::runtime_error("write error");
}

}  // namespace dngconv
