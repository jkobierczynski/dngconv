// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Feeds the source-metadata parser hand-built files in each container layout
// it understands (plain TIFF in both byte orders, Canon CR3 boxes, Fuji RAF
// and Panasonic RW2 with their EXIF inside an embedded JPEG), then damaged
// ones. Needs no camera files.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "source_metadata.hpp"
#include "test_util.hpp"

using namespace dngconv;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

// ---- building blocks ----------------------------------------------------------

void put16(Bytes& b, bool be, uint16_t v) {
    if (be) {
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v));
    } else {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
    }
}

void put32(Bytes& b, bool be, uint32_t v) {
    if (be) {
        put16(b, true, static_cast<uint16_t>(v >> 16));
        put16(b, true, static_cast<uint16_t>(v));
    } else {
        put16(b, false, static_cast<uint16_t>(v));
        put16(b, false, static_cast<uint16_t>(v >> 16));
    }
}

Bytes enc16(bool be, std::initializer_list<uint16_t> values) {
    Bytes b;
    for (uint16_t v : values) put16(b, be, v);
    return b;
}

Bytes enc32(bool be, std::initializer_list<uint32_t> values) {
    Bytes b;
    for (uint32_t v : values) put32(b, be, v);
    return b;
}

Bytes text(const std::string& s) {
    Bytes b(s.begin(), s.end());
    b.push_back(0);
    return b;
}

struct Field {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    Bytes value;  // already in the byte order of the file
};

// A TIFF structure under construction. Directories are appended one at a
// time, children before parents, so a parent can name its children's offsets.
struct TiffBuilder {
    bool be;
    Bytes b;

    explicit TiffBuilder(bool bigEndian, uint16_t magic = 42) : be(bigEndian) {
        b.push_back(be ? 'M' : 'I');
        b.push_back(be ? 'M' : 'I');
        put16(b, be, magic);
        put32(b, be, 0);  // first IFD, patched by finish()
    }

    // Appends raw bytes and returns where they start.
    uint32_t blob(const Bytes& data) {
        if (b.size() & 1) b.push_back(0);
        const uint32_t at = static_cast<uint32_t>(b.size());
        b.insert(b.end(), data.begin(), data.end());
        return at;
    }

    // Appends a directory; out-of-line values follow it. `valueOffsets`
    // receives the offset of each field's value (0 for inline ones).
    uint32_t ifd(const std::vector<Field>& fields, std::vector<uint32_t>* valueOffsets = nullptr) {
        if (b.size() & 1) b.push_back(0);
        const uint32_t at = static_cast<uint32_t>(b.size());
        uint32_t data = at + 2 + 12 * static_cast<uint32_t>(fields.size()) + 4;
        Bytes table, values;
        put16(table, be, static_cast<uint16_t>(fields.size()));
        if (valueOffsets) valueOffsets->clear();
        for (const Field& f : fields) {
            put16(table, be, f.tag);
            put16(table, be, f.type);
            put32(table, be, f.count);
            if (f.value.size() <= 4) {
                Bytes padded = f.value;
                padded.resize(4, 0);
                table.insert(table.end(), padded.begin(), padded.end());
                if (valueOffsets) valueOffsets->push_back(0);
            } else {
                put32(table, be, data + static_cast<uint32_t>(values.size()));
                if (valueOffsets) valueOffsets->push_back(data + static_cast<uint32_t>(values.size()));
                values.insert(values.end(), f.value.begin(), f.value.end());
                if (values.size() & 1) values.push_back(0);
            }
        }
        put32(table, be, 0);
        b.insert(b.end(), table.begin(), table.end());
        b.insert(b.end(), values.begin(), values.end());
        return at;
    }

    Bytes finish(uint32_t firstIfd) {
        Bytes out = b;
        Bytes off = enc32(be, {firstIfd});
        std::copy(off.begin(), off.end(), out.begin() + 4);
        return out;
    }
};

