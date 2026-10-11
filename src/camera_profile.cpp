// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "camera_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "tiff_writer.hpp"

namespace dngconv {

namespace {

namespace tag {
constexpr uint16_t UniqueCameraModel = 50708;
constexpr uint16_t ColorMatrix1 = 50721;
constexpr uint16_t ColorMatrix2 = 50722;
constexpr uint16_t ReductionMatrix1 = 50725;
constexpr uint16_t ReductionMatrix2 = 50726;
constexpr uint16_t CalibrationIlluminant1 = 50778;
constexpr uint16_t CalibrationIlluminant2 = 50779;
constexpr uint16_t ProfileCalibrationSignature = 50932;
constexpr uint16_t ProfileName = 50936;
constexpr uint16_t ProfileHueSatMapDims = 50937;
constexpr uint16_t ProfileHueSatMapData1 = 50938;
constexpr uint16_t ProfileHueSatMapData2 = 50939;
constexpr uint16_t ProfileToneCurve = 50940;
constexpr uint16_t ProfileEmbedPolicy = 50941;
constexpr uint16_t ProfileCopyright = 50942;
constexpr uint16_t ForwardMatrix1 = 50964;
constexpr uint16_t ForwardMatrix2 = 50965;
constexpr uint16_t ProfileLookTableDims = 50981;
constexpr uint16_t ProfileLookTableData = 50982;
constexpr uint16_t ProfileHueSatMapEncoding = 51107;
constexpr uint16_t ProfileLookTableEncoding = 51108;
constexpr uint16_t BaselineExposureOffset = 51109;
constexpr uint16_t DefaultBlackRender = 51110;
// DNG 1.6
constexpr uint16_t CalibrationIlluminant3 = 52529;
constexpr uint16_t ColorMatrix3 = 52531;
constexpr uint16_t ForwardMatrix3 = 52532;
constexpr uint16_t IlluminantData1 = 52533;
constexpr uint16_t IlluminantData2 = 52534;
constexpr uint16_t IlluminantData3 = 52535;
constexpr uint16_t ProfileHueSatMapData3 = 52537;
constexpr uint16_t ReductionMatrix3 = 52538;
// DNG 1.7
constexpr uint16_t RGBTables = 52539;
constexpr uint16_t ProfileGainTableMap2 = 52544;
constexpr uint16_t ProfileDynamicRange = 52551;
constexpr uint16_t ProfileGroupName = 52552;
}  // namespace tag

// TIFF field types.
constexpr uint16_t kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kUndefined = 7,
                   kSRational = 10, kFloat = 11;

enum class Kind {
    Text,       // ASCII, or BYTE holding UTF-8
    Matrix,     // signed rationals, three per colour plane
    Number16,   // one SHORT
    Number32,   // one LONG
    Dims,       // three LONGs
    Floats,     // a table of floats
    Exposure,   // one signed rational
    Opaque,     // a block the profile's maker defined; carried as it is
};

struct FieldRule {
    uint16_t tag;
    Kind kind;
    unsigned version;  // DNG version that introduced it
    const char* name;
};

// Everything that belongs to a camera profile. Other tags in a .dcp file are
// not carried over.
const FieldRule kRules[] = {
    {tag::ColorMatrix1, Kind::Matrix, 0x0104, "ColorMatrix1"},
    {tag::ColorMatrix2, Kind::Matrix, 0x0104, "ColorMatrix2"},
    {tag::ReductionMatrix1, Kind::Matrix, 0x0104, "ReductionMatrix1"},
    {tag::ReductionMatrix2, Kind::Matrix, 0x0104, "ReductionMatrix2"},
    {tag::CalibrationIlluminant1, Kind::Number16, 0x0104, "CalibrationIlluminant1"},
    {tag::CalibrationIlluminant2, Kind::Number16, 0x0104, "CalibrationIlluminant2"},
    {tag::ProfileCalibrationSignature, Kind::Text, 0x0104, "ProfileCalibrationSignature"},
    {tag::ProfileName, Kind::Text, 0x0104, "ProfileName"},
    {tag::ProfileHueSatMapDims, Kind::Dims, 0x0104, "ProfileHueSatMapDims"},
    {tag::ProfileHueSatMapData1, Kind::Floats, 0x0104, "ProfileHueSatMapData1"},
    {tag::ProfileHueSatMapData2, Kind::Floats, 0x0104, "ProfileHueSatMapData2"},
    {tag::ProfileToneCurve, Kind::Floats, 0x0104, "ProfileToneCurve"},
    {tag::ProfileEmbedPolicy, Kind::Number32, 0x0104, "ProfileEmbedPolicy"},
    {tag::ProfileCopyright, Kind::Text, 0x0104, "ProfileCopyright"},
    {tag::ForwardMatrix1, Kind::Matrix, 0x0104, "ForwardMatrix1"},
    {tag::ForwardMatrix2, Kind::Matrix, 0x0104, "ForwardMatrix2"},
    {tag::ProfileLookTableDims, Kind::Dims, 0x0104, "ProfileLookTableDims"},
    {tag::ProfileLookTableData, Kind::Floats, 0x0104, "ProfileLookTableData"},
    {tag::ProfileHueSatMapEncoding, Kind::Number32, 0x0104, "ProfileHueSatMapEncoding"},
    {tag::ProfileLookTableEncoding, Kind::Number32, 0x0104, "ProfileLookTableEncoding"},
    {tag::BaselineExposureOffset, Kind::Exposure, 0x0104, "BaselineExposureOffset"},
    {tag::DefaultBlackRender, Kind::Number32, 0x0104, "DefaultBlackRender"},
    {tag::CalibrationIlluminant3, Kind::Number16, 0x0106, "CalibrationIlluminant3"},
    {tag::ColorMatrix3, Kind::Matrix, 0x0106, "ColorMatrix3"},
    {tag::ForwardMatrix3, Kind::Matrix, 0x0106, "ForwardMatrix3"},
    {tag::IlluminantData1, Kind::Opaque, 0x0106, "IlluminantData1"},
    {tag::IlluminantData2, Kind::Opaque, 0x0106, "IlluminantData2"},
    {tag::IlluminantData3, Kind::Opaque, 0x0106, "IlluminantData3"},
    {tag::ProfileHueSatMapData3, Kind::Floats, 0x0106, "ProfileHueSatMapData3"},
    {tag::ReductionMatrix3, Kind::Matrix, 0x0106, "ReductionMatrix3"},
    {tag::RGBTables, Kind::Opaque, 0x0107, "RGBTables"},
    {tag::ProfileGainTableMap2, Kind::Opaque, 0x0107, "ProfileGainTableMap2"},
    {tag::ProfileDynamicRange, Kind::Opaque, 0x0107, "ProfileDynamicRange"},
    {tag::ProfileGroupName, Kind::Text, 0x0107, "ProfileGroupName"},
};

const FieldRule* ruleFor(uint16_t t) {
    for (const FieldRule& r : kRules)
        if (r.tag == t) return &r;
    return nullptr;
}

// A profile is a few megabytes at the very most (three 90x30x30 tables are
// under three); anything larger is not one.
constexpr size_t kMaxProfileBytes = 64u << 20;
constexpr unsigned kMaxEntries = 500;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

struct Reader {
    const uint8_t* data;
    size_t size;
    bool bigEndian;

