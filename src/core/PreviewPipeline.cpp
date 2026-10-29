#include "PreviewPipeline.h"
#include "CfaUtil.h"
#include "MergeEngine.h"
#include "Parallel.h"
#include "RawLoader.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

namespace hdr {

namespace {

// LRU cache of decoded raw frames keyed by path; entries are invalidated when the file changes.
class FrameCache {
public:
    std::shared_ptr<const RawImage> find(const fs::path& path) {
        const Key key = makeKey(path);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key.id);
        if (it == map_.end()) return nullptr;
        if (it->second.mtime != key.mtime) {
            erase(it);
            return nullptr;
        }
        lru_.splice(lru_.end(), lru_, it->second.lruPos);
        return it->second.image;
    }

    void insert(const fs::path& path, std::shared_ptr<const RawImage> image) {
        const Key key = makeKey(path);
        const uint64_t bytes = static_cast<uint64_t>(image->data.size()) * sizeof(uint16_t);
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = map_.find(key.id); it != map_.end()) erase(it);
        if (bytes > budget_) return;
        while (total_ + bytes > budget_ && !lru_.empty()) {
            erase(map_.find(lru_.front()));
        }
        lru_.push_back(key.id);
        map_[key.id] = Entry{std::move(image), key.mtime, bytes, std::prev(lru_.end())};
        total_ += bytes;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        lru_.clear();
        total_ = 0;
    }

    void setBudget(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        budget_ = bytes;
        while (total_ > budget_ && !lru_.empty()) erase(map_.find(lru_.front()));
    }

private:
    struct Key {
        fs::path::string_type id;
        fs::file_time_type mtime;
    };
    struct Entry {
        std::shared_ptr<const RawImage> image;
        fs::file_time_type mtime;
        uint64_t bytes = 0;
        std::list<fs::path::string_type>::iterator lruPos;
    };
    using Map = std::unordered_map<fs::path::string_type, Entry>;

    static Key makeKey(const fs::path& path) {
        std::error_code ec;
        Key k;
        k.id = fs::absolute(path, ec).lexically_normal().native();
        k.mtime = fs::last_write_time(path, ec);
        return k;
    }

    void erase(Map::iterator it) {
        if (it == map_.end()) return;
        total_ -= it->second.bytes;
        lru_.erase(it->second.lruPos);
        map_.erase(it);
    }

    std::mutex mutex_;
    std::list<fs::path::string_type> lru_;
    Map map_;
    uint64_t total_ = 0;
    uint64_t budget_ = 1ull << 30;
};

FrameCache& frameCache() {
    static FrameCache cache;
    return cache;
}

std::pair<Status, std::shared_ptr<const RawImage>> loadCached(const fs::path& path, std::stop_token stopToken) {
    if (auto cached = frameCache().find(path)) return {Status::success(), std::move(cached)};
    auto [status, img] = RawLoader::loadFull(path, stopToken);
    if (!status) return {status, nullptr};
    auto shared = std::make_shared<const RawImage>(std::move(img));
    frameCache().insert(path, shared);
    return {Status::success(), std::move(shared)};
}

} // namespace

HistogramData::HistogramData() {
    xValues.resize(kNumBins);
    for (int i = 0; i < kNumBins; ++i) {
        xValues[i] = static_cast<double>(i);
    }
    clear();
}

void HistogramData::clear() {
    r.assign(kNumBins, 0.0);
    g.assign(kNumBins, 0.0);
    b.assign(kNumBins, 0.0);
    luma.assign(kNumBins, 0.0);
}

void PreviewPipeline::clearFrameCache() {
    frameCache().clear();
}

void PreviewPipeline::setFrameCacheBudget(uint64_t bytes) {
    frameCache().setBudget(bytes);
}

