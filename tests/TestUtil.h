#pragma once
// Shared helpers for the unit tests.

#include "core/RawImage.h"
#include <filesystem>
#include <random>
#include <string>

namespace testutil {

namespace fs = std::filesystem;

// Unique per call, so tests can run in parallel (ctest -j) without clobbering each other.
inline fs::path uniqueTempPath(const std::string& stem, const std::string& ext = ".dng") {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return fs::temp_directory_path() / (stem + "_" + std::to_string(rng()) + ext);
}

// Removes the file when the test scope ends (also on failure).
struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& stem, const std::string& ext = ".dng") : path(uniqueTempPath(stem, ext)) {}
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

struct TempDir {
    fs::path path;
    explicit TempDir(const char* stem) : path(uniqueTempPath(stem, "")) { fs::create_directories(path); }
    explicit TempDir(const fs::path& exactPath) : path(exactPath) { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Metadata source used by the DNG writer tests (14-bit, black 256, RGGB).
inline hdr::RawImage makeMetaSource(int w, int h) {
    hdr::RawImage img;
    img.width = w;
    img.height = h;
    img.cfa = hdr::CfaPattern{}; // RGGB
    img.whiteLevel = 16383.f;
    img.blackLevel = {256.f, 256.f, 256.f, 256.f};
    img.exposure.iso = 100.f;
    img.exposure.exposureTime = 0.01f;
    img.exposure.fNumber = 2.8f;
    img.exposure.make = "TestCamera";
    img.exposure.model = "TestModel";
    img.colorMatrix1 = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    img.calibrationIlluminant1 = 21;
    img.asShotNeutral = {1.f, 1.f, 1.f};
    img.data = hdr::ImageU16(w, h);
    img.defaultCropOrigin = {0, 0};
    img.defaultCropSize = {w, h};
    img.activeArea = {0, 0, h, w};
    return img;
}

// Deterministic CFA test pattern in [0, 1).
inline hdr::ImageF makeCfaPattern(int w, int h) {
    hdr::ImageF cfa(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            cfa.at(y, x) = static_cast<float>((x + y) % 100) / 100.0f;
        }
    }
    return cfa;
}

}  // namespace testutil
