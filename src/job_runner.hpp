// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// Runs a list of independent jobs on a few threads and reports on them in the
// order of the list, whatever order they finish in: the output of a run with
// eight workers reads exactly like that of a run with one.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dngconv {

/// What a job has to say: text for standard output and for standard error.
struct JobReport {
    std::string out;
    std::string err;
};

/// Calls `work(index, report)` for every index below `count`, on up to
/// `workers` threads, and `emit(index, report)` on the calling thread, in
/// index order, as soon as a job and all those before it are done.
///
/// `work` must not throw: a job reports failure through its own results.
/// Should it throw all the same, the first exception is rethrown here once
/// the jobs already running have finished.
template <typename Work, typename Emit>
void runJobs(size_t count, unsigned workers, Work work, Emit emit) {
    if (workers > count) workers = static_cast<unsigned>(count);
    if (workers <= 1) {
        for (size_t i = 0; i < count; ++i) {
            JobReport report;
            work(i, report);
            emit(i, report);
        }
        return;
    }

    std::vector<JobReport> reports(count);
    std::vector<char> done(count, 0);
    std::mutex mutex;
    std::condition_variable finished;
    size_t next = 0;
    std::exception_ptr error;

    auto worker = [&] {
        for (;;) {
            size_t i;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (next >= count || error) return;
                i = next++;
            }
            std::exception_ptr failure;
            try {
                work(i, reports[i]);
            } catch (...) {
                failure = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (failure && !error) error = failure;
                done[i] = 1;
            }
            finished.notify_all();
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned t = 0; t < workers; ++t) pool.emplace_back(worker);

    // Hand the reports over in order. A job that was never started because
    // of an error elsewhere is not waited for.
    for (size_t i = 0; i < count; ++i) {
        std::unique_lock<std::mutex> lock(mutex);
        finished.wait(lock, [&] { return done[i] || (error && i >= next); });
        if (!done[i]) break;
        JobReport report = std::move(reports[i]);
        lock.unlock();
        emit(i, report);
    }
    for (std::thread& t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

}  // namespace dngconv
