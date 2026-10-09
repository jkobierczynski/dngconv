// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// The embedded original: packing, unpacking, damaged input, and the way
// through a DNG file and back.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "dng_writer.hpp"
#include "original_raw.hpp"
#include "test_util.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

uint32_t be32(const Bytes& b, size_t at) {
    return (static_cast<uint32_t>(b.at(at)) << 24) | (static_cast<uint32_t>(b.at(at + 1)) << 16) |
           (static_cast<uint32_t>(b.at(at + 2)) << 8) | b.at(at + 3);
}

void putBe32(Bytes& b, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) b.push_back(static_cast<uint8_t>(v >> shift));
}

Bytes makeData(size_t size, bool compressible, uint64_t seed) {
    testutil::Random rnd(seed);
    Bytes data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = compressible ? static_cast<uint8_t>((i / 7) % 23 + rnd.below(2))
                               : static_cast<uint8_t>(rnd.next());
    return data;
}

// A zlib stream written by hand with "stored" (uncompressed) deflate blocks,
// so that unpacking is tested against data the packer did not produce.
Bytes storedZlibStream(const uint8_t* data, size_t size) {
    Bytes out = {0x78, 0x01};
    size_t at = 0;
    do {
        const size_t n = std::min<size_t>(65535, size - at);
        out.push_back(at + n == size ? 1 : 0);  // final-block flag, block type 0
        out.push_back(static_cast<uint8_t>(n & 0xff));
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(~n & 0xff));
        out.push_back(static_cast<uint8_t>((~n >> 8) & 0xff));
        out.insert(out.end(), data + at, data + at + n);
        at += n;
    } while (at < size);
    uint32_t a = 1, b = 0;  // Adler-32
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    putBe32(out, (b << 16) | a);
    return out;
}

// The tag contents for a file, assembled independently of packOriginalRaw.
Bytes handPacked(const Bytes& file) {
    const size_t blocks = (file.size() + 65535) / 65536;
    std::vector<Bytes> streams;
    for (size_t i = 0; i < blocks; ++i) {
        const size_t begin = i * 65536;
        streams.push_back(storedZlibStream(&file[begin], std::min<size_t>(65536, file.size() - begin)));
    }
    Bytes out;
    putBe32(out, static_cast<uint32_t>(file.size()));
    uint32_t offset = static_cast<uint32_t>(4 * (blocks + 2));
    for (const Bytes& s : streams) {
        putBe32(out, offset);
        offset += static_cast<uint32_t>(s.size());
    }
    putBe32(out, offset);
    for (const Bytes& s : streams) out.insert(out.end(), s.begin(), s.end());
    out.resize(out.size() + 28, 0);  // the seven unused items
    return out;
}

