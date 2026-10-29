#pragma once
// Shared value types for the HDR DNG Merger core library.

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hdr {

namespace fs = std::filesystem;

struct Status {
    bool ok = true;
    std::string message;

    static Status success() { return {true, ""}; }
    static Status error(std::string msg) { return {false, std::move(msg)}; }
    explicit operator bool() const { return ok; }
};

template <typename T>
struct Image {
    int width = 0;
    int height = 0;
    std::vector<T> data;

    Image() = default;
    Image(int w, int h) : width(w), height(h), data(static_cast<size_t>(w) * h) {}
    Image(int w, int h, T val) : width(w), height(h), data(static_cast<size_t>(w) * h, val) {}

    T* row(int y) { return data.data() + static_cast<size_t>(y) * width; }
    const T* row(int y) const { return data.data() + static_cast<size_t>(y) * width; }

    T& at(int y, int x) { return data[static_cast<size_t>(y) * width + x]; }
    const T& at(int y, int x) const { return data[static_cast<size_t>(y) * width + x]; }

    bool empty() const { return data.empty() || width <= 0 || height <= 0; }
    size_t size() const { return data.size(); }
};

using ImageF = Image<float>;
using ImageU16 = Image<uint16_t>;
using ImageU8 = Image<uint8_t>;

struct ImageRgbF {
    int width = 0;
    int height = 0;
    std::vector<float> data; // 3 * width * height interleaved (R, G, B)

    ImageRgbF() = default;
    ImageRgbF(int w, int h) : width(w), height(h), data(static_cast<size_t>(w) * h * 3) {}
    ImageRgbF(int w, int h, float val) : width(w), height(h), data(static_cast<size_t>(w) * h * 3, val) {}

    float* pixel(int y, int x) { return data.data() + (static_cast<size_t>(y) * width + x) * 3; }
    const float* pixel(int y, int x) const { return data.data() + (static_cast<size_t>(y) * width + x) * 3; }

    bool empty() const { return data.empty() || width <= 0 || height <= 0; }
};

struct ThumbnailImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // 4 * width * height
    bool empty() const { return rgba.empty() || width <= 0 || height <= 0; }
};

enum class ListThumbnailSize {
    Small = 0,
    Medium = 1,
    Large = 2
};

inline const char* listThumbnailSizeName(ListThumbnailSize sz) {
    switch (sz) {
        case ListThumbnailSize::Small: return "Small";
        case ListThumbnailSize::Medium: return "Medium";
        case ListThumbnailSize::Large: return "Large";
        default: return "Unknown";
    }
}

// 2x2 Bayer pattern. Index = (y&1)*2 + (x&1). Values: 0=R, 1=G, 2=B.
struct CfaPattern {
    std::array<uint8_t, 4> c{0, 1, 1, 2};  // RGGB

    uint8_t at(int y, int x) const { return c[((y & 1) << 1) | (x & 1)]; }
    // Pattern as seen when the origin is moved to (dy, dx).
    CfaPattern shifted(int dy, int dx) const {
        CfaPattern p;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) p.c[y * 2 + x] = at(y + dy, x + dx);
        return p;
    }
    std::string name() const {
        static const char k[] = "RGB";
        return {k[c[0]], k[c[1]], k[c[2]], k[c[3]]};
    }
};

struct ExposureInfo {
    double exposureTime = 0.0;  // seconds
    double fNumber = 0.0;
    double iso = 0.0;
    double timestamp = 0.0;  // seconds since epoch (as parsed by LibRaw)
    std::string dateTime;    // "YYYY:MM:DD HH:MM:SS"
    std::string make, model;
    int width = 0, height = 0;  // visible size

    // Relative photometric exposure (proportional to the light reaching each raw value).
    double relativeExposure() const {
        const double t = exposureTime > 0 ? exposureTime : 1.0;
        const double s = iso > 0 ? iso : 100.0;
        const double n = fNumber > 0 ? fNumber : 1.0;
        return t * s / (n * n);
    }

    std::string formatExposure() const {
        if (exposureTime <= 0.0) return "Unknown";
        if (exposureTime < 0.5) {
            double inv = 1.0 / exposureTime;
            return "1/" + std::to_string(static_cast<int>(std::round(inv))) + "s, ISO " +
                   std::to_string(static_cast<int>(std::round(iso)));
        }
        return std::to_string(static_cast<int>(exposureTime)) + "s, ISO " +
               std::to_string(static_cast<int>(std::round(iso)));
    }
};

// Colour data needed for previews / non-DNG output.
struct ColorInfo {
    std::array<double, 9> camXyz{};          // XYZ -> camera (3x3, row-major), LibRaw cam_xyz
    std::array<double, 3> camMul{1, 1, 1};   // as-shot WB multipliers, normalised G = 1
    std::array<double, 9> rgbCam{1, 0, 0, 0, 1, 0, 0, 0, 1};  // WB'd camera -> linear sRGB
    double baselineExposure = 0.0;           // from source DNG (EV)
};

// Int16 = 16-bit unsigned integer CFA (uncompressed strips) — the only form Capture One reads.
// F16/F24/F32 = floating-point CFA DNG (Adobe / LibRaw / darktable / RawTherapee only).
enum class FloatBits { Auto = 0, Int16 = 1, F16 = 16, F24 = 24, F32 = 32 };
enum class AlignMode { None = 0, Translation = 1 };
enum class MergeMode { BestExposure = 0, WeightedAverage = 1 };
enum class DeghostLevel { Off = 0, Low = 1, Medium = 2, High = 3 };
enum class ToneMapOp {
    None = 0,
    Reinhard = 1,
    Filmic = 2,
    ReinhardLuma = 3,
    Hable = 4,
    AgX = 5,
    Lottes = 6,
    Uchimura = 7,
    Count
};

// Preview / Lightroom slider values. Ranges: exposure [-5,5] EV, others [-100,100].
struct ToneParams {
    float exposure = 0.f;
    float highlights = 0.f;
    float shadows = 0.f;
    float whites = 0.f;
    float blacks = 0.f;
    float contrast = 0.f;
    float vibrance = 0.f;
    float saturation = 0.f;
    float temperature = 0.f;
    float tint = 0.f;
    ToneMapOp toneMap = ToneMapOp::Filmic;

    bool isNeutral() const {
        return exposure == 0 && highlights == 0 && shadows == 0 && whites == 0 && blacks == 0 &&
               contrast == 0 && vibrance == 0 && saturation == 0 && temperature == 0 && tint == 0;
    }
    bool operator==(const ToneParams&) const = default;
};

struct MergeSettings {
    AlignMode align = AlignMode::Translation;
    MergeMode mode = MergeMode::BestExposure;
    DeghostLevel deghost = DeghostLevel::Off;
    FloatBits bits = FloatBits::Auto;
    bool embedPreview = true;      // full-size 8-bit preview in IFD0 (otherwise a small thumbnail)
    bool writeLightroomSettings = true;  // export `tone` as XMP crs:* develop settings
    ToneParams tone;               // slider values exported when writeLightroomSettings is set
    float clipThreshold = 0.92f;   // normalised raw value treated as saturated
    int featherRadius = 8;         // in 2x2 quads
    int maxAlignShift = 128;       // raw pixels
    int referenceFrame = -1;       // -1 = auto (middle exposure)
    std::string namePattern = "{first}_HDR";
    bool skipExisting = false;
};

// progress in [0,1], short stage description
using ProgressFn = std::function<void(float progress, const std::string& stage)>;

}  // namespace hdr
