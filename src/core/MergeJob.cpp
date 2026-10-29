#include "MergeJob.h"
#include "RawLoader.h"
#include "MergeEngine.h"
#include "DngWriter.h"
#include "MetadataCopier.h"
#include "Parallel.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <cmath>
#include <format>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace hdr {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Case-insensitive comparison key for a (possibly not yet existing) path.
std::wstring pathKey(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(p, ec);
    if (ec) abs = fs::absolute(p, ec);
    std::wstring s = (ec ? p : abs).lexically_normal().wstring();
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool isSameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec)) return true;
    return pathKey(a) == pathKey(b);
}

fs::path temporarySibling(const fs::path& target) {
    std::random_device rd;
    const std::string suffix = std::format(".{:08x}.tmp", rd());
    fs::path tmp = target;
    tmp += suffix;
    return tmp;
}

void removeQuietly(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

Status executeImpl(const BracketGroup& group, const fs::path& outputPath, const MergeSettings& settings,
                   const ProgressFn& progress, std::stop_token stopToken) {
    if (group.files.empty()) {
        return Status::error("Bracket group has no files");
    }
    if (stopToken.stop_requested()) {
        return Status::error("Cancelled");
    }
    if (Status s = MergeJob::checkOutputPath(outputPath, group); !s) {
        return s;
    }

    std::vector<FileEntry> activeFiles;
    std::vector<size_t> originalIndices;
    for (size_t i = 0; i < group.files.size(); ++i) {
        if (group.files[i].enabled) {
            activeFiles.push_back(group.files[i]);
            originalIndices.push_back(i);
        }
    }
    if (activeFiles.empty()) {
        return Status::error("All frames in bracket group are excluded/disabled");
    }

    // 1. Load full raw frames
    auto loadProgress = [&](float p, const std::string& desc) {
        if (progress) progress(0.05f + 0.35f * p, desc);
    };
    auto [loadStatus, frames] = MergeJob::loadFrames(activeFiles, loadProgress, stopToken);
    if (!loadStatus) return loadStatus;
    if (stopToken.stop_requested()) return Status::error("Cancelled");

    int rawRefIdx = group.getResolvedReferenceIndex();
    int mappedRefIdx = -1;
    for (size_t i = 0; i < originalIndices.size(); ++i) {
        if (static_cast<int>(originalIndices[i]) == rawRefIdx) {
            mappedRefIdx = static_cast<int>(i);
            break;
        }
    }

    MergeSettings settingsForMerge = settings;
    settingsForMerge.referenceFrame = mappedRefIdx;  // -1 = engine picks the middle exposure

    // 2. Align, estimate exposures and merge
    auto subProgress = [&](float p, const std::string& desc) {
        if (progress) progress(0.45f + 0.40f * p, desc);
    };
    MergeResult mergeResult = MergeEngine::merge(frames, {}, settingsForMerge, subProgress, stopToken);
    if (!mergeResult.status) {
        return mergeResult.status;
    }
    if (stopToken.stop_requested()) return Status::error("Cancelled");

    if (mappedRefIdx < 0) {
        // Same choice the engine made: middle exposure
        std::vector<size_t> order(frames.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return mergeResult.exposures[a] > mergeResult.exposures[b];
        });
        mappedRefIdx = static_cast<int>(order[order.size() / 2]);
    }

    // The source pixels are no longer needed; free them before allocating the output buffers.
    for (auto& f : frames) f.data = ImageU16();

    // 3. Reference metadata frame: baseline exposure and the crop shared by all aligned frames
    RawImage& metaFrame = frames[mappedRefIdx];
    metaFrame.color.baselineExposure = mergeResult.baselineExposure;
    {
        const auto& v = mergeResult.validRect;
        const int ox = std::max(metaFrame.defaultCropOrigin[0], v[0]);
        const int oy = std::max(metaFrame.defaultCropOrigin[1], v[1]);
        const int ex = std::min(metaFrame.defaultCropOrigin[0] + metaFrame.defaultCropSize[0], v[2]);
        const int ey = std::min(metaFrame.defaultCropOrigin[1] + metaFrame.defaultCropSize[1], v[3]);
        if (ex > ox && ey > oy) {
            metaFrame.defaultCropOrigin = {ox, oy};
            metaFrame.defaultCropSize = {ex - ox, ey - oy};
        }
    }

    const FloatBits bits = DngWriter::resolveFloatBits(settings.bits, metaFrame.whiteLevel);
    if (bits == FloatBits::Int16) {
        const auto [minE, maxE] = std::minmax_element(mergeResult.exposures.begin(), mergeResult.exposures.end());
        const double evSpan = (*minE > 0.0) ? std::log2(*maxE / *minE) : 0.0;
        const double sensorBits = std::log2(std::max(2.0, static_cast<double>(metaFrame.whiteLevel - metaFrame.blackLevel[1])));
        if (sensorBits + evSpan > 16.5) {
            spdlog::warn("{}: merged range needs ~{:.1f} bits but 16-bit integer output keeps 16; deep shadows lose "
                         "precision. Choose 32-bit float output if your raw converter supports it.",
                         outputPath.filename().string(), sensorBits + evSpan);
        }
    }

    // 4. Write the DNG to a temporary file next to the target
    const fs::path tempPath = temporarySibling(outputPath);
    if (progress) progress(0.90f, bits == FloatBits::Int16 ? "Writing 16-bit DNG..." : "Writing floating-point DNG...");
    DngWriteParams writeParams;
    writeParams.bits = settings.bits;
    writeParams.compress = true;  // uncompressed float DNGs are unreadable for LibRaw-based apps
    writeParams.embedPreview = settings.embedPreview;

    Status writeStatus = DngWriter::writeFloatDng(tempPath, mergeResult.cfa, mergeResult.width,
                                                  mergeResult.height, metaFrame, writeParams);
    mergeResult.cfa = ImageF();
    if (!writeStatus) {
        removeQuietly(tempPath);
        return writeStatus;
    }

    // 5. Copy EXIF, GPS, and XMP metadata from reference frame
    if (progress) progress(0.95f, "Copying metadata (GPS, EXIF, XMP)...");
    MetadataCopyOptions metaOpts;
    for (const auto& f : activeFiles) {
        metaOpts.sourceFiles.push_back(f.path.filename().string());
    }
    metaOpts.fallbackExposure = activeFiles[mappedRefIdx].info;
    if (settings.writeLightroomSettings) {
        metaOpts.lightroomTone = settings.tone;
    }
    Status copyStatus = MetadataCopier::copyMetadata(activeFiles[mappedRefIdx].path, tempPath, metaOpts);
    if (!copyStatus) {
        spdlog::warn("Metadata copy warning: {}", copyStatus.message);
    }

    if (stopToken.stop_requested()) {
        removeQuietly(tempPath);
        return Status::error("Cancelled");
    }

    // 6. Move into place (replaces an existing output of the same name)
    std::error_code ec;
    fs::rename(tempPath, outputPath, ec);
    if (ec) {
        removeQuietly(tempPath);
        return Status::error("Failed to move merged DNG into place (" + outputPath.filename().string() + "): " + ec.message());
    }

    if (progress) progress(1.0f, "Completed");
    return Status::success();
}

} // namespace

