#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "TestUtil.h"
#include "core/PreviewPipeline.h"
#include "core/BracketGrouper.h"
#include "core/RawLoader.h"
#include "core/DngWriter.h"
#include <filesystem>

using namespace hdr;
namespace fs = std::filesystem;
using testutil::TempFile;

TEST_CASE("PreviewPipeline histogram computation", "[preview]") {
    const int W = 100, H = 100;
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t* p = rgba.data() + (static_cast<size_t>(y) * W + x) * 4;
            p[0] = static_cast<uint8_t>(x * 255 / (W - 1));
            p[1] = static_cast<uint8_t>(y * 255 / (H - 1));
            p[2] = 128;
            p[3] = 255;
        }
    }

    HistogramData hist = PreviewPipeline::computeHistogramRgba8(rgba.data(), W, H);

    REQUIRE(hist.xValues.size() == 256);
    REQUIRE(hist.r.size() == 256);
    REQUIRE(hist.g.size() == 256);
    REQUIRE(hist.b.size() == 256);
    REQUIRE(hist.luma.size() == 256);

    // Blue is constant 128: the tallest bin, normalised to 1
    CHECK(hist.b[128] == Catch::Approx(1.0));
    CHECK(hist.b[0] == 0.0);
    double sumR = 0.0, sumL = 0.0;
    for (int i = 0; i < 256; ++i) {
        sumR += hist.r[i];
        sumL += hist.luma[i];
        CHECK(hist.r[i] <= 1.0);
    }
    CHECK(sumR > 0.0);
    CHECK(sumL > 0.0);
}

TEST_CASE("PreviewPipeline empty image handling", "[preview]") {
    HistogramData hist = PreviewPipeline::computeHistogramRgba8(nullptr, 0, 0);

    REQUIRE(hist.xValues.size() == 256);
    for (int i = 0; i < 256; ++i) {
        CHECK(hist.r[i] == 0.0);
        CHECK(hist.g[i] == 0.0);
        CHECK(hist.b[i] == 0.0);
        CHECK(hist.luma[i] == 0.0);
    }
}

TEST_CASE("PreviewPipeline and Thumbnail Loading with Synthetic DNG", "[preview]") {
    const int W = 256;
    const int H = 256;
    RawImage origImg = testutil::makeMetaSource(W, H);
    ImageF cfa = testutil::makeCfaPattern(W, H);

    TempFile tempDng("test_preview_sample");

    DngWriteParams writeParams;
    writeParams.bits = FloatBits::F32;
    writeParams.compress = true;
    writeParams.embedPreview = true;

    auto ws = DngWriter::writeFloatDng(tempDng.path, cfa, W, H, origImg, writeParams);
    REQUIRE(ws.ok);

    // Test preview generation from file
    auto [sPrev, imgPrev] = PreviewPipeline::generateSingleFramePreview(tempDng.path);
    CHECK(sPrev.ok);
    CHECK(!imgPrev.empty());
    CHECK(imgPrev.width == W / 2);
    CHECK(imgPrev.height == H / 2);

    // Test thumbnail extraction
    auto [sThumb, thumb] = RawLoader::loadThumbnail(tempDng.path, 480);
    CHECK(sThumb.ok);
    CHECK(!thumb.empty());
    CHECK(thumb.width > 0);
    CHECK(thumb.height > 0);
    CHECK(thumb.rgba.size() == static_cast<size_t>(thumb.width) * thumb.height * 4);

    // Test half-res loading
    auto [sHalf, imgHalf] = RawLoader::loadHalfRes(tempDng.path);
    CHECK(sHalf.ok);
    CHECK(imgHalf.width > 0);
    CHECK(imgHalf.height > 0);

    PreviewPipeline::clearFrameCache();
}

TEST_CASE("Merged preview rejects frames of different size instead of reading out of bounds", "[preview]") {
    TempFile a("test_prev_a"), b("test_prev_b");
    DngWriteParams params;
    params.bits = FloatBits::Int16;
    RawImage metaA = testutil::makeMetaSource(128, 128);
    RawImage metaB = testutil::makeMetaSource(96, 64);
    REQUIRE(DngWriter::writeFloatDng(a.path, testutil::makeCfaPattern(128, 128), 128, 128, metaA, params).ok);
    REQUIRE(DngWriter::writeFloatDng(b.path, testutil::makeCfaPattern(96, 64), 96, 64, metaB, params).ok);

    BracketGroup group;
    group.name = "mixed";
    FileEntry fa, fb;
    fa.path = a.path;
    fb.path = b.path;
    group.files = {fa, fb};

    auto [status, img] = PreviewPipeline::generateMergedPreview(group, MergeSettings{});
    CHECK_FALSE(status.ok);
    CHECK(img.empty());
    PreviewPipeline::clearFrameCache();
}
