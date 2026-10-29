#include "MetadataCopier.h"
#include <exiv2/exiv2.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <mutex>
#include <set>

namespace hdr {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// IFD0 tags worth carrying over. Everything else in Exif.Image describes the source file's own
// structure or raw data (strip/tile layout, BlackLevel, LinearizationTable, NoiseProfile, digests,
// RawDataUniqueID, OriginalRawFile*, ...) or is written by DngWriter (colour matrices, neutral,
// orientation, BaselineExposure) and must not be copied.
const std::set<std::string>& imageTagAllowlist() {
    static const std::set<std::string> keys = {
        "Exif.Image.Make", "Exif.Image.Model", "Exif.Image.UniqueCameraModel", "Exif.Image.LocalizedCameraModel",
        "Exif.Image.Artist", "Exif.Image.Copyright", "Exif.Image.ImageDescription", "Exif.Image.DateTime",
        "Exif.Image.DateTimeOriginal", "Exif.Image.CameraSerialNumber", "Exif.Image.LensInfo",
        "Exif.Image.Rating", "Exif.Image.RatingPercent",
        "Exif.Image.XPTitle", "Exif.Image.XPComment", "Exif.Image.XPAuthor", "Exif.Image.XPKeywords", "Exif.Image.XPSubject",
        "Exif.Image.CameraCalibration1", "Exif.Image.CameraCalibration2", "Exif.Image.CameraCalibrationSignature",
        "Exif.Image.AnalogBalance", "Exif.Image.BaselineSharpness",
        "Exif.Image.ProfileName", "Exif.Image.ProfileCalibrationSignature", "Exif.Image.ProfileCopyright",
        "Exif.Image.ProfileEmbedPolicy", "Exif.Image.AsShotProfileName", "Exif.Image.DefaultBlackRender",
        "Exif.Image.ProfileHueSatMapDims", "Exif.Image.ProfileHueSatMapData1", "Exif.Image.ProfileHueSatMapData2",
        "Exif.Image.ProfileHueSatMapEncoding", "Exif.Image.ProfileLookTableDims", "Exif.Image.ProfileLookTableData",
        "Exif.Image.ProfileLookTableEncoding", "Exif.Image.ProfileToneCurve",
    };
    return keys;
}

Exiv2::URational toRational(double v, bool reciprocalForSmall) {
    if (reciprocalForSmall && v > 0.0 && v < 1.0) {
        return {1u, static_cast<uint32_t>(std::lround(1.0 / v))};
    }
    return {static_cast<uint32_t>(std::lround(v * 10.0)), 10u};
}

void writeBasicExif(Exiv2::ExifData& exif, const ExposureInfo& info) {
    if (info.exposureTime > 0.0) exif["Exif.Photo.ExposureTime"] = toRational(info.exposureTime, true);
    if (info.fNumber > 0.0) exif["Exif.Photo.FNumber"] = toRational(info.fNumber, false);
    if (info.iso > 0.0) exif["Exif.Photo.ISOSpeedRatings"] = static_cast<uint16_t>(std::lround(std::min(info.iso, 65535.0)));
    if (!info.dateTime.empty()) {
        exif["Exif.Photo.DateTimeOriginal"] = info.dateTime;
        exif["Exif.Image.DateTime"] = info.dateTime;
    }
}

bool shouldCopyXmpKey(const std::string& key) {
    // Source develop settings belong to a single frame, tiff:* describes the source file layout,
    // and xmpMM:* identifiers would make the merged file look like the source document.
    return !startsWith(key, "Xmp.crs.") && !startsWith(key, "Xmp.tiff.") &&
           !startsWith(key, "Xmp.xmpMM.") && !startsWith(key, "Xmp.hdrmerge.");
}

// Lightroom / ACR develop settings for the preview sliders. Temperature/tint are relative in our
// preview but absolute (Kelvin) in crs:, and the tone-map operator has no equivalent, so those
// are not exported.
void writeLightroomXmp(Exiv2::XmpData& xmp, const ToneParams& t) {
    auto slider = [](float v) { return std::format("{:+d}", static_cast<int>(std::lround(std::clamp(v, -100.f, 100.f)))); };
    xmp["Xmp.crs.ProcessVersion"] = std::string("11.0");
    xmp["Xmp.crs.HasSettings"] = std::string("True");
    xmp["Xmp.crs.Exposure2012"] = std::format("{:+.2f}", std::clamp(t.exposure, -5.f, 5.f));
    xmp["Xmp.crs.Contrast2012"] = slider(t.contrast);
    xmp["Xmp.crs.Highlights2012"] = slider(t.highlights);
    xmp["Xmp.crs.Shadows2012"] = slider(t.shadows);
    xmp["Xmp.crs.Whites2012"] = slider(t.whites);
    xmp["Xmp.crs.Blacks2012"] = slider(t.blacks);
    xmp["Xmp.crs.Vibrance"] = slider(t.vibrance);
    xmp["Xmp.crs.Saturation"] = slider(t.saturation);
}

} // namespace

void MetadataCopier::initialize() {
    static std::once_flag once;
    std::call_once(once, [] {
        Exiv2::XmpParser::initialize();
        Exiv2::XmpProperties::registerNs("http://hdrmerge.org/xmp/1.0/", "hdrmerge");
    });
}

