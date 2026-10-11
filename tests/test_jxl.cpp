// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// JPEG XL in DNG 1.7: rows and columns stored field by field, the checksum
// of compressed data, and, in a build with libjxl, the codec and the files
// the writer makes with it. The first two are tried on files built by hand
// and run in every build.
//
// usage: test_jxl <scratch folder> [keep]
// With "keep", the DNGs stay in the folder together with their samples
// (<name>.raw16, little-endian), for comparing with another reader.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dng_check.hpp"
#include "dng_digest.hpp"
#include "dng_writer.hpp"
#include "jxl_codec.hpp"
#include "md5.hpp"
#include "test_util.hpp"
#include "tiff_builder.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
using namespace tiffbuilder;

namespace {

using State = DngCheck::State;

bool contains(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

Field digestTag(uint16_t tag, const Md5Digest& d) { return bytes(tag, Bytes(d.begin(), d.end()), 1); }

// A minimal DNG with its raw data in strips or tiles given as they are.
struct HandImage {
    uint32_t width = 0, height = 0, planes = 1;
    uint32_t compression = 1;
    bool tiled = false;
    uint32_t tileWidth = 0, tileHeight = 0;  // or rows per strip in tileHeight
    std::vector<Bytes> chunks;
    std::vector<Field> extra;       // for the raw directory
};

Bytes handDng(const HandImage& img, bool be = false) {
    Bytes f = {static_cast<uint8_t>(be ? 'M' : 'I'), static_cast<uint8_t>(be ? 'M' : 'I')};
    put16(f, 42, be);
    put32(f, 8, be);
    uint32_t at = 0x800;
    std::vector<uint32_t> offsets, counts;
    for (const Bytes& c : img.chunks) {
        offsets.push_back(at);
        counts.push_back(static_cast<uint32_t>(c.size()));
        at += static_cast<uint32_t>(c.size() + (c.size() & 1));
    }
    std::vector<int> depth(img.planes, 16);
    std::vector<Field> fields = {
        longs(254, {0}, be), longs(256, {img.width}, be), longs(257, {img.height}, be), shorts(258, depth, be, 3),
        shorts(259, {static_cast<int>(img.compression)}, be, 3),
        shorts(262, {img.planes == 1 ? 32803 : 34892}, be, 3), shorts(277, {static_cast<int>(img.planes)}, be, 3),
        bytes(50706, {1, 7, 1, 0}, 1)};
    if (img.tiled) {
        fields.push_back(longs(322, {img.tileWidth}, be));
        fields.push_back(longs(323, {img.tileHeight}, be));
        fields.push_back(longs(324, offsets, be));
        fields.push_back(longs(325, counts, be));
    } else {
        fields.push_back(longs(278, {img.tileHeight}, be));
        fields.push_back(longs(273, offsets, be));
        fields.push_back(longs(279, counts, be));
    }
    fields.insert(fields.end(), img.extra.begin(), img.extra.end());
    placeDirectory(f, 8, fields, be);
    if (f.size() > 0x800) std::fprintf(stderr, "handDng: directory too large\n");
    f.resize(0x800, 0);
    for (const Bytes& c : img.chunks) {
        f.insert(f.end(), c.begin(), c.end());
        if (c.size() & 1) f.push_back(0);
    }
    return f;
}

Bytes words(const std::vector<uint16_t>& v, bool be) {
    Bytes b;
    for (uint16_t x : v) put16(b, x, be);
    return b;
}

// Where line `i` of `size` lines is stored when they are kept in `factor`
// fields: written out here the slow way, by listing the fields.
std::vector<uint32_t> storedPositions(uint32_t size, uint32_t factor) {
    std::vector<uint32_t> position(size);
    uint32_t next = 0;
    for (uint32_t field = 0; field < factor; ++field)
        for (uint32_t i = field; i < size; i += factor) position[i] = next++;
    return position;
}

// The frame as a DNG with interleave factors stores it.
std::vector<uint16_t> interleaved(const std::vector<uint16_t>& pixels, uint32_t w, uint32_t h, uint32_t planes,
                                  uint32_t rowFactor, uint32_t columnFactor) {
    const std::vector<uint32_t> row = storedPositions(h, rowFactor), column = storedPositions(w, columnFactor);
    std::vector<uint16_t> stored(pixels.size());
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t k = 0; k < planes; ++k)
                stored[(static_cast<size_t>(row[y]) * w + column[x]) * planes + k] =
                    pixels[(static_cast<size_t>(y) * w + x) * planes + k];
    return stored;
}

std::vector<uint16_t> noise(size_t count, uint32_t seed, uint32_t mask = 0xffff) {
    testutil::Random rnd(seed);
    std::vector<uint16_t> v(count);
    for (uint16_t& x : v) x = static_cast<uint16_t>(rnd.next() & mask);
    return v;
}

// ---- rows and columns stored field by field: files built by hand --------------------------

void testInterleave(const fs::path& dir) {
    struct Case {
        uint32_t w, h, planes, rowFactor, columnFactor;
    };
    // Even and odd sizes, one direction and both, factors that leave fields
    // of different lengths, and a factor as large as the image.
    const Case cases[] = {{8, 6, 1, 2, 1}, {8, 6, 1, 1, 2}, {8, 6, 1, 2, 2},  {9, 7, 1, 2, 2},  {13, 11, 1, 6, 6},
                          {7, 5, 3, 2, 3}, {10, 4, 1, 3, 1}, {5, 5, 1, 5, 5}, {31, 17, 1, 2, 2}};
    uint32_t seed = 100;
    for (const Case& c : cases)
        for (bool be : {false, true}) {
            const std::vector<uint16_t> pixels = noise(static_cast<size_t>(c.w) * c.h * c.planes, ++seed);
            HandImage img;
            img.width = c.w;
            img.height = c.h;
            img.planes = c.planes;
            img.tileHeight = c.h;
            img.chunks = {words(interleaved(pixels, c.w, c.h, c.planes, c.rowFactor, c.columnFactor), be)};
            if (c.rowFactor > 1) img.extra.push_back(shorts(50975, {static_cast<int>(c.rowFactor)}, be, 3));
            if (c.columnFactor > 1) img.extra.push_back(longs(52547, {c.columnFactor}, be));
            const StoredRawImage back = readStoredRawImage(writeFile(dir, "jxl_hand_fields.dng", handDng(img, be)));
            CHECK(back.width == c.w && back.height == c.h && back.planes == c.planes);
            CHECK(back.samples == pixels);
        }

    // Factors that cannot be are damage, not a crash.
    for (uint32_t bad : {0u, 9u, 70000u}) {
        HandImage img;
        img.width = 8;
        img.height = 6;
        img.tileHeight = 6;
        img.chunks = {words(noise(48, 5), false)};
        img.extra.push_back(longs(52547, {bad}));
        const DngCheck c = checkDng(writeFile(dir, "jxl_hand_fields.dng", handDng(img)));
        CHECK(c.rawImage == State::Damaged);
    }
}

// ---- the checksum of compressed data: files built by hand -------------------------------------

void testCompressedDigest(const fs::path& dir) {
    // Tiles of any bytes: the checksum does not look inside them.
    for (uint32_t compression : {52546u, 34892u}) {
        HandImage img;
        img.width = 40;
        img.height = 30;
        img.planes = 3;
        img.compression = compression;
        img.tiled = true;
        img.tileWidth = 16;
        img.tileHeight = 16;
        Md5 all;
        for (uint32_t i = 0; i < 6; ++i) {
            Bytes tile(50 + i * 7);
            for (size_t k = 0; k < tile.size(); ++k) tile[k] = static_cast<uint8_t>(k * (i + 3) + i);
            const Md5Digest d = Md5::of(tile.data(), tile.size());
            all.update(d.data(), d.size());
            img.chunks.push_back(tile);
        }
        // Lossy JPEG may keep its tables apart from the tiles; they count as well.
        Bytes tables = {0xff, 0xd8, 0xff, 0xdb, 0, 3, 1, 0xff, 0xd9};
        Md5 withTables = all;
        if (compression == 34892) {
            const Md5Digest d = Md5::of(tables.data(), tables.size());
            withTables.update(d.data(), d.size());
        }
        const Md5Digest tilesOnly = all.finish();
        const Md5Digest expected = compression == 34892 ? withTables.finish() : tilesOnly;

        HandImage complete = img;
        if (compression == 34892) complete.extra.push_back(bytes(347, tables));
        complete.extra.push_back(digestTag(51111, expected));
        const Bytes sound = handDng(complete);
        const fs::path file = writeFile(dir, "jxl_hand_digest.dng", sound);
        DngCheck c = checkDng(file);
        CHECK(c.rawImage == State::Intact && c.intact() && !c.damaged());
        CHECK(c.rawDetail == "NewRawImageDigest");
        CHECK(c.width == 40 && c.height == 30 && c.planes == 3);
        for (unsigned threads : {1u, 4u}) CHECK(checkDng(file, threads).rawImage == State::Intact);

        // One bit anywhere in a tile, or in the tables.
        const tiffreader::Ifd ifd = tiffreader::parseFirstIfd(std::string(sound.begin(), sound.end()));
        for (uint32_t tile : {0u, 3u, 5u}) {
            Bytes b = sound;
            b[ifd.at(324).u32(tile) + 20] ^= 0x10;
            c = checkDng(writeFile(dir, "jxl_hand_digest.dng", b));
            CHECK(c.rawImage == State::Damaged && c.damaged());
            CHECK(c.rawDetail == "the raw image data does not match its checksum");
        }
        if (compression == 34892) {
            Bytes b = sound;
            b[ifd.at(347).offset + 6] ^= 0x01;
            CHECK(checkDng(writeFile(dir, "jxl_hand_digest.dng", b)).rawImage == State::Damaged);
            // Without the tables counted the checksum would be another.
            HandImage wrong = img;
            wrong.extra.push_back(bytes(347, tables));
            wrong.extra.push_back(digestTag(51111, tilesOnly));
            CHECK(checkDng(writeFile(dir, "jxl_hand_digest.dng", handDng(wrong))).rawImage == State::Damaged);
        }
        // The older tag is compared the same way.
        HandImage old = img;
        if (compression == 34892) old.extra.push_back(bytes(347, tables));
        old.extra.push_back(digestTag(50972, expected));
        c = checkDng(writeFile(dir, "jxl_hand_digest.dng", handDng(old)));
        CHECK(c.rawImage == State::Intact && c.rawDetail == "RawImageDigest");

        // A tile missing from the end of the file.
        Bytes cut(sound.begin(), sound.end() - 30);
        c = checkDng(writeFile(dir, "jxl_hand_digest.dng", cut));
        CHECK(c.truncated && c.damaged());

        // No checksum: nothing to compare. (The tiles here are not pictures;
        // a build with libjxl finds that out for JPEG XL.)
        c = checkDng(writeFile(dir, "jxl_hand_digest.dng", handDng(img)));
        if (compression == 52546 && jxlAvailable())
            CHECK(c.rawImage == State::Damaged);
        else
            CHECK(c.rawImage == State::Unverified && !c.damaged());
    }
}

// ---- frames for the writer ---------------------------------------------------------------

RawImage frame(uint32_t width, uint32_t height, uint32_t spp, uint32_t seed) {
    RawImage img;
    img.width = width;
    img.height = height;
    img.samplesPerPixel = spp;
    img.colorPlanes = spp == 1 ? 3 : spp;
    img.activeBottom = height;
    img.activeRight = width;
    img.cropWidth = width;
    img.cropHeight = height;
    img.blackLevel.assign(spp, 64.0);
    img.whiteLevel = {16383, 16383, 16383, 16383};
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}, {0.1, 0.2, 0.3}}};
    img.uniqueCameraModel = "Testmake Model T";
    if (spp == 1) {
        img.isCfa = true;
        img.cfaRows = img.cfaCols = 2;
        img.cfaPattern = {0, 1, 1, 2};
    }
    // A scene with structure (so that it compresses) and noise on top, each
    // colour of the mosaic at its own level, and the extremes present.
    testutil::Random rnd(seed);
    img.pixels.resize(img.sampleCount());
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            for (uint32_t k = 0; k < spp; ++k) {
                const uint32_t colour = spp == 1 ? (y % 2) * 2 + x % 2 : k;
                const uint32_t level = 400 + 2500 * colour + (x * 7 + y * 3) % 1500 + rnd.below(64);
                img.pixels[(static_cast<size_t>(y) * width + x) * spp + k] = static_cast<uint16_t>(level);
            }
    img.pixels.front() = 0;
    img.pixels.back() = 65535;
    return img;
}