Status MergeJob::execute(
    const BracketGroup& group,
    const fs::path& outputPath,
    const MergeSettings& settings,
    ProgressFn progress,
    std::stop_token stopToken
) {
    try {
        return executeImpl(group, outputPath, settings, progress, stopToken);
    } catch (const std::bad_alloc&) {
        return Status::error("Out of memory while merging " + group.name);
    } catch (const std::exception& e) {
        return Status::error(std::string("Unexpected error: ") + e.what());
    }
}

std::pair<Status, std::vector<RawImage>> MergeJob::loadFrames(
    const std::vector<FileEntry>& files,
    ProgressFn progress,
    std::stop_token stopToken
) {
    const size_t n = files.size();
    std::vector<RawImage> frames(n);
    std::vector<Status> statuses(n);
    std::atomic<int> loaded{0};

    if (progress) progress(0.0f, "Loading " + std::to_string(n) + " raw frames...");
    parallelFor(0, static_cast<int>(n), 1, [&](int i) {
        if (stopToken.stop_requested()) {
            statuses[i] = Status::error("Cancelled");
            return;
        }
        auto [status, img] = RawLoader::loadFull(files[i].path, stopToken);
        statuses[i] = status;
        if (status) frames[i] = std::move(img);
        const int done = ++loaded;
        if (progress) {
            progress(static_cast<float>(done) / n, "Loaded " + files[i].path.filename().string() + " (" +
                     std::to_string(done) + "/" + std::to_string(n) + ")");
        }
    });

    if (stopToken.stop_requested()) return {Status::error("Cancelled"), {}};
    for (size_t i = 0; i < n; ++i) {
        if (!statuses[i]) {
            return {Status::error("Failed to load " + files[i].path.filename().string() + ": " + statuses[i].message), {}};
        }
    }
    for (size_t i = 1; i < n; ++i) {
        if (frames[i].width != frames[0].width || frames[i].height != frames[0].height) {
            return {Status::error("Image dimensions mismatch in group: " + files[i].path.filename().string()), {}};
        }
        if (frames[i].cfa.c != frames[0].cfa.c) {
            return {Status::error("CFA pattern mismatch in group: " + files[i].path.filename().string()), {}};
        }
    }
    return {Status::success(), std::move(frames)};
}