bool unpackThrows(const Bytes& packed) {
    try {
        unpackOriginalRaw(packed.data(), packed.size());
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

RawImage smallImage() {
    RawImage img;
    img.width = 640;
    img.height = 480;
    img.samplesPerPixel = 1;
    img.isCfa = true;
    img.cfaRows = img.cfaCols = 2;
    img.cfaPattern = {0, 1, 1, 2};
    img.activeBottom = img.height;
    img.activeRight = img.width;
    img.cropLeft = img.cropTop = 2;
    img.cropWidth = img.width - 4;
    img.cropHeight = img.height - 4;
    img.blackLevel = {0};
    img.whiteLevel = {4095, 4095, 4095, 4095};
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}, {0, 0, 0}}};
    img.uniqueCameraModel = "Testmake Model T";
    img.pixels.resize(img.sampleCount());
    for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<uint16_t>((i * 37) & 0xfff);
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path();

    // ---- pack and unpack ----------------------------------------------------------
    uint64_t seed = 1;
    for (size_t size : {size_t{0}, size_t{1}, size_t{100}, size_t{65535}, size_t{65536},
                        size_t{65537}, size_t{131072}, size_t{200001}}) {
        for (bool compressible : {true, false}) {
            const Bytes file = makeData(size, compressible, seed++);
            const Bytes packed = packOriginalRaw(file, 0);
            CHECK(unpackOriginalRaw(packed.data(), packed.size()) == file);
            CHECK(packOriginalRaw(file, 1) == packed);  // the same with one thread

            // Layout: length, offsets counted from the start, seven zero items.
            const size_t blocks = (size + 65535) / 65536;
            CHECK(be32(packed, 0) == size);
            if (blocks) {
                CHECK(be32(packed, 4) == 4 * (blocks + 2));
                for (size_t i = 0; i < blocks; ++i) {
                    CHECK(be32(packed, 4 * (i + 2)) > be32(packed, 4 * (i + 1)));
                    CHECK(packed.at(be32(packed, 4 * (i + 1))) == 0x78);  // a zlib stream
                }
                CHECK(be32(packed, 4 * (blocks + 1)) + 28 == packed.size());
            } else {
                CHECK(packed.size() == 4 + 28);
            }
            for (size_t i = packed.size() - 28; i < packed.size(); ++i) CHECK(packed[i] == 0);
            if (compressible && size >= 65536) CHECK(packed.size() < size / 2);

            // Data assembled by other means unpacks to the same file.
            if (size > 0) {
                const Bytes other = handPacked(file);
                CHECK(unpackOriginalRaw(other.data(), other.size()) == file);
            }
        }
    }

    // ---- damaged input: an error, never a crash or a wrong result -------------------
    {
        const Bytes file = makeData(150000, true, 77);
        const Bytes packed = packOriginalRaw(file, 1);
        CHECK(unpackThrows({}));
        CHECK(unpackThrows({0, 0}));
        for (size_t cut : {size_t{4}, size_t{9}, size_t{16}, size_t{20}, size_t{200},
                           packed.size() / 2, packed.size() - 29})
            CHECK(unpackThrows(Bytes(packed.begin(), packed.begin() + cut)));

        Bytes bad = packed;
        bad[3] ^= 0x01;  // length off by one
        CHECK(unpackThrows(bad));
        bad = packed;
        bad[0] = 0x7f;  // absurd length
        CHECK(unpackThrows(bad));
        bad = packed;
        bad[7] ^= 0x40;  // first offset moved
        CHECK(unpackThrows(bad));
        bad = packed;
        std::swap(bad[8], bad[12]);
        std::swap(bad[9], bad[13]);
        std::swap(bad[10], bad[14]);
        std::swap(bad[11], bad[15]);  // offsets out of order
        CHECK(unpackThrows(bad));
        bad = packed;
        bad[be32(packed, 4) + 40] ^= 0xff;  // inside the first compressed block
        CHECK(unpackThrows(bad));

        // Random damage anywhere: whatever comes back has the announced length.
        testutil::Random rnd(5);
        for (int round = 0; round < 300; ++round) {
            Bytes fuzzed = packed;
            for (int k = 0; k < 1 + round % 4; ++k)
                fuzzed[rnd.below(static_cast<uint32_t>(fuzzed.size()))] ^= static_cast<uint8_t>(1u << rnd.below(8));
            try {
                const Bytes result = unpackOriginalRaw(fuzzed.data(), fuzzed.size());
                CHECK(result.size() == be32(fuzzed, 0));
            } catch (const std::runtime_error&) {
            }
        }
    }

    // ---- through a DNG file and back -------------------------------------------------
    {
        const fs::path file = dir / "dngconv_test_original.dng";
        const Bytes original = makeData(300000, true, 9);

        for (DngByteOrder order : {DngByteOrder::Little, DngByteOrder::Big}) {
            RawImage img = smallImage();
            img.originalFile = original;
            img.originalFileName = "shot 01.NEF";
            DngWriteOptions options;
            options.byteOrder = order;
            writeDng(img, file, options);

            const EmbeddedOriginal found = readEmbeddedOriginal(file);
            CHECK(found.present);
            CHECK(found.fileName == "shot 01.NEF");
            CHECK(found.originalSize == original.size());
            CHECK(found.hasDigest && found.digestMatches);
            CHECK(unpackOriginalRaw(found.packed.data(), found.packed.size()) == original);

            const EmbeddedOriginal brief = readEmbeddedOriginal(file, false);
            CHECK(brief.present && brief.packed.empty() && brief.originalSize == original.size());

            const std::string bytes = tiffreader::slurp(file);
            const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(bytes);
            CHECK(ifd0.at(50828).type == 7 && ifd0.at(50973).type == 1 && ifd0.at(50973).count == 16);
            CHECK(ifd0.at(50828).data == found.packed);
            // A small original stays with the other values, ahead of the image data.
            const tiffreader::Ifd rawIfd = tiffreader::parseIfd(bytes, ifd0.at(330).u32());
            CHECK(ifd0.at(50828).offset < rawIfd.at(324).u32());
        }

        // A large one is written last, after the pictures.
        {
            RawImage img = smallImage();
            img.originalFile = makeData(5 << 20, false, 10);
            img.originalFileName = "big.raw";
            writeDng(img, file);
            const std::string bytes = tiffreader::slurp(file);
            const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(bytes);
            const tiffreader::Ifd rawIfd = tiffreader::parseIfd(bytes, ifd0.at(330).u32());
            const tiffreader::Entry& tiles = rawIfd.at(324);
            const uint32_t lastTile = tiles.u32(tiles.count - 1);
            CHECK(ifd0.at(50828).offset > lastTile);
            CHECK(ifd0.at(50828).offset + ifd0.at(50828).count <= bytes.size());
            const EmbeddedOriginal found = readEmbeddedOriginal(file);
            CHECK(found.digestMatches);
            CHECK(unpackOriginalRaw(found.packed.data(), found.packed.size()) == img.originalFile);

            // One flipped bit in the stored copy is noticed.
            std::string damaged = bytes;
            damaged[ifd0.at(50828).offset + ifd0.at(50828).count / 2] ^= 0x04;
            std::ofstream(file, std::ios::binary | std::ios::trunc).write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
            const EmbeddedOriginal hurt = readEmbeddedOriginal(file);
            CHECK(hurt.present && hurt.hasDigest && !hurt.digestMatches);
        }

        // Names from a file are reduced to a bare file name.
        const std::pair<const char*, const char*> names[] = {
            {"../../x/evil.NEF", "evil.NEF"}, {"C:\\photos\\a.raw", "a.raw"}, {"/etc/passwd", "passwd"},
            {"..", ""},                       {"dir/", ""},                   {"plain.CR3", "plain.CR3"},
            {"trailing. ", "trailing"},
        };
        for (const auto& [stored, expected] : names) {
            RawImage img = smallImage();
            img.originalFile = Bytes(100, 7);
            img.originalFileName = stored;
            writeDng(img, file);
            const EmbeddedOriginal found = readEmbeddedOriginal(file, false);
            if (found.fileName != expected)
                std::fprintf(stderr, "name '%s' became '%s', expected '%s'\n", stored, found.fileName.c_str(), expected);
            CHECK(found.present && found.fileName == expected);
        }

        // Nothing embedded: by option, and when there is nothing to embed.
        RawImage img = smallImage();
        img.originalFile = original;
        DngWriteOptions without;
        without.embedOriginal = false;
        writeDng(img, file, without);
        CHECK(!readEmbeddedOriginal(file).present);
        img.originalFile.clear();
        writeDng(img, file);
        CHECK(!readEmbeddedOriginal(file).present);
        const tiffreader::Ifd bare = tiffreader::parseFirstIfd(tiffreader::slurp(file));
        CHECK(bare.count(50828) == 0 && bare.count(50973) == 0);

        // Not a TIFF, and not there at all.
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "this is not a DNG file at all, just some text";
        CHECK(!readEmbeddedOriginal(file).present);
        CHECK(!readEmbeddedOriginal(dir / "dngconv_test_no_such_file.dng").present);

        if (!testutil::failureCount()) {
            std::error_code ec;
            fs::remove(file, ec);
        }
    }

    return testutil::finish("test_original");
}