void makeXTrans(RawImage& img) {
    img.cfaRows = img.cfaCols = 6;
    img.cfaPattern = {1, 1, 0, 1, 1, 2, 1, 1, 2, 1, 1, 0, 2, 0, 1, 0, 2, 1,
                      1, 1, 2, 1, 1, 0, 1, 1, 0, 1, 1, 2, 0, 2, 1, 2, 0, 1};
}

Md5Digest digestOfTiles(const std::string& file, const tiffreader::Ifd& raw) {
    Md5 all;
    for (uint32_t i = 0; i < raw.at(324).count; ++i) {
        const Md5Digest d = Md5::of(file.data() + raw.at(324).u32(i), raw.at(325).u32(i));
        all.update(d.data(), d.size());
    }
    return all.finish();
}

void keep(const fs::path& dir, const std::string& name, const RawImage& img) {
    std::ofstream out(dir / (name + ".raw16"), std::ios::binary | std::ios::trunc);
    const Bytes le = words(img.pixels, false);
    out.write(reinterpret_cast<const char*>(le.data()), static_cast<std::streamsize>(le.size()));
}

// ---- without libjxl: refused, cleanly ---------------------------------------------------------

void testWithoutLibrary() {
    CHECK(jxlVersion().empty());
    const std::vector<uint16_t> pixels(16, 7);
    bool threw = false;
    try {
        encodeJxlLossless(pixels.data(), 4, 4, 1, 3);
    } catch (const std::runtime_error& e) {
        threw = contains(e.what(), "no JPEG XL support");
    }
    CHECK(threw);
    threw = false;
    try {
        const uint8_t bytes[4] = {0xff, 0x0a, 0, 0};
        decodeJxl(bytes, sizeof bytes, 100);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        DngWriteOptions options;
        options.compression = DngCompression::JpegXl;
        std::ostringstream out(std::ios::binary);
        writeDng(frame(64, 48, 1, 1), out, options);
    } catch (const std::runtime_error& e) {
        threw = contains(e.what(), "no JPEG XL support");
    }
    CHECK(threw);
}

