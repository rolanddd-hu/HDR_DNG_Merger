#include "RawLoader.h"
#include "CfaUtil.h"
#include "TiffUtil.h"
#include <libraw/libraw.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <memory>

// Only embedded JPEG previews are decoded; keep the attack surface of stb_image small.
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS (1 << 15)
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace hdr {

namespace {

std::string lowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

int openRaw(LibRaw& raw, const fs::path& path) {
#if defined(_WIN32) && defined(LIBRAW_WIN32_UNICODEPATHS)
    return raw.open_file(path.wstring().c_str());
#else
    return raw.open_file(path.string().c_str());
#endif
}

// Human-readable reason for a LibRaw open/unpack failure, with format-specific hints.
std::string openErrorMessage(const fs::path& path, int ret) {
    std::string msg = "Failed to open raw file " + path.filename().string() + ": " + libraw_strerror(ret);
    if (ret == LIBRAW_FILE_UNSUPPORTED || ret == LIBRAW_UNSUPPORTED_THUMBNAIL) {
        const std::string ext = lowerExt(path);
        if (ext == ".nef") {
            msg += " (Nikon HE/HE* high-efficiency NEFs are not supported - use Lossless compressed RAW)";
        } else if (ext == ".cr3" || ext == ".arw" || ext == ".3fr" || ext == ".iiq" || ext == ".raf") {
            msg += " (this camera model may be newer than the bundled LibRaw decoder)";
        }
    }
    return msg;
}

// Our merge / preview / DNG pipeline only handles 2x2 RGB Bayer mosaics.
// Must be called after open_file() (unpack not required).
Status checkSupportedSensor(LibRaw& raw, const fs::path& path) {
    const auto& id = raw.imgdata.idata;
    const std::string name = path.filename().string();

    if (raw.is_sraw()) {
        return Status::error("Reduced-size RAW (Canon sRAW/mRAW, Nikon small/medium NEF) is not Bayer data and cannot be merged: " + name);
    }
    if (raw.is_fuji_rotated()) {
        return Status::error("Fujifilm SuperCCD (rotated sensor) files are not supported: " + name);
    }
    if (id.filters == 9) {
        return Status::error("Fujifilm X-Trans sensors (6x6 colour filter) are not supported - only Bayer sensors can be merged: " + name);
    }
    if (id.filters == 0) {
        return Status::error("Non-Bayer raw data (linear/demosaiced DNG, multi-shot or monochrome) is not supported: " + name);
    }

    // Colour channels must be plain R/G/B (rejects CMYG and RGBE 4-colour sensors).
    const int top = raw.imgdata.sizes.top_margin;
    const int left = raw.imgdata.sizes.left_margin;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            int c = raw.COLOR(top + y, left + x);
            if (c < 0 || c > 3) {
                return Status::error("Unsupported colour filter layout: " + name);
            }
            char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(id.cdesc[c])));
            if (ch != 'R' && ch != 'G' && ch != 'B') {
                return Status::error(std::string("Unsupported colour filter (") + id.cdesc +
                                     ") - only RGB Bayer sensors can be merged: " + name);
            }
            // Pattern must repeat every 2x2 pixels.
            if (raw.COLOR(top + y, left + x) != raw.COLOR(top + (y % 2), left + (x % 2))) {
                return Status::error("Colour filter pattern does not repeat every 2x2 pixels: " + name);
            }
        }
    }
    return Status::success();
}

// LibRaw flip -> TIFF/EXIF Orientation (same mapping dcraw uses when writing TIFFs).
int flipToOrientation(int flip) {
    static const int map[8] = {1, 2, 4, 3, 5, 8, 6, 7};
    return (flip >= 0 && flip < 8) ? map[flip] : 1;
}

