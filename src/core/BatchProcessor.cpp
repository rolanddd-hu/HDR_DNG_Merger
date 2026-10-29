#include "BatchProcessor.h"
#include "MergeJob.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace hdr {

namespace {

uint64_t autoMemoryBudget() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<uint64_t>(static_cast<double>(status.ullAvailPhys) * 0.7);
    }
#endif
    return UINT64_MAX;
}

} // namespace

BatchProcessor::BatchProcessor(size_t maxWorkers) {
    setMaxWorkers(maxWorkers);
}

BatchProcessor::~BatchProcessor() {
    cancelAll();
    for (auto& w : workers_) w.request_stop();
    cv_.notify_all();
    workers_.clear();  // joins
}

void BatchProcessor::setMaxWorkers(size_t workers) {
    if (workers == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        workers = std::clamp(hw > 0 ? hw / 2 : 2u, 1u, 8u);
    }
    maxWorkers_ = std::min<size_t>(workers, 64);
}

uint64_t BatchProcessor::estimateJobBytes(const BracketGroup& group) {
    uint64_t pixels = 0;
    uint64_t frames = 0;
    for (const auto& f : group.files) {
        if (!f.enabled) continue;
        frames++;
        pixels = std::max<uint64_t>(pixels, static_cast<uint64_t>(std::max(f.info.width, 0)) *
                                                static_cast<uint64_t>(std::max(f.info.height, 0)));
    }
    if (pixels == 0) pixels = 24'000'000;  // unknown size: assume a 24 MP sensor
    // Per pixel: uint16 frames + LibRaw decode buffers (~4 B/frame while loading), weights
    // (~1.5 B/frame), float output (4 B) and the output strip (2 B).
    return pixels * (frames * 6 + 8);
}

std::shared_ptr<BatchJobItem> BatchProcessor::addJob(const BracketGroup& group, const fs::path& outputPath, const MergeSettings& settings) {
    auto item = std::make_shared<BatchJobItem>();
    item->group = group;
    item->outputPath = outputPath;
    item->settings = settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item->id = nextId_++;
        jobs_.push_back(item);
        pending_.push_back(item);
    }
    cv_.notify_all();
    return item;
}

void BatchProcessor::clearFinished() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(jobs_, [](const auto& j) { return isFinished(j->status.load()); });
}

void BatchProcessor::start() {
    // Idle workers from an earlier run are replaced when the worker count changed.
    std::vector<std::jthread> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        memoryBudget_ = memoryBudgetSetting_ ? memoryBudgetSetting_ : autoMemoryBudget();
        const bool busy = std::any_of(jobs_.begin(), jobs_.end(),
                                      [](const auto& j) { return j->status.load() == JobStatus::Running; });
        if (!workers_.empty() && workers_.size() != maxWorkers_ && !busy) {
            retired = std::move(workers_);
            workers_.clear();
        }
    }
    for (auto& w : retired) w.request_stop();
    cv_.notify_all();
    retired.clear();  // joins outside the lock

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (workers_.empty()) {
            for (size_t i = 0; i < maxWorkers_; ++i) {
                workers_.emplace_back([this](std::stop_token st) { workerLoop(st); });
            }
        }
    }
    cv_.notify_all();
}

void BatchProcessor::cancelAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& j : jobs_) {
            j->stopSource.request_stop();
            if (j->status.load() == JobStatus::Queued) {
                j->status.store(JobStatus::Cancelled);
            }
        }
        pending_.clear();
    }
    cv_.notify_all();
}

void BatchProcessor::waitForIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] {
        return std::none_of(jobs_.begin(), jobs_.end(), [](const auto& j) { return !isFinished(j->status.load()); });
    });
}

bool BatchProcessor::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& j) { return !isFinished(j->status.load()); });
}

std::vector<std::shared_ptr<BatchJobItem>> BatchProcessor::getJobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_;
}

void BatchProcessor::workerLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        std::shared_ptr<BatchJobItem> job;
        uint64_t reserved = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait(lock, st, [&] { return !pending_.empty(); })) return;  // stop requested
            job = std::move(pending_.front());
            pending_.pop_front();

            // Wait for enough memory, unless nothing else is running.
            reserved = estimateJobBytes(job->group);
            cv_.wait(lock, st, [&] {
                return memoryInUse_ == 0 || memoryInUse_ + reserved <= memoryBudget_ ||
                       job->status.load() != JobStatus::Queued;
            });
            if (job->status.load() != JobStatus::Queued) {
                cv_.notify_all();  // cancelled while waiting for memory
                continue;
            }
            if (st.stop_requested()) {
                pending_.push_front(std::move(job));  // worker retired: hand the job back
                return;
            }
            job->status.store(JobStatus::Running);
            memoryInUse_ += reserved;
        }

        runJob(*job);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            memoryInUse_ -= reserved;
        }
        cv_.notify_all();
    }
}

void BatchProcessor::runJob(BatchJobItem& job) {
    const auto startTime = std::chrono::steady_clock::now();
    const std::stop_token jobStop = job.stopSource.get_token();

    if (jobStop.stop_requested()) {
        job.status.store(JobStatus::Cancelled);
        return;
    }

    std::error_code ec;
    if (job.settings.skipExisting && fs::exists(job.outputPath, ec)) {
        job.progress.store(1.0f);
        job.status.store(JobStatus::Skipped);
        return;
    }

    auto progressCb = [&job](float p, const std::string& stage) {
        job.progress.store(p);
        job.setStage(stage);
    };

    Status status;
    try {
        status = MergeJob::execute(job.group, job.outputPath, job.settings, progressCb, jobStop);
    } catch (const std::exception& e) {
        status = Status::error(std::string("Unexpected error: ") + e.what());
    }

    job.elapsedSeconds.store(std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count());

    if (jobStop.stop_requested()) {
        job.status.store(JobStatus::Cancelled);
    } else if (status) {
        job.progress.store(1.0f);
        job.status.store(JobStatus::Done);
        spdlog::info("Merged {} -> {} ({:.1f} s)", job.group.name, job.outputPath.filename().string(), job.elapsedSeconds.load());
    } else {
        job.setErrorMessage(status.message);
        job.status.store(JobStatus::Failed);
        spdlog::error("Merge of {} failed: {}", job.group.name, status.message);
    }
}

} // namespace hdr