bool MetadataCopier::shouldCopyExifKey(const std::string& key, bool sourceIsDng) {
    if (startsWith(key, "Exif.Image.")) {
        return imageTagAllowlist().count(key) > 0;
    }
    // Other IFDs (thumbnails, sub-images, extra image IFDs) describe the source's structure.
    if (startsWith(key, "Exif.Thumbnail.") || startsWith(key, "Exif.SubImage") ||
        startsWith(key, "Exif.SubThumb") || startsWith(key, "Exif.Image2.") || startsWith(key, "Exif.Image3.")) {
        return false;
    }
    const bool stdGroup = startsWith(key, "Exif.Photo.") || startsWith(key, "Exif.GPSInfo.") ||
                          startsWith(key, "Exif.Iop.");
    if (stdGroup) {
        if (startsWith(key, "Exif.Photo.InteroperabilityTag")) return false;   // pointer, managed by Exiv2
        if (!sourceIsDng && startsWith(key, "Exif.Photo.MakerNote")) return false;
        if (!sourceIsDng && key.find(".0x") != std::string::npos) return false;  // unknown vendor tags
        return true;
    }
    // Maker-note groups (Exif.DJI.*, Exif.Canon.*, ...) only survive a DNG->DNG rewrite;
    // for proprietary raws they would bloat or corrupt the DNG structure.
    return sourceIsDng;
}

Status MetadataCopier::copyMetadata(
    const fs::path& sourcePath,
    const fs::path& destinationPath,
    const MetadataCopyOptions& options
) {
    initialize();
    try {
        // Source may be a format Exiv2 cannot parse (IIQ, FFF, ...). That is not fatal:
        // we fall back to the LibRaw-derived exposure info.
        Exiv2::Image::UniquePtr srcImage;
        std::string srcError;
        try {
            srcImage = Exiv2::ImageFactory::open(sourcePath.string());
            if (srcImage) srcImage->readMetadata();
        } catch (const std::exception& e) {
            srcError = e.what();
            srcImage.reset();
        }
        if (!srcImage) {
            spdlog::info("Exiv2 cannot read metadata of {} ({}); writing basic EXIF from raw decoder instead",
                         sourcePath.filename().string(), srcError.empty() ? "unsupported format" : srcError);
        }

        std::string srcExt = sourcePath.extension().string();
        std::transform(srcExt.begin(), srcExt.end(), srcExt.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const bool srcIsDng = (srcExt == ".dng");

        auto dstImage = Exiv2::ImageFactory::open(destinationPath.string());
        if (!dstImage) {
            return Status::error("Failed to open destination image: " + destinationPath.filename().string());
        }
        dstImage->readMetadata();

        // 1. EXIF and GPS
        if (options.copyExif || options.copyGps) {
            auto& dstExif = dstImage->exifData();

            if (srcImage) {
                const auto& srcExif = srcImage->exifData();
                for (auto it = srcExif.begin(); it != srcExif.end(); ++it) {
                    const std::string key = it->key();
                    if (!shouldCopyExifKey(key, srcIsDng)) continue;

                    const bool isGps = startsWith(key, "Exif.GPSInfo");
                    if (isGps && !options.copyGps) continue;
                    if (!isGps && !options.copyExif) continue;

                    dstExif[key] = it->value();
                }
            }

            // Make sure the essentials exist even if the source had no (readable) EXIF.
            if (options.copyExif && dstExif.findKey(Exiv2::ExifKey("Exif.Photo.ExposureTime")) == dstExif.end()) {
                writeBasicExif(dstExif, options.fallbackExposure);
            }
        }

        // 2. XMP
        auto& dstXmp = dstImage->xmpData();
        if (options.copyXmp) {
            if (srcImage) {
                const auto& srcXmp = srcImage->xmpData();
                for (auto it = srcXmp.begin(); it != srcXmp.end(); ++it) {
                    if (shouldCopyXmpKey(it->key())) {
                        dstXmp[it->key()] = it->value();
                    }
                }
            }

            dstXmp["Xmp.hdrmerge.software"] = std::string("HDR DNG Merger ") + HDR_VERSION_STRING;
            if (!options.sourceFiles.empty()) {
                std::string joined;
                for (size_t i = 0; i < options.sourceFiles.size(); ++i) {
                    if (i > 0) joined += ", ";
                    joined += options.sourceFiles[i];
                }
                dstXmp["Xmp.hdrmerge.sourceFiles"] = joined;
            }
        }
        if (options.lightroomTone && !options.lightroomTone->isNeutral()) {
            writeLightroomXmp(dstXmp, *options.lightroomTone);
        }

        // 3. IPTC
        if (options.copyIptc && srcImage) {
            dstImage->setIptcData(srcImage->iptcData());
        }

        dstImage->writeMetadata();
        return Status::success();
    } catch (const std::exception& e) {
        return Status::error("Exiv2 error: " + std::string(e.what()));
    }
}

Status MetadataCopier::updateExifExposure(
    const fs::path& filePath,
    uint32_t shutterNum,
    uint32_t shutterDen,
    const std::string& dateTimeStr
) {
    initialize();
    try {
        auto img = Exiv2::ImageFactory::open(filePath.string());
        if (!img) {
            return Status::error("Failed to open file for EXIF update: " + filePath.filename().string());
        }
        img->readMetadata();
        auto& exif = img->exifData();
        exif["Exif.Photo.ExposureTime"] = Exiv2::URational(shutterNum, shutterDen);
        if (!dateTimeStr.empty()) {
            exif["Exif.Image.DateTime"] = dateTimeStr;
            exif["Exif.Photo.DateTimeOriginal"] = dateTimeStr;
            exif["Exif.Photo.DateTimeDigitized"] = dateTimeStr;
        }
        img->writeMetadata();
        return Status::success();
    } catch (const std::exception& e) {
        return Status::error("Exiv2 error: " + std::string(e.what()));
    }
}

} // namespace hdr
