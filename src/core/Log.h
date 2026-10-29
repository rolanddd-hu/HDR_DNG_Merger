#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace hdr {

class RingBufferSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit RingBufferSink(size_t maxLines = 1000) : maxLines_(maxLines) {}

    std::vector<std::string> getLines() {
        std::lock_guard<std::mutex> lock(this->mutex_);
        return std::vector<std::string>(lines_.begin(), lines_.end());
    }

    void clear() {
        std::lock_guard<std::mutex> lock(this->mutex_);
        lines_.clear();
        version_++;
    }

    // Incremented on every change, so readers can skip copying unchanged contents.
    uint64_t version() const { return version_.load(); }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);
        std::string line(formatted.data(), formatted.size());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        lines_.push_back(std::move(line));
        while (lines_.size() > maxLines_) {
            lines_.pop_front();
        }
        version_++;
    }

    void flush_() override {}

private:
    size_t maxLines_;
    std::deque<std::string> lines_;
    std::atomic<uint64_t> version_{0};
};

class Logger {
public:
    // Console + in-memory ring buffer; when logDirectory is given also a rotating log file there.
    static void init(const std::filesystem::path& logDirectory = {});
    static std::shared_ptr<RingBufferSink> getRingBuffer();
};

} // namespace hdr
