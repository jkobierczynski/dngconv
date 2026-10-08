// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Checks the TIFF container writer by parsing its output byte by byte.
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_util.hpp"
#include "tiff_reader.hpp"
#include "tiff_writer.hpp"

using namespace dngconv;
using tiffreader::Ifd;
using tiffreader::parseIfd;
using tiffreader::rd16;
using tiffreader::rd32;

namespace {

void checkRational(double value, uint32_t expectNum, uint32_t expectDen) {
    uint32_t n = 0, d = 0;
    toRational(value, n, d);
    if (n != expectNum || d != expectDen)
        std::fprintf(stderr, "toRational(%g) = %u/%u, expected %u/%u\n", value, n, d, expectNum,
                     expectDen);
    CHECK(n == expectNum && d == expectDen);
}

}  // namespace

int main() {
    // ---- rational conversion ------------------------------------------------
    checkRational(0.0, 0, 1);
    checkRational(-3.0, 0, 1);
    checkRational(1.0, 1, 1);
    checkRational(4.5, 9, 2);
    checkRational(5.6, 28, 5);
    checkRational(0.004, 1, 250);
    checkRational(0.0125, 1, 80);
    checkRational(static_cast<float>(1.0 / 3.0), 1, 3);
    checkRational(static_cast<float>(1.0 / 30.0), 1, 30);
    checkRational(static_cast<float>(1.0 / 8000.0), 1, 8000);
    checkRational(static_cast<float>(0.8293), 8293, 10000);
    checkRational(30.0, 30, 1);
    checkRational(25600.0, 25600, 1);
    checkRational(1e12, 4294967295u, 1);
    {
        uint32_t n, d;
        toRational(3.14159265358979, n, d);
        CHECK(d != 0);
        CHECK_NEAR(static_cast<double>(n) / d, 3.14159265358979, 1e-6);
        toRational(0.013139, n, d);
        CHECK(d != 0);
        CHECK_NEAR(static_cast<double>(n) / d, 0.013139, 1e-8);
    }

    // ---- container layout ---------------------------------------------------
    TiffWriter writer;
    TiffIfd& ifd0 = writer.addIfd();
    TiffIfd& child = writer.addIfd();

    const std::vector<uint8_t> oddChunk = {1, 2, 3, 4, 5};
    const std::vector<uint8_t> evenChunk = {9, 8, 7, 6};

    ifd0.setLong(256, 640);
    ifd0.setShort(259, 1);
    ifd0.setAscii(271, "Make");          // 5 bytes: stored out of line
    ifd0.setAscii(272, "abc");           // 4 bytes: stored inline
    ifd0.setShorts(258, {8, 8, 8});      // 6 bytes: out of line
    ifd0.setSRationals(50721, {-0.5, 1.25});
    ifd0.setRationals(33434, {0.004});
    // "unknown" is written as 0/0
    ifd0.setRationals(50736, {24, std::numeric_limits<double>::quiet_NaN()});
    ifd0.setSubIfds(330, {&child});
    ifd0.setChunks(273, 279, {oddChunk, evenChunk});
    child.setBytes(50706, {1, 4, 0, 0});
    child.setChunks(324, 325, {evenChunk});

    std::ostringstream os(std::ios::binary);
    writer.write(os);
    const std::string f = os.str();

    CHECK(f.size() > 8);
    CHECK(f[0] == 'I' && f[1] == 'I');
    CHECK(rd16(f, 2) == 42);
    CHECK(f.size() % 2 == 0);

    uint32_t next = 1;
    const Ifd p0 = parseIfd(f, rd32(f, 4), &next);
    CHECK(next == 0);
    CHECK(p0.size() == 11);
    CHECK(p0.at(256).type == 4 && p0.at(256).u32() == 640);
    CHECK(p0.at(259).type == 3 && p0.at(259).u16() == 1);
    CHECK(p0.at(271).type == 2 && p0.at(271).count == 5);
    CHECK(p0.at(271).text() == "Make");
    CHECK(p0.at(272).count == 4 && p0.at(272).text() == "abc");
    CHECK(p0.at(258).count == 3 && p0.at(258).u16(0) == 8 && p0.at(258).u16(2) == 8);

    // signed rationals: -1/2 and 5/4
    CHECK(p0.at(50721).type == 10 && p0.at(50721).count == 2);
    CHECK(static_cast<int32_t>(p0.at(50721).u32(0)) == -1 && p0.at(50721).u32(1) == 2);
    CHECK(p0.at(50721).u32(2) == 5 && p0.at(50721).u32(3) == 4);
    CHECK(p0.at(33434).u32(0) == 1 && p0.at(33434).u32(1) == 250);
    CHECK(p0.at(50736).u32(0) == 24 && p0.at(50736).u32(1) == 1);
    CHECK(p0.at(50736).u32(2) == 0 && p0.at(50736).u32(3) == 0);

    // chunks: offsets point at the data, byte counts are the unpadded sizes
    CHECK(p0.at(273).count == 2 && p0.at(279).count == 2);
    CHECK(p0.at(279).u32(0) == 5 && p0.at(279).u32(1) == 4);
    const uint32_t off0 = p0.at(273).u32(0), off1 = p0.at(273).u32(1);
    CHECK(off0 % 2 == 0 && off1 % 2 == 0);
    CHECK(f.compare(off0, 5, std::string(oddChunk.begin(), oddChunk.end())) == 0);
    CHECK(f.compare(off1, 4, std::string(evenChunk.begin(), evenChunk.end())) == 0);

    // sub-IFD
    const Ifd p1 = parseIfd(f, p0.at(330).u32(), &next);
    CHECK(next == 0);
    CHECK(p1.size() == 3);
    CHECK(p1.at(50706).type == 1 && p1.at(50706).data[1] == 4);
    const uint32_t off2 = p1.at(324).u32();
    CHECK(f.compare(off2, 4, std::string(evenChunk.begin(), evenChunk.end())) == 0);

    // ---- big-endian output: same content, other byte order --------------------
    {
        std::ostringstream be(std::ios::binary);
        writer.write(be, true);
        const std::string g = be.str();
        CHECK(g.size() == f.size());
        CHECK(g[0] == 'M' && g[1] == 'M' && g[2] == 0 && g[3] == 42);
        const Ifd b0 = parseIfd(g, rd32(g, 4), &next);
        CHECK(next == 0 && b0.size() == p0.size());
        for (const auto& [tag, entry] : p0) {
            if (tag == 273 || tag == 330) continue;  // offsets: compared through their targets
            CHECK(b0.count(tag) == 1 && b0.at(tag).type == entry.type &&
                  b0.at(tag).count == entry.count && b0.at(tag).data == entry.data);
        }
        // SHORT 1 is stored left-justified as 00 01 in a big-endian entry.
        const size_t entry259 = rd32(g, 4) + 2 + 12 * 2;  // tags 256, 258, 259 ...
        CHECK(rd16(g, entry259) == 259 && g[entry259 + 8] == 0 && g[entry259 + 9] == 1);
        CHECK(g.compare(b0.at(273).u32(0), 5, std::string(oddChunk.begin(), oddChunk.end())) == 0);
        const Ifd b1 = parseIfd(g, b0.at(330).u32());
        CHECK(b1.at(50706).data == p1.at(50706).data);
    }

    // ---- values pinned to a file offset ----------------------------------------
    {
        TiffWriter w;
        TiffIfd& a = w.addIfd();
        TiffIfd& b = w.addIfd();
        const std::vector<uint8_t> note(100, 0x5a), other(50, 0x33), big(3000, 0x11);
        a.setLong(256, 1);
        a.setBytes(1000, big);               // ordinary value, larger than the gap before 200
        a.setSubIfds(330, {&b});
        b.setPinned(37500, 7, 100, note, 200);
        b.setPinned(37501, 7, 50, other, 250);   // collides with the first: placed normally
        b.setPinned(37502, 7, 50, other, 301);   // odd offset: placed normally
        b.setPinned(37503, 7, 50, other, 900000);  // beyond the end: placed normally
        b.setChunks(273, 279, {evenChunk});
        std::ostringstream ps(std::ios::binary);
        w.write(ps);
        const std::string h = ps.str();
        const Ifd h0 = tiffreader::parseFirstIfd(h);
        const Ifd h1 = parseIfd(h, h0.at(330).u32());
        CHECK(h1.at(37500).offset == 200 && h1.at(37500).data == note);
        CHECK(b.valueOffset(37500) == 200);
        CHECK(h1.at(37501).offset != 250 && h1.at(37501).data == other);
        CHECK(h1.at(37502).offset != 301 && h1.at(37502).data == other);
        CHECK(h1.at(37503).offset < 5000 && h1.at(37503).data == other);
        CHECK(h.size() < 5000);
        CHECK(h0.at(1000).data == big);
        // Nothing else may touch the pinned bytes.
        CHECK(h0.at(1000).offset >= 300 || h0.at(1000).offset + 3000 <= 200);
        CHECK(h.compare(h1.at(273).u32(), 4, std::string(evenChunk.begin(), evenChunk.end())) == 0);

        // A value pinned right behind the header pushes IFD0 further back.
        TiffWriter w2;
        TiffIfd& c = w2.addIfd();
        c.setPinned(37500, 7, 100, note, 8);
        std::ostringstream ps2(std::ios::binary);
        w2.write(ps2);
        const std::string k = ps2.str();
        CHECK(rd32(k, 4) >= 108);
        CHECK(tiffreader::parseFirstIfd(k).at(37500).offset == 8);

        // Mismatched length for the declared type is refused.
        bool threw = false;
        try {
            c.setRaw(1, 3, 2, {1, 2, 3});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    return testutil::finish("test_tiff");
}
