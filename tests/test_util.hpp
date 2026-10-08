// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Minimal test helpers: no framework, just counted checks.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace testutil {

inline int& failureCount() {
    static int n = 0;
    return n;
}

inline void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAILED %s:%d: %s\n", file, line, expr);
        ++failureCount();
    }
}

// Small deterministic generator, identical on every platform.
struct Random {
    uint64_t state;
    explicit Random(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32_t>(state >> 16);
    }
    uint32_t below(uint32_t n) { return next() % n; }
};

inline int finish(const char* name) {
    if (failureCount()) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", name, failureCount());
        return 1;
    }
    std::printf("%s: all checks passed\n", name);
    return 0;
}

}  // namespace testutil

#define CHECK(expr) ::testutil::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) \
    ::testutil::check(std::fabs((a) - (b)) <= (tol), #a " ~= " #b, __FILE__, __LINE__)
