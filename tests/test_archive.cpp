// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Keeping an archive of DNGs: the note of its source a DNG carries and the
// comparison that updating by content rests on; adding fields to a file in
// place; and writing the checksums into files that have none.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "dng_append.hpp"
#include "dng_check.hpp"
#include "dng_digest.hpp"
#include "dng_writer.hpp"
#include "jxl_codec.hpp"
#include "original_raw.hpp"
#include "sidecar.hpp"
#include "source_record.hpp"
#include "test_util.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace fs = std::filesystem;
using tiffreader::rd16;
using tiffreader::rd32;
using File = std::string;  // a whole file's bytes

namespace {

using State = DngCheck::State;
constexpr uint16_t kDngVersion = 50706, kRawDigest = 50972, kOriginalDigest = 50973, kOriginalData = 50828,
                   kNewRawDigest = 51111;

File readAll(const fs::path& p) { return tiffreader::slurp(p); }

void writeAll(const fs::path& p, const File& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string hex(const Md5Digest& d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

std::string hex(const std::vector<uint8_t>& d) {
    Md5Digest a{};
    if (d.size() == a.size()) std::copy(d.begin(), d.end(), a.begin());
    return hex(a);
}

// Where the entry of a tag lies in the first directory; 0 if it has none.
size_t entryAt(const File& f, uint16_t tag) {
    const size_t at = rd32(f, 4);
    const unsigned count = rd16(f, at);
    for (unsigned i = 0; i < count; ++i)
        if (rd16(f, at + 2 + 12 * i) == tag) return at + 2 + 12 * i;
    return 0;
}

// The file with entries taken out of its first directory, nothing else moved.
File withoutTags(const File& f, std::initializer_list<uint16_t> tags) {
    const size_t at = rd32(f, 4);
    const unsigned count = rd16(f, at);
    File entries;
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint16_t tag = rd16(f, at + 2 + 12 * i);
        bool drop = false;
        for (uint16_t t : tags) drop = drop || t == tag;
        if (drop) continue;
        entries += f.substr(at + 2 + 12 * i, 12);
        ++kept;
    }
    const File link = f.substr(at + 2 + 12 * count, 4);
    File out = f;
    const bool be = tiffreader::isBigEndian(f);
    out[at + (be ? 0 : 1)] = static_cast<char>(kept >> 8);
    out[at + (be ? 1 : 0)] = static_cast<char>(kept & 0xff);
    out.replace(at + 2, entries.size(), entries);
    out.replace(at + 2 + entries.size(), 4, link);
    return out;
}

// Everything of `before` is still in `after`, apart from the four bytes of
// the header that point to the first directory.
bool onlyGrew(const File& before, const File& after) {
    return after.size() > before.size() && after.compare(0, 4, before, 0, 4) == 0 &&
           after.compare(8, before.size() - 8, before, 8, before.size() - 8) == 0;
}

RawImage mosaic(uint32_t w, uint32_t h, uint64_t seed) {
    RawImage img;
    img.width = w;
    img.height = h;
    img.samplesPerPixel = 1;
    img.isCfa = true;
    img.cfaRows = img.cfaCols = 2;
    img.cfaPattern = {0, 1, 1, 2};
    img.activeBottom = h;
    img.activeRight = w;
    img.cropWidth = w;
    img.cropHeight = h;
    img.blackLevel = {0};
    img.whiteLevel = {4095, 4095, 4095, 4095};
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}, {0, 0, 0}}};
    img.uniqueCameraModel = "Testmake Model T";
    testutil::Random rnd(seed);
    img.pixels.resize(img.sampleCount());
    for (size_t i = 0; i < img.pixels.size(); ++i)
        img.pixels[i] = static_cast<uint16_t>(((i % w) * 3 + (i / w) * 5 + rnd.below(64)) & 0xfff);
    return img;
}

RawImage fullColour(uint32_t w, uint32_t h) {
    RawImage img = mosaic(w, h, 3);
    img.isCfa = false;
    img.samplesPerPixel = 3;
    img.cfaRows = img.cfaCols = 0;
    img.cfaPattern.clear();
    img.blackLevel = {0, 0, 0};
    img.pixels.resize(img.sampleCount());
    for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<uint16_t>((i * 31) & 0x3fff);
    return img;
}

