// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Camera profiles: reading .dcp files (built here byte by byte, in both byte
// orders, sound and broken), choosing the ones that fit a camera, and
// storing them in a DNG as its main profile and as extra profiles.
//
// usage: test_profile <scratch folder>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "camera_profile.hpp"
#include "dng_writer.hpp"
#include "test_util.hpp"
#include "tiff_builder.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace tb = tiffbuilder;
namespace fs = std::filesystem;

namespace {

constexpr uint16_t kUniqueCameraModel = 50708, kColorMatrix1 = 50721, kColorMatrix2 = 50722,
                   kCalibrationIlluminant1 = 50778, kCalibrationIlluminant2 = 50779,
                   kExtraCameraProfiles = 50933, kProfileName = 50936, kHueSatDims = 50937,
                   kHueSatData1 = 50938, kHueSatData2 = 50939, kToneCurve = 50940,
                   kEmbedPolicy = 50941, kProfileCopyright = 50942, kForwardMatrix1 = 50964,
                   kLookDims = 50981, kLookData = 50982, kColorMatrix3 = 52531,
                   kCalibrationIlluminant3 = 52529, kDngVersion = 50706, kAsShotNeutral = 50728;

const std::vector<double> kMatrixA = {0.9, -0.3, -0.1, -0.4, 1.2, 0.2, -0.05, 0.2, 0.7};
const std::vector<double> kMatrixD = {0.7, -0.1, -0.1, -0.5, 1.3, 0.2, -0.1, 0.2, 0.6};
const std::vector<double> kForward = {0.6, 0.2, 0.16, 0.25, 0.7, 0.05, 0.0, 0.05, 0.77};

// Signed rationals in a chosen byte order (the builder's own are little-endian).
tb::Field matrix(uint16_t tag, const std::vector<double>& v, bool be) {
    tb::Field f{tag, 10, static_cast<uint32_t>(v.size()), {}};
    for (double x : v) {
        tb::put32(f.data, static_cast<uint32_t>(static_cast<int32_t>(std::lround(x * 10000))), be);
        tb::put32(f.data, 10000, be);
    }
    return f;
}

std::vector<float> table(size_t values, float seed) {
    std::vector<float> t(values);
    for (size_t i = 0; i < values; ++i) t[i] = seed + static_cast<float>(i % 17) * 0.01f;
    return t;
}

struct Recipe {
    bool bigEndian = false;
    std::string name = "Test Standard";
    std::string camera = "Testmake Model T";
    uint32_t policy = 3;
    bool second = true;
    bool tables = true;
    // Called last, to break things.
    std::function<void(std::vector<tb::Field>&)> spoil;
};

tb::Bytes dcp(const Recipe& r) {
    const bool be = r.bigEndian;
    std::vector<tb::Field> f;
    if (!r.camera.empty()) f.push_back(tb::ascii(kUniqueCameraModel, r.camera));
    f.push_back(matrix(kColorMatrix1, kMatrixA, be));
    f.push_back(tb::shorts(kCalibrationIlluminant1, {17}, be, 3));
    if (r.second) {
        f.push_back(matrix(kColorMatrix2, kMatrixD, be));
        f.push_back(tb::shorts(kCalibrationIlluminant2, {21}, be, 3));
    }
    f.push_back(matrix(kForwardMatrix1, kForward, be));
    if (!r.name.empty()) f.push_back(tb::ascii(kProfileName, r.name));
    f.push_back(tb::ascii(kProfileCopyright, "Copyright of nobody"));
    f.push_back(tb::longs(kEmbedPolicy, {r.policy}, be));
    if (r.tables) {
        f.push_back(tb::longs(kHueSatDims, {6, 4, 1}, be));
        f.push_back(tb::floats(kHueSatData1, table(6 * 4 * 1 * 3, 0.5f), be));
        if (r.second) f.push_back(tb::floats(kHueSatData2, table(6 * 4 * 1 * 3, 0.25f), be));
        f.push_back(tb::longs(kLookDims, {4, 3, 2}, be));
        f.push_back(tb::floats(kLookData, table(4 * 3 * 2 * 3, 1.0f), be));
        f.push_back(tb::floats(kToneCurve, {0.0f, 0.0f, 0.25f, 0.4f, 1.0f, 1.0f}, be));
    }
    f.push_back(tb::ascii(270, "a tag that is not part of a profile"));
    if (r.spoil) r.spoil(f);

    tb::Bytes file;
    file.push_back(be ? 'M' : 'I');
    file.push_back(be ? 'M' : 'I');
    tb::put16(file, 0x4352, be);
    tb::put32(file, 8, be);
    tb::placeDirectory(file, 8, f, be);
    return file;
}

CameraProfile parse(const tb::Bytes& b) { return parseCameraProfile(b.data(), b.size()); }

// The message a broken profile is refused with; empty if it was accepted.
std::string refusal(const tb::Bytes& b) {
    try {
        parse(b);
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

bool contains(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

void replaceField(std::vector<tb::Field>& fields, const tb::Field& with) {
    for (tb::Field& f : fields)
        if (f.tag == with.tag) f = with;
}

void dropField(std::vector<tb::Field>& fields, uint16_t tag) {
    fields.erase(std::remove_if(fields.begin(), fields.end(), [&](const tb::Field& f) { return f.tag == tag; }),
                 fields.end());
}

RawImage image(uint32_t planes = 3) {
    RawImage img;
    img.width = 64;
    img.height = 48;
    img.samplesPerPixel = 1;
    img.isCfa = true;
    img.cfaRows = img.cfaCols = 2;
    img.colorPlanes = planes;
    img.cfaPattern = {0, 1, 1, 2};
    if (planes == 4) {
        img.cfaPattern = {0, 1, 3, 2};
        img.planeColor = {kCyan, kYellow, kGreen, kMagenta};
    }
    img.activeBottom = img.height;
    img.activeRight = img.width;
    img.cropWidth = img.width;
    img.cropHeight = img.height;
    img.blackLevel = {0};
    img.whiteLevel = {4095, 4095, 4095, 4095};
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8, -0.2, -0.1}, {-0.5, 1.3, 0.2}, {-0.1, 0.3, 0.6}, {0.1, 0.2, 0.3}}};
    img.hasAsShotNeutral = true;
    img.asShotNeutral = {0.5, 1.0, 0.6, 0.8};
    img.uniqueCameraModel = "Testmake Model T";
    img.pixels.resize(img.sampleCount());
    for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<uint16_t>((i * 37) & 0xfff);
    return img;
}

std::string write(const RawImage& img, const DngWriteOptions& options) {
    std::ostringstream out(std::ios::binary);
    writeDng(img, out, options);
    return out.str();
}

std::shared_ptr<const CameraProfile> shared(const tb::Bytes& b) {
    return std::make_shared<const CameraProfile>(parse(b));
}

// ---- reading ----------------------------------------------------------------

void testReading() {
    const CameraProfile le = parse(dcp({}));
    CHECK(le.name == "Test Standard");
    CHECK(le.cameraModel == "Testmake Model T");
    CHECK(le.copyright == "Copyright of nobody");
    CHECK(le.embedPolicy == 3);
    CHECK(le.colorPlanes == 3);
    CHECK(le.illuminants == 2);
    CHECK(le.hasHueSatMap && le.hasLookTable && le.hasToneCurve && le.hasForwardMatrix);
    CHECK(le.dngVersion == 0x0104);
    CHECK(le.mayBeEmbedded());
    // The camera model and the stray tag are not part of what gets embedded.
    CHECK(le.find(kUniqueCameraModel) == nullptr);
    CHECK(le.find(270) == nullptr);
    CHECK(le.fields.size() == 14);
    for (size_t i = 1; i < le.fields.size(); ++i) CHECK(le.fields[i - 1].tag < le.fields[i].tag);
    CHECK(le.summary() == "two illuminants (A, D65), forward matrix, hue and saturation table 6x4x1, "
                          "look table 4x3x2, tone curve");

    // The same profile stored big-endian is the same profile.
    Recipe big;
    big.bigEndian = true;
    const CameraProfile be = parse(dcp(big));
    CHECK(be.digest == le.digest);
    CHECK(be.fields.size() == le.fields.size());
    for (size_t i = 0; i < le.fields.size() && i < be.fields.size(); ++i) {
        CHECK(be.fields[i].tag == le.fields[i].tag);
        CHECK(be.fields[i].type == le.fields[i].type);
        CHECK(be.fields[i].data == le.fields[i].data);
    }

    // A minimal profile: one matrix, nothing else.
    Recipe bare;
    bare.second = false;
    bare.tables = false;
    bare.name.clear();
    bare.camera.clear();
    CameraProfile minimal = parse(dcp(bare));
    CHECK(minimal.illuminants == 1);
    CHECK(!minimal.hasHueSatMap && !minimal.hasLookTable && !minimal.hasToneCurve);
    CHECK(minimal.cameraModel.empty());
    CHECK(minimal.displayName() == "unnamed profile");
    minimal.fileName = "bare.dcp";
    CHECK(minimal.displayName() == "bare.dcp");
    CHECK(minimal.digest != le.digest);
    CHECK(minimal.summary() == "one illuminant (A), forward matrix");

    // Dimensions and numbers stored as SHORT are accepted and normalised.
    Recipe shortDims;
    shortDims.spoil = [](std::vector<tb::Field>& f) {
        replaceField(f, tb::shorts(kHueSatDims, {6, 4, 1}, false, 3));
        replaceField(f, tb::shorts(kEmbedPolicy, {3}, false, 3));
    };
    const CameraProfile normal = parse(dcp(shortDims));
    CHECK(normal.digest == le.digest);

    // Three illuminants need DNG 1.6.
    Recipe three;
    three.spoil = [](std::vector<tb::Field>& f) {
        f.push_back(matrix(kColorMatrix3, kMatrixD, false));
        f.push_back(tb::shorts(kCalibrationIlluminant3, {23}, false, 3));
    };
    const CameraProfile triple = parse(dcp(three));
    CHECK(triple.illuminants == 3);
    CHECK(triple.dngVersion == 0x0106);
    CHECK(contains(triple.summary(), "three illuminants (A, D65, D50)"));

    // Four colours.
    Recipe four;
    four.second = false;
    four.tables = false;
    four.spoil = [](std::vector<tb::Field>& f) {
        std::vector<double> twelve = kMatrixA;
        twelve.insert(twelve.end(), {0.1, 0.2, 0.3});
        replaceField(f, matrix(kColorMatrix1, twelve, false));
        dropField(f, kForwardMatrix1);
    };
    CHECK(parse(dcp(four)).colorPlanes == 4);
}

void testRefusals() {
    const tb::Bytes good = dcp({});
    CHECK(refusal(good).empty());

    CHECK(contains(refusal({}), "too short"));
    CHECK(contains(refusal({'P', 'K', 3, 4, 0, 0, 0, 0, 0, 0}), "not a camera profile"));
    tb::Bytes tiff = good;
    tiff[2] = 42;
    tiff[3] = 0;
    CHECK(contains(refusal(tiff), "TIFF or DNG"));

    // Cut short anywhere: refused, never read out of bounds.
    for (size_t cut : {size_t{9}, size_t{20}, good.size() / 2, good.size() - 1}) {
        tb::Bytes shortened(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(!refusal(shortened).empty());
    }
    // A directory offset pointing nowhere.
    tb::Bytes lost = good;
    lost[4] = 0xff;
    lost[5] = 0xff;
    lost[6] = 0xff;
    lost[7] = 0x7f;
    CHECK(!refusal(lost).empty());

    const auto spoiled = [](std::function<void(std::vector<tb::Field>&)> how) {
        Recipe r;
        r.spoil = std::move(how);
        return refusal(dcp(r));
    };
    CHECK(contains(spoiled([](auto& f) { dropField(f, kColorMatrix1); }), "no colour matrix"));
    CHECK(contains(spoiled([](auto& f) {
        replaceField(f, matrix(kColorMatrix1, {1, 0, 0, 0, 1, 0, 0, 0}, false));
    }), "three or four colours"));
    CHECK(contains(spoiled([](auto& f) {
        replaceField(f, matrix(kColorMatrix2, {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, false));
    }), "ColorMatrix2 does not fit"));
    CHECK(contains(spoiled([](auto& f) {
        replaceField(f, matrix(kColorMatrix1, {1, 2, 3, 2, 4, 6, 0, 0, 1}, false));
    }), "cannot be inverted"));
    CHECK(contains(spoiled([](auto& f) {
        tb::Field zero = matrix(kColorMatrix1, kMatrixA, false);
        zero.data[4] = zero.data[5] = zero.data[6] = zero.data[7] = 0;  // first denominator
        replaceField(f, zero);
    }), "division by zero"));
    CHECK(contains(spoiled([](auto& f) { replaceField(f, tb::longs(kColorMatrix1, {1, 0, 0, 0, 1, 0, 0, 0, 1})); }),
                   "wrong type"));
    CHECK(contains(spoiled([](auto& f) { dropField(f, kHueSatDims); }), "no dimensions"));
    CHECK(contains(spoiled([](auto& f) { replaceField(f, tb::longs(kHueSatDims, {6, 5, 1})); }),
                   "does not have the size"));
    CHECK(contains(spoiled([](auto& f) { replaceField(f, tb::longs(kLookDims, {4, 1, 6})); }),
                   "impossible dimensions"));
    CHECK(contains(spoiled([](auto& f) {
        std::vector<float> t = table(4 * 3 * 2 * 3, 1.0f);
        t[5] = std::nanf("");
        replaceField(f, tb::floats(kLookData, t));
    }), "not numbers"));
    CHECK(contains(spoiled([](auto& f) { replaceField(f, tb::floats(kToneCurve, {0.0f, 0.0f, 1.0f})); }),
                   "tone curve"));
    CHECK(contains(spoiled([](auto& f) { f.push_back(tb::ascii(kProfileName, "again")); }), "occurs twice"));
    CHECK(contains(spoiled([](auto& f) { f.push_back(matrix(kColorMatrix3, kMatrixD, false)); dropField(f, kColorMatrix2); }),
                   "third colour matrix"));

    // A value that claims to lie beyond the end of the file.
    tb::Bytes beyond = good;
    // The second directory entry (after the stray description) is the camera model.
    const size_t entry = 8 + 2 + 12;
    CHECK(tiffreader::rd16(std::string(beyond.begin(), beyond.end()), entry) == kUniqueCameraModel);
    beyond[entry + 8] = 0xf0;
    beyond[entry + 9] = 0xff;
    beyond[entry + 10] = 0xff;
    beyond[entry + 11] = 0x0f;
    CHECK(contains(refusal(beyond), "outside the file"));
}

void testPacking() {
    const CameraProfile p = parse(dcp({}));
    for (bool be : {false, true}) {
        const std::vector<uint8_t> block = packCameraProfile(p, be);
        CHECK(block.size() > 8);
        CHECK(block[0] == (be ? 'M' : 'I') && block[1] == block[0]);
        CHECK((be ? (block[2] << 8 | block[3]) : (block[3] << 8 | block[2])) == 0x4352);
        const CameraProfile back = parseCameraProfile(block.data(), block.size());
        CHECK(back.digest == p.digest);
        CHECK(back.name == p.name);
        CHECK(back.cameraModel.empty());
    }
}

void testCameraNames() {
    CHECK(sameCameraModel("Olympus E-M10", "OLYMPUS E-M10"));
    CHECK(sameCameraModel("Sony ILCE-7M3", "SONY  ILCE-7M3 "));
    CHECK(sameCameraModel("Canon EOS 5D Mark II", "Canon EOS 5D MarkII"));
    CHECK(!sameCameraModel("Pentax K-1", "Pentax K-1 Mark II"));
    CHECK(!sameCameraModel("Canon EOS 5D Mark II", "Canon EOS 5D Mark III"));
    CHECK(!sameCameraModel("", "Canon EOS R8"));
}

void testLibrary(const fs::path& dir) {
    const fs::path folder = dir / "profiles";
    fs::remove_all(folder);
    fs::create_directories(folder / "sub");

    Recipe portrait;
    portrait.name = "Test Portrait";
    Recipe other;
    other.name = "Other Standard";
    other.camera = "Othermake X";
    Recipe generic;
    generic.name = "Generic";
    generic.camera.clear();
    Recipe locked;
    locked.name = "Locked";
    locked.policy = 2;
    Recipe twin;  // same name as the standard profile, different content
    twin.tables = false;
    Recipe four;
    four.name = "Four";
    four.second = false;
    four.tables = false;
    four.spoil = [](std::vector<tb::Field>& f) {
        std::vector<double> twelve = kMatrixA;
        twelve.insert(twelve.end(), {0.1, 0.2, 0.3});
        replaceField(f, matrix(kColorMatrix1, twelve, false));
        dropField(f, kForwardMatrix1);
    };

    tb::writeFile(folder, "a-standard.dcp", dcp({}));
    tb::writeFile(folder, "b-portrait.DCP", dcp(portrait));
    tb::writeFile(folder, "c-other.dcp", dcp(other));
    tb::writeFile(folder / "sub", "d-generic.dcp", dcp(generic));
    tb::writeFile(folder / "sub", "e-locked.dcp", dcp(locked));
    tb::writeFile(folder / "sub", "f-twin.dcp", dcp(twin));
    tb::writeFile(folder / "sub", "g-four.dcp", dcp(four));
    tb::writeFile(folder / "sub", "h-copy-of-standard.dcp", dcp({}));
    tb::writeFile(folder, "broken.dcp", {'I', 'I', 0x52, 0x43, 8, 0, 0, 0, 0, 0});
    tb::writeFile(folder, "notes.txt", {'h', 'i'});

    ProfileLibrary library;
    std::vector<std::string> skipped;
    library.add(folder, skipped);
    CHECK(library.size() == 7);  // the copy is the same profile; the broken one is out
    CHECK(skipped.size() == 1);
    CHECK(!skipped.empty() && contains(skipped[0], "broken.dcp"));

    std::vector<std::string> notes;
    auto chosen = library.select("TESTMAKE Model-T", 3, false, notes);
    CHECK(chosen.size() == 3);
    if (chosen.size() == 3) {
        CHECK(chosen[0]->name == "Test Standard" && chosen[0]->fileName == "a-standard.dcp");
        CHECK(chosen[1]->name == "Test Portrait");
        CHECK(chosen[2]->name == "Generic");
    }
    // Why the others are missing: one may not be embedded, one has a name
    // already taken, one is for another kind of sensor.
    CHECK(notes.size() == 3);
    const std::string all = notes.size() == 3 ? notes[0] + "|" + notes[1] + "|" + notes[2] : "";
    CHECK(contains(all, "never embed"));
    CHECK(contains(all, "two profiles are named 'Test Standard'"));
    CHECK(contains(all, "4 colours"));

    notes.clear();
    chosen = library.select("Othermake X", 3, false, notes);
    CHECK(chosen.size() == 2);  // its own and the generic one
    CHECK(notes.empty());

    notes.clear();
    chosen = library.select("Unknown Camera", 3, false, notes);
    CHECK(chosen.size() == 1 && chosen[0]->name == "Generic");
    CHECK(notes.empty());

    notes.clear();
    chosen = library.select("Unknown Camera", 3, true, notes);
    CHECK(chosen.size() == 4);

    notes.clear();
    chosen = library.select("Testmake Model T", 4, false, notes);
    CHECK(chosen.size() == 1 && chosen[0]->name == "Four");

    // A file named directly: read, or refused with its name.
    ProfileLibrary single;
    single.add(folder / "c-other.dcp", skipped);
    CHECK(single.size() == 1);
    single.add(folder / "c-other.dcp", skipped);
    CHECK(single.size() == 1);
    bool threw = false;
    try {
        single.add(folder / "broken.dcp", skipped);
    } catch (const std::exception& e) {
        threw = contains(e.what(), "broken.dcp");
    }
    CHECK(threw);
    threw = false;
    try {
        single.add(folder / "missing.dcp", skipped);
    } catch (const std::exception& e) {
        threw = contains(e.what(), "no such file");
    }
    CHECK(threw);
}

// ---- writing ------------------------------------------------------------------

void testWriting() {
    Recipe portraitRecipe;
    portraitRecipe.name = "Test Portrait";
    portraitRecipe.second = false;
    const auto standard = shared(dcp({}));
    const auto portrait = shared(dcp(portraitRecipe));
    Recipe plainRecipe;
    plainRecipe.name = "Plain";
    plainRecipe.tables = false;
    const auto plain = shared(dcp(plainRecipe));

    const RawImage img = image();

    // Without a profile: the decoder's matrix, as before.
    {
        const std::string file = write(img, {});
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(file);
        CHECK(ifd0.count(kColorMatrix1) == 1);
        CHECK(ifd0.count(kProfileName) == 0);
        CHECK(ifd0.count(kExtraCameraProfiles) == 0);
        CHECK(ifd0.at(kCalibrationIlluminant1).u16() == 21);
    }

    for (bool big : {false, true}) {
        DngWriteOptions options;
        options.byteOrder = big ? DngByteOrder::Big : DngByteOrder::Little;
        options.profiles = {standard, portrait, plain};
        const std::string file = write(img, options);
        CHECK(tiffreader::isBigEndian(file) == big);
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(file);

        // Every tag of the first profile is in the first directory, value for value.
        for (const TiffField& f : standard->fields) {
            CHECK(ifd0.count(f.tag) == 1);
            if (!ifd0.count(f.tag)) continue;
            CHECK(ifd0.at(f.tag).type == f.type);
            CHECK(ifd0.at(f.tag).count == f.count);
            CHECK(ifd0.at(f.tag).data == f.data);
        }
        CHECK(ifd0.at(kProfileName).text() == "Test Standard");
        CHECK(ifd0.at(kCalibrationIlluminant1).u16() == 17);  // the profile's, not the decoder's D65
        CHECK(ifd0.count(kUniqueCameraModel) == 1);
        CHECK(ifd0.at(kUniqueCameraModel).text() == "Testmake Model T");
        CHECK(ifd0.count(kAsShotNeutral) == 1);  // the white balance belongs to the picture
        CHECK(ifd0.at(kDngVersion).data == (std::vector<uint8_t>{1, 4, 0, 0}));

        // The others follow as blocks of their own.
        CHECK(ifd0.count(kExtraCameraProfiles) == 1);
        if (!ifd0.count(kExtraCameraProfiles)) continue;
        const tiffreader::Entry& extra = ifd0.at(kExtraCameraProfiles);
        CHECK(extra.type == 4 && extra.count == 2);
        const std::shared_ptr<const CameraProfile> expected[] = {portrait, plain};
        for (uint32_t i = 0; i < extra.count && i < 2; ++i) {
            const uint32_t at = extra.u32(i);
            CHECK(at > 8 && at < file.size());
            if (at >= file.size()) continue;
            CHECK(file[at] == (big ? 'M' : 'I'));
            const CameraProfile back = parseCameraProfile(
                reinterpret_cast<const uint8_t*>(file.data()) + at, file.size() - at);
            CHECK(back.digest == expected[i]->digest);
            CHECK(back.name == expected[i]->name);
        }
    }

    // One profile only: no extra-profiles tag.
    {
        DngWriteOptions options;
        options.profiles = {plain};
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(write(img, options));
        CHECK(ifd0.at(kProfileName).text() == "Plain");
        CHECK(ifd0.count(kExtraCameraProfiles) == 0);
        CHECK(ifd0.count(kHueSatData1) == 0);
    }

    // A profile with tags of a later DNG version raises the version of the file.
    {
        Recipe three;
        three.spoil = [](std::vector<tb::Field>& f) {
            f.push_back(matrix(kColorMatrix3, kMatrixD, false));
            f.push_back(tb::shorts(kCalibrationIlluminant3, {23}, false, 3));
        };
        DngWriteOptions options;
        options.profiles = {plain, shared(dcp(three))};
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(write(img, options));
        CHECK(ifd0.at(kDngVersion).data == (std::vector<uint8_t>{1, 6, 0, 0}));
    }

    // A profile for another kind of sensor is refused, not written.
    {
        DngWriteOptions options;
        options.profiles = {standard};
        bool threw = false;
        try {
            write(image(4), options);
        } catch (const std::invalid_argument& e) {
            threw = contains(e.what(), "Test Standard");
        }
        CHECK(threw);
    }

    // A monochrome picture has no colour to describe: profiles are ignored.
    {
        RawImage mono = image();
        mono.isCfa = false;
        mono.colorPlanes = 1;
        mono.cfaPattern.clear();
        mono.cfaRows = mono.cfaCols = 0;
        DngWriteOptions options;
        options.profiles = {standard};
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(write(mono, options));
        CHECK(ifd0.count(kProfileName) == 0);
        CHECK(ifd0.count(kColorMatrix1) == 0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_profile <scratch folder>\n");
        return 2;
    }
    const fs::path dir = fs::path(argv[1]) / "profile_files";
    fs::create_directories(dir);

    testReading();
    testRefusals();
    testPacking();
    testCameraNames();
    testLibrary(dir);
    testWriting();
    return testutil::finish("test_profile");
}
