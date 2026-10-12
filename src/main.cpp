// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// dngconv command-line front end.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

#include "camera_profile.hpp"
#include "dng_check.hpp"
#include "dng_writer.hpp"
#include "job_runner.hpp"
#include "jxl_codec.hpp"
#include "original_raw.hpp"
#include "raw_reader.hpp"
#include "sidecar.hpp"
#include "source_record.hpp"
#include "version.hpp"

namespace fs = std::filesystem;
using namespace dngconv;

namespace {

struct Settings {
    std::vector<std::string> inputs;
    std::string output;
    DngCompression compression = DngCompression::LosslessJpeg;
    int jxlEffort = 3;
    DngPreview preview = DngPreview::Auto;
    uint32_t previewSize = 1024;  // longest edge of a rendered preview; 0 = largest
    bool makerNotes = true;
    bool embedOriginal = false;
    LensCorrectionMode lensCorrections = LensCorrectionMode::Auto;
    bool sidecar = true;
    std::vector<std::string> profiles;  // .dcp files and folders of them
    bool profileAnyCamera = false;
    bool recursive = false;
    bool force = false;
    bool update = false;
    bool updateByContent = false;  // decide by the source's checksum instead of file times
    bool sourceRecord = true;      // note the source's checksum in each DNG
    bool addChecksums = false;     // check command: write the checksums a file lacks
    bool keepTimes = false;
    bool verify = false;
    bool info = false;
    bool quiet = false;
    unsigned threads = 0;   // all threads together; 0 = one per core
    unsigned parallel = 0;  // files worked on at once; 0 = decide from the above
};

struct Job {
    fs::path input;
    fs::path relative;   // path below the output directory, extension still the source's
    bool named = false;  // given as a file argument, not found by scanning a folder
};

const char* const kUsage =
    "Usage: dngconv [options] <file or folder>...\n"
    "       dngconv extract [options] <DNG file or folder>...\n"
    "       dngconv check [options] <DNG file or folder>...\n"
    "\n"
    "Converts camera raw files to Digital Negative (DNG). The extract command\n"
    "restores the original files from DNGs written with --embed-original; the\n"
    "check command tells whether DNG files are still intact. See\n"
    "'dngconv extract --help' and 'dngconv check --help'.\n"
    "\n"
    "Options:\n"
    "  -o, --output <path>       output file (one input) or output folder\n"
    "                            (default: next to each source file)\n"
    "  -c, --compression <mode>  lossless (default; lossless JPEG, read by every\n"
    "                            DNG program), jxl (lossless JPEG XL: smaller,\n"
    "                            but needs a reader of DNG 1.7), or none\n"
    "      --jxl-effort <n>      how hard JPEG XL tries, 1 (fastest) to 9\n"
    "                            (smallest, much slower); default 3\n"
    "      --preview <which>     the JPEG preview in the DNG: auto (default; the\n"
    "                            camera's own, or one rendered from the raw data\n"
    "                            if the file has none), camera, render, or none\n"
    "      --preview-size <n>    longest edge of a rendered preview in pixels\n"
    "                            (default 1024), or max for the largest possible\n"
    "      --no-preview          the same as --preview none\n"
    "      --no-maker-notes      do not copy the camera maker's private metadata\n"
    "  -e, --embed-original      store the source file inside the DNG, so that\n"
    "                            'dngconv extract' can restore it later\n"
    "      --lens-corrections <which>\n"
    "                            pass the camera's lens corrections on to the\n"
    "                            DNG reader: auto (default; those the camera was\n"
    "                            set to apply), all, or none\n"
    "      --no-sidecar          do not take over an XMP sidecar file (IMG.xmp or\n"
    "                            IMG.CR3.xmp next to IMG.CR3)\n"
    "      --profile <path>      embed camera profiles: a .dcp file, or a folder\n"
    "                            that is searched for them. Each picture gets\n"
    "                            the profiles made for its camera, the first\n"
    "                            given as its default. May be repeated\n"
    "      --profile-any-camera  embed the profiles whatever camera they name\n"
    "  -r, --recursive           descend into sub-folders\n"
    "  -f, --force               overwrite existing DNG files\n"
    "  -u, --update              skip files whose DNG exists and is not older than\n"
    "                            the source; convert the others\n"
    "      --update-by <what>    how --update decides (and turns it on): time\n"
    "                            (default), or content: the source file and its\n"
    "                            sidecar are compared with the checksums noted in\n"
    "                            the DNG when it was made, whatever the file times\n"
    "      --no-source-record    do not note the source file's checksum in the DNG\n"
    "  -t, --keep-times          give each DNG the modification time of its source\n"
    "      --verify              re-read each DNG and compare it with the source\n"
    "                            (pixels, and the embedded original if any)\n"
    "  -j, --jobs <n>            threads in total (default: all cores)\n"
    "  -p, --parallel <n>        files converted at the same time (default: up to\n"
    "                            4, as far as the threads allow)\n"
    "  -i, --info                show what the decoder finds; write nothing\n"
    "  -q, --quiet               only report problems\n"
    "  -V, --version             show version information\n"
    "  -h, --help                show this help\n";

const char* const kExtractUsage =
    "Usage: dngconv extract [options] <DNG file or folder>...\n"
    "\n"
    "Restores the original camera raw file stored in a DNG (by 'dngconv\n"
    "--embed-original', or by another converter that embeds originals). The file\n"
    "gets the name it had before conversion. Its checksum is verified first.\n"
    "\n"
    "Options:\n"
    "  -o, --output <path>       output file (one input) or output folder\n"
    "                            (default: next to each DNG)\n"
    "  -r, --recursive           descend into sub-folders\n"
    "  -f, --force               overwrite existing files\n"
    "  -t, --keep-times          give each restored file the modification time of\n"
    "                            its DNG\n"
    "  -j, --jobs <n>            threads in total (default: all cores)\n"
    "  -p, --parallel <n>        files worked on at the same time (default: up to 4)\n"
    "  -q, --quiet               only report problems\n"
    "  -h, --help                show this help\n";

const char* const kCheckUsage =
    "Usage: dngconv check [options] <DNG file or folder>...\n"
    "\n"
    "Tells whether DNG files are still what their writer stored: recomputes the\n"
    "checksum of the raw image data and of the embedded original file, if there\n"
    "is one, and compares them with the checksums in the file. Works on DNGs\n"
    "from any converter; files without checksums are reported as not verified.\n"
    "\n"
    "Options:\n"
    "  -a, --add                 write the checksums into files that have none\n"
    "                            (most DNGs written by cameras), so that they can\n"
    "                            be checked from then on. Nothing in the file is\n"
    "                            moved or changed; a few hundred bytes are added\n"
    "                            at its end. Damaged files are left alone\n"
    "  -t, --keep-times          with --add, leave each file's modification time\n"
    "                            as it was\n"
    "  -r, --recursive           descend into sub-folders\n"
    "  -j, --jobs <n>            threads in total (default: all cores)\n"
    "  -p, --parallel <n>        files checked at the same time (default: up to 4)\n"
    "  -q, --quiet               only report files that are not intact\n"
    "  -h, --help                show this help\n"
    "\n"
    "Exit status: 0 if no file is damaged, 1 if one is or could not be read,\n"
    "2 for a usage error.\n";

// Extensions picked up when a folder is given. Files named explicitly are
// always tried, whatever their extension.
const std::set<std::string>& rawExtensions() {
    static const std::set<std::string> ext = {
        ".3fr", ".ari", ".arw", ".bay", ".cap", ".cr2", ".cr3", ".crw", ".dcr", ".dcs",
        ".drf", ".eip", ".erf", ".fff", ".gpr", ".iiq", ".k25", ".kdc", ".mdc", ".mef",
        ".mos", ".mrw", ".nef", ".nrw", ".orf", ".ori", ".pef", ".ptx", ".pxn", ".raf",
        ".raw", ".rw2", ".rwl", ".rwz", ".sr2", ".srf", ".srw", ".sti", ".x3f"};
    return ext;
}

std::string lowerExtension(const fs::path& p) {
    std::string e = p.extension().u8string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return e;
}

std::string show(const fs::path& p) { return p.u8string(); }

std::string megabytes(std::uintmax_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
std::string format(const char* fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    return buffer;
}

// Reads a count given as an option value. Returns false, with a message, if
// it is not a number in 1..1024.
bool parseCount(const std::vector<std::string>& args, size_t& i, const char* command, unsigned& out) {
    char* end = nullptr;
    const long n = i + 1 < args.size() ? std::strtol(args[i + 1].c_str(), &end, 10) : 0;
    if (i + 1 >= args.size() || !end || *end || n < 1 || n > 1024) {
        std::fprintf(stderr, "%s: option %s needs a number from 1 to 1024\n", command, args[i].c_str());
        return false;
    }
    out = static_cast<unsigned>(n);
    ++i;
    return true;
}

// How the threads are spent: on how many files at once, and how many each
// file gets for its own work (compressing tiles, hashing).
struct ThreadPlan {
    unsigned files = 1;
    unsigned perFile = 1;
};

ThreadPlan planThreads(const Settings& s, size_t jobs) {
    unsigned total = s.threads ? s.threads : std::thread::hardware_concurrency();
    if (total == 0) total = 1;
    ThreadPlan plan;
    // Every file being worked on is held in memory in full, so the default
    // stops at four; -p lifts that.
    plan.files = s.parallel ? s.parallel : std::min(total, 4u);
    plan.files = static_cast<unsigned>(std::min<size_t>(plan.files, std::max<size_t>(jobs, 1)));
    plan.perFile = std::max(1u, total / plan.files);
    return plan;
}

// Hands a job's messages to the terminal: warnings and errors first, as
// they were printed before the line that says what was done.
void emitReport(const JobReport& report) {
    if (!report.err.empty()) {
        std::fputs(report.err.c_str(), stderr);
        std::fflush(stderr);
    }
    if (!report.out.empty()) {
        std::fputs(report.out.c_str(), stdout);
        std::fflush(stdout);
    }
}

std::vector<std::string> commandLine(int argc, char** argv) {
    std::vector<std::string> args;
#if defined(_WIN32)
    // The narrow argv is in the ANSI code page; fetch the real Unicode line.
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; wide && i < count; ++i) args.push_back(fs::path(wide[i]).u8string());
    if (wide) LocalFree(wide);
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return args;
}

bool parseArguments(const std::vector<std::string>& args, Settings& s, int& exitCode) {
    auto needValue = [&](size_t& i, const std::string& name) -> const std::string* {
        if (i + 1 >= args.size()) {
            std::fprintf(stderr, "dngconv: option %s needs a value\n", name.c_str());
            return nullptr;
        }
        return &args[++i];
    };

    bool optionsDone = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (optionsDone || a.empty() || a[0] != '-' || a == "-") {
            s.inputs.push_back(a);
        } else if (a == "--") {
            optionsDone = true;
        } else if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            exitCode = 0;
            return false;
        } else if (a == "-V" || a == "--version") {
            std::printf("dngconv %s\ndecoder: LibRaw %s (%d camera models)\n", kVersion,
                        decoderVersion().c_str(), supportedCameraCount());
            if (jxlAvailable())
                std::printf("JPEG XL: libjxl %s\n", jxlVersion().c_str());
            else
                std::printf("JPEG XL: not in this build\n");
            exitCode = 0;
            return false;
        } else if (a == "-o" || a == "--output") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            s.output = *v;
        } else if (a == "-c" || a == "--compression") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            if (*v == "lossless") {
                s.compression = DngCompression::LosslessJpeg;
            } else if (*v == "none") {
                s.compression = DngCompression::None;
            } else if (*v == "jxl" || *v == "jpegxl" || *v == "jpeg-xl") {
                s.compression = DngCompression::JpegXl;
            } else {
                std::fprintf(stderr, "dngconv: unknown compression '%s' (use lossless, jxl or none)\n",
                             v->c_str());
                return false;
            }
        } else if (a == "--jxl-effort") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            char* end = nullptr;
            const long n = std::strtol(v->c_str(), &end, 10);
            if (v->empty() || !end || *end || n < 1 || n > 9) {
                std::fprintf(stderr, "dngconv: --jxl-effort needs a number from 1 to 9\n");
                return false;
            }
            s.jxlEffort = static_cast<int>(n);
        } else if (a == "-j" || a == "--jobs") {
            if (!parseCount(args, i, "dngconv", s.threads)) return false;
        } else if (a == "-p" || a == "--parallel") {
            if (!parseCount(args, i, "dngconv", s.parallel)) return false;
        } else if (a == "--no-sidecar") {
            s.sidecar = false;
        } else if (a == "--profile") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            s.profiles.push_back(*v);
        } else if (a == "--profile-any-camera") {
            s.profileAnyCamera = true;
        } else if (a == "-u" || a == "--update") {
            s.update = true;
        } else if (a == "--update-by") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            if (*v == "time") {
                s.updateByContent = false;
            } else if (*v == "content") {
                s.updateByContent = true;
            } else {
                std::fprintf(stderr, "dngconv: unknown choice '%s' for --update-by (use time or content)\n",
                             v->c_str());
                return false;
            }
            s.update = true;
        } else if (a == "--no-source-record") {
            s.sourceRecord = false;
        } else if (a == "-t" || a == "--keep-times") {
            s.keepTimes = true;
        } else if (a == "--no-preview") {
            s.preview = DngPreview::None;
        } else if (a == "--preview") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            if (*v == "auto") {
                s.preview = DngPreview::Auto;
            } else if (*v == "camera") {
                s.preview = DngPreview::Camera;
            } else if (*v == "render") {
                s.preview = DngPreview::Render;
            } else if (*v == "none") {
                s.preview = DngPreview::None;
            } else {
                std::fprintf(stderr,
                             "dngconv: unknown choice '%s' for --preview (use auto, camera, "
                             "render or none)\n",
                             v->c_str());
                return false;
            }
        } else if (a == "--preview-size") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            char* end = nullptr;
            const long n = std::strtol(v->c_str(), &end, 10);
            if (*v == "max") {
                s.previewSize = 0;
            } else if (v->empty() || !end || *end || n < 64 || n > 65535) {
                std::fprintf(stderr, "dngconv: --preview-size needs a number of pixels from 64 to "
                                     "65535, or max\n");
                return false;
            } else {
                s.previewSize = static_cast<uint32_t>(n);
            }
        } else if (a == "--no-maker-notes") {
            s.makerNotes = false;
        } else if (a == "-e" || a == "--embed-original") {
            s.embedOriginal = true;
        } else if (a == "--lens-corrections") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            if (*v == "auto") {
                s.lensCorrections = LensCorrectionMode::Auto;
            } else if (*v == "all") {
                s.lensCorrections = LensCorrectionMode::All;
            } else if (*v == "none") {
                s.lensCorrections = LensCorrectionMode::None;
            } else {
                std::fprintf(stderr,
                             "dngconv: unknown choice '%s' for --lens-corrections "
                             "(use auto, all or none)\n",
                             v->c_str());
                return false;
            }
        } else if (a == "-r" || a == "--recursive") {
            s.recursive = true;
        } else if (a == "-f" || a == "--force") {
            s.force = true;
        } else if (a == "--verify") {
            s.verify = true;
        } else if (a == "-i" || a == "--info") {
            s.info = true;
        } else if (a == "-q" || a == "--quiet") {
            s.quiet = true;
        } else {
            std::fprintf(stderr, "dngconv: unknown option '%s'\n", a.c_str());
            return false;
        }
    }
    if (s.inputs.empty()) {
        std::fputs(kUsage, stderr);
        return false;
    }
    return true;
}

