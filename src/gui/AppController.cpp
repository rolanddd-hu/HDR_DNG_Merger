#include "AppController.h"
#include "../core/FileScanner.h"
#include "../core/MergeJob.h"
#include "../core/PathUtil.h"
#include "../core/RawLoader.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace hdr::gui {

namespace {

constexpr uint64_t kPreviewCacheBudget = 1ull << 30;   // 1 GiB of preview images
constexpr size_t kMaxThumbnailTextures = 256;          // ~180 MB of 480 px RGBA textures
constexpr int kThumbnailMaxDim = 480;
constexpr int kThumbnailWorkers = 2;

const ImageRgbF& emptyImage() {
    static const ImageRgbF empty;
    return empty;
}

} // namespace

AppController::AppController() {
    settings_ = AppSettings::load();
    toneParams_ = settings_.tone;
    lastRenderedToneParams_ = toneParams_;
    batchProcessor_.setMaxWorkers(static_cast<size_t>(settings_.threadCount));

    for (int i = 0; i < kThumbnailWorkers; ++i) {
        thumbnailWorkers_.emplace_back([this](std::stop_token st) {
            while (true) {
                std::pair<std::string, fs::path> job;
                {
                    std::unique_lock<std::mutex> lock(thumbnailMutex_);
                    if (!thumbnailCv_.wait(lock, st, [&] { return !thumbnailQueue_.empty(); })) return;
                    job = std::move(thumbnailQueue_.front());
                    thumbnailQueue_.pop_front();
                }
                try {
                    auto [status, thumb] = RawLoader::loadThumbnail(job.second, kThumbnailMaxDim);
                    std::lock_guard<std::mutex> lock(thumbnailMutex_);
                    if (status.ok && !thumb.empty() && requestedThumbnails_.count(job.first)) {
                        pendingUploadThumbnails_.emplace_back(job.first, std::move(thumb));
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Thumbnail for {} failed: {}", job.second.filename().string(), e.what());
                }
            }
        });
    }
}

AppController::~AppController() {
    previewStop_.request_stop();
    scanStop_.request_stop();
    for (auto& w : thumbnailWorkers_) w.request_stop();
    stopAndJoin(previewTasks_);
    stopAndJoin(scanTasks_);
    thumbnailWorkers_.clear();  // joins (the condition variable wakes on stop)

    settings_.tone = toneParams_;
    settings_.save();
}

void AppController::stopAndJoin(std::vector<BackgroundTask>& tasks) {
    for (auto& t : tasks) t.thread.request_stop();
    tasks.clear();
}

bool AppController::initRenderResources() {
    return toneShader_.init();
}

void AppController::shutdownRenderResources() {
    thumbnailTextures_.clear();
    srcTexture_.release();
    displayTexture_.release();
    beforeTexture_.release();
    motionTexture_.release();
    sourceTexture_.release();
    histogramTexture_.release();
    toneShader_.release();
}

std::string AppController::previewStatus() const {
    std::lock_guard<std::mutex> lock(previewMutex_);
    return previewStatusText_;
}

const ImageRgbF& AppController::currentLinearImage() const {
    return currentLinearImage_ ? *currentLinearImage_ : emptyImage();
}

void AppController::renderToneTextures(bool updateHistogram) {
    toneShader_.renderToTexture(srcTexture_, displayTexture_, toneParams_, showClipping);
    if (updateHistogram && srcTexture_.isValid()) {
        // Histogram of the actual display pipeline output (without clipping overlay), rendered small.
        const int hw = std::min(256, srcTexture_.width());
        const int hh = std::max(1, srcTexture_.height() * hw / std::max(1, srcTexture_.width()));
        std::vector<uint8_t> pixels;
        if (toneShader_.renderToTexture(srcTexture_, histogramTexture_, toneParams_, false, hw, hh) &&
            histogramTexture_.readRgba8(pixels)) {
            histogramData_ = PreviewPipeline::computeHistogramRgba8(pixels.data(), hw, hh);
        }
    }
    lastRenderedToneParams_ = toneParams_;
    lastRenderedShowClipping_ = showClipping;
}

void AppController::uploadPreviewImage(std::shared_ptr<const ImageRgbF> img, const std::vector<uint8_t>& motionMask, const std::vector<uint8_t>& sourceMap) {
    if (!img || img->empty()) return;
    currentLinearImage_ = std::move(img);
    const ImageRgbF& image = *currentLinearImage_;
    srcTexture_.uploadRgbFloat(image.width, image.height, image.data.data());

    // Render neutral 'Before' texture
    ToneParams neutralParams;
    neutralParams.toneMap = ToneMapOp::Filmic;
    toneShader_.renderToTexture(srcTexture_, beforeTexture_, neutralParams, false);

    // Render adjusted 'Display' texture and its histogram
    renderToneTextures(true);

    const size_t quadCount = static_cast<size_t>(image.width) * image.height;

    // Upload motion mask texture if present
    if (!motionMask.empty() && motionMask.size() == quadCount) {
        std::vector<uint8_t> rgba(motionMask.size() * 4, 0);
        for (size_t i = 0; i < motionMask.size(); ++i) {
            if (motionMask[i]) {
                rgba[i * 4 + 0] = 255; // R
                rgba[i * 4 + 1] = 0;   // G
                rgba[i * 4 + 2] = 128; // B (magenta)
                rgba[i * 4 + 3] = 160; // Alpha
            }
        }
        motionTexture_.uploadRgba8(image.width, image.height, rgba.data());
    } else {
        motionTexture_.release();
    }

    // Upload source map texture if present
    if (!sourceMap.empty() && sourceMap.size() == quadCount) {
        static const uint8_t palette[][3] = {
            { 40, 130, 240 }, // Frame 0: Blue
            { 46, 204, 113 }, // Frame 1: Green
            { 240, 160, 20 }, // Frame 2: Amber
            { 160, 60, 220 }, // Frame 3: Purple
            { 235, 60, 60 },  // Frame 4: Red
            { 20, 200, 200 }, // Frame 5: Cyan
            { 240, 220, 40 }  // Frame 6: Yellow
        };
        constexpr size_t numPalette = sizeof(palette) / sizeof(palette[0]);

        std::vector<uint8_t> rgba(sourceMap.size() * 4, 0);
        for (size_t i = 0; i < sourceMap.size(); ++i) {
            size_t colorIdx = sourceMap[i] % numPalette;
            rgba[i * 4 + 0] = palette[colorIdx][0];
            rgba[i * 4 + 1] = palette[colorIdx][1];
            rgba[i * 4 + 2] = palette[colorIdx][2];
            rgba[i * 4 + 3] = 135; // Semi-transparent overlay
        }
        sourceTexture_.uploadRgba8(image.width, image.height, rgba.data());
    } else {
        sourceTexture_.release();
    }

    previewReady_ = true;
    isPreviewLoading_ = false;
}

void AppController::reapFinishedTasks() {
    auto reap = [](std::vector<BackgroundTask>& tasks) {
        std::erase_if(tasks, [](BackgroundTask& t) { return t.done->load(); });  // jthread joins instantly
    };
    reap(previewTasks_);
    reap(scanTasks_);
}

void AppController::evictThumbnails() {
    if (thumbnailTextures_.size() <= kMaxThumbnailTextures) return;
    std::vector<std::pair<uint64_t, std::string>> byAge;
    byAge.reserve(thumbnailTextures_.size());
    for (const auto& [key, entry] : thumbnailTextures_) byAge.emplace_back(entry.lastUsedFrame, key);
    std::sort(byAge.begin(), byAge.end());

    const size_t toRemove = thumbnailTextures_.size() - kMaxThumbnailTextures;
    std::lock_guard<std::mutex> lock(thumbnailMutex_);
    for (size_t i = 0; i < toRemove; ++i) {
        // Never evict something drawn in the previous frame (it would just be reloaded).
        if (byAge[i].first + 1 >= frameCounter_) break;
        thumbnailTextures_.erase(byAge[i].second);
        requestedThumbnails_.erase(byAge[i].second);
    }
}

void AppController::update() {
    ++frameCounter_;
    reapFinishedTasks();

    // 0. Upload newly decoded thumbnails; keep the texture cache bounded
    {
        std::vector<std::pair<std::string, ThumbnailImage>> toUpload;
        {
            std::lock_guard<std::mutex> lock(thumbnailMutex_);
            toUpload.swap(pendingUploadThumbnails_);
        }
        for (auto& [key, thumb] : toUpload) {
            ThumbnailEntry entry;
            if (entry.texture.uploadRgba8(thumb.width, thumb.height, thumb.rgba.data())) {
                entry.lastUsedFrame = frameCounter_;
                thumbnailTextures_[key] = std::move(entry);
            }
        }
        evictThumbnails();
    }

    // 1. Merge finished folder / file scans
    {
        std::vector<std::vector<FileEntry>> results;
        {
            std::lock_guard<std::mutex> lock(scanMutex_);
            results.swap(pendingScanResults_);
        }
        for (auto& r : results) mergeScanResults(std::move(r));
    }

    // 2. Check if new preview image arrived from background thread
    {
        PendingPreview pending;
        bool hasPending = false;
        {
            std::lock_guard<std::mutex> lock(previewMutex_);
            if (hasNewPendingImage_) {
                pending = std::move(pendingPreview_);
                pendingPreview_ = {};
                hasNewPendingImage_ = false;
                hasPending = true;
            }
        }
        if (hasPending && pending.image && !pending.image->empty()) {
            if (!pending.cacheKey.empty()) {
                const uint64_t bytes = pending.image->data.size() * sizeof(float) +
                                       pending.motionMask.size() + pending.sourceMap.size();
                storeInPreviewCache(pending.cacheKey, PreviewCacheItem{pending.image, pending.motionMask, pending.sourceMap, bytes});
            }
            uploadPreviewImage(std::move(pending.image), pending.motionMask, pending.sourceMap);
        }
    }

    // 3. Re-render when tone parameters or the clipping overlay changed
    if (previewReady_ && srcTexture_.isValid()) {
        const bool paramsChanged = !(toneParams_ == lastRenderedToneParams_);
        const bool clippingChanged = (showClipping != lastRenderedShowClipping_);
        if (paramsChanged || clippingChanged) {
            renderToneTextures(paramsChanged);
        }
    }
}

void AppController::storeInPreviewCache(const std::string& key, PreviewCacheItem item) {
    if (auto it = previewCache_.find(key); it != previewCache_.end()) {
        previewCacheBytes_ -= it->second.bytes;
        previewCacheOrder_.remove(key);
        previewCache_.erase(it);
    }
    previewCacheBytes_ += item.bytes;
    previewCache_[key] = std::move(item);
    previewCacheOrder_.push_back(key);
    while (previewCacheBytes_ > kPreviewCacheBudget && previewCacheOrder_.size() > 1) {
        const std::string oldest = previewCacheOrder_.front();
        previewCacheOrder_.pop_front();
        if (auto it = previewCache_.find(oldest); it != previewCache_.end()) {
            previewCacheBytes_ -= it->second.bytes;
            previewCache_.erase(it);
        }
    }
}

void AppController::rememberInputFolder(const fs::path& folder) {
    settings_.lastInputFolder = toUtf8(folder);
    if (settings_.lastOutputFolder.empty()) {
        settings_.lastOutputFolder = toUtf8(folder / "merged_hdr");
    }
    settings_.save();
}

void AppController::startScan(std::function<std::vector<FileEntry>(std::stop_token)> scan) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    activeScans_++;
    const std::stop_token clearToken = scanStop_.get_token();
    scanTasks_.push_back({std::jthread([this, scan = std::move(scan), done, clearToken](std::stop_token own) {
        std::vector<FileEntry> entries;
        try {
            // Stops when either the controller shuts down or clearAll() invalidates the scan.
            std::stop_source combined;
            std::stop_callback onOwn(own, [&] { combined.request_stop(); });
            std::stop_callback onClear(clearToken, [&] { combined.request_stop(); });
            entries = scan(combined.get_token());
            if (!combined.stop_requested()) {
                std::lock_guard<std::mutex> lock(scanMutex_);
                pendingScanResults_.push_back(std::move(entries));
            }
        } catch (const std::exception& e) {
            spdlog::error("Scanning failed: {}", e.what());
        }
        activeScans_--;
        done->store(true);
    }), done});
}

