#include "Log.h"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace hdr {

static std::shared_ptr<RingBufferSink> g_ringBuffer;

void Logger::init(const std::filesystem::path& logDirectory) {
    if (g_ringBuffer) return;
    g_ringBuffer = std::make_shared<RingBufferSink>(1000);
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(spdlog::level::info);

    std::vector<spdlog::sink_ptr> sinks{consoleSink, g_ringBuffer};
    if (!logDirectory.empty()) {
        try {
            std::filesystem::create_directories(logDirectory);
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (logDirectory / "hdrmerge.log").string(), 5 * 1024 * 1024, 3));
        } catch (const std::exception&) {
            // Logging to file is best effort.
        }
    }

    auto logger = std::make_shared<spdlog::logger>("hdrmerge", sinks.begin(), sinks.end());
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

std::shared_ptr<RingBufferSink> Logger::getRingBuffer() {
    if (!g_ringBuffer) {
        init();
    }
    return g_ringBuffer;
}

} // namespace hdr