// Fill DNG colour tags from LibRaw's built-in camera data for proprietary raws (CR2, NEF, ARW, ...).
// LibRaw's cam_xyz is the Adobe XYZ(D65)->camera matrix, i.e. exactly what DNG ColorMatrix expects.
void fillColorFromLibRaw(const LibRaw& raw, RawImage& img, bool overrideAll) {
    const auto& col = raw.imgdata.color;

    bool hasMatrix = (img.colorMatrix1[0] != 0.f || img.colorMatrix1[4] != 0.f);
    if (overrideAll || !hasMatrix) {
        bool valid = false;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                img.colorMatrix1[r * 3 + c] = col.cam_xyz[r][c];
                if (col.cam_xyz[r][c] != 0.f) valid = true;
            }
        }
        if (valid) {
            img.calibrationIlluminant1 = 21; // D65
            img.colorMatrix2 = {};
            img.calibrationIlluminant2 = 0;
            img.forwardMatrix1 = {};
            img.forwardMatrix2 = {};
        } else {
            img.colorMatrix1 = {};
            spdlog::warn("No colour matrix known for {} {} - output colours may be wrong",
                         raw.imgdata.idata.make, raw.imgdata.idata.model);
        }
    }

    if (overrideAll || img.asShotNeutral.size() < 3) {
        const auto& m = img.color.camMul;
        if (m[0] > 0.0 && m[1] > 0.0 && m[2] > 0.0) {
            img.asShotNeutral = {static_cast<float>(m[1] / m[0]), 1.0f, static_cast<float>(m[1] / m[2])};
        }
    }
}

std::string formatTimestamp(time_t timestamp) {
    if (timestamp <= 0) return {};
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &timestamp);
#else
    gmtime_r(&timestamp, &tm_buf);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

// Area-averaging downscale of an RGBA8 image so its long side is at most targetMaxDim.
ThumbnailImage downsampleRgba(const uint8_t* src, int srcW, int srcH, int targetMaxDim) {
    if (srcW <= 0 || srcH <= 0) return {};
    const float scale = std::min(1.0f, static_cast<float>(targetMaxDim) / static_cast<float>(std::max(srcW, srcH)));
    const int dstW = std::max(1, static_cast<int>(std::round(srcW * scale)));
    const int dstH = std::max(1, static_cast<int>(std::round(srcH * scale)));

    ThumbnailImage thumb;
    thumb.width = dstW;
    thumb.height = dstH;
    thumb.rgba.resize(static_cast<size_t>(dstW) * dstH * 4);

    for (int y = 0; y < dstH; ++y) {
        const int sy0 = static_cast<int>(static_cast<int64_t>(y) * srcH / dstH);
        const int sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * srcH / dstH));
        uint8_t* dstRow = thumb.rgba.data() + static_cast<size_t>(y) * dstW * 4;
        for (int x = 0; x < dstW; ++x) {
            const int sx0 = static_cast<int>(static_cast<int64_t>(x) * srcW / dstW);
            const int sx1 = std::max(sx0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * srcW / dstW));
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t* s = src + (static_cast<size_t>(sy) * srcW + sx0) * 4;
                for (int sx = sx0; sx < sx1; ++sx, s += 4) {
                    sum[0] += s[0];
                    sum[1] += s[1];
                    sum[2] += s[2];
                    sum[3] += s[3];
                }
            }
            const uint32_t n = static_cast<uint32_t>((sy1 - sy0) * (sx1 - sx0));
            uint8_t* d = dstRow + static_cast<size_t>(x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
    }
    return thumb;
}

} // namespace

std::pair<Status, ExposureInfo> RawLoader::loadMetadata(const fs::path& path) {
    auto raw = std::make_unique<LibRaw>();
    int ret = openRaw(*raw, path);
    if (ret != LIBRAW_SUCCESS) {
        return {Status::error(openErrorMessage(path, ret)), {}};
    }

    ExposureInfo info;
    info.exposureTime = raw->imgdata.other.shutter;
    info.fNumber = raw->imgdata.other.aperture;
    info.iso = raw->imgdata.other.iso_speed;
    info.timestamp = static_cast<double>(raw->imgdata.other.timestamp);
    info.make = raw->imgdata.idata.make;
    info.model = raw->imgdata.idata.model;
    info.width = raw->imgdata.sizes.width;
    info.height = raw->imgdata.sizes.height;
    info.dateTime = formatTimestamp(static_cast<time_t>(raw->imgdata.other.timestamp));

    // Keep the metadata (so the file still sorts / groups sensibly) but flag unsupported sensors.
    Status sensorStatus = checkSupportedSensor(*raw, path);
    return {sensorStatus, info};
}