void AppController::mergeScanResults(std::vector<FileEntry> entries) {
    std::unordered_set<fs::path::string_type> known;
    for (const auto& existing : scannedFiles_) known.insert(existing.path.native());
    size_t added = 0;
    for (auto& entry : entries) {
        if (known.insert(entry.path.native()).second) {
            scannedFiles_.push_back(std::move(entry));
            added++;
        }
    }
    if (added == 0) return;
    spdlog::info("Added {} raw file(s)", added);
    FileScanner::sortEntries(scannedFiles_);
    rebuildGroups();
}

void AppController::addFiles(const std::vector<fs::path>& paths) {
    if (paths.empty()) return;
    std::error_code ec;
    const fs::path parent = paths[0].parent_path();
    if (fs::is_directory(parent, ec)) rememberInputFolder(parent);
    startScan([paths](std::stop_token st) { return FileScanner::scanFiles(paths, st); });
}

void AppController::addFolder(const fs::path& folder) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return;
    rememberInputFolder(folder);
    startScan([folder](std::stop_token st) { return FileScanner::scanPath(folder, st); });
}

void AppController::clearAll() {
    // Invalidate (without waiting for) running previews and scans
    previewStop_.request_stop();
    previewStop_ = std::stop_source();
    scanStop_.request_stop();
    scanStop_ = std::stop_source();
    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        previewGeneration_++;
        hasNewPendingImage_ = false;
        pendingPreview_ = {};
        previewStatusText_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(scanMutex_);
        pendingScanResults_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex_);
        thumbnailQueue_.clear();
        requestedThumbnails_.clear();
        pendingUploadThumbnails_.clear();
    }
    thumbnailTextures_.clear();
    PreviewPipeline::clearFrameCache();

    scannedFiles_.clear();
    groups_.clear();
    selectedGroupIdx_ = -1;
    selectedFrameIdx_ = -1;
    previewReady_ = false;
    isPreviewLoading_ = false;
    currentLinearImage_.reset();
    histogramData_.clear();
    previewCache_.clear();
    previewCacheOrder_.clear();
    previewCacheBytes_ = 0;
    srcTexture_.release();
    displayTexture_.release();
    beforeTexture_.release();
    motionTexture_.release();
    sourceTexture_.release();
}

