#pragma once
#include "../core/Types.h"
#include "../core/Settings.h"
#include "../core/BracketGrouper.h"
#include "../core/BatchProcessor.h"
#include "render/GlTexture.h"
#include "render/ToneShader.h"
#include "../core/PreviewPipeline.h"
#include <vector>
#include <deque>
#include <functional>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <filesystem>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <stop_token>

namespace hdr::gui {

class AppController {
public:
    AppController();
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    bool initRenderResources();
    // Frees all GL objects; must run while the GL context is still current.
    void shutdownRenderResources();
    void update(); // Called every frame in main loop

    // File / Group management (scanning runs in the background)
    void addFiles(const std::vector<fs::path>& paths);
    void addFolder(const fs::path& folder);
    void clearAll();
    void regroup(int framesPerBracket);
    void autoDetectGrouping();
    bool isScanning() const { return activeScans_.load() > 0; }

    // Group editing
    void splitGroup(size_t groupIdx, size_t frameIdx);
    void joinWithNext(size_t groupIdx);
    void excludeFrame(size_t groupIdx, size_t frameIdx);
    void setReferenceFrame(size_t groupIdx, size_t frameIdx);
    void setFrameEnabled(size_t groupIdx, size_t frameIdx, bool enabled);

    // Selection & Preview
    void selectGroup(int groupIdx);
    void selectFrame(int frameIdx); // -1 = Merged, 0..N-1 = individual frame
    void triggerPreview();
    void resetToneParams();

    // Batch operations
    void startBatch();
    void cancelBatch();
    void mergeSingleGroup(size_t groupIdx);

    // Output folder (UTF-8 setting resolved to a path; default: <cwd>/merged_hdr)
    fs::path outputDirectory() const;
    void openOutputFolder() const;
    static void openFolderInExplorer(const fs::path& folder);

    // Accessors
    AppSettings& settings() { return settings_; }
    const AppSettings& settings() const { return settings_; }
    const std::vector<BracketGroup>& bracketGroups() const { return groups_; }
    int selectedGroupIndex() const { return selectedGroupIdx_; }
    int selectedFrameIndex() const { return selectedFrameIdx_; }
    ToneParams& toneParams() { return toneParams_; }
    const ToneParams& toneParams() const { return toneParams_; }
    BatchProcessor& batchProcessor() { return batchProcessor_; }
    const BatchProcessor& batchProcessor() const { return batchProcessor_; }

    GlTexture& displayTexture() { return displayTexture_; }
    GlTexture& beforeTexture() { return beforeTexture_; }
    GlTexture& motionTexture() { return motionTexture_; }
    GlTexture& sourceTexture() { return sourceTexture_; }
    bool hasMotionMask() const { return motionTexture_.isValid(); }
    bool hasSourceMap() const { return sourceTexture_.isValid(); }
    bool isPreviewReady() const { return previewReady_; }
    bool isPreviewLoading() const { return isPreviewLoading_; }
    std::string previewStatus() const;
    const HistogramData& histogram() const { return histogramData_; }
    const ImageRgbF& currentLinearImage() const;

    // Thumbnail access (loaded on demand in the background)
    GlTexture* getThumbnail(const fs::path& path);
    GlTexture* getGroupThumbnail(const BracketGroup& group);

    // Preview view controls
    float zoomLevel = 1.0f;
    float panOffsetX = 0.0f;
    float panOffsetY = 0.0f;
    bool fitToWindow = true;
    bool showClipping = false;
    bool showBeforeAfter = false;
    bool showMotionMask = false;
    bool showSourceMap = false;
    float splitPosition = 0.5f;
    bool resetLayoutRequested = false;
    bool requestExit = false;

