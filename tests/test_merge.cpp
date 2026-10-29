#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "core/Types.h"
#include "core/RawImage.h"
#include "core/ExposureEstimator.h"
#include "core/MergeEngine.h"
#include "core/FileScanner.h"
#include "core/BracketGrouper.h"
#include "core/RawLoader.h"
#include "core/MergeJob.h"
#include <cmath>
#include <filesystem>

TEST_CASE("Synthetic Bracket Merge Accuracy", "[merge]") {
    const int W = 64;
    const int H = 64;

    // Create a smooth ground truth radiance map in [0.01, 4.0]
    std::vector<float> groundTruth(W * H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float normX = static_cast<float>(x) / (W - 1);
            groundTruth[y * W + x] = 0.05f + 3.95f * normX;
        }
    }

    // 3 brackets with exposures: 0.25 (dark, recovers highlights), 1.0 (medium), 4.0 (bright, recovers shadows)
    std::vector<double> exposures = {0.25, 1.0, 4.0};
    std::vector<hdr::RawImage> frames;

    for (size_t i = 0; i < exposures.size(); ++i) {
        hdr::RawImage frame;
        frame.width = W;
        frame.height = H;
        frame.data = hdr::ImageU16(W, H);
        frame.whiteLevel = 65535.f;
        frame.blackLevel = {0.f, 0.f, 0.f, 0.f};
        frame.exposure.exposureTime = exposures[i] / 1000.0;
        frame.exposure.fNumber = 2.0;
        frame.exposure.iso = 100.0;

        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float v = groundTruth[y * W + x] * static_cast<float>(exposures[i]) / 4.0f; // Scale so max radiance lands <= 1.0
                if (v > 1.0f) v = 1.0f; // Clip
                frame.data.at(y, x) = static_cast<uint16_t>(v * 65535.f);
            }
        }
        frames.push_back(frame);
    }

    // Run exposure estimator
    auto estimatedExp = hdr::ExposureEstimator::estimateExposures(frames);
    REQUIRE(estimatedExp.size() == 3);

    // Verify relative ratios: ratio of frame 1 to frame 0 should be approx 4.0
    double ratio01 = estimatedExp[1] / estimatedExp[0];
    double ratio12 = estimatedExp[2] / estimatedExp[1];
    CHECK_THAT(ratio01, Catch::Matchers::WithinRel(4.0, 0.05));
    CHECK_THAT(ratio12, Catch::Matchers::WithinRel(4.0, 0.05));

    // Run merge
    hdr::MergeSettings settings;
    settings.clipThreshold = 0.90f;
    settings.featherRadius = 2;

    auto result = hdr::MergeEngine::merge(frames, estimatedExp, settings);
    REQUIRE(result.status.ok);
    REQUIRE(result.width == W);
    REQUIRE(result.height == H);

    // Verify output properties
    double mse = 0.0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float val = result.cfa.at(y, x);
            CHECK_FALSE(std::isnan(val));
            CHECK_FALSE(std::isinf(val));
            CHECK(val >= 0.0f);
            CHECK(val <= 1.0f);

            // Compare to expected scaled radiance (relative to darkest frame: 0.25 / 4.0 = 1/16)
            float expected = groundTruth[y * W + x] / 16.0f;
            float diff = val - expected;
            mse += diff * diff;
        }
    }
    mse /= (W * H);
    double psnr = 10.0 * std::log10(1.0 / std::max(mse, 1e-10));
    CHECK(psnr > 30.0); // High accuracy reconstruction
}

#include "core/RawLoader.h"
#include <filesystem>
#include <iostream>