void RawLoader::readDngTagsWithTiff(const fs::path& path, RawImage& img) {
    TIFF* tif = openTiff(path, "r");
    if (!tif) return;

    double baselineExp = 0.0;
    if (getTiffFloat(tif, TIFFTAG_BASELINEEXPOSURE, baselineExp)) {
        img.color.baselineExposure = baselineExp;
    }

    uint16_t orient = 1;
    if (TIFFGetField(tif, TIFFTAG_ORIENTATION, &orient)) {
        img.orientation = orient;
    }

    float* asShotNeutral = nullptr;
    uint16_t neutralCount = 0;
    if (TIFFGetField(tif, TIFFTAG_ASSHOTNEUTRAL, &neutralCount, &asShotNeutral) && asShotNeutral && neutralCount >= 3) {
        img.asShotNeutral.assign(asShotNeutral, asShotNeutral + 3);
    }

    auto readMatrix = [&](uint32_t tag, std::array<float, 9>& out) {
        float* m = nullptr;
        uint16_t count = 0;
        if (TIFFGetField(tif, tag, &count, &m) && m && count >= 9) {
            std::copy(m, m + 9, out.begin());
        }
    };
    readMatrix(TIFFTAG_COLORMATRIX1, img.colorMatrix1);
    readMatrix(TIFFTAG_COLORMATRIX2, img.colorMatrix2);
    readMatrix(TIFFTAG_FORWARDMATRIX1, img.forwardMatrix1);
    readMatrix(TIFFTAG_FORWARDMATRIX2, img.forwardMatrix2);

    uint16_t calib1 = 0;
    if (TIFFGetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, &calib1)) {
        img.calibrationIlluminant1 = calib1;
    }

    uint16_t calib2 = 0;
    if (TIFFGetField(tif, TIFFTAG_CALIBRATIONILLUMINANT2, &calib2)) {
        img.calibrationIlluminant2 = calib2;
    }

    auto readOpcode = [&](uint32_t tag, std::vector<uint8_t>& out) {
        uint32_t count = 0;
        void* data = nullptr;
        if (TIFFGetField(tif, tag, &count, &data) && count > 0 && data) {
            out.resize(count);
            std::memcpy(out.data(), data, count);
        }
    };

    auto readGeometry = [&]() {
        uint32_t* activeArea = nullptr;
        if (TIFFGetField(tif, TIFFTAG_ACTIVEAREA, &activeArea) && activeArea) {
            img.activeArea = {static_cast<int>(activeArea[0]), static_cast<int>(activeArea[1]),
                              static_cast<int>(activeArea[2]), static_cast<int>(activeArea[3])};
        }

        float* cropOrigin = nullptr;
        if (TIFFGetField(tif, TIFFTAG_DEFAULTCROPORIGIN, &cropOrigin) && cropOrigin) {
            img.defaultCropOrigin = {static_cast<int>(cropOrigin[0]), static_cast<int>(cropOrigin[1])};
        }

        float* cropSize = nullptr;
        if (TIFFGetField(tif, TIFFTAG_DEFAULTCROPSIZE, &cropSize) && cropSize) {
            img.defaultCropSize = {static_cast<int>(cropSize[0]), static_cast<int>(cropSize[1])};
        }
    };

    readOpcode(TIFFTAG_OPCODELIST1, img.opcodeList1);
    readOpcode(TIFFTAG_OPCODELIST2, img.opcodeList2);
    readOpcode(TIFFTAG_OPCODELIST3, img.opcodeList3);
    readGeometry();

    uint16_t subIfdCount = 0;
    uint64_t* subIfdOffsets = nullptr;
    if (TIFFGetField(tif, TIFFTAG_SUBIFD, &subIfdCount, &subIfdOffsets) && subIfdCount > 0 && subIfdOffsets) {
        if (TIFFSetSubDirectory(tif, subIfdOffsets[0])) {
            if (img.opcodeList1.empty()) readOpcode(TIFFTAG_OPCODELIST1, img.opcodeList1);
            if (img.opcodeList2.empty()) readOpcode(TIFFTAG_OPCODELIST2, img.opcodeList2);
            if (img.opcodeList3.empty()) readOpcode(TIFFTAG_OPCODELIST3, img.opcodeList3);
            readGeometry();
        }
    }

    TIFFClose(tif);
}