void AppController::rebuildGroups() {
    if (scannedFiles_.empty()) {
        groups_.clear();
        selectedGroupIdx_ = -1;
        return;
    }
    auto res = BracketGrouper::groupFixedN(scannedFiles_, settings_.framesPerBracket);
    groups_ = std::move(res.groups);
    if (!res.warning.empty()) spdlog::info("{}", res.warning);
    if (groups_.empty()) {
        selectedGroupIdx_ = -1;
        return;
    }
    if (selectedGroupIdx_ < 0 || selectedGroupIdx_ >= static_cast<int>(groups_.size())) {
        selectedGroupIdx_ = -1;
        selectGroup(0);
    } else {
        triggerPreview();
    }
}

void AppController::regroup(int framesPerBracket) {
    settings_.framesPerBracket = framesPerBracket;
    rebuildGroups();
}

void AppController::autoDetectGrouping() {
    if (scannedFiles_.empty()) return;
    int detectedN = BracketGrouper::autoDetectBracketSize(scannedFiles_);
    if (detectedN >= 2 && detectedN <= 9) {
        settings_.framesPerBracket = detectedN;
    }
    rebuildGroups();
}

void AppController::splitGroup(size_t groupIdx, size_t frameIdx) {
    if (groupIdx >= groups_.size()) return;
    auto& grp = groups_[groupIdx];
    if (frameIdx == 0 || frameIdx >= grp.files.size()) return;

    BracketGroup newGrp;
    int maxId = 0;
    for (const auto& g : groups_) maxId = std::max(maxId, g.id);
    newGrp.id = maxId + 1;
    newGrp.name = grp.name + " (split)";
    newGrp.files.assign(grp.files.begin() + frameIdx, grp.files.end());
    grp.files.erase(grp.files.begin() + frameIdx, grp.files.end());
    if (grp.referenceIndex >= static_cast<int>(grp.files.size())) grp.referenceIndex = -1;

    BracketGrouper::validateGroup(grp);
    BracketGrouper::validateGroup(newGrp);

    groups_.insert(groups_.begin() + groupIdx + 1, std::move(newGrp));
    triggerPreview();
}