template <typename Iterator>
void scanFolder(const fs::path& root, const std::set<std::string>& extensions,
                std::vector<Job>& jobs) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (Iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && extensions.count(lowerExtension(it->path())))
            found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    for (const auto& f : found) jobs.push_back({f, fs::relative(f, root, ec), false});
}

// Returns false if any input does not exist.
bool collectJobs(const Settings& s, const std::set<std::string>& extensions,
                 std::vector<Job>& jobs, bool& anyFolder) {
    bool ok = true;
    anyFolder = false;
    for (const std::string& arg : s.inputs) {
        const fs::path p = fs::u8path(arg);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            anyFolder = true;
            if (s.recursive)
                scanFolder<fs::recursive_directory_iterator>(p, extensions, jobs);
            else
                scanFolder<fs::directory_iterator>(p, extensions, jobs);
        } else if (fs::exists(p, ec)) {
            jobs.push_back({p, p.filename(), true});
        } else {
            std::fprintf(stderr, "dngconv: %s: no such file or folder\n", arg.c_str());
            ok = false;
        }
    }
    return ok;
}

const char* colourName(uint8_t code) {
    static const char* const names[] = {"R", "G", "B", "C", "M", "Y", "W"};
    return code < 7 ? names[code] : "?";
}

// How closely the DNG polynomials follow the camera's curves.
std::string fitQuality(const LensOpcodes& lens) {
    char buffer[96];
    std::string out;
    if (lens.planes) {
        std::snprintf(buffer, sizeof buffer, "geometry within %.2f px", lens.warpErrorPixels);
        out = buffer;
    }
    if (lens.vignetting) {
        std::snprintf(buffer, sizeof buffer, "%sbrightness within %.1f %%", out.empty() ? "" : ", ",
                      lens.gainErrorPercent);
        out += buffer;
    }
    return out;
}

