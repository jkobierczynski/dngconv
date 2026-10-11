// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "sidecar.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace dngconv {

namespace fs = std::filesystem;

fs::path findXmpSidecar(const fs::path& rawFile) {
    // "name.xmp" first: it is what the programs that also read DNG write.
    fs::path stem = rawFile;
    stem.replace_extension();
    for (const fs::path& base : {stem, rawFile})
        for (const char* extension : {".xmp", ".XMP"}) {
            fs::path candidate = base;
            candidate += extension;
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec) && !fs::equivalent(candidate, rawFile, ec))
                return candidate;
        }
    return {};
}

std::vector<uint8_t> readXmpSidecar(const fs::path& sidecar) {
    constexpr std::uintmax_t kMaxSize = 16u << 20;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(sidecar, ec);
    if (ec || size == 0 || size > kMaxSize) return {};
    std::ifstream in(sidecar, std::ios::binary);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!in || !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) return {};

    // An XMP packet is XML holding an x:xmpmeta or at least an rdf:RDF
    // element. Anything else under this name is not ours to embed.
    const auto contains = [&](const char* what) {
        const size_t n = std::strlen(what);
        return std::search(data.begin(), data.end(), what, what + n) != data.end();
    };
    if (!contains("<x:xmpmeta") && !contains("<rdf:RDF")) return {};
    return data;
}

}  // namespace dngconv