Status MergeJob::checkOutputPath(const fs::path& outputPath, const BracketGroup& group) {
    if (outputPath.empty() || outputPath.filename().empty()) {
        return Status::error("No output file name");
    }
    for (const auto& f : group.files) {
        if (isSameFile(outputPath, f.path)) {
            return Status::error("Refusing to overwrite source file " + f.path.filename().string() +
                                 " - choose a different output folder or naming pattern");
        }
    }
    std::error_code ec;
    if (fs::is_directory(outputPath, ec)) {
        return Status::error("Output path is a directory: " + outputPath.filename().string());
    }
    return Status::success();
}

std::string MergeJob::sanitizeFilename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*') {
            out += '_';
        } else {
            out += static_cast<char>(c);
        }
    }
    // Windows strips trailing dots and spaces; leading ones produce hidden/odd names.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(out.begin());
    if (out.empty()) out = "HDR";

    static const std::set<std::string> reserved = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    const std::string stem = toLower(out.substr(0, out.find('.')));
    if (reserved.count(stem)) out = "_" + out;
    return out;
}

std::string MergeJob::formatOutputFilename(
    const std::string& pattern,
    const BracketGroup& group,
    int groupIndex
) {
    if (group.files.empty()) {
        return sanitizeFilename(pattern.empty() ? "HDR" : pattern) + ".dng";
    }

    std::vector<FileEntry> activeFiles;
    for (const auto& f : group.files) {
        if (f.enabled) activeFiles.push_back(f);
    }
    if (activeFiles.empty()) {
        activeFiles = group.files;
    }

    int rawRefIdx = group.getResolvedReferenceIndex();
    const FileEntry& refEntry = (rawRefIdx >= 0 && rawRefIdx < static_cast<int>(group.files.size()))
                                    ? group.files[rawRefIdx]
                                    : activeFiles.front();

    std::string firstStem = activeFiles.front().path.stem().string();
    std::string lastStem = activeFiles.back().path.stem().string();
    std::string refStem = refEntry.path.stem().string();

    int idx = (groupIndex >= 0) ? (groupIndex + 1) : group.id;
    if (idx <= 0) idx = 1;

    std::string result = pattern.empty() ? "{first}_HDR" : pattern;

    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t startPos = 0;
        while ((startPos = str.find(from, startPos)) != std::string::npos) {
            str.replace(startPos, from.length(), to);
            startPos += to.length();
        }
    };

    replaceAll(result, "{first}", firstStem);
    replaceAll(result, "{last}", lastStem);
    replaceAll(result, "{ref}", refStem);
    replaceAll(result, "{index}", std::to_string(idx));
    replaceAll(result, "{n}", std::to_string(activeFiles.size()));

    // ISO
    int isoVal = static_cast<int>(std::round(refEntry.info.iso));
    replaceAll(result, "{iso}", std::to_string(isoVal > 0 ? isoVal : 100));

    // Shutter speed (filename friendly: e.g. "1_250s" or "2s")
    std::string shutterStr;
    if (refEntry.info.exposureTime > 0.0) {
        if (refEntry.info.exposureTime < 0.5) {
            int inv = static_cast<int>(std::round(1.0 / refEntry.info.exposureTime));
            shutterStr = "1_" + std::to_string(inv) + "s";
        } else {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << refEntry.info.exposureTime;
            std::string s = ss.str();
            for (char& c : s) if (c == '.') c = '_';
            shutterStr = s + "s";
        }
    } else {
        shutterStr = "unknown_shutter";
    }
    replaceAll(result, "{shutter}", shutterStr);

    // F-number / aperture
    std::string fStr;
    if (refEntry.info.fNumber > 0.0) {
        std::ostringstream ss;
        ss << "f" << std::fixed << std::setprecision(1) << refEntry.info.fNumber;
        std::string s = ss.str();
        for (char& c : s) if (c == '.') c = '_';
        fStr = s;
    } else {
        fStr = "unknown_f";
    }
    replaceAll(result, "{fnumber}", fStr);

    // Camera make/model
    std::string camStr = refEntry.info.make;
    if (!refEntry.info.model.empty()) {
        if (!camStr.empty()) camStr += "_";
        camStr += refEntry.info.model;
    }
    for (char& c : camStr) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    if (camStr.empty()) camStr = "Camera";
    replaceAll(result, "{camera}", camStr);

    // Date
    std::string dateStr = refEntry.info.dateTime;
    std::string cleanDate;
    if (dateStr.size() >= 19 && dateStr[4] == ':' && dateStr[7] == ':') {
        cleanDate = dateStr.substr(0, 4) + dateStr.substr(5, 2) + dateStr.substr(8, 2);
        if (dateStr[10] == ' ') {
            cleanDate += "_" + dateStr.substr(11, 2) + dateStr.substr(14, 2) + dateStr.substr(17, 2);
        }
    } else {
        for (char c : dateStr) {
            if (std::isdigit(static_cast<unsigned char>(c))) cleanDate += c;
            else if (c == ' ') cleanDate += '_';
        }
    }
    if (cleanDate.empty()) cleanDate = "Date";
    replaceAll(result, "{date}", cleanDate);

    // Ensure .dng extension (case-insensitive), then make the whole thing a safe single file name
    if (result.size() >= 4 && toLower(result.substr(result.size() - 4)) == ".dng") {
        result.resize(result.size() - 4);
    }
    return sanitizeFilename(result) + ".dng";
}

std::vector<fs::path> MergeJob::planOutputPaths(
    const std::vector<BracketGroup>& groups,
    const std::string& pattern,
    const fs::path& outputDir
) {
    std::vector<fs::path> paths;
    paths.reserve(groups.size());
    std::set<std::string> used;
    for (size_t i = 0; i < groups.size(); ++i) {
        const std::string name = formatOutputFilename(pattern, groups[i], static_cast<int>(i));
        const std::string stem = name.substr(0, name.size() - 4);
        std::string candidate = name;
        for (int n = 2; used.count(toLower(candidate)); ++n) {
            candidate = stem + "_" + std::to_string(n) + ".dng";
        }
        used.insert(toLower(candidate));
        paths.push_back(outputDir / candidate);
    }
    return paths;
}

} // namespace hdr