// What a camera profile file holds.
void printProfile(const fs::path& path, const CameraProfile& p) {
    static const char* const policies[] = {"copying allowed", "embed if used", "never embed",
                                           "no restrictions"};
    std::printf("%s\n", show(path).c_str());
    std::printf("  profile       %s\n", p.name.empty() ? "(unnamed)" : p.name.c_str());
    if (!p.groupName.empty()) std::printf("  group         %s\n", p.groupName.c_str());
    std::printf("  for camera    %s\n", p.cameraModel.empty() ? "any" : p.cameraModel.c_str());
    std::printf("  contents      %s\n", p.summary().c_str());
    if (!p.copyright.empty()) std::printf("  copyright     %s\n", p.copyright.c_str());
    std::printf("  embedding     %s\n", p.embedPolicy < 4 ? policies[p.embedPolicy] : "unknown policy");
    if (p.dngVersion > 0x0104)
        std::printf("  note          uses tags of DNG %u.%u\n", p.dngVersion >> 8, p.dngVersion & 0xff);
}

void printInfo(const fs::path& path, const RawReadResult& r, const Settings& settings,
               const ProfileLibrary& library) {
    const LensCorrectionMode lensMode = settings.lensCorrections;
    const RawImage& img = r.image;
    std::printf("%s\n", show(path).c_str());
    std::printf("  camera        %s\n", img.uniqueCameraModel.c_str());
    if (!img.lensModel.empty()) std::printf("  lens          %s\n", img.lensModel.c_str());
    if (!img.dateTime.empty()) std::printf("  taken         %s\n", img.dateTime.c_str());
    if (img.exposureTime > 0 || img.fNumber > 0 || img.iso > 0) {
        std::printf("  exposure     ");
        if (img.exposureTime >= 1)
            std::printf(" %.1f s", img.exposureTime);
        else if (img.exposureTime > 0)
            std::printf(" 1/%.0f s", 1.0 / img.exposureTime);
        if (img.fNumber > 0) std::printf(" f/%.1f", img.fNumber);
        if (img.iso > 0) std::printf(" ISO %.0f", img.iso);
        if (img.focalLength > 0) std::printf(" %.0f mm", img.focalLength);
        std::printf("\n");
    }
    std::printf("  frame         %u x %u\n", img.width, img.height);
    std::printf("  active area   %u x %u at (%u, %u)\n", img.activeRight - img.activeLeft,
                img.activeBottom - img.activeTop, img.activeLeft, img.activeTop);
    std::printf("  default crop  %u x %u at (%u, %u)\n", img.cropWidth, img.cropHeight,
                img.cropLeft, img.cropTop);
    if (img.isCfa) {
        std::printf("  sensor        %ux%u mosaic:", img.cfaCols, img.cfaRows);
        for (uint32_t row = 0; row < img.cfaRows; ++row) {
            std::printf(" ");
            for (uint32_t col = 0; col < img.cfaCols; ++col)
                std::printf("%s", colourName(img.planeColor[img.cfaPattern[row * img.cfaCols + col]]));
        }
        std::printf("\n");
    } else {
        std::printf("  sensor        %s, %u sample%s per pixel\n",
                    img.colorPlanes == 1 ? "monochrome" : "full colour", img.samplesPerPixel,
                    img.samplesPerPixel == 1 ? "" : "s");
    }
    std::printf("  black level  ");
    for (size_t i = 0; i < img.blackLevel.size() && i < 8; ++i)
        std::printf(" %g", img.blackLevel[i]);
    if (img.blackLevel.size() > 8) std::printf(" ...");
    std::printf("\n  white level   %g\n", img.whiteLevel[0]);
    if (img.hasAsShotNeutral) {
        std::printf("  as-shot neutral");
        for (uint32_t p = 0; p < img.colorPlanes; ++p) std::printf(" %.4f", img.asShotNeutral[p]);
        std::printf("\n");
    }
    std::printf("  orientation   %d\n", img.orientation);
    if (!img.source.exif.empty() || !img.source.gps.empty() || !img.source.makerNote.empty()) {
        std::printf("  metadata      %zu EXIF tags", img.source.exif.size());
        if (!img.source.gps.empty()) std::printf(", %zu GPS tags", img.source.gps.size());
        if (!img.source.makerNote.empty())
            std::printf(", maker note of %zu bytes", img.source.makerNote.data.size());
        std::printf("\n");
    } else {
        std::printf("  metadata      nothing to copy (format not understood or no EXIF)\n");
    }
    if (!img.lens.empty()) {
        const LensCorrection& lens = img.lens;
        std::string found;
        const auto add = [&](bool has, bool enabled, bool inData, const char* name) {
            if (!has) return;
            if (!found.empty()) found += ", ";
            found += name;
            if (inData)
                found += " (already applied to the raw data by the camera)";
            else if (!enabled)
                found += " (off in the camera)";
        };
        add(lens.hasDistortion(), lens.distortionEnabled, false, "distortion");
        add(lens.hasCa(), lens.caEnabled, false, "chromatic aberration");
        add(lens.hasVignetting(), lens.vignettingEnabled, lens.vignettingInData, "vignetting");
        std::printf("  lens data     %s: %s\n", lens.origin.c_str(), found.c_str());
        const LensOpcodes opcodes = makeLensOpcodes(img, lensMode);
        if (opcodes.any())
            std::printf("  corrections   %s (%s)\n", opcodes.summary().c_str(),
                        fitQuality(opcodes).c_str());
        else
            std::printf("  corrections   none would be written\n");
        for (const auto& note : opcodes.notes) std::printf("  note          %s\n", note.c_str());
    }
    {
        DngWriteOptions wo;
        wo.preview = settings.preview;
        wo.previewSize = settings.previewSize;
        const PreviewPlan plan = planPreview(img, wo);
        if (plan.kind == PreviewPlan::Kind::Camera)
            std::printf("  preview       the camera's JPEG, %u x %u\n", plan.width, plan.height);
        else if (plan.kind == PreviewPlan::Kind::Rendered && settings.preview == DngPreview::Render)
            std::printf("  preview       to be rendered from the raw data, %u x %u\n", plan.width, plan.height);
        else if (plan.kind == PreviewPlan::Kind::Rendered)
            std::printf("  preview       %s; one of %u x %u would be rendered from the raw data\n",
                        img.previewJpeg.empty() ? "none in the file" : "only a small one in the file",
                        plan.width, plan.height);
        else
            std::printf("  preview       none\n");
    }
    if (!img.xmpSidecarName.empty())
        std::printf("  sidecar       %s (%zu bytes of XMP)\n", img.xmpSidecarName.c_str(), img.xmpSidecar.size());
    if (!library.empty() && img.colorPlanes >= 3) {
        std::vector<std::string> notes;
        const auto chosen = library.select(img.uniqueCameraModel, img.colorPlanes,
                                           settings.profileAnyCamera, notes);
        if (chosen.empty())
            std::printf("  profile       none of the %zu given is for this camera\n", library.size());
        for (size_t i = 0; i < chosen.size(); ++i)
            std::printf("  %s %s (%s): %s\n", i == 0 ? "profile      " : "also offered ",
                        chosen[i]->displayName().c_str(), chosen[i]->fileName.c_str(),
                        chosen[i]->summary().c_str());
        for (const auto& note : notes) std::printf("  note          %s\n", note.c_str());
    }
    if (r.sourceIsDng) {
        std::printf("  note          this file already is a DNG\n");
        try {
            const EmbeddedOriginal original = readEmbeddedOriginal(path, false);
            if (original.present)
                std::printf("  original      embedded: %s (%s); restore it with 'dngconv extract'\n",
                            original.fileName.empty() ? "unnamed" : original.fileName.c_str(),
                            megabytes(original.originalSize).c_str());
        } catch (const std::exception& e) {
            std::printf("  original      embedded but unreadable: %s\n", e.what());
        }
    }
    for (const auto& w : r.warnings) std::printf("  warning       %s\n", w.c_str());
}