std::pair<Status, RawImage> RawLoader::loadFull(const fs::path& path, std::stop_token stopToken) {
    auto raw = std::make_unique<LibRaw>();
    int ret = openRaw(*raw, path);
    if (ret != LIBRAW_SUCCESS) {
        return {Status::error(openErrorMessage(path, ret)), {}};
    }

    if (Status s = checkSupportedSensor(*raw, path); !s) {
        return {s, {}};
    }

    {
        // LibRaw's decoders poll the cancel flag, so a stop request aborts long unpacks.
        std::stop_callback cancelDecode(stopToken, [&raw] { raw->setCancelFlag(); });
        ret = raw->unpack();
    }
    if (stopToken.stop_requested() || ret == LIBRAW_CANCELLED_BY_CALLBACK) {
        return {Status::error("Cancelled"), {}};
    }
    if (ret != LIBRAW_SUCCESS) {
        return {Status::error("Failed to decode raw data of " + path.filename().string() + ": " + std::string(libraw_strerror(ret))), {}};
    }

    const auto& sizes = raw->imgdata.sizes;
    const int visWidth = sizes.width;
    const int visHeight = sizes.height;
    const int topMargin = sizes.top_margin;
    const int leftMargin = sizes.left_margin;
    // Row stride in pixels (raw_pitch is in bytes and may include padding).
    const size_t rowStride = sizes.raw_pitch
                                 ? sizes.raw_pitch / sizeof(ushort)
                                 : static_cast<size_t>(sizes.raw_width);

    ushort* rawPixels = raw->imgdata.rawdata.raw_image;
    if (!rawPixels) {
        return {Status::error("No single-channel Bayer data found (multi-shot or non-Bayer file?): " + path.filename().string()), {}};
    }

    // Never trust the file's geometry: the visible area must lie inside the decoded buffer.
    if (visWidth < 2 || visHeight < 2 || topMargin < 0 || leftMargin < 0 ||
        static_cast<size_t>(leftMargin) + static_cast<size_t>(visWidth) > rowStride ||
        leftMargin + visWidth > static_cast<int>(sizes.raw_width) ||
        topMargin + visHeight > static_cast<int>(sizes.raw_height)) {
        return {Status::error("Invalid raw image geometry in " + path.filename().string()), {}};
    }

    RawImage img;
    img.width = visWidth;
    img.height = visHeight;
    img.originalFileName = path.filename().string();
    img.data = ImageU16(visWidth, visHeight);

    for (int y = 0; y < visHeight; ++y) {
        const ushort* srcRow = rawPixels + static_cast<size_t>(topMargin + y) * rowStride + leftMargin;
        uint16_t* dstRow = img.data.row(y);
        std::memcpy(dstRow, srcRow, static_cast<size_t>(visWidth) * sizeof(uint16_t));
    }

    // Determine Bayer CFA pattern at visible origin
    for (int vy = 0; vy < 2; ++vy) {
        for (int vx = 0; vx < 2; ++vx) {
            int colorIdx = raw->COLOR(topMargin + vy, leftMargin + vx);
            char ch = (colorIdx >= 0 && colorIdx < 4) ? raw->imgdata.idata.cdesc[colorIdx] : 'G';
            uint8_t cVal = 1; // G
            if (ch == 'R' || ch == 'r') cVal = 0;
            else if (ch == 'B' || ch == 'b') cVal = 2;
            img.cfa.c[vy * 2 + vx] = cVal;
        }
    }

    // Black level = global black + per-color cblack[0..3] + optional repeat pattern
    // (cblack[4] x cblack[5] dims, values at cblack[6..]) — DNG BlackLevelRepeatDim lands there.
    {
        const auto& col = raw->imgdata.color;
        const unsigned patH = col.cblack[4], patW = col.cblack[5];
        std::array<float, 3> sum{0.f, 0.f, 0.f};
        std::array<int, 3> cnt{0, 0, 0};
        for (int vy = 0; vy < 2; ++vy) {
            for (int vx = 0; vx < 2; ++vx) {
                int rawRow = topMargin + vy, rawCol = leftMargin + vx;
                int lrColor = raw->COLOR(rawRow, rawCol);
                float bl = static_cast<float>(col.black);
                if (lrColor >= 0 && lrColor < 4) bl += static_cast<float>(col.cblack[lrColor]);
                if (patH > 0 && patW > 0 && patH * patW <= 4096 - 6) {
                    bl += static_cast<float>(col.cblack[6 + (rawRow % patH) * patW + (rawCol % patW)]);
                }
                uint8_t c = img.cfa.c[vy * 2 + vx];
                sum[c] += bl;
                cnt[c]++;
            }
        }
        for (int c = 0; c < 3; ++c) {
            img.blackLevel[c] = cnt[c] ? sum[c] / cnt[c] : 0.f;
        }
        img.blackLevel[3] = img.blackLevel[1];
    }

    img.whiteLevel = static_cast<float>(raw->imgdata.color.maximum);
    if (img.whiteLevel <= 0.f && raw->imgdata.color.linear_max[0] > 0) {
        img.whiteLevel = static_cast<float>(raw->imgdata.color.linear_max[0]);
    }
    if (img.whiteLevel <= 0.f) {
        img.whiteLevel = 65535.f;
    }

    img.exposure.exposureTime = raw->imgdata.other.shutter;
    img.exposure.fNumber = raw->imgdata.other.aperture;
    img.exposure.iso = raw->imgdata.other.iso_speed;
    img.exposure.timestamp = static_cast<double>(raw->imgdata.other.timestamp);
    img.exposure.make = raw->imgdata.idata.make;
    img.exposure.model = raw->imgdata.idata.model;
    img.exposure.width = visWidth;
    img.exposure.height = visHeight;
    img.exposure.dateTime = formatTimestamp(static_cast<time_t>(raw->imgdata.other.timestamp));

    for (int i = 0; i < 3; ++i) {
        img.color.camMul[i] = raw->imgdata.color.cam_mul[i];
    }
    if (img.color.camMul[1] > 0.0) {
        img.color.camMul[0] /= img.color.camMul[1];
        img.color.camMul[2] /= img.color.camMul[1];
        img.color.camMul[1] = 1.0;
    } else {
        img.color.camMul = {1.0, 1.0, 1.0};
    }

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            img.color.camXyz[r * 3 + c] = raw->imgdata.color.cam_xyz[r][c];
            img.color.rgbCam[r * 3 + c] = raw->imgdata.color.rgb_cam[r][c];
        }
    }

    // Default crop and active area fallbacks
    img.defaultCropOrigin = {0, 0};
    img.defaultCropSize = {visWidth, visHeight};
    img.activeArea = {0, 0, visHeight, visWidth};

    if (raw->imgdata.idata.dng_version != 0) {
        readDngTagsWithTiff(path, img);
        // Rare DNGs without colour tags: fall back to LibRaw's camera table.
        fillColorFromLibRaw(*raw, img, /*overrideAll=*/false);

        // OpcodeList1 works in stored-image coordinates. Our buffer starts at the ActiveArea origin,
        // so a list written for a different origin would hit the wrong pixels: drop it.
        if (!img.opcodeList1.empty() && (topMargin != 0 || leftMargin != 0)) {
            spdlog::info("{}: dropping OpcodeList1 (ActiveArea offset {},{})", img.originalFileName, topMargin, leftMargin);
            img.opcodeList1.clear();
        }
    } else {
        // Proprietary raw (CR2, CR3, NEF, ARW, RW2, ORF, PEF, RAF, 3FR, IIQ, ...): there are no
        // DNG tags to read, so take orientation and colour data from LibRaw.
        img.orientation = flipToOrientation(raw->imgdata.sizes.flip);
        fillColorFromLibRaw(*raw, img, /*overrideAll=*/true);
    }

    return {Status::success(), std::move(img)};
}

