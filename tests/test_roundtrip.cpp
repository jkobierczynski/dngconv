// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Writes synthetic raw frames as DNG and reads them back through the raw
// decoder: every sample and the essential metadata must survive unchanged.
// Needs no camera files, so it runs anywhere.
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "dng_writer.hpp"
#include "raw_reader.hpp"
#include "test_util.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace fs = std::filesystem;

namespace {

RawImage baseImage(uint32_t width, uint32_t height, uint32_t spp) {
    RawImage img;
    img.width = width;
    img.height = height;
    img.samplesPerPixel = spp;
    img.colorPlanes = 3;
    img.activeTop = 0;
    img.activeLeft = 0;
    img.activeBottom = height;
    img.activeRight = width;
    img.cropWidth = width;
    img.cropHeight = height;
    img.blackLevel.assign(spp, 0.0);
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8293, -0.1789, -0.1094, }, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}, {0, 0, 0}}};
    img.hasAsShotNeutral = true;
    img.asShotNeutral = {0.5, 1.0, 0.625, 1.0};
    img.make = "Testmake";
    img.model = "Model T";
    img.uniqueCameraModel = "Testmake Model T";
    img.dateTime = "2026:10:08 22:00:00";
    img.exposureTime = 1.0 / 250.0;
    img.fNumber = 5.6;
    img.iso = 800;
    img.focalLength = 50;
    return img;
}

void fillPixels(RawImage& img, uint32_t maxValue, uint64_t seed, bool noise) {
    testutil::Random rnd(seed);
    img.pixels.resize(img.sampleCount());
    size_t i = 0;
    for (uint32_t y = 0; y < img.height; ++y)
        for (uint32_t x = 0; x < img.width; ++x)
            for (uint32_t k = 0; k < img.samplesPerPixel; ++k) {
                uint32_t v;
                if (noise)
                    v = rnd.next() % (maxValue + 1);
                else
                    v = (x * 5 + y * 3 + ((x & 1) + 2 * (y & 1)) * 300 + k * 150 + rnd.below(40)) %
                        (maxValue + 1);
                img.pixels[i++] = static_cast<uint16_t>(v);
            }
}

void setBayer(RawImage& img, const std::vector<uint8_t>& pattern) {
    img.isCfa = true;
    img.cfaRows = img.cfaCols = 2;
    img.cfaPattern = pattern;
}

struct Case {
    std::string name;
    RawImage image;
    bool checkPatternAndLevels = true;
};

