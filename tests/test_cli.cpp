// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Runs the dngconv program itself on DNG files made for the purpose: the
// check and extract commands, their exit codes, the order of their reports
// when several files are worked on at once, and the options around them.
// (Conversion needs camera files, which the tests do not have; the scripts
// under tools/ cover it.)
//
// usage: test_cli <path to dngconv> <scratch folder>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "dng_writer.hpp"
#include "test_util.hpp"
#include "tiff_builder.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace fs = std::filesystem;

namespace {

fs::path gProgram;
fs::path gDir;

std::string quoted(const fs::path& p) {
    fs::path native = p;
    native.make_preferred();
    return "\"" + native.string() + "\"";
}

struct Run {
    int status = -1;
    std::string out, err;
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    std::string s = os.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// Runs the program with the given arguments (already quoted where needed).
Run run(const std::string& arguments) {
    const fs::path outFile = gDir / "stdout.txt", errFile = gDir / "stderr.txt";
    std::string command = quoted(gProgram) + " " + arguments + " > " + quoted(outFile) + " 2> " + quoted(errFile);
#if defined(_WIN32)
    // cmd.exe strips one pair of quotes around the whole line.
    command = "\"" + command + "\"";
#endif
    Run r;
    const int raw = std::system(command.c_str());
#if defined(_WIN32)
    r.status = raw;
#else
    r.status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
    r.out = slurp(outFile);
    r.err = slurp(errFile);
    return r;
}

// File systems keep times with different precision; two seconds covers them all.
bool sameTime(fs::file_time_type a, fs::file_time_type b) {
    const auto diff = a > b ? a - b : b - a;
    return diff <= std::chrono::seconds(2);
}

bool contains(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

int lines(const std::string& text) { return static_cast<int>(std::count(text.begin(), text.end(), '\n')); }

RawImage image(uint32_t seed, const std::string& originalName) {
    RawImage img;
    img.width = 300;
    img.height = 200;
    img.samplesPerPixel = 1;
    img.isCfa = true;
    img.cfaRows = img.cfaCols = 2;
    img.cfaPattern = {0, 1, 1, 2};
    img.activeBottom = img.height;
    img.activeRight = img.width;
    img.cropWidth = img.width;
    img.cropHeight = img.height;
    img.blackLevel = {0};
    img.whiteLevel = {4095, 4095, 4095, 4095};
    img.hasColorMatrix = true;
    img.colorMatrix = {{{0.8293, -0.1789, -0.1094}, {-0.5025, 1.2925, 0.2327}, {-0.1199, 0.2769, 0.6108}, {0, 0, 0}}};
    img.uniqueCameraModel = "Testmake Model T";
    img.pixels.resize(img.sampleCount());
    for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<uint16_t>((i * 37 + seed * 101) & 0xfff);
    if (!originalName.empty()) {
        img.originalFileName = originalName;
        img.originalFile.resize(5000 + seed * 100);
        for (size_t i = 0; i < img.originalFile.size(); ++i) img.originalFile[i] = static_cast<uint8_t>(i * (seed + 3));
    }
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: test_cli <dngconv> <scratch folder>\n");
        return 2;
    }
    gProgram = argv[1];
    gDir = fs::path(argv[2]) / "cli_test";
    std::error_code ec;
    fs::remove_all(gDir, ec);
    fs::create_directories(gDir / "in" / "sub");
    fs::create_directories(gDir / "out");

    // Twelve DNGs with embedded originals, one without, one damaged.
    const int kFiles = 12;
    std::vector<RawImage> images;
    for (int i = 0; i < kFiles; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "shot%02d", i);
        images.push_back(image(static_cast<uint32_t>(i), std::string(name) + ".raw"));
        writeDng(images.back(), gDir / "in" / (std::string(name) + ".dng"));
    }
    writeDng(image(50, ""), gDir / "in" / "sub" / "plain.dng");
    {
        // A damaged copy: one byte of the embedded original changed.
        const fs::path damaged = gDir / "in" / "sub" / "damaged.dng";
        writeDng(image(51, "damaged.raw"), damaged);
        const std::string bytes = tiffreader::slurp(damaged);
        const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(bytes);
        std::fstream f(damaged, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(ifd0.at(50828).offset + 200));
        f.put(static_cast<char>(bytes[ifd0.at(50828).offset + 200] ^ 0x55));
    }