// Reads the written DNG back and compares every sample with the source.
void verifyOutput(const RawImage& source, const fs::path& dng, bool jxl, unsigned threads) {
    if (jxl) {
        // The decoder library does not read JPEG XL; the reader of the check
        // command does, and the file's own checksum is tried as well.
        const StoredRawImage back = readStoredRawImage(dng, threads);
        if (back.width != source.width || back.height != source.height || back.planes != source.samplesPerPixel)
            throw std::runtime_error("verification failed: frame size differs after re-reading");
        if (back.samples != source.pixels) {
            size_t differing = 0;
            for (size_t i = 0; i < source.pixels.size(); ++i)
                if (back.samples[i] != source.pixels[i]) ++differing;
            throw std::runtime_error("verification failed: " + std::to_string(differing) +
                                     " samples differ after re-reading");
        }
        if (checkDng(dng, threads).rawImage == DngCheck::State::Damaged)
            throw std::runtime_error("verification failed: the raw data does not match its checksum");
    } else {
    RawReadOptions ro;
    ro.loadPreview = false;
    ro.copyMetadata = false;
    ro.readLensData = false;
    ro.readSidecar = false;
    const RawReadResult back = readRaw(dng, ro);
    const RawImage& b = back.image;
    if (b.width != source.width || b.height != source.height ||
        b.samplesPerPixel != source.samplesPerPixel)
        throw std::runtime_error("verification failed: frame size differs after re-reading");
    if (b.pixels != source.pixels) {
        size_t differing = 0;
        for (size_t i = 0; i < source.pixels.size(); ++i)
            if (b.pixels[i] != source.pixels[i]) ++differing;
        throw std::runtime_error("verification failed: " + std::to_string(differing) +
                                 " samples differ after re-reading");
    }
    if (b.activeTop != source.activeTop || b.activeLeft != source.activeLeft ||
        b.activeBottom != source.activeBottom || b.activeRight != source.activeRight)
        throw std::runtime_error("verification failed: active area differs after re-reading");
    }

    // The embedded original must come back byte for byte.
    if (!source.originalFile.empty()) {
        const EmbeddedOriginal original = readEmbeddedOriginal(dng);
        if (!original.present)
            throw std::runtime_error("verification failed: the embedded original is missing");
        if (!original.hasDigest || !original.digestMatches)
            throw std::runtime_error("verification failed: checksum of the embedded original");
        if (unpackOriginalRaw(original.packed.data(), original.packed.size()) != source.originalFile)
            throw std::runtime_error("verification failed: the embedded original differs from "
                                     "the source file");
    }
}