// ---- the codec ------------------------------------------------------------------------------------

void testCodec() {
    CHECK(!jxlVersion().empty() && jxlVersion().find('.') != std::string::npos);

    struct Case {
        uint32_t w, h, channels;
    };
    uint32_t seed = 1;
    for (const Case& c : {Case{1, 1, 1}, Case{3, 5, 1}, Case{64, 64, 1}, Case{257, 129, 1}, Case{1, 1, 3},
                          Case{40, 23, 3}, Case{300, 2, 1}})
        for (int effort : {1, 3, 5})
            // Full-range noise, values of a 12-bit sensor, and a flat picture.
            for (uint32_t mask : {0xffffu, 0x0fffu, 0u}) {
                std::vector<uint16_t> pixels = noise(static_cast<size_t>(c.w) * c.h * c.channels, ++seed, mask);
                if (mask == 0) std::fill(pixels.begin(), pixels.end(), uint16_t{65535});
                const std::vector<uint8_t> jxl = encodeJxlLossless(pixels.data(), c.w, c.h, c.channels, effort);
                // A bare codestream, or the container libjxl uses to mark
                // data that needs more than the basic level of the format,
                // as sixteen lossless bits do.
                const bool bare = jxl.size() > 2 && jxl[0] == 0xff && jxl[1] == 0x0a;
                const bool boxed = jxl.size() > 12 && jxl[3] == 0x0c && jxl[4] == 'J' && jxl[5] == 'X' && jxl[6] == 'L';
                CHECK(bare || boxed);
                const JxlPicture back = decodeJxl(jxl.data(), jxl.size(), pixels.size());
                CHECK(back.width == c.w && back.height == c.h && back.channels == c.channels);
                CHECK(back.bitsPerSample == 16 && back.originalValues);
                CHECK(back.samples == pixels);
            }

    // Effort out of range is brought into range, not refused.
    const std::vector<uint16_t> pixels = noise(32 * 32, 9, 0x3fff);
    CHECK(decodeJxl(encodeJxlLossless(pixels.data(), 32, 32, 1, -4).data(),
                    encodeJxlLossless(pixels.data(), 32, 32, 1, -4).size(), pixels.size()).samples == pixels);
    CHECK(!encodeJxlLossless(pixels.data(), 32, 32, 1, 99).empty());

    // What the encoder cannot take.
    for (const Case& c : {Case{0, 4, 1}, Case{4, 0, 1}, Case{4, 4, 2}, Case{4, 4, 4}, Case{4, 4, 0}}) {
        bool threw = false;
        try {
            encodeJxlLossless(pixels.data(), c.w, c.h, c.channels, 3);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // What the decoder must refuse: nothing, garbage, a stream cut short at
    // any place, a picture larger than the caller has room for.
    const std::vector<uint8_t> good = encodeJxlLossless(pixels.data(), 32, 32, 1, 3);
    const auto refused = [](const uint8_t* data, size_t size, size_t room) {
        try {
            decodeJxl(data, size, room);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    CHECK(refused(nullptr, 0, 1000));
    const std::vector<uint8_t> garbage = {'n', 'o', 't', ' ', 'j', 'x', 'l', 0, 1, 2, 3, 4};
    CHECK(refused(garbage.data(), garbage.size(), 1000));
    for (size_t cut : {size_t{1}, size_t{2}, size_t{8}, good.size() / 2, good.size() - 1})
        CHECK(refused(good.data(), cut, pixels.size()));
    CHECK(refused(good.data(), good.size(), pixels.size() - 1));
    // Changed bytes: refused or decoded to something, never a crash.
    testutil::Random rnd(77);
    for (int round = 0; round < 300; ++round) {
        std::vector<uint8_t> bad = good;
        bad[rnd.below(static_cast<uint32_t>(bad.size()))] ^= static_cast<uint8_t>(1u << rnd.below(8));
        try {
            const JxlPicture p = decodeJxl(bad.data(), bad.size(), pixels.size());
            CHECK(p.samples.size() <= pixels.size());
        } catch (const std::runtime_error&) {
        }
    }
}

// ---- the writer and the reader ------------------------------------------------------------------

void testFiles(const fs::path& dir, bool keepFiles) {
    struct Case {
        const char* name;
        uint32_t w, h, spp;
        bool xtrans;
        uint32_t tileSize;
    };
    const Case cases[] = {
        {"bayer", 1024, 768, 1, false, 512},      {"bayer_odd", 1037, 771, 1, false, 512},
        {"bayer_many_tiles", 600, 410, 1, false, 64}, {"bayer_small", 50, 38, 1, false, 512},
        {"bayer_tiny", 2, 2, 1, false, 512},      {"bayer_strip", 3, 300, 1, false, 64},
        {"xtrans", 612, 408, 1, true, 512},       {"xtrans_odd", 617, 413, 1, true, 64},
        {"rgb", 333, 201, 3, false, 128},         {"mono", 320, 240, 1, false, 128},
    };
    uint32_t seed = 500;
    for (const Case& c : cases) {
        RawImage img = frame(c.w, c.h, c.spp, ++seed);
        if (c.xtrans) makeXTrans(img);
        const bool mono = std::string(c.name) == "mono";
        if (mono) {
            img.isCfa = false;
            img.colorPlanes = 1;
            img.cfaPattern.clear();
            img.cfaRows = img.cfaCols = 0;
        }
        const uint32_t rowFactor = img.isCfa && img.cfaRows < c.h ? img.cfaRows : 1;
        const uint32_t columnFactor = img.isCfa && img.cfaCols < c.w ? img.cfaCols : 1;

        DngWriteOptions options;
        options.compression = DngCompression::JpegXl;
        options.tileSize = c.tileSize;
        options.jxlEffort = 2;
        const fs::path file = dir / (std::string("jxl_") + c.name + ".dng");
        writeDng(img, file, options);
        const std::string bytes = tiffreader::slurp(file);
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(bytes);
        // The raw image is the first sub-directory when there is a thumbnail.
        const tiffreader::Ifd raw = ifd0.count(330) ? tiffreader::parseIfd(bytes, ifd0.at(330).u32(0)) : ifd0;

        // ---- what the directory says ----
        CHECK(raw.at(259).u16() == 52546);
        CHECK(raw.at(258).u16() == 16);
        CHECK(raw.at(256).u32() == c.w && raw.at(257).u32() == c.h);
        const uint32_t tw = raw.at(322).u32(), th = raw.at(323).u32();
        CHECK(tw >= 1 && tw <= c.w && th >= 1 && th <= c.h);
        // No tile is larger than asked for, and none much smaller than it could be.
        CHECK(tw <= c.tileSize && th <= c.tileSize);
        const uint32_t across = (c.w + tw - 1) / tw, down = (c.h + th - 1) / th;
        CHECK(raw.at(324).count == across * down && raw.at(325).count == across * down);
        if (rowFactor > 1)
            CHECK(raw.at(50975).number() == rowFactor);
        else
            CHECK(raw.count(50975) == 0);
        if (columnFactor > 1)
            CHECK(raw.at(52547).number() == columnFactor);
        else
            CHECK(raw.count(52547) == 0);
        CHECK(raw.at(52553).type == 11 && raw.at(52553).u32() == 0);  // distance 0.0: lossless
        CHECK(raw.at(52554).u32() == 2);
        CHECK(ifd0.at(50706).data == (std::vector<uint8_t>{1, 7, 1, 0}));
        CHECK(ifd0.at(50707).data == (std::vector<uint8_t>{1, 7, static_cast<uint8_t>(columnFactor > 1 ? 1 : 0), 0}));
        // Where the frame divides evenly, tile edges fall on field edges.
        if (c.w % columnFactor == 0 && (c.w / columnFactor) % tw == 0) CHECK(c.w % tw == 0);

        // ---- every tile is a JPEG XL picture of the tile's size ----
        for (uint32_t i = 0; i < raw.at(324).count; ++i) {
            const JxlPicture tile = decodeJxl(reinterpret_cast<const uint8_t*>(bytes.data()) + raw.at(324).u32(i),
                                              raw.at(325).u32(i), static_cast<size_t>(tw) * th * c.spp);
            CHECK(tile.width == tw && tile.height == th && tile.channels == c.spp && tile.originalValues);
            // The first tile lies inside the first field: the samples of
            // every n-th row and column, n the period of the mosaic.
            if (i == 0 && tw * columnFactor <= c.w + columnFactor - 1 && th * rowFactor <= c.h + rowFactor - 1) {
                bool same = true;
                for (uint32_t y = 0; y < th && y * rowFactor < c.h; ++y)
                    for (uint32_t x = 0; x < tw && x * columnFactor < c.w; ++x)
                        for (uint32_t k = 0; k < c.spp; ++k)
                            if (tile.samples[(static_cast<size_t>(y) * tw + x) * c.spp + k] !=
                                img.pixels[(static_cast<size_t>(y) * rowFactor * c.w + x * columnFactor) * c.spp + k])
                                same = false;
                CHECK(same);
            }
        }

        // ---- the checksum is that of the compressed tiles ----
        const Md5Digest tilesDigest = digestOfTiles(bytes, raw);
        CHECK(ifd0.at(51111).data == Bytes(tilesDigest.begin(), tilesDigest.end()));
        // The identifier is the one a lossless-JPEG DNG of the same frame gets.
        {
            std::ostringstream other(std::ios::binary);
            DngWriteOptions plain;
            writeDng(img, other, plain);
            const tiffreader::Ifd plain0 = tiffreader::parseFirstIfd(other.str());
            CHECK(plain0.at(50781).data == ifd0.at(50781).data);
            CHECK(plain0.at(51111).data != ifd0.at(51111).data);
        }

        // ---- and the samples come back ----
        const StoredRawImage back = readStoredRawImage(file, 3);
        CHECK(back.width == c.w && back.height == c.h && back.planes == c.spp);
        CHECK(back.samples == img.pixels);
        DngCheck check = checkDng(file);
        CHECK(check.rawImage == State::Intact && check.intact());

        // ---- damage ----
        {
            Bytes b(bytes.begin(), bytes.end());
            b[raw.at(324).u32(raw.at(324).count - 1) + raw.at(325).u32(raw.at(325).count - 1) / 2] ^= 0x20;
            check = checkDng(writeFile(dir, "jxl_damaged.dng", b));
            CHECK(check.rawImage == State::Damaged);
        }
        // Without checksums a damaged tile still shows, by not decoding or by
        // nothing at all; a sound file is "not verified".
        {
            DngWriteOptions bare = options;
            bare.digests = false;
            const fs::path plainFile = dir / "jxl_nodigest.dng";
            writeDng(img, plainFile, bare);
            check = checkDng(plainFile);
            CHECK(check.rawImage == State::Unverified && !check.damaged());
            CHECK(readStoredRawImage(plainFile).samples == img.pixels);
        }

        // The same file whatever the number of threads.
        {
            std::ostringstream one(std::ios::binary), four(std::ios::binary);
            DngWriteOptions o = options;
            o.threads = 1;
            writeDng(img, one, o);
            o.threads = 4;
            writeDng(img, four, o);
            CHECK(one.str() == four.str());
        }

        if (keepFiles) {
            keep(dir, std::string("jxl_") + c.name, img);
        } else {
            std::error_code ec;
            fs::remove(file, ec);
        }
    }

    // Two or four samples per pixel have no JPEG XL form in DNG.
    for (uint32_t spp : {2u, 4u}) {
        RawImage img = frame(64, 48, spp, 3);
        img.colorPlanes = spp == 2 ? 3 : 4;
        DngWriteOptions options;
        options.compression = DngCompression::JpegXl;
        bool threw = false;
        try {
            std::ostringstream out(std::ios::binary);
            writeDng(img, out, options);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // A tile that is a JPEG XL picture of the wrong size is damage.
    {
        const std::vector<uint16_t> small = noise(8 * 8, 4, 0xfff);
        HandImage img;
        img.width = 16;
        img.height = 16;
        img.compression = 52546;
        img.tiled = true;
        img.tileWidth = 16;
        img.tileHeight = 16;
        const std::vector<uint8_t> tile = encodeJxlLossless(small.data(), 8, 8, 1, 2);
        img.chunks = {Bytes(tile.begin(), tile.end())};
        bool threw = false;
        try {
            readStoredRawImage(writeFile(dir, "jxl_hand_size.dng", handDng(img)));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(checkDng(dir / "jxl_hand_size.dng").rawImage == State::Damaged);
    }

    std::error_code ec;
    for (const char* name : {"jxl_damaged.dng", "jxl_nodigest.dng", "jxl_hand_size.dng"}) fs::remove(dir / name, ec);
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path();
    const bool keepFiles = argc > 2;

    testInterleave(dir);
    testCompressedDigest(dir);
    if (jxlAvailable()) {
        testCodec();
        testFiles(dir, keepFiles);
    } else {
        testWithoutLibrary();
        std::printf("test_jxl: this build has no libjxl; the codec and the writer were not tried\n");
    }
    std::error_code ec;
    for (const char* name : {"jxl_hand_fields.dng", "jxl_hand_digest.dng"}) fs::remove(dir / name, ec);
    return testutil::finish("test_jxl");
}