    const std::string in = quoted(gDir / "in"), out = quoted(gDir / "out");

    // ---- the basics ---------------------------------------------------------------------
    {
        Run r = run("--version");
        CHECK(r.status == 0 && contains(r.out, "dngconv "));
        r = run("check --help");
        CHECK(r.status == 0 && contains(r.out, "Usage: dngconv check"));
        r = run("check");
        CHECK(r.status == 2);
        r = run("check --no-such-option " + in);
        CHECK(r.status == 2);
        r = run("check -j 0 " + in);
        CHECK(r.status == 2);
        r = run("check " + quoted(gDir / "nothing-here.dng"));
        CHECK(r.status == 1);
    }

    // ---- check: verdicts, exit codes and order, however many files at once ----------------
    {
        std::string reference;
        for (const char* threads : {"-p 1", "-p 3 -j 6", "-p 12", ""}) {
            const Run r = run(std::string("check ") + threads + " " + in);
            CHECK(r.status == 0);
            CHECK(lines(r.out) == kFiles + 1);
            CHECK(contains(r.out, "12 checked: 12 intact, 0 damaged"));
            CHECK(contains(r.out, "shot00.dng: intact (raw image, embedded original)"));
            // The same report, line for line.
            if (reference.empty()) reference = r.out;
            CHECK(r.out == reference);
            // In the order of the names.
            size_t last = 0;
            for (int i = 0; i < kFiles; ++i) {
                char name[32];
                std::snprintf(name, sizeof name, "shot%02d.dng", i);
                const size_t at = r.out.find(name);
                CHECK(at != std::string::npos && at >= last);
                last = at;
            }
        }
        // With the sub-folder: one file without an original, one damaged.
        Run r = run("check -r " + in);
        CHECK(r.status == 1);
        CHECK(contains(r.out, "damaged.dng: DAMAGED:"));
        CHECK(contains(r.out, "plain.dng: intact (raw image)"));
        CHECK(contains(r.out, "14 checked: 13 intact, 1 damaged"));
        // Quiet: only what is not intact.
        r = run("check -q -r " + in);
        CHECK(r.status == 1 && lines(r.out) == 1 && contains(r.out, "DAMAGED"));
        r = run("check -q " + in);
        CHECK(r.status == 0 && r.out.empty() && r.err.empty());
    }