void AppController::joinWithNext(size_t groupIdx) {
    if (groupIdx + 1 >= groups_.size()) return;
    auto& grp = groups_[groupIdx];
    auto& nextGrp = groups_[groupIdx + 1];

    grp.files.insert(grp.files.end(), nextGrp.files.begin(), nextGrp.files.end());
    BracketGrouper::validateGroup(grp);

    groups_.erase(groups_.begin() + groupIdx + 1);
    if (selectedGroupIdx_ > static_cast<int>(groupIdx)) selectedGroupIdx_--;
    triggerPreview();
}

void AppController::excludeFrame(size_t groupIdx, size_t frameIdx) {
    if (groupIdx >= groups_.size()) return;
    auto& grp = groups_[groupIdx];
    if (frameIdx >= grp.files.size()) return;

    grp.files.erase(grp.files.begin() + frameIdx);
    if (grp.referenceIndex == static_cast<int>(frameIdx)) grp.referenceIndex = -1;
    else if (grp.referenceIndex > static_cast<int>(frameIdx)) grp.referenceIndex--;
    if (selectedFrameIdx_ >= static_cast<int>(grp.files.size())) selectedFrameIdx_ = -1;

    if (grp.files.empty()) {
        groups_.erase(groups_.begin() + groupIdx);
        if (selectedGroupIdx_ >= static_cast<int>(groups_.size())) {
            selectedGroupIdx_ = static_cast<int>(groups_.size()) - 1;
        }
    } else {
        BracketGrouper::validateGroup(grp);
    }
    triggerPreview();
}