Bytes box(const char* type, const Bytes& payload) {
    Bytes b;
    put32(b, true, static_cast<uint32_t>(payload.size() + 8));
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

void append(Bytes& to, const Bytes& more) { to.insert(to.end(), more.begin(), more.end()); }

Bytes jpegWithExif(const Bytes& tiff) {
    Bytes j = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10};  // SOI, then a 16-byte APP0
    j.resize(j.size() + 14, 0x4a);
    j.push_back(0xff);
    j.push_back(0xe1);
    put16(j, true, static_cast<uint16_t>(tiff.size() + 8));
    for (char ch : {'E', 'x', 'i', 'f', '\0', '\0'}) j.push_back(static_cast<uint8_t>(ch));
    append(j, tiff);
    append(j, {0xff, 0xda, 0x00, 0x02, 0x12, 0x34, 0xff, 0xd9});
    return j;
}

fs::path writeFile(const fs::path& dir, const std::string& name, const Bytes& data) {
    const fs::path p = dir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return p;
}

const TiffField* find(const std::vector<TiffField>& fields, uint16_t tag) {
    for (const TiffField& f : fields)
        if (f.tag == tag) return &f;
    return nullptr;
}

uint32_t le32(const TiffField& f, size_t index) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | f.data.at(index * 4 + i);
    return v;
}

uint16_t le16(const TiffField& f, size_t index) {
    return static_cast<uint16_t>(f.data.at(index * 2) | (f.data.at(index * 2 + 1) << 8));
}

Bytes makerNoteBytes() {
    Bytes note = {'T', 'E', 'S', 'T', 'N', 'O', 'T', 'E', 0, 1};
    for (int i = 0; i < 90; ++i) note.push_back(static_cast<uint8_t>(i * 5 + 1));
    return note;
}

// A complete camera-style TIFF: IFD0 -> EXIF (with maker note) and GPS.
// `noteOffset` receives where the maker note ended up.
Bytes cameraTiff(bool be, uint32_t* noteOffset = nullptr, uint16_t magic = 42,
                 const std::vector<Field>& extraIfd0 = {}, uint16_t iso = 400) {
    TiffBuilder t(be, magic);
    const uint32_t interop = t.ifd({{1, 2, 4, text("R98")}});

    std::vector<uint32_t> at;
    const uint32_t exif = t.ifd(
        {
            {33434, 5, 1, enc32(be, {1, 125})},                        // ExposureTime
            {34855, 3, 1, enc16(be, {iso})},                           // ISO
            {37380, 10, 1, enc32(be, {0xfffffffeu, 3})},               // ExposureBias -2/3
            {37500, 7, 100, makerNoteBytes()},                         // MakerNote
            {37510, 7, 8, {'A', 'S', 'C', 'I', 'I', 0, 0, 0}},         // UserComment
            {40965, 4, 1, enc32(be, {interop})},                       // InteropIFD
            {41730, 7, 8, {0, 2, 0, 2, 0, 1, 1, 2}},                   // CFAPattern
            {42033, 2, 6, text("SN123")},                              // BodySerialNumber
            {42034, 5, 4, enc32(be, {24, 1, 70, 1, 28, 10, 28, 10})},  // LensSpecification
            {59932, 7, 6, {0, 0, 0, 0, 0, 0}},                         // Padding
            {65000, 12, 1, be ? Bytes{0x3f, 0xf8, 0, 0, 0, 0, 0, 0}    // DOUBLE 1.5
                              : Bytes{0, 0, 0, 0, 0, 0, 0xf8, 0x3f}},
            {65001, 99, 1, {1, 2, 3, 4}},                              // unknown type: skipped
        },
        &at);
    if (noteOffset) *noteOffset = at[3];

    const uint32_t gps = t.ifd({
        {0, 1, 4, {2, 3, 0, 0}},
        {1, 2, 2, text("N")},
        {2, 5, 3, enc32(be, {50, 1, 55, 1, 3050, 100})},
        {5, 1, 1, {0}},
    });

    std::vector<Field> ifd0 = {
        {270, 2, 12, text("a fine day ")},
        {271, 2, 20, text("NIKON CORPORATION  ")},
        {272, 2, 10, text("NIKON D3S")},
        {315, 2, 4, text("Me ")},
        {33432, 2, 9, text("(c) 2026")},
        {34665, 4, 1, enc32(be, {exif})},
        {34853, 4, 1, enc32(be, {gps})},
    };
    for (const Field& f : extraIfd0) ifd0.push_back(f);
    std::sort(ifd0.begin(), ifd0.end(), [](const Field& a, const Field& b) { return a.tag < b.tag; });
    return t.finish(t.ifd(ifd0));
}