TEST_CASE("Filename Token Formatting", "[tokens]") {
    hdr::BracketGroup group;
    group.id = 1;
    group.name = "Bracket 001";

    hdr::FileEntry f1;
    f1.path = "sample/RAW_0001.dng";
    f1.info.exposureTime = 1.0 / 250.0;
    f1.info.fNumber = 2.8;
    f1.info.iso = 100.0;
    f1.info.make = "Camera";
    f1.info.model = "ModelX";
    f1.info.dateTime = "2026:10:08 15:30:00";
    f1.enabled = true;

    hdr::FileEntry f2;
    f2.path = "sample/RAW_0002.dng";
    f2.info.exposureTime = 1.0 / 1000.0;
    f2.info.fNumber = 2.8;
    f2.info.iso = 100.0;
    f2.enabled = true;

    hdr::FileEntry f3;
    f3.path = "sample/RAW_0003.dng";
    f3.info.exposureTime = 1.0 / 60.0;
    f3.info.fNumber = 2.8;
    f3.info.iso = 100.0;
    f3.enabled = true;

    group.files = {f1, f2, f3};
    group.referenceIndex = 0;

    CHECK(hdr::MergeJob::formatOutputFilename("{first}_HDR", group, 0) == "RAW_0001_HDR.dng");
    CHECK(hdr::MergeJob::formatOutputFilename("{last}_HDR", group, 0) == "RAW_0003_HDR.dng");
    CHECK(hdr::MergeJob::formatOutputFilename("{ref}_HDR_{iso}_{shutter}_{fnumber}", group, 0) == "RAW_0001_HDR_100_1_250s_f2_8.dng");
    CHECK(hdr::MergeJob::formatOutputFilename("{camera}_{date}_{index}_{n}", group, 0) == "Camera_ModelX_20261008_153000_1_3.dng");
}

TEST_CASE("Motion Deghosting Detection", "[deghost]") {
    const int W = 32;
    const int H = 32;

    std::vector<hdr::RawImage> frames(2);
    for (int i = 0; i < 2; ++i) {
        frames[i].width = W;
        frames[i].height = H;
        frames[i].data = hdr::ImageU16(W, H);
        frames[i].whiteLevel = 65535.f;
        frames[i].blackLevel = {0.f, 0.f, 0.f, 0.f};
        frames[i].exposure.exposureTime = 1.0;
        frames[i].exposure.fNumber = 2.0;
        frames[i].exposure.iso = 100.0;

        // Static background at normalized 0.20
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                frames[i].data.at(y, x) = static_cast<uint16_t>(0.20f * 65535.f);
            }
        }
    }

    // In frame 1, create a moving bright object (value 0.85f) in region [12..20, 12..20]
    for (int y = 12; y < 20; ++y) {
        for (int x = 12; x < 20; ++x) {
            frames[1].data.at(y, x) = static_cast<uint16_t>(0.85f * 65535.f);
        }
    }

    std::vector<double> exposures = {1.0, 1.0};
    hdr::MergeSettings settings;
    settings.referenceFrame = 0;
    settings.align = hdr::AlignMode::None;

    // 1. Without deghosting: moving object causes ghosting
    settings.deghost = hdr::DeghostLevel::Off;
    auto resOff = hdr::MergeEngine::merge(frames, exposures, settings);
    REQUIRE(resOff.status.ok);
    CHECK(resOff.motionMask.empty());

    // 2. With Medium deghosting: motion divergence is detected
    settings.deghost = hdr::DeghostLevel::Medium;
    auto resMed = hdr::MergeEngine::merge(frames, exposures, settings);
    REQUIRE(resMed.status.ok);
    REQUIRE_FALSE(resMed.motionMask.empty());

    // Quad (7, 7) corresponds to raw pixels (14, 14) which is inside the moving region
    int qw = W / 2;
    CHECK(resMed.motionMask[7 * qw + 7] == 1);

    // In the moving area, the merge should lock to reference frame value (~0.20f)
    float blendedVal = resMed.cfa.at(14, 14);
    CHECK_THAT(blendedVal, Catch::Matchers::WithinAbs(0.20f, 0.05f));
}

TEST_CASE("Frame Exclusion Filtering", "[exclude]") {
    hdr::BracketGroup group;
    group.id = 1;

    hdr::FileEntry f1{"f1.dng", {1.0 / 1000.0, 2.8, 100.0}, {}, true};
    hdr::FileEntry f2{"f2.dng", {1.0 / 250.0, 2.8, 100.0}, {}, false}; // excluded
    hdr::FileEntry f3{"f3.dng", {1.0 / 60.0, 2.8, 100.0}, {}, true};

    group.files = {f1, f2, f3};

    // Resolved reference should skip f2
    int refIdx = group.getResolvedReferenceIndex();
    CHECK((refIdx == 0 || refIdx == 2));
    CHECK(group.files[refIdx].enabled == true);

    // Formatted name count token {n} should count enabled frames only (2)
    std::string name = hdr::MergeJob::formatOutputFilename("test_{n}", group, 0);
    CHECK(name == "test_2.dng");
}

