#pragma once
#include "Types.h"
#include <filesystem>
#include <stop_token>
#include <vector>
#include <string>

namespace hdr {

struct FileEntry {
    fs::path path;
    ExposureInfo info;
    Status status;
    bool enabled = true;
};

class FileScanner {
public:
    // Lower-case extensions including the dot (".dng", ".cr3", ...).
    static const std::vector<std::string>& supportedExtensions();
    static bool isSupportedExtension(const fs::path& path);

    // Scans a directory (non-recursive) or a single file. Unreadable directories yield no entries
    // instead of throwing. Metadata is read in parallel; a stop request returns early.
    static std::vector<FileEntry> scanPath(const fs::path& pathOrDir, std::stop_token stopToken = {});
    static std::vector<FileEntry> scanFiles(const std::vector<fs::path>& paths, std::stop_token stopToken = {});

    // Capture time, then file name. Files without a timestamp sort after those with one.
    static void sortEntries(std::vector<FileEntry>& entries);
};

} // namespace hdr
