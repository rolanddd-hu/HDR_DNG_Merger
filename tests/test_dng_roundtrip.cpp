#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "TestUtil.h"
#include "core/Types.h"
#include "core/RawLoader.h"
#include "core/DngWriter.h"
#include "core/TiffUtil.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using testutil::TempFile;

namespace {

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) v = mant ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(mant | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// DNG 24-bit float, little-endian bytes: 1 sign, 7 exponent (bias 63), 16 mantissa bits.
float fp24ToFloat(const uint8_t* b) {
    const uint32_t v = b[0] | (b[1] << 8) | (b[2] << 16);
    const uint32_t sign = (v >> 23) & 1, exp = (v >> 16) & 0x7f, mant = v & 0xffff;
    if (exp == 0) return sign ? -0.f : 0.f;
    const float f = std::ldexp(1.0f + static_cast<float>(mant) / 65536.0f, static_cast<int>(exp) - 63);
    return sign ? -f : f;
}

// Decodes the raw SubIFD of a DNG written by DngWriter with libtiff (independent of our encoder).
std::vector<float> readRawPlane(const fs::path& path, int& w, int& h, uint16_t& bps, uint16_t& sampleFormat) {
    std::vector<float> out;
    // "c": no strip chopping, so a single uncompressed strip is read as one strip
    TIFF* tif = hdr::openTiff(path, "rc");
    REQUIRE(tif);
    uint16_t subCount = 0;
    uint64_t* subOffsets = nullptr;
    REQUIRE(TIFFGetField(tif, TIFFTAG_SUBIFD, &subCount, &subOffsets));
    REQUIRE(TIFFSetSubDirectory(tif, subOffsets[0]));
    uint32_t uw = 0, uh = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &uw);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &uh);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
    w = static_cast<int>(uw);
    h = static_cast<int>(uh);
    out.assign(static_cast<size_t>(w) * h, 0.f);
    const int bytesPer = bps / 8;

    if (TIFFIsTiled(tif)) {
        uint32_t tw = 0, th = 0;
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
        std::vector<uint8_t> tile(TIFFTileSize(tif));
        for (uint32_t ty = 0; ty < uh; ty += th) {
            for (uint32_t tx = 0; tx < uw; tx += tw) {
                REQUIRE(TIFFReadEncodedTile(tif, TIFFComputeTile(tif, tx, ty, 0, 0), tile.data(), static_cast<tmsize_t>(tile.size())) > 0);
                for (uint32_t y = 0; y < th && ty + y < uh; ++y) {
                    for (uint32_t x = 0; x < tw && tx + x < uw; ++x) {
                        const uint8_t* p = tile.data() + (static_cast<size_t>(y) * tw + x) * bytesPer;
                        float v = 0.f;
                        if (bps == 16) {
                            uint16_t hbits;
                            std::memcpy(&hbits, p, 2);
                            v = halfToFloat(hbits);
                        } else if (bps == 24) {
                            v = fp24ToFloat(p);
                        } else {
                            std::memcpy(&v, p, 4);
                        }
                        out[static_cast<size_t>(ty + y) * w + tx + x] = v;
                    }
                }
            }
        }
    } else {
        std::vector<uint8_t> strip(TIFFStripSize(tif));
        REQUIRE(strip.size() >= out.size() * 2);
        REQUIRE(TIFFReadEncodedStrip(tif, 0, strip.data(), static_cast<tmsize_t>(strip.size())) > 0);
        for (size_t i = 0; i < out.size(); ++i) {
            uint16_t v;
            std::memcpy(&v, strip.data() + i * 2, 2);
            out[i] = v / 65535.f;
        }
    }
    TIFFClose(tif);
    return out;
}

} // namespace

