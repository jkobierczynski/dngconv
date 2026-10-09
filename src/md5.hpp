// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// MD5 message digest (RFC 1321). DNG uses it for its raw-data fingerprints;
// it is not used here for anything security related.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dngconv {

using Md5Digest = std::array<uint8_t, 16>;

class Md5 {
public:
    Md5();

    /// Feeds more data. May be called any number of times.
    void update(const void* data, size_t length);

    /// Completes the computation. The object must not be updated afterwards.
    Md5Digest finish();

    /// One-shot convenience.
    static Md5Digest of(const void* data, size_t length);

private:
    void transform(const uint8_t* block);

    uint32_t state_[4];
    uint64_t length_ = 0;  // bytes fed so far
    uint8_t buffer_[64];
};

}  // namespace dngconv