// Writes a file through a temporary one, so that an interrupted run leaves no
// truncated file under the final name.
void writeFileSafely(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::path tmp = path;
    tmp += ".part";
    try {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create " + show(tmp));
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) throw std::runtime_error("write error on " + show(tmp));
        fs::rename(tmp, path);
    } catch (...) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        throw;
    }
}

// Decides whether -o names a file or a folder: a file only when there is
// exactly one explicitly named input and the path is not an existing folder.
bool outputIsSingleFile(const Settings& settings, const std::vector<Job>& jobs, bool anyFolder) {
    if (settings.output.empty()) return false;
    std::error_code ec;
    const bool endsWithSeparator = settings.output.back() == '/' || settings.output.back() == '\\';
    return jobs.size() == 1 && !anyFolder && !fs::is_directory(fs::u8path(settings.output), ec) &&
           !endsWithSeparator;
}

bool parseExtractArguments(const std::vector<std::string>& args, Settings& s, int& exitCode) {
    bool optionsDone = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (optionsDone || a.empty() || a[0] != '-' || a == "-") {
            s.inputs.push_back(a);
        } else if (a == "--") {
            optionsDone = true;
        } else if (a == "-h" || a == "--help") {
            std::fputs(kExtractUsage, stdout);
            exitCode = 0;
            return false;
        } else if (a == "-o" || a == "--output") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "dngconv: option %s needs a value\n", a.c_str());
                return false;
            }
            s.output = args[++i];
        } else if (a == "-r" || a == "--recursive") {
            s.recursive = true;
        } else if (a == "-f" || a == "--force") {
            s.force = true;
        } else if (a == "-t" || a == "--keep-times") {
            s.keepTimes = true;
        } else if (a == "-j" || a == "--jobs") {
            if (!parseCount(args, i, "dngconv extract", s.threads)) return false;
        } else if (a == "-p" || a == "--parallel") {
            if (!parseCount(args, i, "dngconv extract", s.parallel)) return false;
        } else if (a == "-q" || a == "--quiet") {
            s.quiet = true;
        } else {
            std::fprintf(stderr, "dngconv extract: unknown option '%s'\n", a.c_str());
            return false;
        }
    }
    if (s.inputs.empty()) {
        std::fputs(kExtractUsage, stderr);
        return false;
    }
    return true;
}

// The extract command: restores embedded original files from DNGs.
int runExtract(const std::vector<std::string>& args) {
    Settings settings;
    int exitCode = 2;
    if (!parseExtractArguments(args, settings, exitCode)) return exitCode;

    static const std::set<std::string> dngExtension = {".dng"};
    std::vector<Job> jobs;
    bool anyFolder = false;
    const bool allFound = collectJobs(settings, dngExtension, jobs, anyFolder);
    if (jobs.empty()) {
        if (allFound) std::fprintf(stderr, "dngconv: no DNG files found\n");
        return 1;
    }
    const bool outputIsFile = outputIsSingleFile(settings, jobs, anyFolder);
    const fs::path outputRoot = settings.output.empty() ? fs::path() : fs::u8path(settings.output);

    // A first pass over the DNGs, reading only the names: it settles where
    // each original goes before any is written, so that two DNGs holding a
    // file of the same name cannot race for it.
    enum class Outcome { Extracted, NoOriginal, Failed };
    std::vector<Outcome> outcomes(jobs.size(), Outcome::Failed);
    std::vector<fs::path> targets(jobs.size());
    std::vector<std::string> early(jobs.size());  // a failure found in this pass
    std::vector<char> absent(jobs.size(), 0);
    {
        std::map<std::string, size_t> taken;
        for (size_t i = 0; i < jobs.size(); ++i) {
            const Job& job = jobs[i];
            try {
                const EmbeddedOriginal original = readEmbeddedOriginal(job.input, false);
                if (!original.present) {
                    absent[i] = 1;
                    continue;
                }
                if (outputIsFile) {
                    targets[i] = outputRoot;
                } else {
                    // Without a recorded name, fall back to the DNG's own.
                    const fs::path fileName = original.fileName.empty()
                                                  ? fs::path(job.input.stem()) += ".original"
                                                  : fs::u8path(original.fileName);
                    const fs::path folder = outputRoot.empty() ? job.input.parent_path()
                                                               : outputRoot / job.relative.parent_path();
                    targets[i] = folder / fileName;
                }
                const auto [first, fresh] = taken.emplace(targets[i].lexically_normal().u8string(), i);
                if (!fresh)
                    early[i] = "holds '" + show(targets[i].filename()) + "', as does '" +
                               show(jobs[first->second].input) + "'; extract them into separate folders";
            } catch (const std::exception& e) {
                early[i] = e.what();
            }
        }
    }

    const ThreadPlan plan = planThreads(settings, jobs.size());
    const auto extractOne = [&](size_t index, JobReport& report) {
        const Job& job = jobs[index];
        const std::string name = show(job.input);
        try {
            if (!early[index].empty()) throw std::runtime_error(early[index]);
            if (absent[index]) {
                // In a folder, DNGs without an original are expected; a file
                // asked for by name that has none is a failed request.
                if (job.named) throw std::runtime_error("holds no embedded original");
                outcomes[index] = Outcome::NoOriginal;
                return;
            }
            const EmbeddedOriginal original = readEmbeddedOriginal(job.input);
            if (!original.present) throw std::runtime_error("holds no embedded original");
            if (original.hasDigest && !original.digestMatches)
                throw std::runtime_error("the embedded original is damaged (checksum mismatch); "
                                         "nothing was written");
            const std::vector<uint8_t> bytes =
                unpackOriginalRaw(original.packed.data(), original.packed.size());

            const fs::path& target = targets[index];
            std::error_code ec;
            if (fs::exists(target, ec)) {
                if (fs::equivalent(target, job.input, ec))
                    throw std::runtime_error("output would overwrite the DNG itself");
                if (!settings.force)
                    throw std::runtime_error("'" + show(target) +
                                             "' already exists (use --force to overwrite)");
            }
            if (target.has_parent_path()) {
                fs::create_directories(target.parent_path(), ec);
                if (ec)
                    throw std::runtime_error("cannot create folder '" +
                                             show(target.parent_path()) + "': " + ec.message());
            }
            writeFileSafely(target, bytes);
            if (settings.keepTimes) {
                const fs::file_time_type time = fs::last_write_time(job.input, ec);
                if (!ec) fs::last_write_time(target, time, ec);
                if (ec)
                    report.err += format("dngconv: %s: warning: could not set the time of '%s': %s\n",
                                         name.c_str(), show(target).c_str(), ec.message().c_str());
            }
            if (!settings.quiet)
                report.out = format("%s -> %s  (%s%s)\n", name.c_str(), show(target).c_str(),
                                    megabytes(bytes.size()).c_str(),
                                    original.hasDigest ? ", checksum verified" : ", no checksum in file");
            outcomes[index] = Outcome::Extracted;
        } catch (const std::exception& e) {
            report.err += format("dngconv: %s: %s\n", name.c_str(), e.what());
            outcomes[index] = Outcome::Failed;
        }
    };
    runJobs(jobs.size(), plan.files, extractOne, [](size_t, const JobReport& report) { emitReport(report); });

    int extracted = 0, skipped = 0, failed = allFound ? 0 : 1;
    for (Outcome o : outcomes) {
        if (o == Outcome::Extracted) ++extracted;
        if (o == Outcome::NoOriginal) ++skipped;
        if (o == Outcome::Failed) ++failed;
    }

    if (!settings.quiet && jobs.size() > 1) {
        std::printf("%d extracted, %d failed", extracted, failed);
        if (skipped) std::printf(", %d without an embedded original", skipped);
        std::printf("\n");
    }
    if (extracted == 0 && failed == 0) {
        std::fprintf(stderr, "dngconv: none of the DNG files holds an embedded original\n");
        return 1;
    }
    return failed ? 1 : 0;
}

