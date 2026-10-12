// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Checks an existing DNG file against the checksums it carries: is the raw
// image data still what the writer stored, and is the embedded original
// file, if there is one, still intact? And, for files that carry none (most
// cameras write none), computes the checksums and writes them into the
// file, so that it can be checked from then on.
//
// The raw data is decoded by this program's own code (uncompressed and
// lossless JPEG, strips or tiles), not by the raw decoder used for
// conversion: the digests are defined over the samples as stored, before
// anything a decoder might do to them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dngconv {

struct DngCheck {
    enum class State {
        Absent,      // the file has no such part
        Intact,      // a checksum is there and matches
        Damaged,     // a checksum does not match, or the data cannot be read
        Unverified,  // nothing to compare with, or a form this program cannot decode
    };

    State rawImage = State::Absent;
    std::string rawDetail;  // which checksum matched, or what is wrong
    State original = State::Absent;
    std::string originalDetail;
    // Whether everything the file's directories point to (previews and
    // metadata as well) lies inside the file. No checksum covers those
    // parts, but a file that was cut short shows here.
    bool truncated = false;
    std::string truncatedDetail;

    // What was looked at.
    uint32_t width = 0, height = 0, planes = 0;
    unsigned bitsPerSample = 0;

    // A checksum is missing that addDngChecksums() could write: the data it
    // would cover was read without a problem.
    bool canAddChecksum = false;

    bool damaged() const {
        return truncated || rawImage == State::Damaged || original == State::Damaged;
    }
    /// Everything that is there was verified.
    bool intact() const {
        return !truncated && rawImage == State::Intact &&
               (original == State::Intact || original == State::Absent);
    }
};

/// The raw image of a DNG exactly as stored: before linearisation, black
/// subtraction or anything else a raw decoder does.
struct StoredRawImage {
    uint32_t width = 0, height = 0, planes = 0;
    unsigned bitsPerSample = 0;
    std::vector<uint16_t> samples;  // width * height * planes, interleaved, row-major
};

/// Throws std::runtime_error if the file is not a DNG, is damaged, or stores
/// its raw data in a form this program does not decode (lossy JPEG, JPEG XL,
/// Deflate, floating point).
StoredRawImage readStoredRawImage(const std::filesystem::path& path, unsigned threads = 0);

/// Throws std::runtime_error if the file cannot be opened or is not a DNG.
/// Damage inside a DNG is reported in the result, not thrown.
DngCheck checkDng(const std::filesystem::path& path, unsigned threads = 0);

/// What addDngChecksums() did.
struct DngChecksumsAdded {
    bool rawImage = false;  // a checksum of the raw image data was written,
    std::string rawTag;     // as "NewRawImageDigest" or "RawImageDigest"
    bool original = false;  // one of the embedded original file was written
    uint64_t bytesAdded = 0;
    DngCheck check;         // the file as it is afterwards
    bool any() const { return rawImage || original; }
};

/// Writes the checksums a DNG lacks into it: of the raw image data, if the
/// file has neither RawImageDigest nor NewRawImageDigest, and of the embedded
/// original, if that has none. The raw data gets the kind of checksum its
/// DNG version knows: NewRawImageDigest from 1.4 on, RawImageDigest before.
///
/// Nothing in the file moves (see dng_append.hpp); it grows by a copy of its
/// first directory. The file is read back afterwards, and put back as it was
/// if the new checksums do not verify.
///
/// Nothing is written to a file that is damaged or cut short, that already
/// has its checksums, or whose raw data this program cannot read; `check`
/// then holds what checkDng() would have said. Throws std::runtime_error if
/// the file is not a DNG or cannot be written to.
DngChecksumsAdded addDngChecksums(const std::filesystem::path& path, unsigned threads = 0);

namespace detail {
/// addDngChecksums() with the reading back done by `recheck` instead of
/// checkDng(), so that the tests can see what happens when it fails.
DngChecksumsAdded addDngChecksums(const std::filesystem::path& path, unsigned threads,
                                  const std::function<DngCheck(const std::filesystem::path&)>& recheck);
}  // namespace detail

/// Whether the file is a DNG at all (a TIFF with a DNGVersion tag). Never
/// throws; an unreadable file is not one.
bool isDngFile(const std::filesystem::path& path);

}  // namespace dngconv
