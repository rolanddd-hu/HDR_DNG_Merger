#pragma once
#include "Types.h"
#include <filesystem>
#include <optional>
#include <vector>
#include <string>

namespace hdr {

struct MetadataCopyOptions {
    bool copyExif = true;
    bool copyGps = true;
    bool copyXmp = true;
    bool copyIptc = true;
    std::vector<std::string> sourceFiles;
    // Used to write basic EXIF (exposure, aperture, ISO, date) when Exiv2 cannot read the
    // source raw format (e.g. Phase One IIQ, Hasselblad FFF).
    ExposureInfo fallbackExposure;
    // When set, written as Lightroom / ACR develop settings (XMP crs:*).
    std::optional<ToneParams> lightroomTone;
};

class MetadataCopier {
public:
    // Thread-safe one-time Exiv2/XMP toolkit initialisation; called implicitly by the functions
    // below, exposed so applications can do it at startup.
    static void initialize();

    static Status copyMetadata(
        const fs::path& sourcePath,
        const fs::path& destinationPath,
        const MetadataCopyOptions& options = {}
    );

    static Status updateExifExposure(
        const fs::path& filePath,
        uint32_t shutterNum,
        uint32_t shutterDen,
        const std::string& dateTimeStr = ""
    );

    // Whether an EXIF key from the source is copied into the merged DNG (exposed for tests).
    static bool shouldCopyExifKey(const std::string& key, bool sourceIsDng);
};

} // namespace hdr