bool parseCheckArguments(const std::vector<std::string>& args, Settings& s, int& exitCode) {
    bool optionsDone = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (optionsDone || a.empty() || a[0] != '-' || a == "-") {
            s.inputs.push_back(a);
        } else if (a == "--") {
            optionsDone = true;
        } else if (a == "-h" || a == "--help") {
            std::fputs(kCheckUsage, stdout);
            exitCode = 0;
            return false;
        } else if (a == "-a" || a == "--add") {
            s.addChecksums = true;
        } else if (a == "-t" || a == "--keep-times") {
            s.keepTimes = true;
        } else if (a == "-j" || a == "--jobs") {
            if (!parseCount(args, i, "dngconv check", s.threads)) return false;
        } else if (a == "-p" || a == "--parallel") {
            if (!parseCount(args, i, "dngconv check", s.parallel)) return false;
        } else if (a == "-r" || a == "--recursive") {
            s.recursive = true;
        } else if (a == "-q" || a == "--quiet") {
            s.quiet = true;
        } else {
            std::fprintf(stderr, "dngconv check: unknown option '%s'\n", a.c_str());
            return false;
        }
    }
    if (s.inputs.empty()) {
        std::fputs(kCheckUsage, stderr);
        return false;
    }
    return true;
}

// The check command: compares DNG files with the checksums they carry.
int runCheck(const std::vector<std::string>& args) {
    Settings settings;
    int exitCode = 2;
    if (!parseCheckArguments(args, settings, exitCode)) return exitCode;

    static const std::set<std::string> dngExtension = {".dng"};
    std::vector<Job> jobs;
    bool anyFolder = false;
    const bool allFound = collectJobs(settings, dngExtension, jobs, anyFolder);
    if (jobs.empty()) {
        if (allFound) std::fprintf(stderr, "dngconv: no DNG files found\n");
        return 1;
    }

    // A file named twice is written to once.
    if (settings.addChecksums) {
        std::set<std::string> seen;
        std::vector<Job> distinct;
        for (Job& job : jobs) {
            std::error_code ec;
            const fs::path real = fs::weakly_canonical(job.input, ec);
            if (seen.insert(ec ? job.input.u8string() : real.u8string()).second) distinct.push_back(std::move(job));
        }
        jobs = std::move(distinct);
    }

    using State = DngCheck::State;
    enum class Outcome { Intact, Damaged, Unverified, Failed };
    std::vector<Outcome> outcomes(jobs.size(), Outcome::Failed);
    std::vector<char> added(jobs.size(), 0);    // checksums were written into the file
    std::vector<char> addable(jobs.size(), 0);  // they could be
    const ThreadPlan plan = planThreads(settings, jobs.size());

    const auto checkOne = [&](size_t index, JobReport& report) {
        const std::string name = show(jobs[index].input);
        try {
            DngCheck c;
            std::string addedNote;  // what was written, to go in front of the verdict
            if (settings.addChecksums) {
                const fs::path& file = jobs[index].input;
                std::error_code ec;
                const fs::file_time_type time = fs::last_write_time(file, ec);
                const bool haveTime = !ec;
                const DngChecksumsAdded done = addDngChecksums(file, plan.perFile);
                c = done.check;
                if (done.any()) {
                    added[index] = 1;
                    addedNote = done.rawImage && done.original
                                    ? "checksums added for the raw image and the embedded original; "
                                    : done.rawImage ? "checksum added for the raw image; "
                                                    : "checksum added for the embedded original; ";
                    if (settings.keepTimes && haveTime) {
                        fs::last_write_time(file, time, ec);
                        if (ec)
                            report.err += format("dngconv: %s: warning: could not set the time back: %s\n",
                                                 name.c_str(), ec.message().c_str());
                    }
                }
            } else {
                c = checkDng(jobs[index].input, plan.perFile);
                addable[index] = c.canAddChecksum ? 1 : 0;
            }
            if (c.damaged()) {
                // A file that is cut short usually has more than one thing
                // wrong with it; the cause is the useful message.
                std::string what = c.truncated ? c.truncatedDetail : "";
                if (!c.truncated && c.rawImage == State::Damaged) what = c.rawDetail;
                if (!c.truncated && c.original == State::Damaged)
                    what += (what.empty() ? "" : "; ") + c.originalDetail;
                // On stdout like the other verdicts, so that one stream holds the whole report.
                report.out = format("%s: DAMAGED: %s\n", name.c_str(), what.c_str());
                outcomes[index] = Outcome::Damaged;
            } else if (c.intact()) {
                // What was written to a file is said even with --quiet.
                if (!settings.quiet || !addedNote.empty())
                    report.out = format("%s: %sintact (raw image%s)\n", name.c_str(), addedNote.c_str(),
                                        c.original == State::Intact ? ", embedded original" : "");
                outcomes[index] = Outcome::Intact;
            } else {
                std::string what = c.rawImage == State::Intact ? "raw image intact" : c.rawDetail;
                if (c.original == State::Intact)
                    what += "; embedded original intact";
                else if (c.original == State::Unverified)
                    what += "; embedded original " + c.originalDetail;
                report.out = format("%s: %snot verified: %s\n", name.c_str(), addedNote.c_str(), what.c_str());
                outcomes[index] = Outcome::Unverified;
            }
        } catch (const std::exception& e) {
            report.err = format("dngconv: %s: %s\n", name.c_str(), e.what());
            outcomes[index] = Outcome::Failed;
        }
    };
    runJobs(jobs.size(), plan.files, checkOne, [](size_t, const JobReport& report) { emitReport(report); });

    int intact = 0, damaged = 0, unverified = 0, failed = allFound ? 0 : 1;
    for (Outcome o : outcomes) {
        if (o == Outcome::Intact) ++intact;
        if (o == Outcome::Damaged) ++damaged;
        if (o == Outcome::Unverified) ++unverified;
        if (o == Outcome::Failed) ++failed;
    }

    const int stamped = static_cast<int>(std::count(added.begin(), added.end(), 1));
    const int couldStamp = static_cast<int>(std::count(addable.begin(), addable.end(), 1));
    if (!settings.quiet && jobs.size() > 1) {
        std::printf("%zu checked: %d intact, %d damaged", jobs.size(), intact, damaged);
        if (unverified) std::printf(", %d not verified", unverified);
        if (failed) std::printf(", %d not readable", failed);
        if (settings.addChecksums) std::printf("; checksums added to %d", stamped);
        std::printf("\n");
    }
    if (!settings.quiet && couldStamp)
        std::printf("%s; 'dngconv check --add' writes %s.\n",
                    couldStamp == 1 ? "1 file has no checksum" : format("%d files have no checksum", couldStamp).c_str(),
                    couldStamp == 1 ? "it" : "them");
    return damaged || failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args = commandLine(argc, argv);
    if (!args.empty() && args.front() == "extract")
        return runExtract(std::vector<std::string>(args.begin() + 1, args.end()));
    if (!args.empty() && args.front() == "check")
        return runCheck(std::vector<std::string>(args.begin() + 1, args.end()));
    if (!args.empty() && args.front() == "convert") args.erase(args.begin());

    Settings settings;
    int exitCode = 2;
    if (!parseArguments(args, settings, exitCode)) return exitCode;

    if (settings.updateByContent && !settings.sourceRecord) {
        std::fprintf(stderr, "dngconv: --update-by content relies on the record that --no-source-record "
                             "leaves out; use one or the other\n");
        return 2;
    }
    if (settings.compression == DngCompression::JpegXl && !jxlAvailable() && !settings.info) {
        std::fprintf(stderr, "dngconv: this build has no JPEG XL support (it was built without libjxl); "
                             "use -c lossless\n");
        return 1;
    }

    // ---- camera profiles ---------------------------------------------------------
    ProfileLibrary library;
    for (const std::string& given : settings.profiles) {
        std::vector<std::string> skipped;
        const size_t before = library.size();
        try {
            library.add(fs::u8path(given), skipped);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "dngconv: --profile: %s\n", e.what());
            return 1;
        }
        for (const auto& why : skipped)
            std::fprintf(stderr, "dngconv: warning: profile left out: %s\n", why.c_str());
        std::error_code ec;
        if (library.size() == before && skipped.empty() && fs::is_directory(fs::u8path(given), ec))
            std::fprintf(stderr, "dngconv: warning: no camera profiles (.dcp) found in '%s'\n",
                         given.c_str());
    }
    if (!settings.profiles.empty() && library.empty()) {
        std::fprintf(stderr, "dngconv: --profile: no usable camera profile was given\n");
        return 1;
    }

    std::vector<Job> jobs;
    bool anyFolder = false;
    bool allFound = collectJobs(settings, rawExtensions(), jobs, anyFolder);
    if (jobs.empty()) {
        if (allFound) std::fprintf(stderr, "dngconv: no raw files found\n");
        return 1;
    }

    // ---- info mode ------------------------------------------------------------
    if (settings.info) {
        int failures = allFound ? 0 : 1;
        for (const Job& job : jobs) {
            try {
                if (lowerExtension(job.input) == ".dcp") {
                    printProfile(job.input, readCameraProfile(job.input));
                    continue;
                }
                RawReadOptions ro;
                ro.metadataOnly = true;
                ro.loadPreview = settings.preview == DngPreview::Auto || settings.preview == DngPreview::Camera;
                printInfo(job.input, readRaw(job.input, ro), settings, library);
            } catch (const std::exception& e) {
                if (isDngFile(job.input))
                    std::fprintf(stderr,
                                 "dngconv: %s: a DNG the decoder cannot read (it may hold JPEG XL data); "
                                 "'dngconv check' still tells whether it is intact\n",
                                 show(job.input).c_str());
                else
                    std::fprintf(stderr, "dngconv: %s: %s\n", show(job.input).c_str(), e.what());
                ++failures;
            }
        }
        return failures ? 1 : 0;
    }

    // ---- where the output goes ------------------------------------------------
    // -o names a file only when there is exactly one explicitly named input and
    // the path is not an existing folder; otherwise it is a folder.
    const fs::path outputRoot = settings.output.empty() ? fs::path() : fs::u8path(settings.output);
    const bool outputIsFile = outputIsSingleFile(settings, jobs, anyFolder);

    // Where each file goes. Two sources that would land on the same DNG
    // (same name in different folders, or IMG.CR2 next to IMG.NEF) must not
    // race each other: the first keeps the name, the other is refused.
    std::vector<fs::path> targets(jobs.size());
    std::vector<std::string> clash(jobs.size());
    {
        std::map<std::string, size_t> taken;
        for (size_t i = 0; i < jobs.size(); ++i) {
            fs::path& target = targets[i];
            if (outputIsFile) {
                target = outputRoot;
            } else if (!outputRoot.empty()) {
                target = outputRoot / jobs[i].relative;
                target.replace_extension(".dng");
            } else {
                target = jobs[i].input;
                target.replace_extension(".dng");
            }
            const auto [first, fresh] = taken.emplace(target.lexically_normal().u8string(), i);
            if (!fresh) clash[i] = show(jobs[first->second].input);
        }
    }

    const ThreadPlan plan = planThreads(settings, jobs.size());
    DngWriteOptions writeOptions;
    writeOptions.compression = settings.compression;
    writeOptions.jxlEffort = settings.jxlEffort;
    writeOptions.threads = plan.perFile;
    writeOptions.preview = settings.preview;
    writeOptions.previewSize = settings.previewSize;
    writeOptions.makerNotes = settings.makerNotes;
    writeOptions.lensCorrections = settings.lensCorrections;
    writeOptions.xmpSidecar = settings.sidecar;
    writeOptions.sourceRecord = settings.sourceRecord;
    writeOptions.software = std::string("dngconv ") + kVersion;

    enum class Outcome { Converted, UpToDate, NoRecord, Failed };
    std::vector<Outcome> outcomes(jobs.size(), Outcome::Failed);

    const auto convertOne = [&](size_t index, JobReport& report) {
        const Job& job = jobs[index];
        const fs::path& target = targets[index];
        const std::string name = show(job.input);
        try {
            if (!clash[index].empty())
                throw std::runtime_error("would be written to '" + show(target) + "', like '" +
                                         clash[index] + "'; give them separate output folders");

            // The time the DNG has to keep up with: the source's, or its
            // sidecar's if that was edited later.
            std::error_code ec;
            fs::file_time_type sourceTime = fs::last_write_time(job.input, ec);
            const bool haveSourceTime = !ec;
            if (settings.sidecar) {
                const fs::path sidecar = findXmpSidecar(job.input);
                const fs::file_time_type sidecarTime =
                    sidecar.empty() ? fs::file_time_type::min() : fs::last_write_time(sidecar, ec);
                if (!sidecar.empty() && !ec && haveSourceTime) sourceTime = std::max(sourceTime, sidecarTime);
            }

            std::string updateNote;  // why an existing DNG is being written again
            bool digestKnown = false;
            uint64_t knownSize = 0;
            Md5Digest knownDigest{};
            if (fs::exists(target, ec)) {
                if (fs::equivalent(target, job.input, ec))
                    throw std::runtime_error("output would overwrite the source file");
                if (settings.update && !settings.force && settings.updateByContent) {
                    // By content: the DNG says which file it was made from.
                    const SourceComparison compared = compareWithSource(target, job.input, settings.sidecar);
                    if (compared.state == SourceComparison::State::NoRecord) {
                        // Nothing to compare with. The DNG may be older than
                        // this feature, come from another program, or have
                        // been rewritten by one; none of that is a reason to
                        // write over it.
                        report.err += format("dngconv: %s: '%s' does not say which file it was made from, "
                                             "so the two cannot be compared; left as it is (--force converts "
                                             "it again)\n",
                                             name.c_str(), show(target).c_str());
                        outcomes[index] = Outcome::NoRecord;
                        return;
                    }
                    if (compared.state == SourceComparison::State::UpToDate) {
                        outcomes[index] = Outcome::UpToDate;
                        return;
                    }
                    updateNote = ", " + compared.what;
                    digestKnown = compared.digestKnown;
                    knownSize = compared.fileSize;
                    knownDigest = compared.fileDigest;
                } else if (settings.update && !settings.force) {
                    const fs::file_time_type targetTime = fs::last_write_time(target, ec);
                    if (!ec && haveSourceTime && targetTime >= sourceTime) {
                        outcomes[index] = Outcome::UpToDate;
                        return;
                    }
                } else if (!settings.force) {
                    throw std::runtime_error("'" + show(target) +
                                             "' already exists (use --force to overwrite, or "
                                             "--update to skip what is done)");
                }
            }

            if (lowerExtension(job.input) == ".dcp")
                throw std::runtime_error("a camera profile, not a picture; give it with --profile");

            const auto started = std::chrono::steady_clock::now();
            RawReadOptions ro;
            ro.loadPreview = settings.preview == DngPreview::Auto || settings.preview == DngPreview::Camera;
            ro.keepOriginalFile = settings.embedOriginal;
            ro.readSidecar = settings.sidecar;
            ro.digestSource = settings.sourceRecord && !digestKnown;
            RawReadResult source;
            try {
                source = readRaw(job.input, ro);
            } catch (const std::exception&) {
                // The decoder does not read every DNG (none with JPEG XL data).
                if (isDngFile(job.input)) throw std::runtime_error("already a DNG; skipped");
                throw;
            }
            if (source.sourceIsDng) throw std::runtime_error("already a DNG; skipped");
            if (digestKnown && settings.sourceRecord) {
                source.image.hasSourceDigest = true;
                source.image.sourceFileSize = knownSize;
                source.image.sourceFileDigest = knownDigest;
            }

            if (target.has_parent_path()) {
                fs::create_directories(target.parent_path(), ec);
                if (ec)
                    throw std::runtime_error("cannot create folder '" +
                                             show(target.parent_path()) + "': " + ec.message());
            }
            // The profiles made for this camera, out of those given.
            DngWriteOptions options = writeOptions;
            if (!library.empty() && source.image.colorPlanes >= 3) {
                options.profiles = library.select(source.image.uniqueCameraModel, source.image.colorPlanes,
                                                  settings.profileAnyCamera, source.warnings);
                if (options.profiles.empty())
                    source.warnings.push_back(
                        format("none of the %zu profile%s given is for '%s'; the DNG keeps the "
                               "decoder's colour matrix",
                               library.size(), library.size() == 1 ? "" : "s",
                               source.image.uniqueCameraModel.c_str()));
            }
            // JPEG XL takes one or three samples per pixel; the rare frame with
            // two or four keeps lossless JPEG.
            if (options.compression == DngCompression::JpegXl && source.image.samplesPerPixel != 1 &&
                source.image.samplesPerPixel != 3) {
                options.compression = DngCompression::LosslessJpeg;
                source.warnings.push_back(format("%u samples per pixel cannot be stored with JPEG XL; "
                                                 "lossless JPEG was used",
                                                 source.image.samplesPerPixel));
            }
            const bool jxl = options.compression == DngCompression::JpegXl;
            const PreviewPlan previewPlan = planPreview(source.image, options);
            std::string previewNote;
            if (previewPlan.kind == PreviewPlan::Kind::Rendered)
                previewNote = format(", preview rendered at %u x %u", previewPlan.width, previewPlan.height);
            std::string profileNote;
            if (!options.profiles.empty()) {
                profileNote = ", profile: " + options.profiles.front()->displayName();
                if (options.profiles.size() > 1)
                    profileNote += format(" + %zu more", options.profiles.size() - 1);
            }

            writeDng(source.image, target, options);
            if (settings.verify) verifyOutput(source.image, target, jxl, plan.perFile);
            const LensOpcodes lens = makeLensOpcodes(source.image, settings.lensCorrections);
            for (const auto& note : lens.notes) source.warnings.push_back(note);
            if (settings.keepTimes && haveSourceTime) {
                fs::last_write_time(target, sourceTime, ec);
                if (ec) source.warnings.push_back("could not set the time of '" + show(target) + "': " + ec.message());
            }

            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            for (const auto& w : source.warnings)
                report.err += format("dngconv: %s: warning: %s\n", name.c_str(), w.c_str());
            if (!settings.quiet) {
                const auto inSize = fs::file_size(job.input, ec);
                const auto outSize = fs::file_size(target, ec);
                const bool sidecar = settings.sidecar && !source.image.xmpSidecarName.empty();
                report.out = format("%s -> %s  (%s, %s -> %s, %.1f s%s%s%s%s%s%s%s%s%s)\n", name.c_str(),
                                    show(target).c_str(), source.image.uniqueCameraModel.c_str(),
                                    megabytes(inSize).c_str(), megabytes(outSize).c_str(), seconds,
                                    updateNote.c_str(), jxl ? ", JPEG XL" : "",
                                    lens.any() ? ", lens: " : "", lens.summary().c_str(),
                                    profileNote.c_str(), previewNote.c_str(),
                                    sidecar ? ", with " : "",
                                    sidecar ? source.image.xmpSidecarName.c_str() : "",
                                    settings.verify ? (settings.embedOriginal
                                                           ? ", pixels and embedded original verified"
                                                           : ", verified")
                                                    : "");
            }
            outcomes[index] = Outcome::Converted;
        } catch (const std::exception& e) {
            report.err += format("dngconv: %s: %s\n", name.c_str(), e.what());
            outcomes[index] = Outcome::Failed;
        }
    };

    runJobs(jobs.size(), plan.files, convertOne, [](size_t, const JobReport& report) { emitReport(report); });

    int converted = 0, upToDate = 0, noRecord = 0, failed = allFound ? 0 : 1;
    for (Outcome o : outcomes) {
        if (o == Outcome::Converted) ++converted;
        if (o == Outcome::UpToDate) ++upToDate;
        if (o == Outcome::NoRecord) ++noRecord;
        if (o == Outcome::Failed) ++failed;
    }
    if (!settings.quiet && (jobs.size() > 1 || upToDate || noRecord)) {
        std::printf("%d converted", converted);
        if (settings.update) std::printf(", %d already up to date", upToDate);
        if (noRecord) std::printf(", %d left as %s (no record of the source)", noRecord, noRecord == 1 ? "it is" : "they are");
        std::printf(", %d failed\n", failed);
    }
    return failed ? 1 : 0;
}