void runCase(const Case& tc, const fs::path& dir, DngCompression compression, unsigned threads,
             DngByteOrder order = DngByteOrder::MatchSource) {
    const RawImage& src = tc.image;
    const std::string label =
        tc.name + (compression == DngCompression::None ? "-none" : "-lossless") +
        (order == DngByteOrder::Big ? "-be" : order == DngByteOrder::Little ? "-le" : "");
    const fs::path file = dir / ("dngconv_test_" + label + ".dng");

    try {
        DngWriteOptions wo;
        wo.compression = compression;
        wo.threads = threads;
        wo.tileSize = 256;
        wo.byteOrder = order;
        writeDng(src, file, wo);

        const std::string head = tiffreader::slurp(file).substr(0, 2);
        if (order == DngByteOrder::Big) CHECK(head == "MM");
        if (order == DngByteOrder::Little) CHECK(head == "II");

        const RawReadResult back = readRaw(file);
        const RawImage& b = back.image;
        CHECK(back.sourceIsDng);
        CHECK(b.width == src.width);
        CHECK(b.height == src.height);
        CHECK(b.samplesPerPixel == src.samplesPerPixel);
        CHECK(b.pixels == src.pixels);
        CHECK(b.activeTop == src.activeTop);
        CHECK(b.activeLeft == src.activeLeft);
        CHECK(b.activeBottom == src.activeBottom);
        CHECK(b.activeRight == src.activeRight);
        CHECK(b.orientation == src.orientation);
        CHECK(b.make == src.make);
        CHECK(b.model == src.model);
        CHECK(b.isCfa == src.isCfa);
        if (src.isCfa && tc.checkPatternAndLevels) {
            CHECK(b.cfaRows == src.cfaRows && b.cfaCols == src.cfaCols);
            CHECK(b.cfaPattern == src.cfaPattern);
            // Compare black levels cell by cell over one common period.
            bool blackOk = !b.blackLevel.empty();
            for (uint32_t r = 0; r < 12 && blackOk; ++r)
                for (uint32_t c = 0; c < 12 && blackOk; ++c) {
                    const double want =
                        src.blackLevel[(r % src.blackRows) * src.blackCols + c % src.blackCols];
                    const double got =
                        b.blackLevel[(r % b.blackRows) * b.blackCols + c % b.blackCols];
                    if (want != got) blackOk = false;
                }
            CHECK(blackOk);
        }
        CHECK_NEAR(b.whiteLevel[0], src.whiteLevel[0], 0.5);
        CHECK_NEAR(b.iso, src.iso, 0.5);
        CHECK_NEAR(b.exposureTime, src.exposureTime, 1e-6);
        CHECK_NEAR(b.fNumber, src.fNumber, 1e-3);
        CHECK_NEAR(b.focalLength, src.focalLength, 1e-3);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "case %s: %s\n", label.c_str(), e.what());
        CHECK(false);
    }
    if (testutil::failureCount())
        std::fprintf(stderr, "(after case %s; file kept: %s)\n", label.c_str(),
                     file.u8string().c_str());
    else {
        std::error_code ec;
        fs::remove(file, ec);
    }
}

TiffField field(uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> data) {
    TiffField f;
    f.tag = tag;
    f.type = type;
    f.count = count;
    f.data = std::move(data);
    return f;
}

std::vector<uint8_t> le32(std::initializer_list<uint32_t> values) {
    std::vector<uint8_t> out;
    for (uint32_t v : values)
        for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(v >> shift));
    return out;
}

std::vector<uint8_t> ascii(const std::string& text) {
    std::vector<uint8_t> out(text.begin(), text.end());
    out.push_back(0);
    return out;
}

// A stand-in for what the source parser delivers: EXIF fields of several
// types, a GPS directory, descriptive tags and a maker note.
SourceMetadata sampleMetadata(bool bigEndianNote, uint32_t noteOffset) {
    SourceMetadata m;
    m.exif.push_back(field(33434, 5, 1, le32({1, 60})));                 // ExposureTime 1/60
    m.exif.push_back(field(37380, 10, 1, le32({0xffffffffu, 3})));       // ExposureBias -1/3
    m.exif.push_back(field(41986, 3, 1, {1, 0}));                        // ExposureMode manual
    m.exif.push_back(field(37510, 7, 12, {'A', 'S', 'C', 'I', 'I', 0, 0, 0, 't', 'e', 's', 't'}));
    m.exif.push_back(field(42036, 2, 9, ascii("Lens 50L")));             // LensModel
    m.exif.push_back(field(41989, 3, 2, {1, 0, 2}));                     // broken: 3 bytes for 2 shorts
    m.gps.push_back(field(0, 1, 4, {2, 3, 0, 0}));
    m.gps.push_back(field(1, 2, 2, ascii("S")));
    m.gps.push_back(field(2, 5, 3, le32({33, 1, 51, 1, 2529, 100})));
    m.ifd0.push_back(field(33432, 2, 12, ascii("(c) Someone")));
    m.ifd0.push_back(field(270, 2, 12, ascii("from source")));
    for (int i = 0; i < 301; ++i) m.makerNote.data.push_back(static_cast<uint8_t>(i * 7 + 3));
    m.makerNote.bigEndian = bigEndianNote;
    m.makerNote.originalOffset = noteOffset;
    return m;
}