void AppController::setReferenceFrame(size_t groupIdx, size_t frameIdx) {
    if (groupIdx >= groups_.size()) return;
    auto& grp = groups_[groupIdx];
    if (frameIdx >= grp.files.size()) return;

    grp.referenceIndex = static_cast<int>(frameIdx);
    triggerPreview();
}

void AppController::setFrameEnabled(size_t groupIdx, size_t frameIdx, bool enabled) {
    if (groupIdx >= groups_.size()) return;
    auto& grp = groups_[groupIdx];
    if (frameIdx >= grp.files.size()) return;

    grp.files[frameIdx].enabled = enabled;
    BracketGrouper::validateGroup(grp);
    triggerPreview();
}

void AppController::selectGroup(int groupIdx) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(groups_.size())) return;
    if (selectedGroupIdx_ != groupIdx) {
        selectedGroupIdx_ = groupIdx;
        selectedFrameIdx_ = -1; // Default to merged
        triggerPreview();
    }
}

void AppController::selectFrame(int frameIdx) {
    if (selectedFrameIdx_ != frameIdx) {
        selectedFrameIdx_ = frameIdx;
        triggerPreview();
    }
}

void AppController::resetToneParams() {
    toneParams_ = ToneParams();
}

std::string AppController::getCacheKey(const BracketGroup& group, int frameIdx) const {
    if (frameIdx >= 0 && frameIdx < static_cast<int>(group.files.size())) {
        return "frame:" + toUtf8(group.files[frameIdx].path);
    }
    const auto& m = settings_.merge;
    std::string key = "merged:";
    for (const auto& f : group.files) {
        if (f.enabled) {
            key += toUtf8(f.path) + ";";
        }
    }
    key += "ref:" + std::to_string(group.getResolvedReferenceIndex()) + ":";
    key += "align:" + std::to_string(static_cast<int>(m.align)) + ":";
    key += "maxShift:" + std::to_string(m.maxAlignShift) + ":";
    key += "deghost:" + std::to_string(static_cast<int>(m.deghost)) + ":";
    key += "mode:" + std::to_string(static_cast<int>(m.mode)) + ":";
    key += "clip:" + std::to_string(m.clipThreshold) + ":";
    key += "feather:" + std::to_string(m.featherRadius);
    return key;
}

void AppController::triggerPreview() {
    if (selectedGroupIdx_ < 0 || selectedGroupIdx_ >= static_cast<int>(groups_.size())) return;
    const auto& grp = groups_[selectedGroupIdx_];

    const std::string key = getCacheKey(grp, selectedFrameIdx_);
    if (auto it = previewCache_.find(key); it != previewCache_.end()) {
        // Cancel whatever is still computing and show the cached result instantly
        previewStop_.request_stop();
        previewStop_ = std::stop_source();
        {
            std::lock_guard<std::mutex> lock(previewMutex_);
            previewGeneration_++;
            hasNewPendingImage_ = false;
            previewStatusText_ = "Ready";
        }
        previewCacheOrder_.remove(key);
        previewCacheOrder_.push_back(key);
        uploadPreviewImage(it->second.image, it->second.motionMask, it->second.sourceMap);
        return;
    }

    startAsyncPreview(grp, selectedFrameIdx_);
}

