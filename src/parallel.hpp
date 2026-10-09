// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// A minimal parallel loop for independent jobs (tiles, blocks).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace dngconv {

inline unsigned workerCount(unsigned requested, size_t jobs) {
    unsigned n = requested ? requested : std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    return static_cast<unsigned>(std::min<size_t>(n, std::max<size_t>(jobs, 1)));
}

// Runs fn(i) for i in [0, jobs) on a few threads; rethrows the first error.
template <typename Fn>
void parallelFor(size_t jobs, unsigned threads, Fn fn) {
    const unsigned n = workerCount(threads, jobs);
    if (n <= 1) {
        for (size_t i = 0; i < jobs; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::mutex errorMutex;
    auto worker = [&] {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= jobs) return;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (!error) error = std::current_exception();
                next.store(jobs);
                return;
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned t = 0; t < n; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

}  // namespace dngconv