    // Hover inspection
    bool hasHoverPixel = false;
    int hoverPixelX = 0;
    int hoverPixelY = 0;
    float hoverR = 0.0f;
    float hoverG = 0.0f;
    float hoverB = 0.0f;

private:
    // A background thread plus a flag it sets when finished, so update() can join it cheaply.
    struct BackgroundTask {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    struct PreviewCacheItem {
        std::shared_ptr<const ImageRgbF> image;
        std::vector<uint8_t> motionMask;
        std::vector<uint8_t> sourceMap;
        uint64_t bytes = 0;
    };
    struct PendingPreview {
        std::shared_ptr<const ImageRgbF> image;
        std::vector<uint8_t> motionMask;
        std::vector<uint8_t> sourceMap;
        std::string cacheKey;
    };
    struct ThumbnailEntry {
        GlTexture texture;
        uint64_t lastUsedFrame = 0;
    };

    void rebuildGroups();
    void startScan(std::function<std::vector<FileEntry>(std::stop_token)> scan);
    void mergeScanResults(std::vector<FileEntry> entries);
    void startAsyncPreview(const BracketGroup& group, int frameIdx);
    std::string getCacheKey(const BracketGroup& group, int frameIdx) const;
    void uploadPreviewImage(std::shared_ptr<const ImageRgbF> img, const std::vector<uint8_t>& motionMask,
                            const std::vector<uint8_t>& sourceMap);
    void renderToneTextures(bool updateHistogram);
    void storeInPreviewCache(const std::string& key, PreviewCacheItem item);
    void reapFinishedTasks();
    void evictThumbnails();
    void rememberInputFolder(const fs::path& folder);
    static void stopAndJoin(std::vector<BackgroundTask>& tasks);

    AppSettings settings_;
    std::vector<FileEntry> scannedFiles_;
    std::vector<BracketGroup> groups_;

    int selectedGroupIdx_ = -1;
    int selectedFrameIdx_ = -1; // -1 = merged, >= 0 = frame index
    ToneParams toneParams_;
    ToneParams lastRenderedToneParams_;
    bool lastRenderedShowClipping_ = false;
    uint64_t frameCounter_ = 0;

    BatchProcessor batchProcessor_;

    // OpenGL render resources
    ToneShader toneShader_;
    GlTexture srcTexture_;
    GlTexture displayTexture_;
    GlTexture beforeTexture_;
    GlTexture motionTexture_;
    GlTexture sourceTexture_;
    GlTexture histogramTexture_;

    // Async preview state
    std::atomic<bool> isPreviewLoading_{false};
    std::atomic<bool> previewReady_{false};
    std::stop_source previewStop_;
    std::vector<BackgroundTask> previewTasks_;
    mutable std::mutex previewMutex_;      // guards the fields below
    uint64_t previewGeneration_ = 0;
    std::string previewStatusText_;
    PendingPreview pendingPreview_;
    bool hasNewPendingImage_ = false;

    // In-memory preview cache (LRU, byte budget) for instant switching between brackets/frames
    std::unordered_map<std::string, PreviewCacheItem> previewCache_;
    std::list<std::string> previewCacheOrder_;
    uint64_t previewCacheBytes_ = 0;

    // Cached linear image for the tone shader & hover readout
    std::shared_ptr<const ImageRgbF> currentLinearImage_;
    HistogramData histogramData_;

    // Background folder / file scanning
    std::stop_source scanStop_;
    std::vector<BackgroundTask> scanTasks_;
    std::atomic<int> activeScans_{0};
    std::mutex scanMutex_;                 // guards pendingScanResults_
    std::vector<std::vector<FileEntry>> pendingScanResults_;

    // Thumbnail workers and LRU texture cache
    void queueThumbnail(const std::string& key, const fs::path& path);
    std::unordered_map<std::string, ThumbnailEntry> thumbnailTextures_;
    std::mutex thumbnailMutex_;            // guards the three fields below
    std::unordered_set<std::string> requestedThumbnails_;
    std::vector<std::pair<std::string, ThumbnailImage>> pendingUploadThumbnails_;
    std::deque<std::pair<std::string, fs::path>> thumbnailQueue_;
    std::condition_variable_any thumbnailCv_;
    std::vector<std::jthread> thumbnailWorkers_;
};

} // namespace hdr::gui
