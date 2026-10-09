// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// The original camera file, stored inside the DNG.
//
// DNG's OriginalRawFileData tag holds a complete copy of the file a DNG was
// made from, so the conversion can be undone. The layout comes from the DNG
// specification: eight items in a row, of which only the first matters
// outside classic Mac OS (the others are a resource fork, file type and
// creator codes, and the same again for a sidecar file). The first item is
// the file's contents, cut into blocks of 64 KiB that are each compressed as
// an independent zlib stream, preceded by the uncompressed length and a table
// of block offsets. All integers are big-endian whatever the DNG's byte order.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dngconv {

/// Builds the contents of the OriginalRawFileData tag for a file's bytes.
/// Blocks are compressed in parallel; `threads` = 0 uses every core. Throws
/// std::runtime_error if the file is 4 GiB or larger, which the layout cannot
/// express.
std::vector<uint8_t> packOriginalRaw(const std::vector<uint8_t>& file, unsigned threads = 0);

/// Recovers the file's bytes from the tag contents. Throws std::runtime_error
/// describing the problem if the data is malformed or does not decompress to
/// the announced length.
std::vector<uint8_t> unpackOriginalRaw(const uint8_t* packed, size_t size);

struct EmbeddedOriginal {
    bool present = false;
    /// OriginalRawFileName, reduced to a bare file name. Empty if the DNG
    /// does not record one.
    std::string fileName;
    /// The OriginalRawFileData tag, still packed.
    std::vector<uint8_t> packed;
    /// Size of the original file, read from the packed data.
    uint64_t originalSize = 0;
    /// OriginalRawFileDigest was present, and whether it matched.
    bool hasDigest = false;
    bool digestMatches = false;
};

/// Looks for an embedded original in a DNG file. With `loadData` false only
/// the name and size are filled in. Returns present = false for a file without
/// one (or one that is not a TIFF at all); throws std::runtime_error only when
/// the file claims to hold an original that cannot be read.
EmbeddedOriginal readEmbeddedOriginal(const std::filesystem::path& dng, bool loadData = true);

/// Reads a whole file into memory. Throws std::runtime_error on failure.
std::vector<uint8_t> readWholeFile(const std::filesystem::path& path);

}  // namespace dngconv
