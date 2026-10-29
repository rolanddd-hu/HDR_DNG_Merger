#include <catch2/catch_test_macros.hpp>
#include "TestUtil.h"
#include "core/Types.h"
#include "core/RawLoader.h"
#include "core/DngWriter.h"
#include "core/MetadataCopier.h"
#include <exiv2/exiv2.hpp>
#include <filesystem>

using namespace hdr;
namespace fs = std::filesystem;
using testutil::TempFile;

TEST_CASE("DNG MetadataCopier and Tag Preservation", "[metadata][dng]") {
    const int W = 128;
    const int H = 128;
    RawImage origImg = testutil::makeMetaSource(W, H);
    ImageF cfa = testutil::makeCfaPattern(W, H);

    TempFile dngSrc("test_meta_src");
    TempFile dngDst("test_meta_dst");

    DngWriteParams params;
    params.bits = FloatBits::F32;
    params.compress = true;
    params.embedPreview = true;

    REQUIRE(DngWriter::writeFloatDng(dngSrc.path, cfa, W, H, origImg, params).ok);
    REQUIRE(DngWriter::writeFloatDng(dngDst.path, cfa, W, H, origImg, params).ok);

    MetadataCopyOptions metaOpts;
    metaOpts.sourceFiles.push_back("test_meta_src.dng");
    auto ms = MetadataCopier::copyMetadata(dngSrc.path, dngDst.path, metaOpts);
    REQUIRE(ms.ok);

    // Verify metadata was copied and can be loaded
    auto [metaStatus, metaInfo] = RawLoader::loadMetadata(dngDst.path);
    REQUIRE(metaStatus.ok);
    CHECK(metaInfo.width == W);
    CHECK(metaInfo.height == H);
    CHECK(metaInfo.make == "TestCamera");
    CHECK(metaInfo.model == "TestModel");

    // Verify Exiv2 tags
    auto imgA = Exiv2::ImageFactory::open(dngDst.path.string());
    REQUIRE(imgA);
    imgA->readMetadata();
    CHECK(!imgA->exifData().empty());
}

TEST_CASE("Only safe EXIF keys are copied into the merged DNG", "[metadata]") {
    for (bool dng : {true, false}) {
        CAPTURE(dng);
        // File identity / raw-structure tags of the source must never be copied
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.RawImageDigest", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.NewRawImageDigest", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.RawDataUniqueID", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.OriginalRawFileName", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.NoiseProfile", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.BlackLevel", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.LinearizationTable", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.ColorMatrix1", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Image.StripOffsets", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.SubImage1.BlackLevel", dng));
        CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Thumbnail.JPEGInterchangeFormat", dng));
        // Descriptive metadata is kept
        CHECK(MetadataCopier::shouldCopyExifKey("Exif.Image.Make", dng));
        CHECK(MetadataCopier::shouldCopyExifKey("Exif.Image.Artist", dng));
        CHECK(MetadataCopier::shouldCopyExifKey("Exif.Photo.ExposureTime", dng));
        CHECK(MetadataCopier::shouldCopyExifKey("Exif.GPSInfo.GPSLatitude", dng));
    }
    // Maker notes only survive DNG -> DNG
    CHECK(MetadataCopier::shouldCopyExifKey("Exif.Photo.MakerNote", true));
    CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Photo.MakerNote", false));
    CHECK_FALSE(MetadataCopier::shouldCopyExifKey("Exif.Canon.ModelID", false));
}

TEST_CASE("Source identity tags are dropped and Lightroom settings exported", "[metadata][dng]") {
    const int W = 64, H = 64;
    RawImage meta = testutil::makeMetaSource(W, H);
    const ImageF cfa = testutil::makeCfaPattern(W, H);
    TempFile src("test_meta_ids_src"), dst("test_meta_ids_dst");
    DngWriteParams params;
    params.bits = FloatBits::Int16;
    REQUIRE(DngWriter::writeFloatDng(src.path, cfa, W, H, meta, params).ok);
    REQUIRE(DngWriter::writeFloatDng(dst.path, cfa, W, H, meta, params).ok);

    std::string dstIdBefore;
    {
        // Give the source tags that must not be inherited
        auto img = Exiv2::ImageFactory::open(src.path.string());
        img->readMetadata();
        auto& exif = img->exifData();
        Exiv2::DataValue digest(Exiv2::unsignedByte);
        const uint8_t bytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        digest.read(bytes, sizeof(bytes));
        exif["Exif.Image.RawImageDigest"] = digest;
        exif["Exif.Image.Artist"] = std::string("Test Artist");
        exif["Exif.Photo.ExposureTime"] = Exiv2::URational(1, 250);
        img->writeMetadata();

        auto d = Exiv2::ImageFactory::open(dst.path.string());
        d->readMetadata();
        dstIdBefore = d->exifData()["Exif.Image.RawDataUniqueID"].toString();
    }

    MetadataCopyOptions opts;
    ToneParams tone;
    tone.exposure = 0.5f;
    tone.highlights = -40.f;
    opts.lightroomTone = tone;
    REQUIRE(MetadataCopier::copyMetadata(src.path, dst.path, opts).ok);

    auto img = Exiv2::ImageFactory::open(dst.path.string());
    img->readMetadata();
    const auto& exif = img->exifData();
    CHECK(exif.findKey(Exiv2::ExifKey("Exif.Image.RawImageDigest")) == exif.end());
    CHECK(exif.findKey(Exiv2::ExifKey("Exif.Image.Artist"))->toString() == "Test Artist");
    CHECK(exif.findKey(Exiv2::ExifKey("Exif.Photo.ExposureTime")) != exif.end());
    CHECK(exif.findKey(Exiv2::ExifKey("Exif.Image.RawDataUniqueID"))->toString() == dstIdBefore);

    const auto& xmp = img->xmpData();
    REQUIRE(xmp.findKey(Exiv2::XmpKey("Xmp.crs.Exposure2012")) != xmp.end());
    CHECK(xmp.findKey(Exiv2::XmpKey("Xmp.crs.Exposure2012"))->toString() == "+0.50");
    CHECK(xmp.findKey(Exiv2::XmpKey("Xmp.crs.Highlights2012"))->toString() == "-40");

    // Still a valid raw file afterwards
    auto [status, info] = RawLoader::loadMetadata(dst.path);
    CHECK(status.ok);
}