std::vector<uint8_t> expectedPrivateData(const MakerNote& note) {
    std::vector<uint8_t> out = {'A', 'd', 'o', 'b', 'e', 0, 'M', 'a', 'k', 'N'};
    const uint32_t length = static_cast<uint32_t>(note.data.size() + 6);
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(length >> shift));
    out.push_back(note.bigEndian ? 'M' : 'I');
    out.push_back(note.bigEndian ? 'M' : 'I');
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(note.originalOffset >> shift));
    out.insert(out.end(), note.data.begin(), note.data.end());
    if (out.size() & 1) out.push_back(0);
    return out;
}

// Writes an image carrying copied metadata and checks where everything went.
void checkCopiedMetadata(const RawImage& base, const fs::path& dir, bool bigEndianNote,
                         uint32_t noteOffset, bool expectPinned) {
    RawImage img = base;
    img.source = sampleMetadata(bigEndianNote, noteOffset);
    const fs::path file = dir / "dngconv_test_metadata.dng";
    writeDng(img, file);
    const std::string bytes = tiffreader::slurp(file);
    CHECK(tiffreader::isBigEndian(bytes) == bigEndianNote);

    const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(bytes);
    CHECK(ifd0.at(33432).text() == "(c) Someone");
    CHECK(ifd0.at(270).text() == "from source");
    CHECK(ifd0.at(50740).type == 1);
    CHECK(ifd0.at(50740).data == expectedPrivateData(img.source.makerNote));

    const tiffreader::Ifd exif = tiffreader::parseIfd(bytes, ifd0.at(34665).u32());
    // The camera's value replaces the decoder's 1/250.
    CHECK(exif.at(33434).u32(0) == 1 && exif.at(33434).u32(1) == 60);
    CHECK(exif.at(37380).type == 10 && static_cast<int32_t>(exif.at(37380).u32(0)) == -1 &&
          exif.at(37380).u32(1) == 3);
    CHECK(exif.at(41986).u16() == 1);
    CHECK(exif.at(37510).type == 7 && exif.at(37510).count == 12 && exif.at(37510).data[8] == 't');
    CHECK(exif.at(42036).text() == "Lens 50L");
    CHECK(exif.count(41989) == 0);         // the broken field is left out
    CHECK(exif.at(34855).u16() == 800);    // decoder values fill what the camera lacks
    CHECK(exif.at(37500).type == 7 && exif.at(37500).data == img.source.makerNote.data);
    if (expectPinned)
        CHECK(exif.at(37500).offset == noteOffset);
    else
        CHECK(exif.at(37500).offset != noteOffset && exif.at(37500).offset % 2 == 0);

    const tiffreader::Ifd gps = tiffreader::parseIfd(bytes, ifd0.at(34853).u32());
    CHECK(gps.size() == 3 && gps.at(1).text() == "S");
    CHECK(gps.at(2).u32(4) == 2529 && gps.at(2).u32(5) == 100);

    // The decoder still gets the picture out, sample for sample.
    try {
        RawReadOptions ro;
        ro.loadPreview = false;
        const RawReadResult back = readRaw(file, ro);
        CHECK(back.image.pixels == img.pixels);
        CHECK(back.image.activeLeft == img.activeLeft && back.image.activeTop == img.activeTop);
        // ... and our own source parser finds the copied metadata again.
        CHECK(back.image.source.makerNote.data == img.source.makerNote.data);
        CHECK(back.image.source.makerNote.bigEndian == bigEndianNote);
        CHECK(back.image.source.gps.size() == 3);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "metadata case (%s note at %u): %s\n",
                     bigEndianNote ? "big-endian" : "little-endian", noteOffset, e.what());
        CHECK(false);
    }

    // Without maker notes: neither copy is written and the file is little-endian.
    DngWriteOptions plain;
    plain.makerNotes = false;
    writeDng(img, file, plain);
    const std::string plainBytes = tiffreader::slurp(file);
    CHECK(!tiffreader::isBigEndian(plainBytes));
    const tiffreader::Ifd plain0 = tiffreader::parseFirstIfd(plainBytes);
    CHECK(plain0.count(50740) == 0);
    const tiffreader::Ifd plainExif = tiffreader::parseIfd(plainBytes, plain0.at(34665).u32());
    CHECK(plainExif.count(37500) == 0 && plainExif.count(33434) == 1);

    if (!testutil::failureCount()) {
        std::error_code ec;
        fs::remove(file, ec);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path();
    std::vector<Case> cases;

    {   // Bayer RGGB, 12 bit, odd frame size, masked borders, per-cell black.
        Case c{"bayer-rggb", baseImage(1037, 771, 1)};
        setBayer(c.image, {0, 1, 1, 2});
        fillPixels(c.image, 4095, 1, false);
        c.image.activeTop = 6;
        c.image.activeLeft = 10;
        c.image.activeBottom = 762;
        c.image.activeRight = 1030;
        c.image.cropLeft = 4;
        c.image.cropTop = 2;
        c.image.cropWidth = 1008;
        c.image.cropHeight = 752;
        c.image.blackRows = c.image.blackCols = 2;
        c.image.blackLevel = {256, 257, 258, 259};
        c.image.whiteLevel = {4000, 4000, 4000, 4000};
        c.image.orientation = 6;
        cases.push_back(c);
    }
    {   // Bayer GBRG, 14 bit, frame smaller than one tile.
        Case c{"bayer-gbrg", baseImage(120, 90, 1)};
        setBayer(c.image, {1, 2, 0, 1});
        fillPixels(c.image, 16383, 2, false);
        c.image.blackLevel = {1024};
        c.image.whiteLevel = {16383, 16383, 16383, 16383};
        cases.push_back(c);
    }
    {   // Odd width below the tile size: exercises the single-component path.
        Case c{"bayer-narrow-odd", baseImage(121, 77, 1)};
        setBayer(c.image, {0, 1, 1, 2});
        fillPixels(c.image, 4095, 7, false);
        c.image.whiteLevel = {4095, 4095, 4095, 4095};
        cases.push_back(c);
    }
    {   // Bayer, full 16-bit noise: worst case for the entropy coder.
        Case c{"bayer-noise", baseImage(515, 300, 1)};
        setBayer(c.image, {2, 1, 1, 0});
        fillPixels(c.image, 65535, 3, true);
        c.image.whiteLevel = {65535, 65535, 65535, 65535};
        cases.push_back(c);
    }
    {   // Fuji X-Trans style 6x6 mosaic.
        Case c{"xtrans", baseImage(606, 402, 1)};
        c.image.isCfa = true;
        c.image.cfaRows = c.image.cfaCols = 6;
        c.image.cfaPattern = {1, 1, 0, 1, 1, 2,  1, 1, 2, 1, 1, 0,  2, 0, 1, 0, 2, 1,
                              1, 1, 2, 1, 1, 0,  1, 1, 0, 1, 1, 2,  0, 2, 1, 2, 0, 1};
        fillPixels(c.image, 16383, 4, false);
        c.image.blackLevel = {1022};
        c.image.whiteLevel = {16000, 16000, 16000, 16000};
        cases.push_back(c);
    }
    {   // Full-colour ("linear raw") data, three samples per pixel.
        Case c{"linear-rgb", baseImage(333, 201, 3)};
        fillPixels(c.image, 16383, 5, false);
        c.image.blackLevel = {0, 0, 0};
        c.image.whiteLevel = {16383, 16383, 16383, 16383};
        cases.push_back(c);
    }
    {   // With a preview in IFD0 the raw image moves to a sub-IFD. Only the
        // frame header of the "JPEG" matters for the container structure.
        Case c{"with-preview", baseImage(400, 300, 1)};
        setBayer(c.image, {0, 1, 1, 2});
        fillPixels(c.image, 4095, 6, false);
        c.image.whiteLevel = {4095, 4095, 4095, 4095};
        c.image.previewJpeg = {0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x08,
                               0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff,
                               0xd9};
        cases.push_back(c);
    }

    for (const Case& c : cases) {
        for (DngByteOrder order : {DngByteOrder::Little, DngByteOrder::Big}) {
            runCase(c, dir, DngCompression::LosslessJpeg, 0, order);
            runCase(c, dir, DngCompression::None, 1, order);
        }
    }

    // Metadata copied from the source: note at an ordinary offset, right
    // after the header, at an unusable (odd) offset, and from a big-endian
    // source, which makes the whole DNG big-endian.
    checkCopiedMetadata(cases.front().image, dir, false, 1000, true);
    checkCopiedMetadata(cases.front().image, dir, false, 8, true);
    checkCopiedMetadata(cases.front().image, dir, false, 1001, false);
    checkCopiedMetadata(cases.front().image, dir, true, 1000, true);
    checkCopiedMetadata(cases.front().image, dir, true, 300000, true);     // amid the pixel data
    checkCopiedMetadata(cases.front().image, dir, false, 50000000, false); // beyond the file's end
    // Same result regardless of thread count.
    runCase(cases.front(), dir, DngCompression::LosslessJpeg, 1);

    // Container structure, checked with the independent TIFF parser.
    {
        // No preview: the raw image is IFD0, with the DNG identification tags.
        RawImage img = cases.front().image;
        img.zeroIsBadPixel = true;
        img.lensInfo = {24, 240, 4, 0};  // last aperture unknown
        img.gps.valid = true;
        img.gps.latitude = {50, 55, 30.5};
        img.gps.latitudeRef = 'N';
        img.gps.longitude = {5, 23, 0};
        img.gps.longitudeRef = 'E';
        img.gps.hasAltitude = true;
        img.gps.altitude = 42.5;
        const fs::path file = dir / "dngconv_test_structure.dng";
        writeDng(img, file);
        const std::string bytes = tiffreader::slurp(file);
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(bytes);
        CHECK(ifd0.at(254).u32() == 0);                 // full-resolution image
        CHECK(ifd0.at(262).u16() == 32803);             // colour filter array
        CHECK(ifd0.at(259).u16() == 7);                 // lossless JPEG
        CHECK(ifd0.at(50706).data == std::vector<uint8_t>({1, 4, 0, 0}));
        CHECK(ifd0.at(50707).data == std::vector<uint8_t>({1, 3, 0, 0}));
        CHECK(ifd0.at(50708).text() == "Testmake Model T");
        CHECK(ifd0.at(271).text() == "Testmake" && ifd0.at(272).text() == "Model T");
        CHECK(ifd0.at(274).u16() == 6);
        CHECK(ifd0.at(33421).u16(0) == 2 && ifd0.at(33421).u16(1) == 2);
        CHECK(ifd0.at(33422).data == std::vector<uint8_t>({0, 1, 1, 2}));
        CHECK(ifd0.at(50829).u32(0) == 6 && ifd0.at(50829).u32(1) == 10 &&
              ifd0.at(50829).u32(2) == 762 && ifd0.at(50829).u32(3) == 1030);
        CHECK(ifd0.at(50719).u32(0) == 4 && ifd0.at(50719).u32(1) == 2);
        CHECK(ifd0.at(50720).u32(0) == 1008 && ifd0.at(50720).u32(1) == 752);
        CHECK(ifd0.at(50717).number() == 4000);
        CHECK(ifd0.at(50721).count == 9 && ifd0.at(50728).count == 3);
        CHECK(ifd0.at(50736).u32(4) == 4 && ifd0.at(50736).u32(6) == 0 &&
              ifd0.at(50736).u32(7) == 0);
        // Tiles cover the frame: ceil(1037/512) x ceil(771/512).
        CHECK(ifd0.at(322).u32() == 512 && ifd0.at(323).u32() == 512);
        CHECK(ifd0.at(324).count == 6 && ifd0.at(325).count == 6);
        // Defect-pixel opcode. The active area starts at an even row and an
        // even column, so the frame origin has the same phase: red = 0.
        CHECK(ifd0.at(51008).type == 7);
        CHECK(ifd0.at(51008).data ==
              std::vector<uint8_t>({0, 0, 0, 1, 0, 0, 0, 4, 1, 3, 0, 0, 0, 0, 0, 1,
                                    0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0}));
        // EXIF sub-directory.
        const tiffreader::Ifd exif = tiffreader::parseIfd(bytes, ifd0.at(34665).u32());
        CHECK(exif.at(33434).u32(0) == 1 && exif.at(33434).u32(1) == 250);
        CHECK(exif.at(34855).u16() == 800);
        CHECK(exif.at(36867).text() == "2026:10:08 22:00:00");
        // GPS sub-directory.
        const tiffreader::Ifd gps = tiffreader::parseIfd(bytes, ifd0.at(34853).u32());
        CHECK(gps.at(1).text() == "N" && gps.at(3).text() == "E");
        CHECK(gps.at(2).count == 3 && gps.at(2).u32(0) == 50 && gps.at(2).u32(1) == 1);
        CHECK(gps.at(2).u32(4) == 61 && gps.at(2).u32(5) == 2);  // 30.5 seconds
        CHECK(gps.at(4).u32(0) == 5 && gps.at(4).u32(2) == 23);
        CHECK(gps.at(5).data.at(0) == 0);                        // above sea level
        CHECK(gps.at(6).u32(0) == 85 && gps.at(6).u32(1) == 2);  // 42.5 m

        // Active area starting on an odd column shifts the phase: the frame
        // origin is then a green pixel on a red row.
        img.activeLeft = 11;
        img.cropWidth = 1000;
        writeDng(img, file);
        const tiffreader::Ifd shifted = tiffreader::parseFirstIfd(tiffreader::slurp(file));
        CHECK(shifted.at(51008).data.at(27) == 1);

        // With a preview, IFD0 describes the JPEG and points at the raw IFD.
        writeDng(cases.back().image, file);
        const std::string withPreview = tiffreader::slurp(file);
        const tiffreader::Ifd p0 = tiffreader::parseFirstIfd(withPreview);
        CHECK(p0.at(254).u32() == 1);                   // reduced-resolution image
        CHECK(p0.at(256).u32() == 8 && p0.at(257).u32() == 8);
        CHECK(p0.at(262).u16() == 6);                   // YCbCr
        CHECK(p0.at(530).u16(0) == 2 && p0.at(530).u16(1) == 2);
        CHECK(p0.at(279).u32() == cases.back().image.previewJpeg.size());
        CHECK(p0.count(50706) == 1 && p0.count(50721) == 1);  // camera tags stay in IFD0
        CHECK(p0.at(50707).data == std::vector<uint8_t>({1, 1, 0, 0}));
        const tiffreader::Ifd rawIfd = tiffreader::parseIfd(withPreview, p0.at(330).u32());
        CHECK(rawIfd.at(254).u32() == 0);
        CHECK(rawIfd.at(256).u32() == 400 && rawIfd.at(257).u32() == 300);
        CHECK(rawIfd.count(51008) == 0);

        if (!testutil::failureCount()) {
            std::error_code ec;
            fs::remove(file, ec);
        }
    }

    // Inconsistent input is refused rather than written.
    {
        RawImage bad = cases.front().image;
        bad.pixels.pop_back();
        bool threw = false;
        try {
            validate(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);

        bad = cases.front().image;
        bad.activeRight = bad.width + 1;
        threw = false;
        try {
            validate(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    return testutil::finish("test_roundtrip");
}
