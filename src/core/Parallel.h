#pragma once
// Minimal fork-join helper used by the image-processing loops.

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace hdr {

inline unsigned hardwareThreads() {
    const unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? n : 4;
}

// Runs fn(i) for every i in [begin, end) on all cores, handing out chunks of `grain` indices.
// The first exception thrown by fn is rethrown on the calling thread.
template <typename Fn>
void parallelFor(int begin, int end, int grain, Fn&& fn) {
    if (end <= begin) return;
    grain = std::max(grain, 1);
    const int chunks = (end - begin + grain - 1) / grain;
    const int numThreads = static_cast<int>(std::min<unsigned>(hardwareThreads(), static_cast<unsigned>(chunks)));
    if (numThreads <= 1) {
        for (int i = begin; i < end; ++i) fn(i);
        return;
    }

    std::atomic<int> next{0};
    std::exception_ptr error;
    std::mutex errorMutex;
    auto worker = [&] {
        try {
            for (int c = next++; c < chunks; c = next++) {
                const int lo = begin + c * grain;
                const int hi = std::min(end, lo + grain);
                for (int i = lo; i < hi; ++i) fn(i);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (!error) error = std::current_exception();
            next = chunks;  // stop handing out work
        }
    };

    {
        std::vector<std::jthread> threads;
        threads.reserve(static_cast<size_t>(numThreads - 1));
        for (int t = 1; t < numThreads; ++t) threads.emplace_back(worker);
        worker();
    }  // joins
    if (error) std::rethrow_exception(error);
}

}  // namespace hdr
