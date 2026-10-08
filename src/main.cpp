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
    bool recursive = false;
    bool force = false;
    bool verify = false;
    bool info = false;
    bool quiet = false;
    unsigned threads = 0;
};

struct Job {
    fs::path input;
    fs::path relative;  // path below the output directory, extension still the source's
};

const char* const kUsage =
    "Usage: dngconv [options] <file or folder>...\n"
    "\n"
    "Converts camera raw files to Digital Negative (DNG).\n"
    "\n"
    "Options:\n"
    "  -o, --output <path>       output file (one input) or output folder\n"
    "                            (default: next to each source file)\n"
    "  -c, --compression <mode>  lossless (default) or none\n"
    "      --no-preview          do not copy the embedded JPEG preview\n"
    "      --no-maker-notes      do not copy the camera maker's private metadata\n"
    "  -r, --recursive           descend into sub-folders\n"
    "  -f, --force               overwrite existing DNG files\n"
    "      --verify              re-read each DNG and compare it with the source\n"
    "  -j, --jobs <n>            compression threads (default: all cores)\n"
    "  -i, --info                show what the decoder finds; write nothing\n"
    "  -q, --quiet               only report problems\n"
    "  -V, --version             show version information\n"
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
void scanFolder(const fs::path& root, std::vector<Job>& jobs) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (Iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && rawExtensions().count(lowerExtension(it->path())))
            found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    for (const auto& f : found) jobs.push_back({f, fs::relative(f, root, ec)});
}

// Returns false if any input does not exist.
bool collectJobs(const Settings& s, std::vector<Job>& jobs, bool& anyFolder) {
    bool ok = true;
    anyFolder = false;
    for (const std::string& arg : s.inputs) {
        const fs::path p = fs::u8path(arg);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            anyFolder = true;
            if (s.recursive)
                scanFolder<fs::recursive_directory_iterator>(p, jobs);
            else
                scanFolder<fs::directory_iterator>(p, jobs);
        } else if (fs::exists(p, ec)) {
            jobs.push_back({p, p.filename()});
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

void printInfo(const fs::path& path, const RawReadResult& r) {
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
    if (r.sourceIsDng) std::printf("  note          this file already is a DNG\n");
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
}

}  // namespace

int main(int argc, char** argv) {
    Settings settings;
    int exitCode = 2;
    if (!parseArguments(commandLine(argc, argv), settings, exitCode)) return exitCode;

    std::vector<Job> jobs;
    bool anyFolder = false;
    bool allFound = collectJobs(settings, jobs, anyFolder);
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
                printInfo(job.input, readRaw(job.input, ro));
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
    fs::path outputRoot;
    bool outputIsFile = false;
    if (!settings.output.empty()) {
        outputRoot = fs::u8path(settings.output);
        std::error_code ec;
        const bool endsWithSeparator =
            settings.output.back() == '/' || settings.output.back() == '\\';
        outputIsFile = jobs.size() == 1 && !anyFolder && !fs::is_directory(outputRoot, ec) &&
                       !endsWithSeparator;
    }

    DngWriteOptions writeOptions;
    writeOptions.compression = settings.compression;
    writeOptions.threads = settings.threads;
    writeOptions.embedPreview = settings.preview;
    writeOptions.makerNotes = settings.makerNotes;
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

            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            for (const auto& w : source.warnings)
                std::fprintf(stderr, "dngconv: %s: warning: %s\n", name.c_str(), w.c_str());
            if (!settings.quiet) {
                const auto inSize = fs::file_size(job.input, ec);
                const auto outSize = fs::file_size(target, ec);
                std::printf("%s -> %s  (%s, %s -> %s, %.1f s%s)\n", name.c_str(),
                            show(target).c_str(), source.image.uniqueCameraModel.c_str(),
                            megabytes(inSize).c_str(), megabytes(outSize).c_str(), seconds,
                            settings.verify ? ", verified" : "");
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
