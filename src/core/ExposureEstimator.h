#pragma once
#include "Types.h"
#include "RawImage.h"
#include "ImageAligner.h"
#include <vector>

namespace hdr {

class ExposureEstimator {
public:
    // Estimate relative exposures for a bracket of raw frames,
    // normalized so the darkest frame has relative exposure 1.0.
    // Returns relative exposures in the same order as input frames.
    // EXIF exposure ratios are refined with the median ratio of shared well-exposed green
    // pixels; `shifts` (from ImageAligner, may be empty) are applied when pairing pixels.
    static std::vector<double> estimateExposures(const std::vector<const RawImage*>& frames,
                                                 const std::vector<ImageShift>& shifts = {});

    static std::vector<double> estimateExposures(const std::vector<RawImage>& frames,
                                                 const std::vector<ImageShift>& shifts = {});
};

} // namespace hdr
