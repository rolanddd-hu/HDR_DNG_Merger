#include "FileScanner.h"
#include "Parallel.h"
#include "RawLoader.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <tuple>

namespace hdr {

const std::vector<std::string>& FileScanner::supportedExtensions() {
    static const std::vector<std::string> supported = {
        ".dng",
        ".cr2", ".cr3",                 // Canon
        ".nef", ".nrw",                 // Nikon
        ".arw", ".srf", ".sr2",         // Sony
        ".raf",                         // Fujifilm (Bayer models / GFX only; X-Trans is flagged)
        ".orf",                         // Olympus / OM System
        ".rw2",                         // Panasonic
        ".pef",                         // Pentax
        ".3fr", ".fff",                 // Hasselblad
        ".iiq",                         // Phase One
        ".raw"
    };
    return supported;
}

bool FileScanner::isSupportedExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto& supported = supportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

std::vector<FileEntry> FileScanner::scanPath(const fs::path& pathOrDir, std::stop_token stopToken) {
    std::vector<fs::path> paths;
    std::error_code ec;
    if (fs::is_directory(pathOrDir, ec)) {
        fs::directory_iterator it(pathOrDir, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            spdlog::warn("Cannot read folder {}: {}", pathOrDir.filename().string(), ec.message());
            return {};
        }
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                spdlog::warn("Error while listing {}: {}", pathOrDir.filename().string(), ec.message());
                break;
            }
            std::error_code fileEc;
            if (it->is_regular_file(fileEc) && isSupportedExtension(it->path())) {
                paths.push_back(it->path());
            }
        }
    } else if (fs::is_regular_file(pathOrDir, ec) && isSupportedExtension(pathOrDir)) {
        paths.push_back(pathOrDir);
    }
    return scanFiles(paths, stopToken);
}

std::vector<FileEntry> FileScanner::scanFiles(const std::vector<fs::path>& paths, std::stop_token stopToken) {
    std::vector<FileEntry> entries(paths.size());
    std::vector<char> done(paths.size(), 0);

    parallelFor(0, static_cast<int>(paths.size()), 1, [&](int i) {
        if (stopToken.stop_requested()) return;
        FileEntry& entry = entries[i];
        entry.path = paths[i];
        auto [status, info] = RawLoader::loadMetadata(paths[i]);
        entry.status = status;
        entry.info = info;
        done[i] = 1;
    });

    if (stopToken.stop_requested()) {
        std::vector<FileEntry> partial;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (done[i]) partial.push_back(std::move(entries[i]));
        }
        entries = std::move(partial);
    }

    sortEntries(entries);
    return entries;
}

void FileScanner::sortEntries(std::vector<FileEntry>& entries) {
    // A single lexicographic key keeps the comparator a strict weak ordering.
    auto key = [](const FileEntry& e) {
        const double ts = e.info.timestamp > 0.0 ? e.info.timestamp : std::numeric_limits<double>::infinity();
        return std::make_tuple(ts, std::cref(e.info.dateTime), e.path.filename().native(), e.path.native());
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const FileEntry& a, const FileEntry& b) {
        return key(a) < key(b);
    });
}

} // namespace hdr