TEST_CASE("DNG Write and Readback Roundtrip", "[dng][roundtrip]") {
    const int W = 256;
    const int H = 256;
    hdr::RawImage origImg = testutil::makeMetaSource(W, H);
    hdr::ImageF cfa = testutil::makeCfaPattern(W, H);

    TempFile tempDng("test_roundtrip");
    hdr::DngWriteParams params;
    params.bits = hdr::FloatBits::F24; // Test 24-bit float
    params.compress = true;
    params.embedPreview = true;

    auto writeStatus = hdr::DngWriter::writeFloatDng(tempDng.path, cfa, W, H, origImg, params);
    REQUIRE(writeStatus.ok);
    REQUIRE(fs::exists(tempDng.path));
    CHECK(fs::file_size(tempDng.path) > 1000);

    // Read back metadata
    auto [metaStatus, metaInfo] = hdr::RawLoader::loadMetadata(tempDng.path);
    REQUIRE(metaStatus.ok);
    CHECK(metaInfo.width == W);
    CHECK(metaInfo.height == H);
}

TEST_CASE("Float and integer CFA samples survive encoding", "[dng][roundtrip]") {
    const auto [bits, compress] = GENERATE(
        std::pair{hdr::FloatBits::F16, true}, std::pair{hdr::FloatBits::F16, false},
        std::pair{hdr::FloatBits::F24, true}, std::pair{hdr::FloatBits::F24, false},
        std::pair{hdr::FloatBits::F32, true}, std::pair{hdr::FloatBits::F32, false},
        std::pair{hdr::FloatBits::Int16, false});
    CAPTURE(static_cast<int>(bits), compress);

    // Non-multiple of the tile size to exercise edge tiles
    const int W = 300, H = 270;
    hdr::RawImage meta = testutil::makeMetaSource(W, H);
    hdr::ImageF cfa(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            cfa.at(y, x) = 0.001f + 0.998f * static_cast<float>((x * 7 + y * 13) % 997) / 997.f;

    TempFile file("test_formats");
    hdr::DngWriteParams params;
    params.bits = bits;
    params.compress = compress;
    REQUIRE(hdr::DngWriter::writeFloatDng(file.path, cfa, W, H, meta, params).ok);

    int w = 0, h = 0;
    uint16_t bps = 0, fmt = 0;
    const auto decoded = readRawPlane(file.path, w, h, bps, fmt);
    REQUIRE(w == W);
    REQUIRE(h == H);
    CHECK(fmt == (bits == hdr::FloatBits::Int16 ? SAMPLEFORMAT_UINT : SAMPLEFORMAT_IEEEFP));

    // Floats: relative error of the mantissa; 16-bit integer: absolute quantisation step.
    const bool isInt = bits == hdr::FloatBits::Int16;
    const float tol = bits == hdr::FloatBits::F16 ? 1e-3f : (bits == hdr::FloatBits::F24 ? 5e-5f : (isInt ? 0.51f / 65535.f : 0.f));
    float maxErr = 0.f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float err = std::abs(decoded[static_cast<size_t>(y) * W + x] - cfa.at(y, x));
            maxErr = std::max(maxErr, isInt ? err : err / cfa.at(y, x));
        }
    }
    CHECK(maxErr <= tol + 1e-7f);

    // LibRaw (used by darktable/RawTherapee and by our own loader) must decode what merges write.
    // It has no reader for uncompressed float DNGs, which is why merges always compress floats.
    if (bits != hdr::FloatBits::Int16 && !compress) return;
    auto [status, img] = hdr::RawLoader::loadFull(file.path);
    REQUIRE(status.ok);
    uint16_t maxRaw = 0;
    for (uint16_t v : img.data.data) maxRaw = std::max(maxRaw, v);
    CHECK(maxRaw > 0);
}

