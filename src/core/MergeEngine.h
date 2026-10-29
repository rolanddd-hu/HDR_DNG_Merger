#pragma once
#include "Types.h"
#include "RawImage.h"
#include <array>
#include <vector>
#include <stop_token>

namespace hdr {

struct MergeResult {
    ImageF cfa;
    int width = 0;
    int height = 0;
    double baselineExposure = 0.0;
    std::vector<std::pair<int, int>> shifts;   // (dx, dy) per frame
    std::vector<double> exposures;             // relative exposures used (darkest = 1)
    std::array<int, 4> validRect{0, 0, 0, 0};  // x0, y0, x1, y1 covered by every aligned frame
    std::vector<uint8_t> motionMask;
    std::vector<uint8_t> sourceMap;
    Status status;
};

class MergeEngine {
public:
    // Merges a bracket. All frames must share size and CFA pattern. When `exposures` is empty
    // they are estimated after alignment (EXIF priors refined from the aligned pixel data).
    static MergeResult merge(
        const std::vector<const RawImage*>& frames,
        std::vector<double> exposures,
        const MergeSettings& settings,
        ProgressFn progress = nullptr,
        std::stop_token stopToken = {}
    );

    static MergeResult merge(
        const std::vector<RawImage>& frames,
        const std::vector<double>& exposures,
        const MergeSettings& settings,
        ProgressFn progress = nullptr,
        std::stop_token stopToken = {}
    );

private:
    static void boxBlur2D(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int r);
};

} // namespace hdr