    // ---- extract: several at once, times kept, clashes refused ----------------------------
    {
        // Give the DNGs a time in the past.
        const fs::file_time_type past = fs::last_write_time(gDir / "in" / "shot00.dng") - std::chrono::hours(24 * 400);
        for (int i = 0; i < kFiles; ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "shot%02d.dng", i);
            fs::last_write_time(gDir / "in" / name, past);
        }
        Run r = run("extract -t -p 4 -o " + out + " " + in);
        CHECK(r.status == 0);
        CHECK(contains(r.out, "12 extracted, 0 failed"));
        for (int i = 0; i < kFiles; ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "shot%02d.raw", i);
            const fs::path restored = gDir / "out" / name;
            CHECK(fs::exists(restored));
            if (!fs::exists(restored)) continue;
            const std::string bytes = tiffreader::slurp(restored);
            const std::vector<uint8_t>& original = images[static_cast<size_t>(i)].originalFile;
            CHECK(bytes.size() == original.size() && std::equal(original.begin(), original.end(), bytes.begin(), [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); }));
            CHECK(sameTime(fs::last_write_time(restored), past));
        }
        // Again: nothing is overwritten without --force.
        r = run("extract -o " + out + " " + in);
        CHECK(r.status == 1 && contains(r.err, "already exists"));
        r = run("extract -f -p 2 -o " + out + " " + in);
        CHECK(r.status == 0);
        // Without -t the restored file is new.
        CHECK(!sameTime(fs::last_write_time(gDir / "out" / "shot03.raw"), past));

        // Recursive: the damaged one fails, the one without an original is counted.
        fs::remove_all(gDir / "out", ec);
        r = run("extract -r -o " + out + " " + in);
        CHECK(r.status == 1);
        CHECK(contains(r.err, "damaged.dng"));
        CHECK(contains(r.out, "12 extracted, 1 failed, 1 without an embedded original"));
        CHECK(!fs::exists(gDir / "out" / "sub" / "damaged.raw"));

        // Two DNGs holding a file of the same name, going to one folder: the
        // first is restored, the second refused, whatever the thread count.
        fs::create_directories(gDir / "twins");
        writeDng(image(1, "same.raw"), gDir / "twins" / "a.dng");
        writeDng(image(2, "same.raw"), gDir / "twins" / "b.dng");
        for (const char* threads : {"-p 1", "-p 2"}) {
            fs::remove(gDir / "twins" / "same.raw", ec);
            r = run(std::string("extract ") + threads + " " + quoted(gDir / "twins"));
            CHECK(r.status == 1);
            CHECK(contains(r.out, "a.dng ->") && contains(r.err, "b.dng") && contains(r.err, "separate folders"));
            CHECK(fs::file_size(gDir / "twins" / "same.raw") == 5000 + 100);
        }
    }

    // ---- preview options --------------------------------------------------------------------
    {
        const std::string missing = quoted(gDir / "no-such-file.raw");
        Run r = run("--preview sideways " + missing);
        CHECK(r.status == 2 && contains(r.err, "auto, camera, render or none"));
        r = run("--preview-size 10 " + missing);
        CHECK(r.status == 2 && contains(r.err, "64 to 65535"));
        r = run("--preview-size large " + missing);
        CHECK(r.status == 2);
        // Accepted: the run then fails on the file that is not there.
        for (const char* options : {"--preview render --preview-size max", "--preview camera", "--preview auto --preview-size 2048",
                                    "--preview none", "--no-preview"}) {
            r = run(std::string(options) + " " + missing);
            CHECK(r.status == 1 && contains(r.err, "no such file"));
        }
    }

    // ---- camera profiles: described with -i, refused when unusable --------------------------
    {
        namespace tb = tiffbuilder;
        tb::Bytes dcp = {'I', 'I', 0x52, 0x43, 8, 0, 0, 0};
        tb::placeDirectory(dcp, 8,
                           {tb::ascii(50708, "Testmake Model T"),
                            tb::rationals(50721, {0.9, -0.3, -0.1, -0.4, 1.2, 0.2, -0.05, 0.2, 0.7}),
                            tb::shorts(50778, {21}, false, 3), tb::ascii(50936, "Test Standard"),
                            tb::longs(50941, {1})});
        const fs::path profile = tb::writeFile(gDir, "standard.dcp", dcp);

        Run r = run("-i " + quoted(profile));
        CHECK(r.status == 0);
        CHECK(contains(r.out, "profile       Test Standard"));
        CHECK(contains(r.out, "for camera    Testmake Model T"));
        CHECK(contains(r.out, "one illuminant (D65)"));
        CHECK(contains(r.out, "embed if used"));

        // A profile is not a picture to convert.
        r = run(quoted(profile));
        CHECK(r.status == 1 && contains(r.err, "--profile"));
        CHECK(!fs::exists(gDir / "standard.dng"));

        // Profiles that cannot be used stop the run before anything is converted.
        r = run("--profile " + quoted(gDir / "missing.dcp") + " " + quoted(profile));
        CHECK(r.status == 1 && contains(r.err, "no such file"));
        r = run("--profile " + quoted(gDir / "in" / "shot00.dng") + " " + quoted(profile));
        CHECK(r.status == 1 && contains(r.err, "TIFF or DNG"));
        r = run("--profile " + quoted(gDir / "twins") + " " + quoted(profile));
        CHECK(r.status == 1 && contains(r.err, "no camera profiles"));
        r = run("--profile");
        CHECK(r.status == 2);
    }

    fs::remove_all(gDir, ec);
    return testutil::finish("test_cli");
}
