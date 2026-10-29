#include "Settings.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace hdr {

namespace {

// Reads one field; a missing key or a value of the wrong type leaves `out` untouched.
template <typename T>
void readField(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end()) return;
    try {
        out = it->get<T>();
    } catch (const json::exception&) {
        spdlog::warn("Settings: ignoring invalid value for '{}'", key);
    }
}

template <typename E>
void readEnum(const json& j, const char* key, E& out) {
    int v = static_cast<int>(out);
    readField(j, key, v);
    out = static_cast<E>(v);
}

bool isValidBits(FloatBits b) {
    return b == FloatBits::Auto || b == FloatBits::Int16 || b == FloatBits::F16 ||
           b == FloatBits::F24 || b == FloatBits::F32;
}

} // namespace

fs::path AppSettings::getSettingsDirectory() {
#if defined(_WIN32)
    wchar_t* appData = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData != nullptr) {
        fs::path dir = fs::path(appData) / "HDR_DNG_Merger";
        free(appData);
        return dir;
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / ".config" / "HDR_DNG_Merger";
    }
#endif
    std::error_code ec;
    return fs::current_path(ec) / ".hdr_settings";
}

fs::path AppSettings::getSettingsFilePath() {
    return getSettingsDirectory() / "settings.json";
}

AppSettings AppSettings::load() {
    return loadFrom(getSettingsFilePath());
}

bool AppSettings::save() const {
    return saveTo(getSettingsFilePath());
}

void AppSettings::validate() {
    framesPerBracket = std::clamp(framesPerBracket, 2, 9);
    threadCount = std::clamp(threadCount, 0, 64);
    windowWidth = std::clamp(windowWidth, 800, 7680);
    windowHeight = std::clamp(windowHeight, 600, 4320);
    if (static_cast<int>(listThumbnailSize) < 0 || static_cast<int>(listThumbnailSize) > 2) {
        listThumbnailSize = ListThumbnailSize::Medium;
    }

    if (!isValidBits(merge.bits)) merge.bits = FloatBits::Auto;
    if (merge.align != AlignMode::None && merge.align != AlignMode::Translation) merge.align = AlignMode::Translation;
    if (merge.mode != MergeMode::BestExposure && merge.mode != MergeMode::WeightedAverage) merge.mode = MergeMode::BestExposure;
    if (static_cast<int>(merge.deghost) < 0 || static_cast<int>(merge.deghost) > 3) merge.deghost = DeghostLevel::Off;
    merge.clipThreshold = std::clamp(merge.clipThreshold, 0.5f, 0.999f);
    merge.featherRadius = std::clamp(merge.featherRadius, 0, 64);
    merge.maxAlignShift = std::clamp(merge.maxAlignShift, 2, 512);
    merge.referenceFrame = -1;
    if (merge.namePattern.empty() || merge.namePattern.size() > 200) merge.namePattern = "{first}_HDR";

    auto clampSlider = [](float& v) { v = std::isfinite(v) ? std::clamp(v, -100.f, 100.f) : 0.f; };
    tone.exposure = std::isfinite(tone.exposure) ? std::clamp(tone.exposure, -5.f, 5.f) : 0.f;
    clampSlider(tone.highlights);
    clampSlider(tone.shadows);
    clampSlider(tone.whites);
    clampSlider(tone.blacks);
    clampSlider(tone.contrast);
    clampSlider(tone.vibrance);
    clampSlider(tone.saturation);
    clampSlider(tone.temperature);
    clampSlider(tone.tint);
    if (static_cast<int>(tone.toneMap) < 0 || tone.toneMap >= ToneMapOp::Count) tone.toneMap = ToneMapOp::Filmic;
}

