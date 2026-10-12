// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Adds fields to the first directory of an existing TIFF file (a DNG)
// without moving anything that is in it.
//
// A TIFF header points to its first directory, and everything else is found
// from there by absolute offsets. So a field can be added by writing a new
// copy of the directory, with the field in it, at the end of the file and
// then changing the four bytes in the header that say where the directory
// is. Nothing else is touched: the picture, the previews and the maker note
// stay byte for byte where they were, which matters for maker notes whose
// internal pointers only hold at their original position. The old directory
// is left behind as a few hundred unused bytes.
//
// The order of writing makes this safe against a crash or a power cut: the
// new directory is written and forced to disk first, and only then does the
// header change. Until that moment the file is the old one with some bytes
// after its end that nothing refers to.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "raw_image.hpp"  // TiffField

namespace dngconv {

/// What appendToFirstIfd() changed, enough to take it back.
struct AppendReceipt {
    uint64_t oldSize = 0;      // length of the file before
    uint32_t oldFirstIfd = 0;  // where its header pointed
    bool bigEndian = false;
    uint64_t newSize = 0;
};

/// Adds `fields` to the first directory of the file. Only fields of the
/// byte-sized types (BYTE, ASCII, UNDEFINED) are taken, and none whose tag
/// the directory already has. Throws std::invalid_argument for those, and
/// std::runtime_error if the file is not a TIFF, cannot be written to, or
/// would grow beyond the 4 GiB that TIFF offsets reach.
AppendReceipt appendToFirstIfd(const std::filesystem::path& path, const std::vector<TiffField>& fields);

/// Puts the file back as it was before appendToFirstIfd().
void undoAppend(const std::filesystem::path& path, const AppendReceipt& receipt);

}  // namespace dngconv
