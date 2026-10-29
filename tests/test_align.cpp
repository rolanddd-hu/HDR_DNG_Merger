#include <catch2/catch_test_macros.hpp>
#include "core/Types.h"
#include "core/RawImage.h"
#include "core/ImageAligner.h"
#include "core/MergeEngine.h"
#include "core/RawLoader.h"
#include "core/ExposureEstimator.h"
#include <cmath>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

TEST_CASE("Ward MTB Alignment - Synthetic Translation", "[align]") {
    const int W = 512;
    const int H = 512;

    hdr::RawImage baseImg;
    baseImg.width = W;
    baseImg.height = H;
    baseImg.whiteLevel = 65535.f;
    baseImg.blackLevel = {0.f, 0.f, 0.f, 0.f};
    baseImg.cfa = hdr::CfaPattern{}; // RGGB
    baseImg.data = hdr::ImageU16(W, H);

    // Create an asymmetric non-periodic pattern (off-center circle + linear ramp)
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float val = 0.10f + 0.0004f * x + 0.0003f * y;
            float dx = static_cast<float>(x - 190);
            float dy = static_cast<float>(y - 310);
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < 110.f) {
                val = 0.85f - (dist / 110.f) * 0.35f;
            }
            baseImg.data.at(y, x) = static_cast<uint16_t>(val * 65535.f);
        }
    }

    // Shift target image by (trueShiftX = +6, trueShiftY = -4) (even numbers, preserving Bayer CFA)
    const int trueShiftX = 6;
    const int trueShiftY = -4;

    hdr::RawImage shiftedImg = baseImg;
    shiftedImg.data = hdr::ImageU16(W, H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int srcY = y - trueShiftY;
            int srcX = x - trueShiftX;
            if (srcY >= 0 && srcY < H && srcX >= 0 && srcX < W) {
                shiftedImg.data.at(y, x) = baseImg.data.at(srcY, srcX);
            } else {
                shiftedImg.data.at(y, x) = static_cast<uint16_t>(0.20f * 65535.f);
            }
        }
    }

    // Also change exposure scale on shifted image to test exposure invariance
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float cur = shiftedImg.normalizedAt(y, x);
            shiftedImg.data.at(y, x) = static_cast<uint16_t>(std::clamp(cur * 0.7f * 65535.f, 0.0f, 65535.f));
        }
    }

    auto shift = hdr::ImageAligner::alignBayer(shiftedImg, baseImg, 64);

    // Shifts must always be even numbers
    CHECK(shift.dx % 2 == 0);
    CHECK(shift.dy % 2 == 0);

    // The detected shift of shiftedImg relative to baseImg should recover true shift
    CHECK(shift.dx == trueShiftX);
    CHECK(shift.dy == trueShiftY);
}

TEST_CASE("Ward MTB Alignment - Multi-Frame Bracket", "[align]") {
    const int W = 256;
    const int H = 256;

    hdr::RawImage refImg;
    refImg.width = W;
    refImg.height = H;
    refImg.whiteLevel = 65535.f;
    refImg.blackLevel = {0.f, 0.f, 0.f, 0.f};
    refImg.cfa = hdr::CfaPattern{}; // RGGB
    refImg.exposure.exposureTime = 0.001f;
    refImg.exposure.iso = 100.f;
    refImg.data = hdr::ImageU16(W, H);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float val = 0.15f + 0.0008f * x + 0.0006f * y;
            float dx = static_cast<float>(x - 120);
            float dy = static_cast<float>(y - 140);
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < 50.f) {
                val = 0.85f - (dist / 50.f) * 0.35f;
            }
            refImg.data.at(y, x) = static_cast<uint16_t>(val * 65535.f);
        }
    }

    auto makeShifted = [&](const hdr::RawImage& src, int sx, int sy, float expScale) {
        hdr::RawImage out = src;
        out.exposure.exposureTime = src.exposure.exposureTime * expScale;
        out.data = hdr::ImageU16(W, H);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                int srcY = y - sy;
                int srcX = x - sx;
                float v = 0.2f;
                if (srcY >= 0 && srcY < H && srcX >= 0 && srcX < W) {
                    v = src.normalizedAt(srcY, srcX);
                }
                out.data.at(y, x) = static_cast<uint16_t>(std::clamp(v * expScale * 65535.f, 0.f, 65535.f));
            }
        }
        return out;
    };

    std::vector<hdr::RawImage> frames;
    frames.push_back(refImg);                                        // Frame 0: (0, 0), 1x exp
    frames.push_back(makeShifted(refImg, 4, -2, 0.5f));              // Frame 1: (+4, -2), 0.5x exp
    frames.push_back(makeShifted(refImg, -2, 2, 2.0f));              // Frame 2: (-2, +2), 2x exp

    int refIdx = 0;
    auto shifts = hdr::ImageAligner::alignBracket(frames, refIdx, 64);

    REQUIRE(shifts.size() == 3);
    CHECK(shifts[0].dx == 0);
    CHECK(shifts[0].dy == 0);
    CHECK(shifts[1].dx == 4);
    CHECK(shifts[1].dy == -2);
    CHECK(shifts[2].dx == -2);
    CHECK(shifts[2].dy == 2);

    // Merge with alignment enabled
    std::vector<double> exposures = {1.0, 0.5, 2.0};
    hdr::MergeSettings settings;
    settings.align = hdr::AlignMode::Translation;
    settings.maxAlignShift = 64;
    settings.referenceFrame = refIdx;

    auto result = hdr::MergeEngine::merge(frames, exposures, settings);
    REQUIRE(result.status.ok);
    REQUIRE(result.shifts.size() == 3);
    CHECK(result.shifts[0].first == 0);
    CHECK(result.shifts[0].second == 0);
    CHECK(result.shifts[1].first == 4);
    CHECK(result.shifts[1].second == -2);
    CHECK(result.cfa.width == W);
    CHECK(result.cfa.height == H);

    // Only the area covered by every shifted frame is reported as valid
    CHECK(result.validRect == std::array<int, 4>{2, 2, W - 4, H - 2});
}
