// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// A note, kept in the DNG, of the file it was made from: the size and MD5 of
// the source file, and the MD5 of the XMP sidecar that was taken over with
// it. A later run compares the note with the files as they are then and
// knows, without relying on file times, whether the DNG is still current.
//
// DNG has no tag for this (OriginalRawFileDigest is the checksum of an
// embedded copy, not of the file), so the note lives in a private TIFF tag
// in the first directory, as a line of text that says what it is:
//
//   dngconv source record 1: size 17137664, MD5 df7a40d18a6764ab774d6efa7a926637,
//   sidecar MD5 f908070b5e6704092d2cc57879b9da55
//
// (one line; the sidecar part only if one was taken over). Anything in the
// tag that does not read exactly like this is taken for somebody else's use
// of the same number. The MD5 is the one `md5sum` prints for the file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "md5.hpp"

namespace dngconv {

/// From the range TIFF leaves to private use (65000 to 65535), away from the
/// numbers other programs are known to have taken.
constexpr uint16_t kSourceRecordTag = 65300;

struct SourceRecord {
    bool present = false;
    uint64_t fileSize = 0;
    Md5Digest fileDigest{};
    /// An XMP sidecar was stored in the DNG along with the picture.
    bool hasSidecar = false;
    Md5Digest sidecarDigest{};
};

/// The text of the tag (type ASCII), without the terminating zero.
std::string packSourceRecord(const SourceRecord& record);

/// present = false for anything that is not such a text, including one of
/// a later format version. A terminating zero may be included in `size`.
SourceRecord unpackSourceRecord(const char* text, size_t size);

/// The record a DNG carries. Never throws: a file that cannot be read, is
/// not a DNG or has no record gives present = false.
SourceRecord readSourceRecord(const std::filesystem::path& dng);

/// How a DNG stands to the file it is supposed to have been made from.
struct SourceComparison {
    enum class State {
        UpToDate,  // the DNG was made from the source, and sidecar, as they are now
        Changed,   // the source or the sidecar is not what the DNG was made from
        NoRecord,  // the DNG does not say what it was made from
    } state = State::NoRecord;
    /// For Changed, what it is that changed: "source changed", "sidecar
    /// added", "sidecar changed", "sidecar no longer there", or the first
    /// with one of the others after a comma.
    std::string what;
    /// The source file's size and MD5, when they had to be taken for the
    /// comparison (they are not if the size alone settles it).
    bool digestKnown = false;
    uint64_t fileSize = 0;
    Md5Digest fileDigest{};
};

/// Compares the record in `dng` with `source` as it is now and, when
/// `withSidecar` is set, with the XMP sidecar next to it (an existing
/// sidecar that conversion would not take over counts as none). Throws
/// std::runtime_error if the source cannot be read.
SourceComparison compareWithSource(const std::filesystem::path& dng, const std::filesystem::path& source,
                                   bool withSidecar);

/// MD5 of a file's contents, read piece by piece. Throws std::runtime_error
/// if the file cannot be read to its end.
Md5Digest digestOfFile(const std::filesystem::path& path, uint64_t& size);

}  // namespace dngconv
