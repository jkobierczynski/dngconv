// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// dngconv command-line front end.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

#include "dng_writer.hpp"
#include "original_raw.hpp"
#include "raw_reader.hpp"
#include "version.hpp"

namespace fs = std::filesystem;
using namespace dngconv;

namespace {

struct Settings {
    std::vector<std::string> inputs;
    std::string output;
    DngCompression compression = DngCompression::LosslessJpeg;
    bool preview = true;
    bool makerNotes = true;
    bool embedOriginal = false;
    LensCorrectionMode lensCorrections = LensCorrectionMode::Auto;
    bool recursive = false;
    bool force = false;
    bool verify = false;
    bool info = false;
    bool quiet = false;
    unsigned threads = 0;
};

struct Job {
    fs::path input;
    fs::path relative;   // path below the output directory, extension still the source's
    bool named = false;  // given as a file argument, not found by scanning a folder
};

const char* const kUsage =
    "Usage: dngconv [options] <file or folder>...\n"
    "       dngconv extract [options] <DNG file or folder>...\n"
    "\n"
    "Converts camera raw files to Digital Negative (DNG). The extract command\n"
    "restores the original files from DNGs written with --embed-original;\n"
    "see 'dngconv extract --help'.\n"
    "\n"
    "Options:\n"
    "  -o, --output <path>       output file (one input) or output folder\n"
    "                            (default: next to each source file)\n"
    "  -c, --compression <mode>  lossless (default) or none\n"
    "      --no-preview          do not copy the embedded JPEG preview\n"
    "      --no-maker-notes      do not copy the camera maker's private metadata\n"
    "  -e, --embed-original      store the source file inside the DNG, so that\n"
    "                            'dngconv extract' can restore it later\n"
    "      --lens-corrections <which>\n"
    "                            pass the camera's lens corrections on to the\n"
    "                            DNG reader: auto (default; those the camera was\n"
    "                            set to apply), all, or none\n"
    "  -r, --recursive           descend into sub-folders\n"
    "  -f, --force               overwrite existing DNG files\n"
    "      --verify              re-read each DNG and compare it with the source\n"
    "                            (pixels, and the embedded original if any)\n"
    "  -j, --jobs <n>            compression threads (default: all cores)\n"
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
    "  -q, --quiet               only report problems\n"
    "  -h, --help                show this help\n";

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
            } else {
                std::fprintf(stderr, "dngconv: unknown compression '%s' (use lossless or none)\n",
                             v->c_str());
                return false;
            }
        } else if (a == "-j" || a == "--jobs") {
            const std::string* v = needValue(i, a);
            if (!v) return false;
            char* end = nullptr;
            const long n = std::strtol(v->c_str(), &end, 10);
            if (!end || *end || n < 1 || n > 1024) {
                std::fprintf(stderr, "dngconv: invalid thread count '%s'\n", v->c_str());
                return false;
            }
            s.threads = static_cast<unsigned>(n);
        } else if (a == "--no-preview") {
            s.preview = false;
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

void printInfo(const fs::path& path, const RawReadResult& r, LensCorrectionMode lensMode) {
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
void verifyOutput(const RawImage& source, const fs::path& dng) {
    RawReadOptions ro;
    ro.loadPreview = false;
    ro.copyMetadata = false;
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

    int extracted = 0, skipped = 0, failed = allFound ? 0 : 1;
    for (const Job& job : jobs) {
        const std::string name = show(job.input);
        try {
            const EmbeddedOriginal original = readEmbeddedOriginal(job.input);
            if (!original.present) {
                // In a folder, DNGs without an original are expected; a file
                // asked for by name that has none is a failed request.
                if (job.named) throw std::runtime_error("holds no embedded original");
                ++skipped;
                continue;
            }
            if (original.hasDigest && !original.digestMatches)
                throw std::runtime_error("the embedded original is damaged (checksum mismatch); "
                                         "nothing was written");
            const std::vector<uint8_t> bytes =
                unpackOriginalRaw(original.packed.data(), original.packed.size());

            fs::path target;
            if (outputIsFile) {
                target = outputRoot;
            } else {
                // Without a recorded name, fall back to the DNG's own.
                const fs::path fileName = original.fileName.empty()
                                              ? fs::path(job.input.stem()) += ".original"
                                              : fs::u8path(original.fileName);
                const fs::path folder = outputRoot.empty() ? job.input.parent_path()
                                                           : outputRoot / job.relative.parent_path();
                target = folder / fileName;
            }

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
            if (!settings.quiet) {
                std::printf("%s -> %s  (%s%s)\n", name.c_str(), show(target).c_str(),
                            megabytes(bytes.size()).c_str(),
                            original.hasDigest ? ", checksum verified" : ", no checksum in file");
                std::fflush(stdout);
            }
            ++extracted;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "dngconv: %s: %s\n", name.c_str(), e.what());
            ++failed;
        }
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

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args = commandLine(argc, argv);
    if (!args.empty() && args.front() == "extract")
        return runExtract(std::vector<std::string>(args.begin() + 1, args.end()));
    if (!args.empty() && args.front() == "convert") args.erase(args.begin());

    Settings settings;
    int exitCode = 2;
    if (!parseArguments(args, settings, exitCode)) return exitCode;

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
                RawReadOptions ro;
                ro.metadataOnly = true;
                ro.loadPreview = false;
                printInfo(job.input, readRaw(job.input, ro), settings.lensCorrections);
            } catch (const std::exception& e) {
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

    DngWriteOptions writeOptions;
    writeOptions.compression = settings.compression;
    writeOptions.threads = settings.threads;
    writeOptions.embedPreview = settings.preview;
    writeOptions.makerNotes = settings.makerNotes;
    writeOptions.lensCorrections = settings.lensCorrections;
    writeOptions.software = std::string("dngconv ") + kVersion;

    int converted = 0, failed = allFound ? 0 : 1;
    for (const Job& job : jobs) {
        const std::string name = show(job.input);
        try {
            fs::path target;
            if (outputIsFile) {
                target = outputRoot;
            } else if (!outputRoot.empty()) {
                target = outputRoot / job.relative;
                target.replace_extension(".dng");
            } else {
                target = job.input;
                target.replace_extension(".dng");
            }

            std::error_code ec;
            if (fs::exists(target, ec)) {
                if (fs::equivalent(target, job.input, ec))
                    throw std::runtime_error("output would overwrite the source file");
                if (!settings.force)
                    throw std::runtime_error("'" + show(target) +
                                             "' already exists (use --force to overwrite)");
            }

            const auto started = std::chrono::steady_clock::now();
            RawReadOptions ro;
            ro.loadPreview = settings.preview;
            ro.keepOriginalFile = settings.embedOriginal;
            RawReadResult source = readRaw(job.input, ro);
            if (source.sourceIsDng) throw std::runtime_error("already a DNG; skipped");

            if (target.has_parent_path()) {
                fs::create_directories(target.parent_path(), ec);
                if (ec)
                    throw std::runtime_error("cannot create folder '" +
                                             show(target.parent_path()) + "': " + ec.message());
            }
            writeDng(source.image, target, writeOptions);
            if (settings.verify) verifyOutput(source.image, target);
            const LensOpcodes lens = makeLensOpcodes(source.image, settings.lensCorrections);
            for (const auto& note : lens.notes) source.warnings.push_back(note);

            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            for (const auto& w : source.warnings)
                std::fprintf(stderr, "dngconv: %s: warning: %s\n", name.c_str(), w.c_str());
            if (!settings.quiet) {
                const auto inSize = fs::file_size(job.input, ec);
                const auto outSize = fs::file_size(target, ec);
                std::printf("%s -> %s  (%s, %s -> %s, %.1f s%s%s%s)\n", name.c_str(),
                            show(target).c_str(), source.image.uniqueCameraModel.c_str(),
                            megabytes(inSize).c_str(), megabytes(outSize).c_str(), seconds,
                            lens.any() ? ", lens: " : "", lens.summary().c_str(),
                            settings.verify ? (settings.embedOriginal
                                                   ? ", pixels and embedded original verified"
                                                   : ", verified")
                                            : "");
                std::fflush(stdout);
            }
            ++converted;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "dngconv: %s: %s\n", name.c_str(), e.what());
            ++failed;
        }
    }

    if (!settings.quiet && jobs.size() > 1)
        std::printf("%d converted, %d failed\n", converted, failed);
    return failed ? 1 : 0;
}
