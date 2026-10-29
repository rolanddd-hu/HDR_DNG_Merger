#pragma once
#include "Types.h"
#include <filesystem>
#include <functional>

namespace hdr {

struct DemoGenResult {
    Status status;
    std::filesystem::path darkPath;
    std::filesystem::path midPath;
    std::filesystem::path brightPath;
    std::filesystem::path hdrPath;
    double evSpan = 6.0;
};

class DemoGenerator {
public:
    static DemoGenResult generateBracket(
        const std::filesystem::path& templatePath,
        const std::filesystem::path& outputDir,
        std::function<void(float, const std::string&)> progress = nullptr
    );
};

} // namespace hdr