namespace {

// Bracket of a smooth radiance ramp; `exifRatio` is the exposure-time ratio recorded in EXIF,
// `dataRatio` the ratio actually present in the pixel data.
std::vector<hdr::RawImage> makeRampBracket(int W, int H, double dataRatio, double exifRatio, std::vector<float>& truth) {
    truth.assign(static_cast<size_t>(W) * H, 0.f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            truth[static_cast<size_t>(y) * W + x] = 0.05f + 3.95f * static_cast<float>(x) / (W - 1);

    std::vector<hdr::RawImage> frames;
    for (int i = 0; i < 3; ++i) {
        hdr::RawImage frame;
        frame.width = W;
        frame.height = H;
        frame.data = hdr::ImageU16(W, H);
        frame.whiteLevel = 65535.f;
        frame.blackLevel = {0.f, 0.f, 0.f, 0.f};
        frame.cfa = hdr::CfaPattern{}; // RGGB: green never sits at even (x, y)
        frame.exposure.exposureTime = std::pow(exifRatio, i) / 1000.0;
        frame.exposure.fNumber = 2.0;
        frame.exposure.iso = 100.0;
        const double scale = std::pow(dataRatio, i) / 16.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                frame.data.at(y, x) = static_cast<uint16_t>(std::min(1.0, truth[static_cast<size_t>(y) * W + x] * scale) * 65535.0);
        frames.push_back(std::move(frame));
    }
    return frames;
}

double psnrVsTruth(const hdr::ImageF& cfa, const std::vector<float>& truth, int W, int H, float scale) {
    double mse = 0.0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double d = cfa.at(y, x) - truth[static_cast<size_t>(y) * W + x] * scale;
            mse += d * d;
        }
    mse /= static_cast<double>(W) * H;
    return 10.0 * std::log10(1.0 / std::max(mse, 1e-12));
}

} // namespace

TEST_CASE("Exposure estimator refines EXIF ratios on RGGB sensors", "[merge][estimator]") {
    std::vector<float> truth;
    // EXIF claims 5x steps, the data has 4x steps (within the 1 EV plausibility window)
    auto frames = makeRampBracket(768, 256, 4.0, 5.0, truth);
    auto exposures = hdr::ExposureEstimator::estimateExposures(frames);
    REQUIRE(exposures.size() == 3);
    CHECK_THAT(exposures[1] / exposures[0], Catch::Matchers::WithinRel(4.0, 0.03));
    CHECK_THAT(exposures[2] / exposures[1], Catch::Matchers::WithinRel(4.0, 0.03));
}

TEST_CASE("Weighted-average merge reconstructs radiance", "[merge]") {
    const int W = 128, H = 64;
    std::vector<float> truth;
    auto frames = makeRampBracket(W, H, 4.0, 4.0, truth);

    hdr::MergeSettings settings;
    settings.mode = hdr::MergeMode::WeightedAverage;
    settings.align = hdr::AlignMode::None;
    settings.featherRadius = 2;

    auto result = hdr::MergeEngine::merge(frames, {}, settings);  // exposures estimated by the engine
    REQUIRE(result.status.ok);
    REQUIRE(result.exposures.size() == 3);
    CHECK(psnrVsTruth(result.cfa, truth, W, H, 1.0f / 16.0f) > 30.0);
}

TEST_CASE("Merge rejects inconsistent frames", "[merge]") {
    std::vector<hdr::RawImage> frames(2);
    frames[0].width = 32; frames[0].height = 32; frames[0].data = hdr::ImageU16(32, 32);
    frames[1].width = 16; frames[1].height = 32; frames[1].data = hdr::ImageU16(16, 32);
    auto res = hdr::MergeEngine::merge(frames, {1.0, 2.0}, hdr::MergeSettings{});
    CHECK_FALSE(res.status.ok);

    frames[1] = frames[0];
    frames[1].cfa = frames[0].cfa.shifted(0, 1);
    res = hdr::MergeEngine::merge(frames, {1.0, 2.0}, hdr::MergeSettings{});
    CHECK_FALSE(res.status.ok);

    frames[1] = frames[0];
    res = hdr::MergeEngine::merge(frames, {1.0, -1.0}, hdr::MergeSettings{});
    CHECK_FALSE(res.status.ok);
}
