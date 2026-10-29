#pragma once
#include "Types.h"
#include "RawImage.h"
#include <vector>

namespace hdr {

struct ImageShift {
    int dx = 0; // Horizontal shift in raw pixels (even integer)
    int dy = 0; // Vertical shift in raw pixels (even integer)
};

class ImageAligner {
public:
    // Computes translation shift (dx, dy) of target frame relative to reference frame
    // using Greg Ward's Multi-Scale Median Threshold Bitmap (MTB) algorithm on log luminance.
    // Shifts are whole 2x2 quads (even pixel counts) so the Bayer mosaic stays aligned;
    // there is no sub-pixel refinement.
    static ImageShift alignBayer(
        const RawImage& target,
        const RawImage& reference,
        int maxShift = 128
    );

    // Aligns all frames in bracket to the designated reference frame (frames in parallel).
    static std::vector<ImageShift> alignBracket(
        const std::vector<const RawImage*>& frames,
        int referenceIndex,
        int maxShift = 128,
        ProgressFn progress = nullptr
    );

    static std::vector<ImageShift> alignBracket(
        const std::vector<RawImage>& frames,
        int referenceIndex,
        int maxShift = 128,
        ProgressFn progress = nullptr
    );
};

} // namespace hdr