std::pair<Status, ImageRgbF> PreviewPipeline::generateMergedPreview(
    const BracketGroup& group,
    const MergeSettings& settings,
    std::stop_token stopToken,
    std::vector<uint8_t>* outMotionMask,
    std::vector<uint8_t>* outSourceMap
) {
    if (group.files.empty()) {
        return {Status::error("Empty bracket group"), {}};
    }
    if (stopToken.stop_requested()) {
        return {Status::error("Cancelled"), {}};
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
        return {Status::error("No enabled frames in bracket group"), {}};
    }

    const int rawRefIdx = group.getResolvedReferenceIndex();
    int mappedRefIdx = -1;
    for (size_t i = 0; i < originalIndices.size(); ++i) {
        if (static_cast<int>(originalIndices[i]) == rawRefIdx) {
            mappedRefIdx = static_cast<int>(i);
            break;
        }
    }

    // Decode (or fetch from cache) all frames in parallel
    std::vector<std::shared_ptr<const RawImage>> frames(activeFiles.size());
    std::vector<Status> statuses(activeFiles.size());
    parallelFor(0, static_cast<int>(activeFiles.size()), 1, [&](int i) {
        auto [status, img] = loadCached(activeFiles[i].path, stopToken);
        statuses[i] = status;
        frames[i] = std::move(img);
    });
    if (stopToken.stop_requested()) return {Status::error("Cancelled"), {}};
    for (const Status& s : statuses) {
        if (!s) return {s, {}};
    }

    std::vector<const RawImage*> framePtrs;
    framePtrs.reserve(frames.size());
    for (const auto& f : frames) framePtrs.push_back(f.get());

    MergeSettings settingsForMerge = settings;
    settingsForMerge.referenceFrame = mappedRefIdx;

    // Size/CFA mismatches (e.g. a group joined across cameras) are rejected by the engine.
    MergeResult mergeRes = MergeEngine::merge(framePtrs, {}, settingsForMerge, nullptr, stopToken);
    if (!mergeRes.status) {
        return {mergeRes.status, {}};
    }

    if (outMotionMask) {
        *outMotionMask = std::move(mergeRes.motionMask);
    }
    if (outSourceMap) {
        *outSourceMap = std::move(mergeRes.sourceMap);
    }

    if (stopToken.stop_requested()) return {Status::error("Cancelled"), {}};

    const RawImage& refImg = *framePtrs[mappedRefIdx >= 0 ? mappedRefIdx : 0];
    const ImageF& cfa = mergeRes.cfa;
    ImageRgbF rgb = quadsToRgb(mergeRes.width, mergeRes.height, refImg.cfa, refImg.color, 1.0f,
                               [&](int y, int x) { return cfa.at(y, x); });
    return {Status::success(), std::move(rgb)};
}

std::pair<Status, ImageRgbF> PreviewPipeline::generateSingleFramePreview(
    const fs::path& framePath,
    std::stop_token stopToken
) {
    if (stopToken.stop_requested()) return {Status::error("Cancelled"), {}};
    auto [status, frame] = loadCached(framePath, stopToken);
    if (!status) return {status, {}};
    return {Status::success(), RawLoader::halfResRgb(*frame)};
}

HistogramData PreviewPipeline::computeHistogramRgba8(const uint8_t* rgba, int width, int height) {
    HistogramData hist;
    if (!rgba || width <= 0 || height <= 0) return hist;

    const size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = rgba + i * 4;
        hist.r[p[0]]++;
        hist.g[p[1]]++;
        hist.b[p[2]]++;
        const int lum = static_cast<int>(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] + 0.5f);
        hist.luma[std::clamp(lum, 0, HistogramData::kNumBins - 1)]++;
    }

    double maxCount = 1.0;
    for (int i = 0; i < HistogramData::kNumBins; ++i) {
        maxCount = std::max({maxCount, hist.r[i], hist.g[i], hist.b[i], hist.luma[i]});
    }
    for (int i = 0; i < HistogramData::kNumBins; ++i) {
        hist.r[i] /= maxCount;
        hist.g[i] /= maxCount;
        hist.b[i] /= maxCount;
        hist.luma[i] /= maxCount;
    }
    return hist;
}

} // namespace hdr