ImageRgbF RawLoader::halfResRgb(const RawImage& img) {
    const NormLut lut(img);
    return quadsToRgb(img.width, img.height, img.cfa, img.color, 1.0f,
                      [&](int y, int x) { return lut.at(img, y, x); });
}

std::pair<Status, ImageRgbF> RawLoader::loadHalfRes(const fs::path& path, std::stop_token stopToken) {
    auto [status, img] = loadFull(path, stopToken);
    if (!status) {
        return {status, {}};
    }
    return {Status::success(), halfResRgb(img)};
}

std::pair<Status, ThumbnailImage> RawLoader::loadThumbnail(const fs::path& path, int maxDim) {
    auto raw = std::make_unique<LibRaw>();
    int ret = openRaw(*raw, path);
    if (ret == LIBRAW_SUCCESS && raw->unpack_thumb() == LIBRAW_SUCCESS) {
        const auto& t = raw->imgdata.thumbnail;
        if (t.tformat == LIBRAW_THUMBNAIL_JPEG && t.thumb && t.tlength > 0) {
            int jw = 0, jh = 0, comp = 0;
            unsigned char* pixels = stbi_load_from_memory(
                reinterpret_cast<const unsigned char*>(t.thumb), static_cast<int>(t.tlength),
                &jw, &jh, &comp, 4);
            if (pixels) {
                ThumbnailImage thumb;
                if (jw > 0 && jh > 0) thumb = downsampleRgba(pixels, jw, jh, maxDim);
                stbi_image_free(pixels);
                if (!thumb.empty()) return {Status::success(), std::move(thumb)};
            }
        } else if (t.tformat == LIBRAW_THUMBNAIL_BITMAP && t.thumb) {
            const int bw = t.twidth;
            const int bh = t.theight;
            const int colors = t.tcolors;
            const size_t pixelCount = static_cast<size_t>(std::max(bw, 0)) * static_cast<size_t>(std::max(bh, 0));
            // The bitmap must actually contain bw*bh*colors bytes (crafted files may lie).
            if (bw > 0 && bh > 0 && (colors == 3 || colors == 4) &&
                static_cast<size_t>(t.tlength) >= pixelCount * colors) {
                std::vector<uint8_t> rgba(pixelCount * 4);
                const uint8_t* src = reinterpret_cast<const uint8_t*>(t.thumb);
                for (size_t i = 0; i < pixelCount; ++i) {
                    rgba[i * 4 + 0] = src[i * colors + 0];
                    rgba[i * 4 + 1] = src[i * colors + 1];
                    rgba[i * 4 + 2] = src[i * colors + 2];
                    rgba[i * 4 + 3] = (colors == 4) ? src[i * colors + 3] : 255;
                }
                return {Status::success(), downsampleRgba(rgba.data(), bw, bh, maxDim)};
            }
        }
    }
    raw.reset();

    // Fallback: decode the raw data and downsample the half-res image
    auto [s, halfImg] = loadHalfRes(path);
    if (!s || halfImg.empty()) {
        return {Status::error("Failed to load thumbnail: " + s.message), {}};
    }

    std::vector<uint8_t> rgba(static_cast<size_t>(halfImg.width) * halfImg.height * 4);
    for (int y = 0; y < halfImg.height; ++y) {
        for (int x = 0; x < halfImg.width; ++x) {
            const float* p = halfImg.pixel(y, x);
            size_t idx = (static_cast<size_t>(y) * halfImg.width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                // Simple gamma so the linear fallback thumbnail is not too dark
                const float v = std::pow(std::clamp(p[c], 0.0f, 1.0f), 1.0f / 2.2f);
                rgba[idx + c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
            }
            rgba[idx + 3] = 255;
        }
    }
    return {Status::success(), downsampleRgba(rgba.data(), halfImg.width, halfImg.height, maxDim)};
}

} // namespace hdr
