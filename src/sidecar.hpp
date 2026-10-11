// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// XMP sidecar files: the small files in which raw processors keep ratings,
// keywords and edits next to a raw file they must not change.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace dngconv {

/// Looks for the sidecar of a raw file: "IMG_0001.xmp" next to
/// "IMG_0001.CR3" (the convention of Adobe's programs and most others), or
/// "IMG_0001.CR3.xmp" (darktable's). Returns an empty path if there is none.
std::filesystem::path findXmpSidecar(const std::filesystem::path& rawFile);

/// Reads a sidecar. Returns an empty vector if the file cannot be read, is
/// larger than 16 MiB, or does not look like XMP.
std::vector<uint8_t> readXmpSidecar(const std::filesystem::path& sidecar);

}  // namespace dngconv
