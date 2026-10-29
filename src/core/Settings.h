#pragma once
#include "Types.h"
#include <string>
#include <filesystem>

namespace hdr {

struct AppSettings {
    MergeSettings merge;
    ToneParams tone;
    std::string lastInputFolder;   // UTF-8
    std::string lastOutputFolder;  // UTF-8
    int framesPerBracket = 3;
    int threadCount = 0; // 0 = auto
    int windowWidth = 1600;
    int windowHeight = 950;
    bool windowMaximized = false;
    bool showListThumbnails = true;
    ListThumbnailSize listThumbnailSize = ListThumbnailSize::Medium;

    // %APPDATA%\HDR_DNG_Merger (or ~/.config/HDR_DNG_Merger)
    static fs::path getSettingsDirectory();
    static fs::path getSettingsFilePath();

    static AppSettings load();
    bool save() const;

    // Missing, malformed or out-of-range values fall back to their defaults individually.
    static AppSettings loadFrom(const fs::path& file);
    // Writes atomically (temporary file + rename).
    bool saveTo(const fs::path& file) const;

    // Clamps every field to its valid range.
    void validate();
};

} // namespace hdr
