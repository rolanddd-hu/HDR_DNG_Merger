#pragma once
#include "Types.h"
#include "RawImage.h"
#include <filesystem>
#include <stop_token>
#include <utility>

namespace hdr {

class RawLoader {
public:
    // Fast metadata read (opens file, reads EXIF and sizes, does not unpack raw pixels)
    static std::pair<Status, ExposureInfo> loadMetadata(const fs::path& path);

    // Full load: extracts visible CFA Bayer buffer, levels, color matrices, DNG opcodes.
    // Decoding is aborted (status "Cancelled") when stopToken is triggered.
    static std::pair<Status, RawImage> loadFull(const fs::path& path, std::stop_token stopToken = {});

    // Half-res superpixel RGB load for quick preview
    static std::pair<Status, ImageRgbF> loadHalfRes(const fs::path& path, std::stop_token stopToken = {});

    // Half-res superpixel RGB of an already decoded frame
    static ImageRgbF halfResRgb(const RawImage& img);

    // Fast thumbnail load (extracts embedded JPEG preview or downsamples half-res)
    static std::pair<Status, ThumbnailImage> loadThumbnail(const fs::path& path, int maxDim = 160);

private:
    static void readDngTagsWithTiff(const fs::path& path, RawImage& img);
};

} // namespace hdr