TEST_CASE("DNG geometry, preview and identity tags", "[dng]") {
    const int W = 400, H = 300;
    hdr::RawImage meta = testutil::makeMetaSource(W, H);
    meta.activeArea = {8, 12, 308, 412};  // source sensor coordinates must not leak into the output
    meta.defaultCropOrigin = {10, 6};
    meta.defaultCropSize = {380, 288};
    meta.color.baselineExposure = 2.5;
    meta.forwardMatrix1 = {0.7f, 0.2f, 0.1f, 0.3f, 0.6f, 0.1f, 0.0f, 0.1f, 0.9f};
    meta.opcodeList1 = {0, 0, 0, 1, 2, 3};
    meta.opcodeList2 = {0, 0, 0, 1, 4, 5, 6};
    meta.opcodeList3 = {0, 0, 0, 1, 7};
    const hdr::ImageF cfa = testutil::makeCfaPattern(W, H);

    TempFile a("test_geom_a"), b("test_geom_b");
    hdr::DngWriteParams params;
    params.bits = hdr::FloatBits::F32;
    REQUIRE(hdr::DngWriter::writeFloatDng(a.path, cfa, W, H, meta, params).ok);
    REQUIRE(hdr::DngWriter::writeFloatDng(b.path, cfa, W, H, meta, params).ok);

    auto readIds = [](const fs::path& p, std::vector<uint8_t>& id, uint32_t& pw, uint32_t& ph, double& be,
                      std::array<uint32_t, 4>& area) {
        TIFF* tif = hdr::openTiff(p, "r");
        REQUIRE(tif);
        uint8_t* raw = nullptr;
        REQUIRE(TIFFGetField(tif, TIFFTAG_RAWDATAUNIQUEID, &raw));
        id.assign(raw, raw + 16);
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &pw);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &ph);
        REQUIRE(hdr::getTiffFloat(tif, TIFFTAG_BASELINEEXPOSURE, be));
        uint16_t subCount = 0;
        uint64_t* subOffsets = nullptr;
        REQUIRE(TIFFGetField(tif, TIFFTAG_SUBIFD, &subCount, &subOffsets));
        REQUIRE(TIFFSetSubDirectory(tif, subOffsets[0]));
        uint32_t* aa = nullptr;
        REQUIRE(TIFFGetField(tif, TIFFTAG_ACTIVEAREA, &aa));
        area = {aa[0], aa[1], aa[2], aa[3]};
        TIFFClose(tif);
    };

    std::vector<uint8_t> idA, idB;
    uint32_t pw = 0, ph = 0;
    double be = 0.0;
    std::array<uint32_t, 4> area{};
    readIds(a.path, idA, pw, ph, be, area);
    readIds(b.path, idB, pw, ph, be, area);

    CHECK(idA != idB);                                       // fresh RawDataUniqueID per file
    CHECK(pw * 3 == ph * 4);                                 // 4:3 preview stays 4:3 (200x150)
    CHECK_THAT(be, Catch::Matchers::WithinAbs(2.5, 1e-4));
    CHECK(area == std::array<uint32_t, 4>{0, 0, static_cast<uint32_t>(H), static_cast<uint32_t>(W)});

    // Tags libtiff does not know natively are written and read back
    auto [status, img] = hdr::RawLoader::loadFull(a.path);
    REQUIRE(status.ok);
    CHECK(img.opcodeList1 == meta.opcodeList1);
    CHECK(img.opcodeList2 == meta.opcodeList2);
    CHECK(img.opcodeList3 == meta.opcodeList3);
    for (int i = 0; i < 9; ++i) {
        CHECK_THAT(img.forwardMatrix1[i], Catch::Matchers::WithinAbs(meta.forwardMatrix1[i], 1e-4));
    }
    CHECK(img.defaultCropOrigin == std::array<int, 2>{10, 6});
    CHECK(img.defaultCropSize == std::array<int, 2>{380, 288});
    CHECK_THAT(img.color.baselineExposure, Catch::Matchers::WithinAbs(2.5, 1e-4));
}

TEST_CASE("Preview size keeps aspect ratio", "[dng]") {
    auto [w1, h1] = hdr::DngWriter::previewSize(8000, 6000, true);
    CHECK(w1 == 1920);
    CHECK(h1 == 1440);
    auto [w2, h2] = hdr::DngWriter::previewSize(4000, 2250, false);
    CHECK(w2 == 256);
    CHECK(h2 == 144);
    auto [w3, h3] = hdr::DngWriter::previewSize(64, 48, true);
    CHECK(w3 == 32);
    CHECK(h3 == 24);
}