void checkExif(const SourceMetadata& m, uint16_t expectIso = 400) {
    const TiffField* exposure = find(m.exif, 33434);
    CHECK(exposure && exposure->type == 5 && exposure->count == 1);
    if (exposure) CHECK(le32(*exposure, 0) == 1 && le32(*exposure, 1) == 125);
    const TiffField* iso = find(m.exif, 34855);
    CHECK(iso && iso->type == 3 && le16(*iso, 0) == expectIso);
    const TiffField* bias = find(m.exif, 37380);
    CHECK(bias && static_cast<int32_t>(le32(*bias, 0)) == -2 && le32(*bias, 1) == 3);
    const TiffField* comment = find(m.exif, 37510);
    CHECK(comment && comment->data.size() == 8 && comment->data[0] == 'A');
    const TiffField* serial = find(m.exif, 42033);
    CHECK(serial && serial->count == 6 && serial->data[0] == 'S');
    const TiffField* lens = find(m.exif, 42034);
    CHECK(lens && lens->count == 4 && le32(*lens, 2) == 70 && le32(*lens, 4) == 28 &&
          le32(*lens, 5) == 10);
    const TiffField* dbl = find(m.exif, 65000);
    CHECK(dbl && dbl->type == 12 && dbl->data == Bytes({0, 0, 0, 0, 0, 0, 0xf8, 0x3f}));
    // Deliberately not copied.
    CHECK(!find(m.exif, 37500) && !find(m.exif, 40965) && !find(m.exif, 41730) &&
          !find(m.exif, 59932) && !find(m.exif, 65001));
    CHECK(m.exif.size() == 7);
}

