#pragma once
#include "Types.h"
#include "RawImage.h"
#include <filesystem>
#include <utility>

namespace hdr {

struct DngWriteParams {
    FloatBits bits = FloatBits::Auto;
    // Float output only: Deflate + floating-point predictor (lossless). Merges always compress,
    // because LibRaw-based converters (darktable, RawTherapee) cannot read uncompressed float DNGs.
    bool compress = true;
    bool embedPreview = true;   // false = small 256 px IFD0 thumbnail instead of a full preview
};

class DngWriter {
public:
    static FloatBits resolveFloatBits(FloatBits requested, float whiteLevel);

    // IFD0 preview dimensions for a CFA image of the given size (aspect ratio preserved).
    static std::pair<int, int> previewSize(int width, int height, bool embedPreview);

    // Writes the CFA data (normalised, 0..1 = black..white of the darkest frame) as a DNG.
    // ActiveArea is always the full buffer; DefaultCrop is taken from metaSource and clamped.
    static Status writeFloatDng(
        const fs::path& outputPath,
        const ImageF& cfaData,
        int width,
        int height,
        const RawImage& metaSource,
        const DngWriteParams& params = {}
    );
};

} // namespace hdr
