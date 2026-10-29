#pragma once
#include "Types.h"
#include "BracketGrouper.h"
#include <cstdint>
#include <vector>
#include <stop_token>
#include <memory>

namespace hdr {

struct HistogramData {
    static constexpr int kNumBins = 256;
    std::vector<double> xValues; // 0 to 255
    std::vector<double> r;
    std::vector<double> g;
    std::vector<double> b;
    std::vector<double> luma;

    HistogramData();
    void clear();
};

class PreviewPipeline {
public:
    // Full-resolution merge of the group (same algorithm as the batch), returned as half-res
    // linear sRGB. Decoded frames are cached so setting tweaks and frame switches don't re-decode.
    static std::pair<Status, ImageRgbF> generateMergedPreview(
        const BracketGroup& group,
        const MergeSettings& settings,
        std::stop_token stopToken = {},
        std::vector<uint8_t>* outMotionMask = nullptr,
        std::vector<uint8_t>* outSourceMap = nullptr
    );

    static std::pair<Status, ImageRgbF> generateSingleFramePreview(
        const fs::path& framePath,
        std::stop_token stopToken = {}
    );

    // Histogram of an already tone-mapped RGBA8 image (e.g. read back from the display texture),
    // so it always matches what is on screen. Bins are normalised to [0, 1].
    static HistogramData computeHistogramRgba8(const uint8_t* rgba, int width, int height);

    // Drops all cached decoded frames.
    static void clearFrameCache();
    // Byte budget of the decoded frame cache (default 1 GiB).
    static void setFrameCacheBudget(uint64_t bytes);
};

} // namespace hdr