AppSettings AppSettings::loadFrom(const fs::path& file) {
    AppSettings s;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return s;
    }

    try {
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open()) return s;
        const json j = json::parse(f);
        if (!j.is_object()) return s;

        readField(j, "lastInputFolder", s.lastInputFolder);
        readField(j, "lastOutputFolder", s.lastOutputFolder);
        readField(j, "framesPerBracket", s.framesPerBracket);
        readField(j, "threadCount", s.threadCount);
        readField(j, "windowWidth", s.windowWidth);
        readField(j, "windowHeight", s.windowHeight);
        readField(j, "windowMaximized", s.windowMaximized);
        readField(j, "showListThumbnails", s.showListThumbnails);
        readEnum(j, "listThumbnailSize", s.listThumbnailSize);

        if (auto m = j.find("merge"); m != j.end() && m->is_object()) {
            readEnum(*m, "align", s.merge.align);
            readEnum(*m, "mode", s.merge.mode);
            readEnum(*m, "deghost", s.merge.deghost);
            readEnum(*m, "bits", s.merge.bits);
            readField(*m, "embedPreview", s.merge.embedPreview);
            readField(*m, "writeLightroomSettings", s.merge.writeLightroomSettings);
            readField(*m, "clipThreshold", s.merge.clipThreshold);
            readField(*m, "featherRadius", s.merge.featherRadius);
            readField(*m, "maxAlignShift", s.merge.maxAlignShift);
            readField(*m, "skipExisting", s.merge.skipExisting);
            readField(*m, "namePattern", s.merge.namePattern);
        }

        if (auto t = j.find("tone"); t != j.end() && t->is_object()) {
            readField(*t, "exposure", s.tone.exposure);
            readField(*t, "highlights", s.tone.highlights);
            readField(*t, "shadows", s.tone.shadows);
            readField(*t, "whites", s.tone.whites);
            readField(*t, "blacks", s.tone.blacks);
            readField(*t, "contrast", s.tone.contrast);
            readField(*t, "vibrance", s.tone.vibrance);
            readField(*t, "saturation", s.tone.saturation);
            readField(*t, "temperature", s.tone.temperature);
            readField(*t, "tint", s.tone.tint);
            readEnum(*t, "toneMap", s.tone.toneMap);
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse settings file {}: {}", file.filename().string(), e.what());
    }

    s.validate();
    return s;
}

bool AppSettings::saveTo(const fs::path& file) const {
    try {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);

        json j;
        j["lastInputFolder"] = lastInputFolder;
        j["lastOutputFolder"] = lastOutputFolder;
        j["framesPerBracket"] = framesPerBracket;
        j["threadCount"] = threadCount;
        j["windowWidth"] = windowWidth;
        j["windowHeight"] = windowHeight;
        j["windowMaximized"] = windowMaximized;
        j["showListThumbnails"] = showListThumbnails;
        j["listThumbnailSize"] = static_cast<int>(listThumbnailSize);

        j["merge"] = {
            {"align", static_cast<int>(merge.align)},
            {"mode", static_cast<int>(merge.mode)},
            {"deghost", static_cast<int>(merge.deghost)},
            {"bits", static_cast<int>(merge.bits)},
            {"embedPreview", merge.embedPreview},
            {"writeLightroomSettings", merge.writeLightroomSettings},
            {"clipThreshold", merge.clipThreshold},
            {"featherRadius", merge.featherRadius},
            {"maxAlignShift", merge.maxAlignShift},
            {"skipExisting", merge.skipExisting},
            {"namePattern", merge.namePattern}
        };

        j["tone"] = {
            {"exposure", tone.exposure},
            {"highlights", tone.highlights},
            {"shadows", tone.shadows},
            {"whites", tone.whites},
            {"blacks", tone.blacks},
            {"contrast", tone.contrast},
            {"vibrance", tone.vibrance},
            {"saturation", tone.saturation},
            {"temperature", tone.temperature},
            {"tint", tone.tint},
            {"toneMap", static_cast<int>(tone.toneMap)}
        };

        // Replace atomically so a crash mid-write never leaves a truncated settings file.
        fs::path tmp = file;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return false;
            f << j.dump(2);
            if (!f.good()) return false;
        }
        fs::rename(tmp, file, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("Failed to write settings file {}: {}", file.filename().string(), e.what());
        return false;
    }
}

} // namespace hdr