void addOriginal(RawImage& img, size_t size) {
    img.originalFileName = "shot.raw";
    img.originalFile.resize(size);
    for (size_t i = 0; i < size; ++i) img.originalFile[i] = static_cast<uint8_t>((i * 7) ^ (i >> 5));
}

const char* const kXmp =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Rating=\"4\"/>"
    "</rdf:RDF></x:xmpmeta>";

SourceRecord unpack(const std::string& text) { return unpackSourceRecord(text.data(), text.size()); }

template <typename Function>
bool throwsRuntime(Function f) {
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    } catch (...) {
    }
    return false;
}

template <typename Function>
bool throwsInvalid(Function f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
    }
    return false;
}

TiffField byteField(uint16_t tag, uint16_t type, const std::string& data) {
    TiffField f;
    f.tag = tag;
    f.type = type;
    f.count = static_cast<uint32_t>(data.size());
    f.data.assign(data.begin(), data.end());
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = (argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path()) / "archive_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // ---- the record of a DNG's source: its text ----------------------------------------------
    {
        SourceRecord r;
        r.present = true;
        r.fileSize = 17137664;
        r.fileDigest = Md5::of("abc", 3);
        const std::string plain = packSourceRecord(r);
        CHECK(plain == "dngconv source record 1: size 17137664, MD5 900150983cd24fb0d6963f7d28e17f72");
        r.hasSidecar = true;
        r.sidecarDigest = Md5::of("", 0);
        const std::string both = packSourceRecord(r);
        CHECK(both == plain + ", sidecar MD5 d41d8cd98f00b204e9800998ecf8427e");

        SourceRecord back = unpack(plain);
        CHECK(back.present && back.fileSize == 17137664 && back.fileDigest == r.fileDigest && !back.hasSidecar);
        back = unpack(both);
        CHECK(back.present && back.hasSidecar && back.sidecarDigest == r.sidecarDigest &&
              back.fileDigest == r.fileDigest);
        // As a TIFF reader hands it over, with its terminating zero.
        back = unpackSourceRecord(both.c_str(), both.size() + 1);
        CHECK(back.present && back.hasSidecar);
        // The largest size there is.
        r.fileSize = UINT64_MAX;
        r.hasSidecar = false;
        CHECK(unpack(packSourceRecord(r)).fileSize == UINT64_MAX);

        // Anything else is not a record.
        CHECK(!unpackSourceRecord(nullptr, 0).present);
        CHECK(!unpack("").present);
        CHECK(!unpack("dngconv source record 1: size ").present);
        CHECK(!unpack(plain.substr(0, plain.size() - 1)).present);  // a digit short
        CHECK(!unpack(plain + "0").present);
        CHECK(!unpack(plain + " ").present);
        CHECK(!unpack(both + ", more").present);
        CHECK(!unpack(" " + plain).present);
        std::string other = plain;
        other[22] = '2';  // a later format
        CHECK(other.find("record 2:") != std::string::npos && !unpack(other).present);
        other = plain;
        other[plain.size() - 1] = 'F';  // capitals are not written
        CHECK(!unpack(other).present);
        other = plain;
        other[plain.size() - 3] = 'g';
        CHECK(!unpack(other).present);
        CHECK(!unpack("dngconv source record 1: size , MD5 900150983cd24fb0d6963f7d28e17f72").present);
        CHECK(!unpack("dngconv source record 1: size -5, MD5 900150983cd24fb0d6963f7d28e17f72").present);
        CHECK(!unpack("dngconv source record 1: size 99999999999999999999999, MD5 900150983cd24fb0d6963f7d28e17f72")
                   .present);
        CHECK(!unpack("dngconv source record 1: size 5, sidecar MD5 900150983cd24fb0d6963f7d28e17f72").present);
        CHECK(!unpack("Somebody else's use of tag 65300").present);
    }

    // ---- the MD5 of a file --------------------------------------------------------------------
    {
        for (size_t size : {size_t{0}, size_t{1}, size_t{4096}, size_t{1} << 20, (size_t{1} << 20) + 1, size_t{2500000}}) {
            File bytes(size, '\0');
            for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>(i * 131 + (i >> 9));
            writeAll(dir / "bytes.bin", bytes);
            uint64_t counted = 99;
            CHECK(digestOfFile(dir / "bytes.bin", counted) == Md5::of(bytes.data(), bytes.size()));
            CHECK(counted == size);
        }
        uint64_t counted = 0;
        CHECK(throwsRuntime([&] { digestOfFile(dir / "not-there.bin", counted); }));
    }

    // ---- the record in a DNG -----------------------------------------------------------------
    {
        RawImage img = mosaic(96, 64, 1);
        const fs::path dng = dir / "record.dng";

        // Without a checksum of the source there is nothing to note.
        writeDng(img, dng);
        CHECK(!tiffreader::parseFirstIfd(readAll(dng)).count(kSourceRecordTag));
        CHECK(!readSourceRecord(dng).present);

        img.hasSourceDigest = true;
        img.sourceFileSize = 123456;
        img.sourceFileDigest = Md5::of("source", 6);
        for (DngByteOrder order : {DngByteOrder::Little, DngByteOrder::Big}) {
            DngWriteOptions options;
            options.byteOrder = order;
            writeDng(img, dng, options);
            const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(readAll(dng));
            CHECK(ifd0.count(kSourceRecordTag) && ifd0.at(kSourceRecordTag).type == 2);
            CHECK(ifd0.at(kSourceRecordTag).text() ==
                  "dngconv source record 1: size 123456, MD5 " + hex(img.sourceFileDigest));
            const SourceRecord r = readSourceRecord(dng);
            CHECK(r.present && r.fileSize == 123456 && r.fileDigest == img.sourceFileDigest && !r.hasSidecar);
            CHECK(checkDng(dng).intact());
        }

        // A sidecar that goes into the DNG goes into the record.
        img.xmpSidecar.assign(kXmp, kXmp + std::char_traits<char>::length(kXmp));
        img.xmpSidecarName = "shot.xmp";
        writeDng(img, dng);
        SourceRecord r = readSourceRecord(dng);
        CHECK(r.present && r.hasSidecar && r.sidecarDigest == Md5::of(img.xmpSidecar.data(), img.xmpSidecar.size()));
        DngWriteOptions options;
        options.xmpSidecar = false;  // one that stays out does not
        writeDng(img, dng, options);
        r = readSourceRecord(dng);
        CHECK(r.present && !r.hasSidecar);

        // The record can be left out.
        options.sourceRecord = false;
        writeDng(img, dng, options);
        CHECK(!tiffreader::parseFirstIfd(readAll(dng)).count(kSourceRecordTag));

        // Files that are no DNGs, and tags that are not records.
        CHECK(!readSourceRecord(dir / "not-there.dng").present);
        writeAll(dir / "text.dng", "This is not a DNG file at all, whatever its name says.");
        CHECK(!readSourceRecord(dir / "text.dng").present);
        writeAll(dir / "empty.dng", "");
        CHECK(!readSourceRecord(dir / "empty.dng").present);
        writeDng(img, dng, options);
        appendToFirstIfd(dng, {byteField(kSourceRecordTag, 2, std::string("Somebody else's text", 21))});
        CHECK(tiffreader::parseFirstIfd(readAll(dng)).count(kSourceRecordTag));
        CHECK(!readSourceRecord(dng).present);
        writeDng(img, dng, options);
        const std::string text = packSourceRecord(r);
        appendToFirstIfd(dng, {byteField(kSourceRecordTag, 7, text)});  // the right text, the wrong type
        CHECK(!readSourceRecord(dng).present);
        writeDng(img, dng, options);
        appendToFirstIfd(dng, {byteField(kSourceRecordTag, 2, std::string(text.c_str(), text.size() + 1))});
        CHECK(readSourceRecord(dng).present);  // and as this program writes it
    }

    // ---- is a DNG still what its source is? ------------------------------------------------------
    {
        using Compared = SourceComparison::State;
        const fs::path folder = dir / "compare";
        fs::create_directories(folder);
        const fs::path source = folder / "shot.raw", sidecar = folder / "shot.xmp", dng = folder / "shot.dng";
        File content(70000, '\0');
        for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>(i * 13 + (i >> 8));
        writeAll(source, content);

        // What the reader and the writer do with a source file and its sidecar.
        const auto convert = [&](bool withSidecar) {
            RawImage img = mosaic(96, 64, 2);
            img.sourceFileDigest = digestOfFile(source, img.sourceFileSize);
            img.hasSourceDigest = true;
            if (withSidecar)
                if (const fs::path found = findXmpSidecar(source); !found.empty()) img.xmpSidecar = readXmpSidecar(found);
            writeDng(img, dng);
        };

        convert(true);
        SourceComparison c = compareWithSource(dng, source, true);
        CHECK(c.state == Compared::UpToDate && c.what.empty());
        CHECK(c.digestKnown && c.fileSize == content.size() &&
              c.fileDigest == Md5::of(content.data(), content.size()));

        // File times do not matter, in either direction.
        const auto now = fs::last_write_time(dng);
        fs::last_write_time(source, now + std::chrono::hours(48));
        CHECK(compareWithSource(dng, source, true).state == Compared::UpToDate);
        fs::last_write_time(source, now - std::chrono::hours(24 * 365));
        fs::last_write_time(dng, now - std::chrono::hours(24 * 700));
        CHECK(compareWithSource(dng, source, true).state == Compared::UpToDate);

        // One byte of the source changed, its size and time as they were.
        const auto sourceTime = fs::last_write_time(source);
        File changed = content;
        changed[40000] = static_cast<char>(changed[40000] ^ 1);
        writeAll(source, changed);
        fs::last_write_time(source, sourceTime);
        c = compareWithSource(dng, source, true);
        CHECK(c.state == Compared::Changed && c.what == "source changed");
        CHECK(c.digestKnown && c.fileDigest == Md5::of(changed.data(), changed.size()));
        // A different size is seen without reading the file.
        writeAll(source, content + "x");
        c = compareWithSource(dng, source, true);
        CHECK(c.state == Compared::Changed && c.what == "source changed" && !c.digestKnown);
        writeAll(source, content);
        CHECK(compareWithSource(dng, source, true).state == Compared::UpToDate);

        // A sidecar appears, changes, and goes again.
        writeAll(sidecar, kXmp);
        c = compareWithSource(dng, source, true);
        CHECK(c.state == Compared::Changed && c.what == "sidecar added");
        CHECK(compareWithSource(dng, source, false).state == Compared::UpToDate);  // not when sidecars are left out
        convert(true);
        CHECK(readSourceRecord(dng).hasSidecar);
        CHECK(compareWithSource(dng, source, true).state == Compared::UpToDate);
        File edited = kXmp;
        edited.replace(edited.find("Rating=\"4\""), 10, "Rating=\"5\"");
        writeAll(sidecar, edited);
        c = compareWithSource(dng, source, true);
        CHECK(c.state == Compared::Changed && c.what == "sidecar changed");
        writeAll(source, changed);
        c = compareWithSource(dng, source, true);
        CHECK(c.state == Compared::Changed && c.what == "source changed, sidecar changed");
        writeAll(source, content);
        c = compareWithSource(dng, source, false);
        CHECK(c.state == Compared::Changed && c.what == "sidecar no longer there");
        fs::remove(sidecar);
        c = compareWithSource(dng, source, true);
        CHECK(c.state == Compared::Changed && c.what == "sidecar no longer there");
        // A file of that name that holds no XMP is no sidecar.
        convert(true);
        writeAll(sidecar, "just some notes");
        CHECK(compareWithSource(dng, source, true).state == Compared::UpToDate);
        fs::remove(sidecar);

        // A DNG that says nothing about its source.
        RawImage img = mosaic(96, 64, 2);
        writeDng(img, dng);
        c = compareWithSource(dng, source, true);
        CHECK(c.state == Compared::NoRecord && !c.digestKnown);
        CHECK(compareWithSource(folder / "not-there.dng", source, true).state == Compared::NoRecord);
        // A source that is gone.
        convert(true);
        fs::remove(source);
        CHECK(throwsRuntime([&] { compareWithSource(dng, source, true); }));
    }

    // ---- adding fields to a file in place --------------------------------------------------------
    {
        const fs::path dng = dir / "append.dng";
        RawImage img = mosaic(160, 120, 4);
        addOriginal(img, 3000);
        for (DngByteOrder order : {DngByteOrder::Little, DngByteOrder::Big}) {
            for (bool oddSize : {false, true}) {
                DngWriteOptions options;
                options.byteOrder = order;
                writeDng(img, dng, options);
                if (oddSize != ((fs::file_size(dng) & 1) != 0)) {
                    std::ofstream more(dng, std::ios::binary | std::ios::app);
                    more.put('\x55');  // a byte nothing refers to
                }
                const File before = readAll(dng);
                CHECK((before.size() & 1) == (oddSize ? 1u : 0u));
                uint32_t linkBefore = 99;
                const tiffreader::Ifd old = tiffreader::parseIfd(before, rd32(before, 4), &linkBefore);

                const std::string sixteen("0123456789abcdef", 16);
                const AppendReceipt receipt = appendToFirstIfd(
                    dng, {byteField(60001, 2, std::string("ab\0", 3)), byteField(60000, 1, sixteen),
                          byteField(270, 2, std::string("A description, somewhere in the middle\0", 39)),
                          byteField(2, 7, std::string("\x01\x02\x03\x04\x05", 5))});
                const File after = readAll(dng);
                CHECK(onlyGrew(before, after));
                CHECK(receipt.oldSize == before.size() && receipt.newSize == after.size());
                CHECK(receipt.oldFirstIfd == rd32(before, 4));
                CHECK(receipt.bigEndian == (order == DngByteOrder::Big));
                // The new directory starts on an even offset, after the old end.
                const uint32_t at = rd32(after, 4);
                CHECK(at % 2 == 0 && at >= before.size() && at <= before.size() + 1);

                // Every old entry as it was (parseIfd checks the order of the
                // tags and that values lie on even offsets), the new ones too.
                uint32_t linkAfter = 77;
                const tiffreader::Ifd now = tiffreader::parseIfd(after, at, &linkAfter);
                CHECK(now.size() == old.size() + 4 && linkAfter == linkBefore);
                for (const auto& [tag, entry] : old) {
                    CHECK(now.count(tag));
                    if (!now.count(tag)) continue;
                    const tiffreader::Entry& e = now.at(tag);
                    CHECK(e.type == entry.type && e.count == entry.count && e.offset == entry.offset &&
                          e.data == entry.data);
                }
                CHECK(now.at(60000).type == 1 && now.at(60000).count == 16 &&
                      std::string(now.at(60000).data.begin(), now.at(60000).data.end()) == sixteen);
                CHECK(now.at(60000).offset >= before.size());
                CHECK(now.at(60001).type == 2 && now.at(60001).offset == 0 && now.at(60001).text() == "ab");
                CHECK(now.at(270).text() == "A description, somewhere in the middle");
                CHECK(now.at(2).type == 7 && now.at(2).count == 5 && now.at(2).data[4] == 5);
                // The file is the DNG it was.
                const DngCheck c = checkDng(dng);
                CHECK(c.intact() && c.original == State::Intact);
                CHECK(readStoredRawImage(dng).samples == img.pixels);
                CHECK(unpackOriginalRaw(readEmbeddedOriginal(dng).packed.data(), readEmbeddedOriginal(dng).packed.size()) ==
                      img.originalFile);

                // A second time, onto the appended directory.
                const File once = after;
                appendToFirstIfd(dng, {byteField(60002, 1, "x")});
                const File twice = readAll(dng);
                CHECK(onlyGrew(once, twice) && tiffreader::parseFirstIfd(twice).size() == old.size() + 5);
                CHECK(checkDng(dng).intact());

                // And back.
                writeAll(dng, after);
                undoAppend(dng, receipt);
                CHECK(readAll(dng) == before);
            }
        }

        // What is refused leaves the file alone.
        writeDng(img, dng);
        const File before = readAll(dng);
        CHECK(throwsInvalid([&] { appendToFirstIfd(dng, {byteField(kDngVersion, 1, "abcd")}); }));  // already there
        CHECK(throwsInvalid([&] { appendToFirstIfd(dng, {byteField(60000, 3, "abcd")}); }));        // not bytes
        CHECK(throwsInvalid([&] { appendToFirstIfd(dng, {byteField(60000, 1, "")}); }));
        CHECK(throwsInvalid([&] { appendToFirstIfd(dng, {byteField(60000, 1, "a"), byteField(60000, 1, "b")}); }));
        TiffField wrongCount = byteField(60000, 1, "abc");
        wrongCount.count = 4;
        CHECK(throwsInvalid([&] { appendToFirstIfd(dng, {wrongCount}); }));
        CHECK(readAll(dng) == before);
        CHECK(throwsRuntime([&] { appendToFirstIfd(dir / "not-there.dng", {byteField(60000, 1, "a")}); }));
        for (const char* notTiff : {"", "II*", "This is text.", "II+\0\x08\0\0\0\0\0"}) {
            const File content(notTiff, notTiff[0] == 'I' && notTiff[2] == '+' ? 10 : std::char_traits<char>::length(notTiff));
            writeAll(dir / "odd.tif", content);
            CHECK(throwsRuntime([&] { appendToFirstIfd(dir / "odd.tif", {byteField(60000, 1, "a")}); }));
            CHECK(readAll(dir / "odd.tif") == content);
        }
        // A header that points outside the file, and a directory that runs past its end.
        File broken = before;
        broken[4] = broken[5] = broken[6] = '\xf0';
        writeAll(dir / "odd.tif", broken);
        CHECK(throwsRuntime([&] { appendToFirstIfd(dir / "odd.tif", {byteField(60000, 1, "a")}); }));
        CHECK(readAll(dir / "odd.tif") == broken);
        broken = before.substr(0, rd32(before, 4) + 20);
        writeAll(dir / "odd.tif", broken);
        CHECK(throwsRuntime([&] { appendToFirstIfd(dir / "odd.tif", {byteField(60000, 1, "a")}); }));
        CHECK(readAll(dir / "odd.tif") == broken);
    }

    // ---- writing the checksums a file lacks ------------------------------------------------------
    {
        struct Variant {
            const char* name;
            DngCompression compression;
            DngByteOrder order;
            bool colour;
            bool original;
        };
        std::vector<Variant> variants = {
            {"lossless JPEG", DngCompression::LosslessJpeg, DngByteOrder::Little, false, true},
            {"lossless JPEG, big-endian", DngCompression::LosslessJpeg, DngByteOrder::Big, false, true},
            {"uncompressed", DngCompression::None, DngByteOrder::Little, false, false},
            {"uncompressed, big-endian", DngCompression::None, DngByteOrder::Big, false, true},
            {"three planes", DngCompression::LosslessJpeg, DngByteOrder::Little, true, false},
        };
        if (jxlAvailable()) {
            variants.push_back({"JPEG XL", DngCompression::JpegXl, DngByteOrder::Little, false, true});
            variants.push_back({"JPEG XL, three planes", DngCompression::JpegXl, DngByteOrder::Big, true, false});
        }
        const fs::path dng = dir / "sums.dng";
        for (const Variant& v : variants) {
            RawImage img = v.colour ? fullColour(300, 200) : mosaic(700, 520, 5);
            if (v.original) addOriginal(img, 150000);
            DngWriteOptions options;
            options.compression = v.compression;
            options.byteOrder = v.order;
            writeDng(img, dng, options);
            const File written = readAll(dng);
            const tiffreader::Ifd full = tiffreader::parseFirstIfd(written);
            CHECK(full.count(kNewRawDigest) && full.count(kOriginalDigest) == (v.original ? 1u : 0u));

            // The same file as a writer without checksums would have left it.
            const File bare = withoutTags(written, {kNewRawDigest, kOriginalDigest});
            writeAll(dng, bare);
            DngCheck c = checkDng(dng);
            CHECK(c.rawImage == State::Unverified && !c.damaged() && !c.intact() && c.canAddChecksum);
            CHECK(c.original == (v.original ? State::Unverified : State::Absent));
            CHECK(readAll(dng) == bare);  // looking does not write

            DngChecksumsAdded done = addDngChecksums(dng, 3);
            if (!done.rawImage || done.rawTag != "NewRawImageDigest" || done.original != v.original)
                std::fprintf(stderr, "  (variant: %s)\n", v.name);
            CHECK(done.any() && done.rawImage && done.rawTag == "NewRawImageDigest" && done.original == v.original);
            CHECK(done.check.intact() && !done.check.canAddChecksum);
            const File stamped = readAll(dng);
            CHECK(onlyGrew(bare, stamped) && done.bytesAdded == stamped.size() - bare.size());
            // The checksums are the ones the writer computed, and typed like them.
            const tiffreader::Ifd now = tiffreader::parseFirstIfd(stamped);
            CHECK(now.size() == full.size());
            CHECK(now.count(kNewRawDigest) && now.at(kNewRawDigest).data == full.at(kNewRawDigest).data &&
                  now.at(kNewRawDigest).type == full.at(kNewRawDigest).type);
            if (v.original)
                CHECK(now.count(kOriginalDigest) && now.at(kOriginalDigest).data == full.at(kOriginalDigest).data &&
                      now.at(kOriginalDigest).type == full.at(kOriginalDigest).type);
            CHECK(!now.count(kRawDigest));
            CHECK(checkDng(dng).intact());
            CHECK(readStoredRawImage(dng).samples == img.pixels);

            // Nothing is left to add; the file stays as it is.
            done = addDngChecksums(dng);
            CHECK(!done.any() && done.bytesAdded == 0 && done.check.intact());
            CHECK(readAll(dng) == stamped);

            // Had the power gone after the new directory was written and
            // before the header was pointed to it, the file would be this:
            // the old one, with bytes after its end that nothing refers to.
            // It reads as before, and the next attempt does the work again.
            File interrupted = stamped;
            interrupted.replace(4, 4, bare, 4, 4);
            writeAll(dng, interrupted);
            c = checkDng(dng);
            CHECK(!c.damaged() && !c.truncated && c.rawImage == State::Unverified && c.canAddChecksum);
            CHECK(readStoredRawImage(dng).samples == img.pixels);
            done = addDngChecksums(dng);
            CHECK(done.rawImage && done.check.intact() && onlyGrew(interrupted, readAll(dng)));
            CHECK(tiffreader::parseFirstIfd(readAll(dng)).at(kNewRawDigest).data == full.at(kNewRawDigest).data);

            // Only the one that is missing is written.
            if (v.original) {
                writeAll(dng, withoutTags(written, {kOriginalDigest}));
                done = addDngChecksums(dng);
                CHECK(done.original && !done.rawImage && done.rawTag.empty() && done.check.intact());
                CHECK(tiffreader::parseFirstIfd(readAll(dng)).at(kOriginalDigest).data == full.at(kOriginalDigest).data);
                writeAll(dng, withoutTags(written, {kNewRawDigest}));
                done = addDngChecksums(dng);
                CHECK(!done.original && done.rawImage && done.check.intact());
                CHECK(tiffreader::parseFirstIfd(readAll(dng)).at(kNewRawDigest).data == full.at(kNewRawDigest).data);
            }
        }

        // A file of a DNG version older than 1.4 gets the checksum that
        // version defines.
        {
            RawImage img = mosaic(700, 520, 6);
            for (DngCompression compression : {DngCompression::LosslessJpeg, DngCompression::None}) {
                DngWriteOptions options;
                options.compression = compression;
                writeDng(img, dng, options);
                File bare = withoutTags(readAll(dng), {kNewRawDigest});
                const size_t version = entryAt(bare, kDngVersion);
                CHECK(version != 0 && bare[version + 8] == 1 && bare[version + 9] == 4);
                bare[version + 9] = 3;
                writeAll(dng, bare);
                const DngChecksumsAdded done = addDngChecksums(dng);
                CHECK(done.rawImage && done.rawTag == "RawImageDigest" && done.check.intact());
                CHECK(done.check.rawDetail == "RawImageDigest");
                const tiffreader::Ifd now = tiffreader::parseFirstIfd(readAll(dng));
                CHECK(now.count(kRawDigest) && !now.count(kNewRawDigest));
                CHECK(hex(now.at(kRawDigest).data) ==
                      hex(rawImageDigest(img.pixels.data(), img.width, img.height, img.samplesPerPixel)));
                CHECK(now.at(kRawDigest).type == 1 && now.at(kRawDigest).count == 16);
            }
        }

        // Nothing is written to a file that is not sound.
        {
            RawImage img = mosaic(700, 520, 7);
            addOriginal(img, 150000);
            writeDng(img, dng);
            const File written = readAll(dng);
            const File bare = withoutTags(written, {kNewRawDigest, kOriginalDigest});

            // Cut short.
            File damaged = bare.substr(0, bare.size() - 5000);
            writeAll(dng, damaged);
            DngChecksumsAdded done = addDngChecksums(dng);
            CHECK(!done.any() && done.check.damaged() && done.check.truncated && !done.check.canAddChecksum);
            CHECK(readAll(dng) == damaged);

            // Raw data that no longer decodes.
            const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(bare);
            const tiffreader::Ifd raw = tiffreader::parseIfd(bare, ifd0.at(330).u32(0));
            damaged = bare;
            for (size_t i = 0; i < 4; ++i) damaged[raw.at(324).u32(0) + i] = '\0';  // no JPEG starts like this
            writeAll(dng, damaged);
            done = addDngChecksums(dng);
            CHECK(!done.any() && done.check.rawImage == State::Damaged && !done.check.canAddChecksum);
            CHECK(readAll(dng) == damaged);

            // An embedded original that no longer unpacks: the raw image is
            // sound, but the file as a whole is not, and it is left alone.
            damaged = bare;
            const size_t packed = ifd0.at(kOriginalData).offset;
            for (size_t i = 0; i < 64; ++i) damaged[packed + 300 + i] = static_cast<char>(damaged[packed + 300 + i] ^ 0x5a);
            writeAll(dng, damaged);
            done = addDngChecksums(dng);
            CHECK(!done.any() && done.check.original == State::Damaged && done.check.rawImage == State::Unverified);
            CHECK(readAll(dng) == damaged);

            // A checksum that is there but wrong is not replaced.
            damaged = written;
            const size_t digestAt = tiffreader::parseFirstIfd(written).at(kNewRawDigest).offset;
            damaged[digestAt + 3] = static_cast<char>(damaged[digestAt + 3] ^ 1);
            writeAll(dng, damaged);
            done = addDngChecksums(dng);
            CHECK(!done.any() && done.check.rawImage == State::Damaged);
            CHECK(readAll(dng) == damaged);

            // Nor one that is there in a form no reader takes (here: typed as text).
            damaged = written;
            const size_t entry = entryAt(damaged, kNewRawDigest);
            CHECK(entry != 0 && rd16(damaged, entry + 2) == 1);
            damaged[entry + 2] = 2;
            writeAll(dng, damaged);
            done = addDngChecksums(dng);
            CHECK(!done.any() && done.check.rawImage == State::Unverified && !done.check.canAddChecksum);
            CHECK(readAll(dng) == damaged);

            // Should the file not verify when it is read back, it is put
            // back as it was. (That cannot be made to happen from outside;
            // the reading back is replaced here by one that finds fault.)
            writeAll(dng, bare);
            for (int fault = 0; fault < 4; ++fault) {
                std::string said;
                try {
                    detail::addDngChecksums(dng, 2, [&](const fs::path& file) -> DngCheck {
                        // The file is complete and sound at this point.
                        DngCheck seen = checkDng(file);
                        CHECK(seen.intact() && readAll(file).size() > bare.size());
                        if (fault == 0) seen.rawImage = State::Damaged, seen.rawDetail = "made-up damage";
                        if (fault == 1) seen.rawImage = State::Unverified;
                        if (fault == 2) seen.original = State::Unverified;
                        if (fault == 3) throw std::runtime_error("made-up failure");
                        return seen;
                    });
                } catch (const std::runtime_error& e) {
                    said = e.what();
                }
                CHECK(said.find("the file did not verify after its checksums were added (") == 0);
                CHECK(said.find("it was put back as it was") != std::string::npos);
                CHECK(said.find(fault == 0 ? "made-up damage" : fault == 3 ? "made-up failure" : "was not found again") !=
                      std::string::npos);
                CHECK(readAll(dng) == bare);
            }

            // Not DNGs.
            writeAll(dir / "text.dng", "This is not a DNG file at all, whatever its name says.");
            CHECK(throwsRuntime([&] { addDngChecksums(dir / "text.dng"); }));
            CHECK(throwsRuntime([&] { addDngChecksums(dir / "not-there.dng"); }));

            // A file that may not be written to: said, and nothing changed.
            // (An administrator may write to it all the same; then there is
            // nothing to test.)
            writeAll(dng, bare);
            fs::permissions(dng, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
            bool writable = false;
            {
                std::fstream probe(dng, std::ios::in | std::ios::out | std::ios::binary);
                writable = probe.is_open();
            }
            if (!writable) {
                std::string said;
                try {
                    addDngChecksums(dng);
                } catch (const std::runtime_error& e) {
                    said = e.what();
                }
                CHECK(said.find("cannot add checksums: the file cannot be opened for writing") == 0);
                CHECK(readAll(dng) == bare);
            }
            fs::permissions(dng, fs::perms::owner_all);
        }
    }

    return testutil::finish("test_archive");
}