void checkGps(const SourceMetadata& m) {
    CHECK(m.gps.size() == 4);
    const TiffField* lat = find(m.gps, 2);
    CHECK(lat && lat->count == 3 && le32(*lat, 0) == 50 && le32(*lat, 4) == 3050 &&
          le32(*lat, 5) == 100);
    const TiffField* ref = find(m.gps, 1);
    CHECK(ref && ref->data.size() == 2 && ref->data[0] == 'N');
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path();
    std::vector<fs::path> created;

    // ---- plain TIFF, both byte orders ---------------------------------------
    for (bool be : {false, true}) {
        uint32_t noteAt = 0;
        const Bytes tiff = cameraTiff(be, &noteAt);
        const fs::path p = writeFile(dir, be ? "dngconv_meta_be.nef" : "dngconv_meta_le.cr2", tiff);
        created.push_back(p);
        const SourceMetadataResult r = readSourceMetadata(p);
        CHECK(r.identityFound());
        CHECK(r.make == "NIKON CORPORATION");  // trailing blanks trimmed
        CHECK(r.model == "NIKON D3S");
        checkExif(r.metadata);
        checkGps(r.metadata);
        CHECK(r.metadata.makerNote.data == makerNoteBytes());
        CHECK(r.metadata.makerNote.bigEndian == be);
        CHECK(noteAt != 0 && r.metadata.makerNote.originalOffset == noteAt);
        CHECK(r.metadata.ifd0.size() == 3);
        const TiffField* copyright = find(r.metadata.ifd0, 33432);
        CHECK(copyright && copyright->type == 2 && copyright->data[0] == '(');
    }

    // ---- Olympus-style magic number -------------------------------------------
    {
        const fs::path p = writeFile(dir, "dngconv_meta.orf", cameraTiff(false, nullptr, 0x4f52));
        created.push_back(p);
        CHECK(readSourceMetadata(p).metadata.exif.size() == 7);
    }

    // ---- Canon CR3: four TIFF blocks in boxes ---------------------------------
    {
        TiffBuilder cmt1(false);
        const Bytes ifd0 = cmt1.finish(cmt1.ifd({
            {271, 2, 6, text("Canon")},
            {272, 2, 13, text("Canon EOS R8")},
            {33432, 2, 5, text("(c)X")},
        }));
        TiffBuilder cmt2(false);
        const Bytes exif = cmt2.finish(cmt2.ifd({
            {33434, 5, 1, enc32(false, {1, 250})},
            {34855, 3, 1, enc16(false, {800})},
        }));
        // CMT3: a TIFF header followed directly by the maker note directory.
        TiffBuilder cmt3(false);
        const Bytes note = cmt3.finish(cmt3.ifd({
            {1, 3, 6, enc16(false, {1, 2, 3, 4, 5, 6})},
            {6, 2, 12, text("Canon EOS R")},
        }));
        TiffBuilder cmt4(false);
        const Bytes gps = cmt4.finish(cmt4.ifd({{0, 1, 4, {2, 3, 0, 0}}}));  // version only

        Bytes canon = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                       0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
        append(canon, box("CNCV", text("CanonCR3_001/00.11.00/00.00.00")));
        append(canon, box("CMT1", ifd0));
        append(canon, box("CMT2", exif));
        append(canon, box("CMT3", note));
        append(canon, box("CMT4", gps));
        Bytes other(16, 0x11);  // some other uuid box first
        Bytes moov = box("uuid", other);
        append(moov, box("uuid", canon));
        Bytes file = box("ftyp", {'c', 'r', 'x', ' ', 0, 0, 0, 1, 'c', 'r', 'x', ' ', 'i', 's', 'o', 'm'});
        append(file, box("free", Bytes(40, 0)));
        append(file, box("moov", moov));
        append(file, box("mdat", Bytes(64, 0x77)));

        const fs::path p = writeFile(dir, "dngconv_meta.cr3", file);
        created.push_back(p);
        const SourceMetadataResult r = readSourceMetadata(p);
        CHECK(r.make == "Canon" && r.model == "Canon EOS R8");
        CHECK(r.metadata.exif.size() == 2);
        const TiffField* iso = find(r.metadata.exif, 34855);
        CHECK(iso && le16(*iso, 0) == 800);
        CHECK(r.metadata.ifd0.size() == 1);
        CHECK(r.metadata.gps.empty());  // a GPS block with only its version is dropped
        // The note is the block minus its 8-byte TIFF header, positioned at 8.
        CHECK(r.metadata.makerNote.originalOffset == 8);
        CHECK(!r.metadata.makerNote.bigEndian);
        CHECK(r.metadata.makerNote.data == Bytes(note.begin() + 8, note.end()));
    }

    // ---- Fuji RAF: EXIF inside the embedded JPEG ------------------------------
    {
        uint32_t noteAt = 0;
        const Bytes jpeg = jpegWithExif(cameraTiff(false, &noteAt));
        Bytes file;
        for (char ch : std::string("FUJIFILMCCD-RAW 0201FF123456")) file.push_back(static_cast<uint8_t>(ch));
        const std::string model = "X-T99";
        Bytes modelField(model.begin(), model.end());
        modelField.resize(32, 0);
        append(file, modelField);
        file.resize(84, 0);
        put32(file, true, 160);                                   // JPEG offset
        put32(file, true, static_cast<uint32_t>(jpeg.size()));    // JPEG length
        file.resize(160, 0);
        append(file, jpeg);
        append(file, Bytes(100, 0x42));

        const fs::path p = writeFile(dir, "dngconv_meta.raf", file);
        created.push_back(p);
        const SourceMetadataResult r = readSourceMetadata(p);
        CHECK(r.make == "FUJIFILM" && r.model == "X-T99");  // from the RAF header, not the JPEG
        checkExif(r.metadata);
        checkGps(r.metadata);
        CHECK(r.metadata.makerNote.data == makerNoteBytes());
        CHECK(r.metadata.makerNote.originalOffset == noteAt);  // counted from the JPEG's TIFF header
    }

    // ---- Panasonic RW2: short EXIF of its own, full one in a JPEG -------------
    {
        uint32_t noteAt = 0;
        const Bytes jpeg = jpegWithExif(cameraTiff(false, &noteAt, 42, {}, 1600));
        TiffBuilder t(false, 0x55);
        const uint32_t exif = t.ifd({{34855, 3, 1, enc16(false, {200})}});
        const Bytes file = t.finish(t.ifd({
            {0x2e, 7, static_cast<uint32_t>(jpeg.size()), jpeg},
            {271, 2, 10, text("Panasonic")},
            {272, 2, 8, text("DMC-GF7")},
            {34665, 4, 1, enc32(false, {exif})},
        }));
        const fs::path p = writeFile(dir, "dngconv_meta.rw2", file);
        created.push_back(p);
        const SourceMetadataResult r = readSourceMetadata(p);
        CHECK(r.make == "Panasonic" && r.model == "DMC-GF7");
        checkExif(r.metadata, 200);  // the raw file's own ISO wins over the JPEG's 1600
        checkGps(r.metadata);
        CHECK(r.metadata.makerNote.data == makerNoteBytes());
        CHECK(r.metadata.makerNote.originalOffset == noteAt);
    }

    // ---- damaged and foreign input: partial results, never a crash ------------
    {
        const Bytes good = cameraTiff(true);
        size_t stillIdentified = 0;
        for (size_t length = 0; length <= good.size(); length += 7) {
            const fs::path p =
                writeFile(dir, "dngconv_meta_cut.tif", Bytes(good.begin(), good.begin() + length));
            const SourceMetadataResult r = readSourceMetadata(p);
            // Whatever comes back must be internally consistent.
            for (const TiffField& f : r.metadata.exif) CHECK(!f.data.empty() && f.count > 0);
            if (r.identityFound()) ++stillIdentified;
        }
        CHECK(stillIdentified >= 1);  // the complete file, at least
        created.push_back(dir / "dngconv_meta_cut.tif");

        // Pointers and counts that lead nowhere.
        TiffBuilder t(false);
        const Bytes wild = t.finish(t.ifd({
            {271, 2, 4000000, enc32(false, {900000})},   // text far outside the file
            {272, 2, 6, text("Model")},
            {34665, 4, 1, enc32(false, {0xfffffff0u})},  // EXIF pointer outside the file
            {34853, 4, 1, enc32(false, {4})},            // GPS pointer into the header
        }));
        const fs::path wildPath = writeFile(dir, "dngconv_meta_wild.tif", wild);
        created.push_back(wildPath);
        const SourceMetadataResult w = readSourceMetadata(wildPath);
        CHECK(!w.identityFound() && w.metadata.exif.empty() && w.metadata.gps.empty());

        testutil::Random rnd(7);
        Bytes noise(5000);
        for (uint8_t& b : noise) b = static_cast<uint8_t>(rnd.next());
        for (const char* start : {"II*\0", "MM\0*", "\0\0\0\x18" "ftypcrx ", "FUJIFILMCCD-RAW "}) {
            Bytes withMagic = noise;
            const std::string magic(start, start[0] == 0 ? 12 : (start[0] == 'F' ? 16 : 4));
            std::copy(magic.begin(), magic.end(), withMagic.begin());
            const fs::path p = writeFile(dir, "dngconv_meta_noise.bin", withMagic);
            const SourceMetadataResult r = readSourceMetadata(p);
            CHECK(r.metadata.makerNote.data.size() <= noise.size());
        }
        created.push_back(dir / "dngconv_meta_noise.bin");

        created.push_back(writeFile(dir, "dngconv_meta_tiny.bin", {'I', 'I', 42, 0}));
        CHECK(!readSourceMetadata(created.back()).identityFound());
        CHECK(!readSourceMetadata(dir / "dngconv_meta_does_not_exist.raw").identityFound());
    }

    if (!testutil::failureCount())
        for (const fs::path& p : created) {
            std::error_code ec;
            fs::remove(p, ec);
        }
    return testutil::finish("test_source_metadata");
}
