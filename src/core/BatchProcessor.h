#pragma once
#include "Types.h"
#include "BracketGrouper.h"
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <stop_token>
#include <condition_variable>
#include <cstdint>

namespace hdr {

enum class JobStatus {
    Queued,
    Running,
    Done,
    Failed,
    Cancelled,
    Skipped
};

inline bool isFinished(JobStatus s) {
    return s != JobStatus::Queued && s != JobStatus::Running;
}

// One merge job. Status/progress are atomics; the text fields are written by the worker thread
// and read by the UI, so they are only reachable through the locked accessors.
class BatchJobItem {
public:
    int id = 0;
    BracketGroup group;
    fs::path outputPath;
    MergeSettings settings;

    std::atomic<JobStatus> status{JobStatus::Queued};
    std::atomic<float> progress{0.0f};
    std::atomic<double> elapsedSeconds{0.0};
    std::stop_source stopSource;

    std::string stage() const {
        std::lock_guard<std::mutex> lock(textMutex_);
        return stage_;
    }
    std::string errorMessage() const {
        std::lock_guard<std::mutex> lock(textMutex_);
        return error_;
    }
    void setStage(std::string s) {
        std::lock_guard<std::mutex> lock(textMutex_);
        stage_ = std::move(s);
    }
    void setErrorMessage(std::string s) {
        std::lock_guard<std::mutex> lock(textMutex_);
        error_ = std::move(s);
    }

private:
    mutable std::mutex textMutex_;
    std::string stage_;
    std::string error_;
};

// Runs merge jobs on a pool of worker threads. Jobs can be added while the pool runs; a memory
// budget keeps concurrently running jobs from exhausting RAM (at least one job always runs).
class BatchProcessor {
public:
    explicit BatchProcessor(size_t maxWorkers = 0);
    ~BatchProcessor();

    BatchProcessor(const BatchProcessor&) = delete;
    BatchProcessor& operator=(const BatchProcessor&) = delete;

    // 0 = auto (half the hardware threads, 1..8). Takes effect when workers are (re)started.
    void setMaxWorkers(size_t workers);
    size_t getMaxWorkers() const { return maxWorkers_; }

    // Bytes the running jobs may use together. 0 = auto (70% of available physical memory).
    void setMemoryBudget(uint64_t bytes) { memoryBudgetSetting_ = bytes; }

    std::shared_ptr<BatchJobItem> addJob(const BracketGroup& group, const fs::path& outputPath, const MergeSettings& settings);

    // Removes finished jobs from the list (queued and running jobs stay).
    void clearFinished();

    // Starts the worker threads if they are not running yet. Never blocks.
    void start();

    // Cancels queued and running jobs. Never blocks; running jobs stop at their next check.
    void cancelAll();

    // Blocks until no job is queued or running.
    void waitForIdle();

    bool isRunning() const;

    // Snapshot of all jobs (safe to iterate while workers update them).
    std::vector<std::shared_ptr<BatchJobItem>> getJobs() const;

    // Rough peak memory of one merge job, from frame sizes in the group metadata.
    static uint64_t estimateJobBytes(const BracketGroup& group);

private:
    void workerLoop(std::stop_token st);
    void runJob(BatchJobItem& job);

    size_t maxWorkers_ = 0;
    uint64_t memoryBudgetSetting_ = 0;
    uint64_t memoryBudget_ = 0;
    uint64_t memoryInUse_ = 0;
    int nextId_ = 1;

    std::vector<std::shared_ptr<BatchJobItem>> jobs_;
    std::deque<std::shared_ptr<BatchJobItem>> pending_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::vector<std::jthread> workers_;
};

} // namespace hdr