void AppController::startAsyncPreview(const BracketGroup& group, int frameIdx) {
    // Never wait for the previous preview: cancel it and let it finish in the background.
    previewStop_.request_stop();
    previewStop_ = std::stop_source();
    const std::stop_token st = previewStop_.get_token();

    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(previewMutex_);
        generation = ++previewGeneration_;
        previewStatusText_ = "Loading preview...";
    }
    isPreviewLoading_ = true;

    auto done = std::make_shared<std::atomic<bool>>(false);
    const std::string cacheKey = getCacheKey(group, frameIdx);
    const MergeSettings mergeSettings = settings_.merge;  // the worker must not read live settings

    previewTasks_.push_back({std::jthread([this, group, frameIdx, cacheKey, mergeSettings, generation, st, done]() {
        std::pair<Status, ImageRgbF> result;
        std::vector<uint8_t> motionMask;
        std::vector<uint8_t> sourceMap;
        try {
            if (frameIdx >= 0 && frameIdx < static_cast<int>(group.files.size())) {
                result = PreviewPipeline::generateSingleFramePreview(group.files[frameIdx].path, st);
            } else {
                result = PreviewPipeline::generateMergedPreview(group, mergeSettings, st, &motionMask, &sourceMap);
            }
        } catch (const std::exception& e) {
            result = {Status::error(std::string("Preview failed: ") + e.what()), {}};
        }

        if (!st.stop_requested()) {
            std::lock_guard<std::mutex> lock(previewMutex_);
            if (generation == previewGeneration_) {
                if (result.first) {
                    pendingPreview_ = PendingPreview{
                        std::make_shared<const ImageRgbF>(std::move(result.second)),
                        std::move(motionMask),
                        std::move(sourceMap),
                        cacheKey
                    };
                    hasNewPendingImage_ = true;
                    previewStatusText_ = "Ready";
                } else {
                    previewStatusText_ = result.first.message;
                    isPreviewLoading_ = false;
                    spdlog::warn("Preview: {}", result.first.message);
                }
            }
        }
        done->store(true);
    }), done});
}

fs::path AppController::outputDirectory() const {
    if (!settings_.lastOutputFolder.empty()) return pathFromUtf8(settings_.lastOutputFolder);
    std::error_code ec;
    return fs::current_path(ec) / "merged_hdr";
}

void AppController::openFolderInExplorer(const fs::path& folder) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return;  // only ever open real folders
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"explore", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void AppController::openOutputFolder() const {
    openFolderInExplorer(outputDirectory());
}

void AppController::startBatch() {
    if (groups_.empty()) return;
    const fs::path outDir = outputDirectory();
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        spdlog::error("Cannot create output folder {}: {}", toUtf8(outDir), ec.message());
        return;
    }

    batchProcessor_.clearFinished();

    MergeSettings jobSettings = settings_.merge;
    jobSettings.tone = toneParams_;
    const auto outPaths = MergeJob::planOutputPaths(groups_, jobSettings.namePattern, outDir);
    for (size_t i = 0; i < groups_.size(); ++i) {
        if (groups_[i].files.empty()) continue;
        batchProcessor_.addJob(groups_[i], outPaths[i], jobSettings);
    }

    batchProcessor_.setMaxWorkers(static_cast<size_t>(settings_.threadCount));
    batchProcessor_.start();
}

void AppController::cancelBatch() {
    batchProcessor_.cancelAll();
}

void AppController::mergeSingleGroup(size_t groupIdx) {
    if (groupIdx >= groups_.size()) return;
    const auto& grp = groups_[groupIdx];
    if (grp.files.empty()) return;

    const fs::path outDir = outputDirectory();
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        spdlog::error("Cannot create output folder {}: {}", toUtf8(outDir), ec.message());
        return;
    }

    // Appends to the queue; a running batch keeps going.
    MergeSettings jobSettings = settings_.merge;
    jobSettings.tone = toneParams_;
    const auto outPaths = MergeJob::planOutputPaths(groups_, jobSettings.namePattern, outDir);
    batchProcessor_.addJob(grp, outPaths[groupIdx], jobSettings);
    batchProcessor_.start();
}

void AppController::queueThumbnail(const std::string& key, const fs::path& path) {
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex_);
        if (!requestedThumbnails_.insert(key).second) return;
        thumbnailQueue_.emplace_back(key, path);
    }
    thumbnailCv_.notify_one();
}

GlTexture* AppController::getThumbnail(const fs::path& path) {
    const std::string key = toUtf8(path);
    auto it = thumbnailTextures_.find(key);
    if (it != thumbnailTextures_.end()) {
        it->second.lastUsedFrame = frameCounter_;
        return &it->second.texture;
    }
    queueThumbnail(key, path);
    return nullptr;
}

GlTexture* AppController::getGroupThumbnail(const BracketGroup& group) {
    if (group.files.empty()) return nullptr;
    int refIdx = group.getResolvedReferenceIndex();
    if (refIdx >= 0 && refIdx < static_cast<int>(group.files.size())) {
        return getThumbnail(group.files[refIdx].path);
    }
    return getThumbnail(group.files[0].path);
}

} // namespace hdr::gui