    uint16_t u16(size_t at) const {
        if (at > size || size - at < 2) fail("the profile is cut short");
        return bigEndian ? static_cast<uint16_t>((data[at] << 8) | data[at + 1])
                         : static_cast<uint16_t>((data[at + 1] << 8) | data[at]);
    }
    uint32_t u32(size_t at) const {
        const uint32_t a = u16(at), b = u16(at + 2);
        return bigEndian ? (a << 16) | b : (b << 16) | a;
    }
};

uint32_t le32(const std::vector<uint8_t>& d, size_t index) {
    const size_t at = index * 4;
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
           (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}

std::string textOf(const TiffField& f) {
    std::string s(f.data.begin(), f.data.end());
    const size_t end = s.find('\0');
    if (end != std::string::npos) s.resize(end);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Value of a SHORT or LONG field with one element.
uint32_t numberOf(const TiffField& f) {
    return f.type == kShort ? static_cast<uint32_t>(f.data[0] | (f.data[1] << 8)) : le32(f.data, 0);
}

double rationalAt(const TiffField& f, size_t index) {
    const int32_t n = static_cast<int32_t>(le32(f.data, index * 2));
    const int32_t d = static_cast<int32_t>(le32(f.data, index * 2 + 1));
    return d == 0 ? std::nan("") : static_cast<double>(n) / d;
}

bool finiteFloats(const TiffField& f) {
    for (uint32_t i = 0; i < f.count; ++i) {
        const uint32_t bits = le32(f.data, i);
        if ((bits & 0x7f800000u) == 0x7f800000u) return false;  // infinity or NaN
    }
    return true;
}

// Shorter names for the standard light sources profiles are calibrated for.
std::string illuminantName(uint32_t code) {
    switch (code) {
        case 1: return "daylight";
        case 2: return "fluorescent";
        case 3: return "tungsten";
        case 4: return "flash";
        case 17: return "A";
        case 18: return "B";
        case 19: return "C";
        case 20: return "D55";
        case 21: return "D65";
        case 22: return "D75";
        case 23: return "D50";
        case 24: return "studio tungsten";
        default: return "light source " + std::to_string(code);
    }
}

std::string normalised(const std::string& s) {
    std::string out;
    for (unsigned char ch : s)
        if (std::isalnum(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
    return out;
}

}  // namespace

const TiffField* CameraProfile::find(uint16_t t) const {
    for (const TiffField& f : fields)
        if (f.tag == t) return &f;
    return nullptr;
}

std::string CameraProfile::displayName() const {
    if (!name.empty()) return name;
    if (!fileName.empty()) return fileName;
    return "unnamed profile";
}

std::string CameraProfile::summary() const {
    std::string out;
    static const char* const counts[] = {"no", "one", "two", "three"};
    out = std::string(counts[std::min(illuminants, 3u)]) + " illuminant" + (illuminants == 1 ? "" : "s");
    std::string lights;
    for (uint16_t t : {tag::CalibrationIlluminant1, tag::CalibrationIlluminant2, tag::CalibrationIlluminant3}) {
        const TiffField* f = find(t);
        if (!f) continue;
        lights += (lights.empty() ? "" : ", ") + illuminantName(numberOf(*f));
    }
    if (!lights.empty()) out += " (" + lights + ")";
    if (hasForwardMatrix) out += ", forward matrix";
    const auto dims = [&](uint16_t t) {
        const TiffField* f = find(t);
        return f ? " " + std::to_string(le32(f->data, 0)) + "x" + std::to_string(le32(f->data, 1)) + "x" +
                       std::to_string(le32(f->data, 2))
                 : std::string();
    };
    if (hasHueSatMap) out += ", hue and saturation table" + dims(tag::ProfileHueSatMapDims);
    if (hasLookTable) out += ", look table" + dims(tag::ProfileLookTableDims);
    if (hasToneCurve) out += ", tone curve";
    return out;
}

CameraProfile parseCameraProfile(const uint8_t* data, size_t size) {
    if (size < 8 || !data) fail("not a camera profile (too short)");
    Reader r{data, size, false};
    if (data[0] == 'M' && data[1] == 'M')
        r.bigEndian = true;
    else if (!(data[0] == 'I' && data[1] == 'I'))
        fail("not a camera profile");
    const uint16_t magic = r.u16(2);
    if (magic == 42) fail("a TIFF or DNG file, not a camera profile (.dcp)");
    if (magic != 0x4352) fail("not a camera profile");

    const uint32_t ifd = r.u32(4);
    if (ifd < 8) fail("the profile's directory is misplaced");
    const unsigned entries = r.u16(ifd);
    if (entries == 0 || entries > kMaxEntries) fail("the profile's directory is damaged");

    CameraProfile p;
    for (unsigned i = 0; i < entries; ++i) {
        const size_t at = static_cast<size_t>(ifd) + 2 + 12 * static_cast<size_t>(i);
        TiffField f;
        f.tag = r.u16(at);
        f.type = r.u16(at + 2);
        f.count = r.u32(at + 4);
        const FieldRule* rule = ruleFor(f.tag);
        if (!rule && f.tag != tag::UniqueCameraModel) continue;
        const char* name = rule ? rule->name : "UniqueCameraModel";
        const Kind kind = rule ? rule->kind : Kind::Text;

        const size_t unit = tiffTypeSize(f.type);
        if (unit == 0 || f.count == 0) fail(std::string(name) + " is empty or of an unknown type");
        const uint64_t bytes = static_cast<uint64_t>(unit) * f.count;
        if (bytes > kMaxProfileBytes) fail(std::string(name) + " is implausibly large");
        size_t where = at + 8;
        if (bytes > 4) where = r.u32(at + 8);
        if (where > size || bytes > size - where) fail(std::string(name) + " lies outside the file");
        f.data.assign(data + where, data + where + static_cast<size_t>(bytes));
        if (r.bigEndian) {
            size_t swap = unit;
            if (f.type == kRational || f.type == kSRational) swap = 4;
            if (swap >= 2)
                for (size_t k = 0; k + swap <= f.data.size(); k += swap)
                    std::reverse(f.data.begin() + static_cast<std::ptrdiff_t>(k),
                                 f.data.begin() + static_cast<std::ptrdiff_t>(k + swap));
        }

        const auto wrongType = [&]() { fail(std::string(name) + " has the wrong type"); };
        switch (kind) {
            case Kind::Text:
                if (f.type != kAscii && f.type != kByte) wrongType();
                break;
            case Kind::Matrix:
                if (f.type != kSRational) wrongType();
                break;
            case Kind::Number16:
            case Kind::Number32: {
                if ((f.type != kShort && f.type != kLong) || f.count != 1) wrongType();
                // Stored the way the specification has it, whichever the file used.
                const uint32_t v = numberOf(f);
                const bool wantShort = kind == Kind::Number16;
                if (wantShort && v > 65535) wrongType();
                f.type = wantShort ? kShort : kLong;
                f.data.assign(wantShort ? 2 : 4, 0);
                for (size_t k = 0; k < f.data.size(); ++k) f.data[k] = static_cast<uint8_t>(v >> (8 * k));
                break;
            }
            case Kind::Dims: {
                if ((f.type != kShort && f.type != kLong) || f.count != 3) wrongType();
                if (f.type == kShort) {
                    std::vector<uint8_t> wide;
                    for (int k = 0; k < 3; ++k) {
                        wide.push_back(f.data[2 * k]);
                        wide.push_back(f.data[2 * k + 1]);
                        wide.push_back(0);
                        wide.push_back(0);
                    }
                    f.data = std::move(wide);
                    f.type = kLong;
                }
                break;
            }
            case Kind::Floats:
                if (f.type != kFloat) wrongType();
                if (!finiteFloats(f)) fail(std::string(name) + " holds values that are not numbers");
                break;
            case Kind::Exposure:
                if ((f.type != kSRational && f.type != kRational) || f.count != 1) wrongType();
                break;
            case Kind::Opaque:
                if (f.type != kUndefined && f.type != kByte) wrongType();
                break;
        }

        if (f.tag == tag::UniqueCameraModel) {
            p.cameraModel = textOf(f);
            continue;
        }
        if (p.find(f.tag)) fail(std::string(name) + " occurs twice");
        p.dngVersion = std::max(p.dngVersion, rule->version);
        p.fields.push_back(std::move(f));
    }
    std::sort(p.fields.begin(), p.fields.end(),
              [](const TiffField& a, const TiffField& b) { return a.tag < b.tag; });

    // ---- does it hold together? -------------------------------------------------
    const TiffField* cm1 = p.find(tag::ColorMatrix1);
    if (!cm1) fail("the profile has no colour matrix");
    if (cm1->count != 9 && cm1->count != 12) fail("ColorMatrix1 is not a matrix for three or four colours");
    p.colorPlanes = cm1->count / 3;
    for (const FieldRule& rule : kRules) {
        if (rule.kind != Kind::Matrix) continue;
        const TiffField* f = p.find(rule.tag);
        if (!f) continue;
        if (f->count != 3 * p.colorPlanes)
            fail(std::string(rule.name) + " does not fit a camera with " +
                 std::to_string(p.colorPlanes) + " colours");
        for (uint32_t i = 0; i < f->count; ++i)
            if (!std::isfinite(rationalAt(*f, i))) fail(std::string(rule.name) + " holds a division by zero");
    }
    if (p.colorPlanes == 3) {
        double m[9];
        for (int i = 0; i < 9; ++i) m[i] = rationalAt(*cm1, static_cast<size_t>(i));
        const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                           m[2] * (m[3] * m[7] - m[4] * m[6]);
        if (!(std::fabs(det) > 1e-9)) fail("ColorMatrix1 cannot be inverted");
    }
    p.illuminants = 1;
    if (p.find(tag::ColorMatrix2)) p.illuminants = 2;
    if (p.find(tag::ColorMatrix3)) {
        if (p.illuminants != 2) fail("the profile has a third colour matrix but no second");
        p.illuminants = 3;
    }
    p.hasForwardMatrix = p.find(tag::ForwardMatrix1) != nullptr;

    const auto checkTable = [&](uint16_t dimsTag, std::initializer_list<uint16_t> dataTags,
                                const char* what) -> bool {
        const TiffField* dims = p.find(dimsTag);
        bool any = false;
        for (uint16_t t : dataTags) {
            const TiffField* table = p.find(t);
            if (!table) continue;
            any = true;
            if (!dims) fail(std::string("the ") + what + " has no dimensions");
            const uint64_t h = le32(dims->data, 0), s = le32(dims->data, 1), v = le32(dims->data, 2);
            if (h < 1 || s < 2 || v < 1 || h > 4096 || s > 4096 || v > 4096)
                fail(std::string("the ") + what + " has impossible dimensions");
            if (h * s * v * 3 != table->count)
                fail(std::string("the ") + what + " does not have the size its dimensions give");
        }
        return any;
    };
    p.hasHueSatMap = checkTable(tag::ProfileHueSatMapDims,
                                {tag::ProfileHueSatMapData1, tag::ProfileHueSatMapData2,
                                 tag::ProfileHueSatMapData3},
                                "hue and saturation table");
    p.hasLookTable = checkTable(tag::ProfileLookTableDims, {tag::ProfileLookTableData}, "look table");
    if (const TiffField* curve = p.find(tag::ProfileToneCurve)) {
        if (curve->count < 4 || curve->count % 2) fail("the tone curve is not a list of points");
        p.hasToneCurve = true;
    }

    if (const TiffField* f = p.find(tag::ProfileName)) p.name = textOf(*f);
    if (const TiffField* f = p.find(tag::ProfileCopyright)) p.copyright = textOf(*f);
    if (const TiffField* f = p.find(tag::ProfileGroupName)) p.groupName = textOf(*f);
    if (const TiffField* f = p.find(tag::ProfileEmbedPolicy)) p.embedPolicy = numberOf(*f);

    Md5 md5;
    for (const TiffField& f : p.fields) {
        const uint8_t head[8] = {static_cast<uint8_t>(f.tag),        static_cast<uint8_t>(f.tag >> 8),
                                 static_cast<uint8_t>(f.type),       static_cast<uint8_t>(f.type >> 8),
                                 static_cast<uint8_t>(f.count),      static_cast<uint8_t>(f.count >> 8),
                                 static_cast<uint8_t>(f.count >> 16), static_cast<uint8_t>(f.count >> 24)};
        md5.update(head, sizeof head);
        md5.update(f.data.data(), f.data.size());
    }
    p.digest = md5.finish();
    return p;
}

CameraProfile readCameraProfile(const std::filesystem::path& file) {
    const std::string shown = file.u8string();
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open '" + shown + "'");
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0) throw std::runtime_error("cannot read '" + shown + "'");
    if (static_cast<uint64_t>(length) > kMaxProfileBytes)
        throw std::runtime_error("'" + shown + "': too large to be a camera profile");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(in.gcount()) != bytes.size())
        throw std::runtime_error("cannot read '" + shown + "'");
    try {
        CameraProfile p = parseCameraProfile(bytes.data(), bytes.size());
        p.fileName = file.filename().u8string();
        return p;
    } catch (const std::exception& e) {
        throw std::runtime_error("'" + shown + "': " + e.what());
    }
}

std::vector<uint8_t> packCameraProfile(const CameraProfile& profile, bool bigEndian) {
    TiffWriter writer;
    TiffIfd& ifd = writer.addIfd();
    for (const TiffField& f : profile.fields) ifd.setRaw(f.tag, f.type, f.count, f.data);
    std::ostringstream out(std::ios::binary);
    writer.write(out, bigEndian);
    const std::string s = out.str();
    std::vector<uint8_t> bytes(s.begin(), s.end());
    // The same structure as a TIFF file, told apart by its magic number.
    bytes[2] = bigEndian ? 0x43 : 0x52;
    bytes[3] = bigEndian ? 0x52 : 0x43;
    return bytes;
}

bool sameCameraModel(const std::string& a, const std::string& b) {
    return normalised(a) == normalised(b);
}

void ProfileLibrary::add(const std::filesystem::path& fileOrFolder, std::vector<std::string>& skipped) {
    namespace fs = std::filesystem;
    const auto take = [&](CameraProfile&& p) {
        for (const auto& have : profiles_)
            if (have->digest == p.digest && have->cameraModel == p.cameraModel) return;  // the same again
        profiles_.push_back(std::make_shared<const CameraProfile>(std::move(p)));
    };

    std::error_code ec;
    if (!fs::is_directory(fileOrFolder, ec)) {
        if (!fs::exists(fileOrFolder, ec))
            throw std::runtime_error("'" + fileOrFolder.u8string() + "': no such file or folder");
        take(readCameraProfile(fileOrFolder));
        return;
    }
    std::vector<fs::path> found;
    for (fs::recursive_directory_iterator it(fileOrFolder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) continue;
        std::string ext = it->path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (ext == ".dcp") found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    for (const fs::path& f : found) {
        try {
            take(readCameraProfile(f));
        } catch (const std::exception& e) {
            skipped.push_back(e.what());
        }
    }
}

std::vector<std::shared_ptr<const CameraProfile>> ProfileLibrary::select(
    const std::string& cameraModel, unsigned colorPlanes, bool anyCamera,
    std::vector<std::string>& notes) const {
    std::vector<std::shared_ptr<const CameraProfile>> chosen;
    for (const auto& p : profiles_) {
        if (!anyCamera && !p->cameraModel.empty() && !sameCameraModel(p->cameraModel, cameraModel))
            continue;
        const std::string from = p->fileName.empty() ? std::string() : " (" + p->fileName + ")";
        if (p->colorPlanes != colorPlanes) {
            notes.push_back("profile '" + p->displayName() + "'" + from + " is for a sensor with " +
                            std::to_string(p->colorPlanes) + " colours, this one has " +
                            std::to_string(colorPlanes) + "; left out");
            continue;
        }
        if (!p->mayBeEmbedded()) {
            notes.push_back("profile '" + p->displayName() + "'" + from +
                            " is marked 'never embed' by its maker; left out");
            continue;
        }
        const auto twin = std::find_if(chosen.begin(), chosen.end(), [&](const auto& c) {
            return c->displayName() == p->displayName();
        });
        if (twin != chosen.end()) {
            notes.push_back("two profiles are named '" + p->displayName() + "'; kept the one from " +
                            ((*twin)->fileName.empty() ? "the first" : (*twin)->fileName) +
                            (p->fileName.empty() ? "" : ", left out " + p->fileName));
            continue;
        }
        chosen.push_back(p);
    }
    return chosen;
}

}  // namespace dngconv
