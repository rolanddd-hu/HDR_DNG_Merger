#pragma once
#include "Types.h"
#include "BracketGrouper.h"
#include "RawImage.h"
#include <filesystem>
#include <stop_token>
#include <utility>
#include <vector>

namespace hdr {

class MergeJob {
public:
    // Loads, merges and writes one bracket. The DNG is written to a temporary file next to
    // outputPath and only renamed into place once pixels and metadata are complete, so a failed
    // or cancelled job never leaves a partial output. Refuses to overwrite any input file.
    static Status execute(
        const BracketGroup& group,
        const fs::path& outputPath,
        const MergeSettings& settings,
        ProgressFn progress = nullptr,
        std::stop_token stopToken = {}
    );

    // Decodes the given files in parallel and checks they form a mergeable bracket
    // (same dimensions and CFA pattern).
    static std::pair<Status, std::vector<RawImage>> loadFrames(
        const std::vector<FileEntry>& files,
        ProgressFn progress = nullptr,
        std::stop_token stopToken = {}
    );

    // Error if outputPath is (or resolves to) one of the group's input files.
    static Status checkOutputPath(const fs::path& outputPath, const BracketGroup& group);

    // Expands the naming pattern; the result is always a single safe file name ending in .dng.
    static std::string formatOutputFilename(
        const std::string& pattern,
        const BracketGroup& group,
        int groupIndex = -1
    );

    // Output paths for a whole batch: formatOutputFilename per group, with "_2", "_3", ...
    // appended where two groups would otherwise get the same file name.
    static std::vector<fs::path> planOutputPaths(
        const std::vector<BracketGroup>& groups,
        const std::string& pattern,
        const fs::path& outputDir
    );

    // Replaces characters that are invalid in Windows file names, neutralises "." / ".." and
    // reserved device names. Never returns an empty string.
    static std::string sanitizeFilename(const std::string& name);
};

} // namespace hdr
