// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// The pieces behind batch conversion: the job runner that keeps reports in
// order, XMP sidecar files, and the sidecar's way into a DNG.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dng_check.hpp"
#include "dng_writer.hpp"
#include "job_runner.hpp"
#include "sidecar.hpp"
#include "test_util.hpp"
#include "tiff_reader.hpp"

using namespace dngconv;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

const char* const kXmp =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Rating=\"4\"/>"
    "</rdf:RDF></x:xmpmeta>";

RawImage smallImage() {
    RawImage img;
    img.width = 64;
    img.height = 48;
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
    for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = static_cast<uint16_t>((i * 37) & 0xfff);
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = (argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path()) / "batch_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // ---- the job runner ---------------------------------------------------------------
    for (unsigned workers : {1u, 2u, 3u, 8u, 100u}) {
        const size_t count = 40;
        std::vector<int> ran(count, 0);
        std::vector<size_t> emitted;
        std::atomic<int> active{0}, peak{0};
        runJobs(
            count, workers,
            [&](size_t i, JobReport& report) {
                const int now = ++active;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                }
                // Later jobs finish first: the order of the reports must not follow.
                std::this_thread::sleep_for(std::chrono::microseconds(200 * (count - i)));
                ++ran[i];
                report.out = "job " + std::to_string(i);
                --active;
            },
            [&](size_t i, const JobReport& report) {
                CHECK(report.out == "job " + std::to_string(i));
                emitted.push_back(i);
            });
        CHECK(emitted.size() == count);
        for (size_t i = 0; i < emitted.size(); ++i) CHECK(emitted[i] == i);
        for (int r : ran) CHECK(r == 1);
        CHECK(peak.load() <= static_cast<int>(std::min<size_t>(workers, count)));
        if (workers > 1) CHECK(peak.load() > 1);
    }
    {
        // Nothing to do.
        int calls = 0;
        runJobs(0, 4, [&](size_t, JobReport&) { ++calls; }, [&](size_t, const JobReport&) { ++calls; });
        CHECK(calls == 0);
        // A job that throws: the error comes out, and nothing hangs.
        for (unsigned workers : {1u, 4u}) {
            bool threw = false;
            std::atomic<int> done{0};
            try {
                runJobs(
                    30, workers,
                    [&](size_t i, JobReport&) {
                        if (i == 7) throw std::runtime_error("job seven");
                        ++done;
                    },
                    [](size_t, const JobReport&) {});
            } catch (const std::runtime_error& e) {
                threw = std::string(e.what()) == "job seven";
            }
            CHECK(threw);
            CHECK(done.load() < 30);
        }
    }

    // ---- finding and reading sidecars ---------------------------------------------------
    {
        const fs::path raw = dir / "IMG_0001.CR3";
        writeText(raw, "raw");
        CHECK(findXmpSidecar(raw).empty());
        // darktable's naming.
        writeText(dir / "IMG_0001.CR3.xmp", kXmp);
        CHECK(findXmpSidecar(raw).filename() == "IMG_0001.CR3.xmp");
        // The plain name is preferred when both exist.
        writeText(dir / "IMG_0001.xmp", kXmp);
        CHECK(findXmpSidecar(raw).filename().u8string().size() == std::string("IMG_0001.xmp").size());
        // Another file's sidecar is not ours.
        writeText(dir / "IMG_0002.NEF", "raw");
        CHECK(findXmpSidecar(dir / "IMG_0002.NEF").empty());
        // A folder of that name is not a sidecar.
        fs::create_directories(dir / "IMG_0003.xmp");
        writeText(dir / "IMG_0003.ARW", "raw");
        CHECK(findXmpSidecar(dir / "IMG_0003.ARW").empty());
        // A file without an extension.
        writeText(dir / "plain", "raw");
        writeText(dir / "plain.xmp", kXmp);
        CHECK(findXmpSidecar(dir / "plain").filename() == "plain.xmp");

        const Bytes xmp = readXmpSidecar(dir / "IMG_0001.xmp");
        CHECK(std::string(xmp.begin(), xmp.end()) == kXmp);
        // Things that are called .xmp and are not XMP.
        writeText(dir / "empty.xmp", "");
        writeText(dir / "text.xmp", "rating: 4\n");
        CHECK(readXmpSidecar(dir / "empty.xmp").empty());
        CHECK(readXmpSidecar(dir / "text.xmp").empty());
        CHECK(readXmpSidecar(dir / "missing.xmp").empty());
        CHECK(readXmpSidecar(dir / "IMG_0003.xmp").empty());
        // The bare RDF form, with an XML declaration in front.
        writeText(dir / "rdf.xmp", "<?xml version=\"1.0\"?>\n<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"/>");
        CHECK(!readXmpSidecar(dir / "rdf.xmp").empty());
    }

    // ---- the sidecar in the DNG -----------------------------------------------------------
    {
        const fs::path file = dir / "sidecar.dng";
        RawImage img = smallImage();
        const std::string inCamera = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">from the camera</x:xmpmeta>";
        // XMP the camera file itself held, as the metadata reader delivers it.
        TiffField cameraXmp;
        cameraXmp.tag = 700;
        cameraXmp.type = 1;
        cameraXmp.count = static_cast<uint32_t>(inCamera.size());
        cameraXmp.data.assign(inCamera.begin(), inCamera.end());
        img.source.ifd0.push_back(cameraXmp);

        const auto xmpOf = [&](const DngWriteOptions& options) {
            writeDng(img, file, options);
            const tiffreader::Ifd ifd0 = tiffreader::parseFirstIfd(tiffreader::slurp(file));
            if (!ifd0.count(700)) return std::string();
            CHECK(ifd0.at(700).type == 1);
            return std::string(ifd0.at(700).data.begin(), ifd0.at(700).data.end());
        };
        // Without a sidecar the camera's XMP is carried over.
        CHECK(xmpOf(DngWriteOptions{}) == inCamera);
        // A sidecar replaces it ...
        img.xmpSidecar.assign(kXmp, kXmp + std::string(kXmp).size());
        img.xmpSidecarName = "IMG.xmp";
        CHECK(xmpOf(DngWriteOptions{}) == kXmp);
        CHECK(checkDng(file).intact());
        // ... unless it is not wanted.
        DngWriteOptions without;
        without.xmpSidecar = false;
        CHECK(xmpOf(without) == inCamera);
        // No XMP anywhere: no tag.
        img.source.ifd0.clear();
        CHECK(xmpOf(without).empty());
        // An odd number of bytes, in a big-endian file.
        img.xmpSidecar.push_back(' ');
        DngWriteOptions big;
        big.byteOrder = DngByteOrder::Big;
        CHECK(xmpOf(big) == std::string(kXmp) + " ");
    }

    fs::remove_all(dir, ec);
    return testutil::finish("test_batch");
}
