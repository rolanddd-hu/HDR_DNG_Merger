#include "DngWriter.h"
#include "CfaUtil.h"
#include "Parallel.h"
#include "TiffUtil.h"
#include <zlib.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace hdr {

static_assert(std::endian::native == std::endian::little, "DNG sample packing assumes a little-endian host");

namespace {

constexpr uint32_t kTileSize = 256;

uint16_t float32_to_float16(float val) {
    uint32_t f;
    std::memcpy(&f, &val, sizeof(float));
    uint32_t sign = (f >> 16) & 0x8000;
    int32_t exp = static_cast<int32_t>((f >> 23) & 0xff) - 127 + 15;
    uint32_t frac = f & 0x007fffff;

    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        frac = (frac | 0x00800000) >> (1 - exp);
        if (frac & 0x1000) frac += 0x2000;
        return static_cast<uint16_t>(sign | (frac >> 13));
    } else if (exp >= 31) {
        if (exp == 143 && frac != 0) {
            return static_cast<uint16_t>(sign | 0x7e00);
        }
        return static_cast<uint16_t>(sign | 0x7c00);
    }

    if (frac & 0x1000) {
        frac += 0x2000;
        if (frac & 0x00800000) {
            frac = 0;
            exp++;
            if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00);
        }
    }
    return static_cast<uint16_t>(sign | (exp << 10) | (frac >> 13));
}

// DNG 24-bit float (1 sign, 7 exponent bits biased by 63, 16 mantissa bits) as a host-order
// (little-endian) 24-bit value, i.e. out3[0] is the least significant byte.
void float32_to_float24_le(float val, uint8_t* out3) {
    uint32_t f;
    std::memcpy(&f, &val, sizeof(float));
    const uint32_t sign = (f >> 31) & 1;
    int32_t exp = static_cast<int32_t>((f >> 23) & 0xff) - 127 + 63;
    const uint32_t frac = f & 0x007fffff;

    uint32_t packed;
    if (exp <= 0) {
        packed = sign << 23;
    } else if (exp >= 127) {
        packed = (sign << 23) | (127u << 16);
    } else {
        uint32_t mantissa = frac >> 7;
        if ((frac & 0x40) && ((frac & 0x3f) || (mantissa & 1))) {
            mantissa++;
            if (mantissa & 0x10000) {
                mantissa = 0;
                exp++;
            }
        }
        packed = exp >= 127 ? (sign << 23) | (127u << 16)
                            : (sign << 23) | (static_cast<uint32_t>(exp) << 16) | mantissa;
    }
    out3[0] = static_cast<uint8_t>(packed & 0xff);
    out3[1] = static_cast<uint8_t>((packed >> 8) & 0xff);
    out3[2] = static_cast<uint8_t>((packed >> 16) & 0xff);
}

// Narkowicz ACES fit, used so the embedded preview is not hard-clipped.
float acesFilm(float x) {
    return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

uint8_t toSrgb8(float c) {
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    const float s = (c <= 0.0031308f) ? (12.92f * c) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
    return static_cast<uint8_t>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
}

// Area-averaged, tone-mapped sRGB preview of the CFA data.
std::vector<uint8_t> renderPreview(const ImageF& cfa, int width, int height, const RawImage& meta,
                                   int prevW, int prevH) {
    const int qW = std::max(1, width / 2);
    const int qH = std::max(1, height / 2);
    const QuadOffsets o = QuadOffsets::from(meta.cfa);
    const float expGain = std::pow(2.0f, static_cast<float>(meta.color.baselineExposure));

    std::vector<uint8_t> rgb(static_cast<size_t>(prevW) * prevH * 3, 0);
    parallelFor(0, prevH, 8, [&](int py) {
        const int qy0 = static_cast<int>(static_cast<int64_t>(py) * qH / prevH);
        const int qy1 = std::max(qy0 + 1, static_cast<int>(static_cast<int64_t>(py + 1) * qH / prevH));
        for (int px = 0; px < prevW; ++px) {
            const int qx0 = static_cast<int>(static_cast<int64_t>(px) * qW / prevW);
            const int qx1 = std::max(qx0 + 1, static_cast<int>(static_cast<int64_t>(px + 1) * qW / prevW));
            double r = 0, g = 0, b = 0;
            int n = 0;
            for (int qy = qy0; qy < qy1 && 2 * qy + 1 < height; ++qy) {
                const int by = qy * 2;
                for (int qx = qx0; qx < qx1 && 2 * qx + 1 < width; ++qx) {
                    const int bx = qx * 2;
                    r += cfa.at(by + o.ry, bx + o.rx);
                    g += 0.5 * (cfa.at(by + o.g1y, bx + o.g1x) + cfa.at(by + o.g2y, bx + o.g2x));
                    b += cfa.at(by + o.by, bx + o.bx);
                    ++n;
                }
            }
            if (n == 0) continue;
            float lin[3];
            cameraToLinearSrgb(static_cast<float>(r / n) * expGain, static_cast<float>(g / n) * expGain,
                               static_cast<float>(b / n) * expGain, meta.color, lin);
            uint8_t* d = rgb.data() + (static_cast<size_t>(py) * prevW + px) * 3;
            for (int c = 0; c < 3; ++c) d[c] = toSrgb8(acesFilm(lin[c]));
        }
    });
    return rgb;
}

// Packs one tile of float samples into little-endian sample bytes, then (when compressing)
// applies the DNG/TIFF floating-point predictor and zlib-compresses the result.
std::vector<uint8_t> encodeFloatTile(const ImageF& cfa, int width, int height, int tileX, int tileY,
                                     FloatBits bits, bool compress) {
    const int bps = static_cast<int>(bits) / 8;
    const size_t rowBytes = static_cast<size_t>(kTileSize) * bps;
    std::vector<uint8_t> samples(rowBytes * kTileSize, 0);

    for (uint32_t ty = 0; ty < kTileSize; ++ty) {
        const int imgY = tileY + static_cast<int>(ty);
        if (imgY >= height) break;
        uint8_t* row = samples.data() + ty * rowBytes;
        for (uint32_t tx = 0; tx < kTileSize; ++tx) {
            const int imgX = tileX + static_cast<int>(tx);
            if (imgX >= width) break;
            const float val = cfa.at(imgY, imgX);
            uint8_t* dst = row + static_cast<size_t>(tx) * bps;
            if (bits == FloatBits::F16) {
                const uint16_t h = float32_to_float16(val);
                std::memcpy(dst, &h, 2);
            } else if (bits == FloatBits::F24) {
                float32_to_float24_le(val, dst);
            } else {
                std::memcpy(dst, &val, 4);
            }
        }
    }
    if (!compress) return samples;

    // Floating-point predictor (Adobe TN3): per row, split samples into byte planes from the most
    // significant byte down, then take byte-wise horizontal differences over the whole row.
    std::vector<uint8_t> planes(samples.size());
    for (uint32_t ty = 0; ty < kTileSize; ++ty) {
        const uint8_t* src = samples.data() + ty * rowBytes;
        uint8_t* dst = planes.data() + ty * rowBytes;
        for (uint32_t i = 0; i < kTileSize; ++i) {
            for (int b = 0; b < bps; ++b) {
                dst[static_cast<size_t>(b) * kTileSize + i] = src[static_cast<size_t>(i) * bps + (bps - 1 - b)];
            }
        }
        for (size_t i = rowBytes - 1; i > 0; --i) {
            dst[i] = static_cast<uint8_t>(dst[i] - dst[i - 1]);
        }
    }

    uLongf outLen = compressBound(static_cast<uLong>(planes.size()));
    std::vector<uint8_t> out(outLen);
    if (compress2(out.data(), &outLen, planes.data(), static_cast<uLong>(planes.size()), 6) != Z_OK) {
        return {};
    }
    out.resize(outLen);
    return out;
}

void fillRandomId(uint8_t (&id)[16]) {
    std::random_device rd;
    for (int i = 0; i < 16; i += 4) {
        const uint32_t v = rd();
        std::memcpy(id + i, &v, 4);
    }
}

} // namespace

FloatBits DngWriter::resolveFloatBits(FloatBits requested, float /*whiteLevel*/) {
    if (requested != FloatBits::Auto) return requested;
    // Default to 16-bit unsigned integer CFA: Capture One does not read floating-point DNGs
    // at all, and 24-bit float is Adobe-only. Use F32 explicitly for float workflows.
    return FloatBits::Int16;
}

std::pair<int, int> DngWriter::previewSize(int width, int height, bool embedPreview) {
    const int qW = std::max(1, width / 2);
    const int qH = std::max(1, height / 2);
    const int maxDim = embedPreview ? 1920 : 256;
    const double scale = std::min(1.0, static_cast<double>(maxDim) / std::max(qW, qH));
    return {std::max(1, static_cast<int>(std::lround(qW * scale))),
            std::max(1, static_cast<int>(std::lround(qH * scale)))};
}

Status DngWriter::writeFloatDng(
    const fs::path& outputPath,
    const ImageF& cfaData,
    int width,
    int height,
    const RawImage& metaSource,
    const DngWriteParams& params
) {
    if (width < 2 || height < 2 || cfaData.empty() || cfaData.width != width || cfaData.height != height) {
        return Status::error("Invalid image dimensions or empty data");
    }

    const FloatBits resolvedBits = resolveFloatBits(params.bits, metaSource.whiteLevel);
    if (resolvedBits != FloatBits::Int16 && resolvedBits != FloatBits::F16 &&
        resolvedBits != FloatBits::F24 && resolvedBits != FloatBits::F32) {
        return Status::error("Unsupported output precision");
    }
    const bool isInt16 = (resolvedBits == FloatBits::Int16);
    const int bps = isInt16 ? 16 : static_cast<int>(resolvedBits);

    TiffErrorState tiffErrors;
    TIFF* tif = openTiff(outputPath, "w", &tiffErrors);
    if (!tif) {
        return Status::error("Failed to create DNG file: " + outputPath.filename().string());
    }
    auto fail = [&](const std::string& msg) {
        TIFFClose(tif);
        return Status::error(msg);
    };

    // ==========================================
    // 1. IFD0: 8-bit sRGB preview (debayered + WB + baseline exposure, tone mapped) + global tags
    // ==========================================
    const auto [prevW, prevH] = previewSize(width, height, params.embedPreview);
    const std::vector<uint8_t> previewRgb = renderPreview(cfaData, width, height, metaSource, prevW, prevH);

    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(prevW));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(prevH));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, static_cast<uint32_t>(prevH));

    // Global DNG metadata in IFD0
    static const uint8_t dngVer[4] = {1, 4, 0, 0};
    static const uint8_t dngBackVer[4] = {1, 3, 0, 0};
    TIFFSetField(tif, TIFFTAG_DNGVERSION, dngVer);
    TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, dngBackVer);

    if (!metaSource.exposure.make.empty()) {
        TIFFSetField(tif, TIFFTAG_MAKE, metaSource.exposure.make.c_str());
    }
    if (!metaSource.exposure.model.empty()) {
        TIFFSetField(tif, TIFFTAG_MODEL, metaSource.exposure.model.c_str());
    }
    std::string uniqueModel = metaSource.exposure.make + " " + metaSource.exposure.model;
    TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, uniqueModel.c_str());
    TIFFSetField(tif, TIFFTAG_SOFTWARE, "HDR DNG Merger " HDR_VERSION_STRING);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, static_cast<uint16_t>(metaSource.orientation));
    TIFFSetField(tif, TIFFTAG_BASELINEEXPOSURE, metaSource.color.baselineExposure);

    // A fresh ID: the merged raw data is new, so raw converters must not reuse a source's cache.
    uint8_t rawDataId[16];
    fillRandomId(rawDataId);
    TIFFSetField(tif, TIFFTAG_RAWDATAUNIQUEID, rawDataId);

    if (metaSource.colorMatrix1[0] != 0.f || metaSource.colorMatrix1[4] != 0.f) {
        TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, metaSource.colorMatrix1.data());
    }
    if (metaSource.colorMatrix2[0] != 0.f || metaSource.colorMatrix2[4] != 0.f) {
        TIFFSetField(tif, TIFFTAG_COLORMATRIX2, 9, metaSource.colorMatrix2.data());
    }
    if (metaSource.calibrationIlluminant1 > 0) {
        TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, static_cast<uint16_t>(metaSource.calibrationIlluminant1));
    }
    if (metaSource.calibrationIlluminant2 > 0) {
        TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT2, static_cast<uint16_t>(metaSource.calibrationIlluminant2));
    }
    if (metaSource.forwardMatrix1[0] != 0.f) {
        TIFFSetField(tif, TIFFTAG_FORWARDMATRIX1, 9, metaSource.forwardMatrix1.data());
    }
    if (metaSource.forwardMatrix2[0] != 0.f) {
        TIFFSetField(tif, TIFFTAG_FORWARDMATRIX2, 9, metaSource.forwardMatrix2.data());
    }
    if (metaSource.asShotNeutral.size() >= 3) {
        TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, static_cast<uint16_t>(3), metaSource.asShotNeutral.data());
    }

    uint64_t subIfdOffsets[1] = { 0 };
    TIFFSetField(tif, TIFFTAG_SUBIFD, 1, subIfdOffsets);

    if (TIFFWriteEncodedStrip(tif, 0, const_cast<uint8_t*>(previewRgb.data()),
                              static_cast<tmsize_t>(previewRgb.size())) < 0) {
        return fail("Failed to write preview strip in IFD0");
    }
    if (!TIFFWriteDirectory(tif)) {
        return fail("Failed to write IFD0");
    }

    // ==========================================
    // 2. SubIFD0: CFA Raw (float tiles, or 16-bit integer strips)
    // ==========================================
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(height));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, isInt16 ? SAMPLEFORMAT_UINT : SAMPLEFORMAT_IEEEFP);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

    const bool compressFloat = !isInt16 && params.compress;
    if (isInt16) {
        // Mirror the layout of camera DNGs (uncompressed, one strip) for maximum compatibility.
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, static_cast<uint32_t>(height));
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    } else {
        TIFFSetField(tif, TIFFTAG_TILEWIDTH, kTileSize);
        TIFFSetField(tif, TIFFTAG_TILELENGTH, kTileSize);
        if (compressFloat) {
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
            TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
        } else {
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
        }
    }

    uint16_t repeatDim[2] = {2, 2};
    TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, repeatDim);
    uint8_t cfaPat[4] = {metaSource.cfa.c[0], metaSource.cfa.c[1], metaSource.cfa.c[2], metaSource.cfa.c[3]};
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, cfaPat);
    uint8_t planeColor[3] = {0, 1, 2};
    TIFFSetField(tif, TIFFTAG_CFAPLANECOLOR, 3, planeColor);
    TIFFSetField(tif, TIFFTAG_CFALAYOUT, 1);
    TIFFSetField(tif, TIFFTAG_BAYERGREENSPLIT, static_cast<uint32_t>(0));

    float blackLevel = 0.0f;
    TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 1, &blackLevel);
    uint32_t whiteLevel = isInt16 ? 65535u : 1u;
    TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &whiteLevel);

    // The buffer already starts at the source's ActiveArea origin, so ours is the whole image.
    uint32_t activeArea[4] = {0, 0, static_cast<uint32_t>(height), static_cast<uint32_t>(width)};
    TIFFSetField(tif, TIFFTAG_ACTIVEAREA, activeArea);

    int cropX = std::clamp(metaSource.defaultCropOrigin[0], 0, width - 1);
    int cropY = std::clamp(metaSource.defaultCropOrigin[1], 0, height - 1);
    int cropW = metaSource.defaultCropSize[0] > 0 ? metaSource.defaultCropSize[0] : width;
    int cropH = metaSource.defaultCropSize[1] > 0 ? metaSource.defaultCropSize[1] : height;
    cropW = std::clamp(cropW, 1, width - cropX);
    cropH = std::clamp(cropH, 1, height - cropY);
    float cropOrigin[2] = {static_cast<float>(cropX), static_cast<float>(cropY)};
    TIFFSetField(tif, TIFFTAG_DEFAULTCROPORIGIN, cropOrigin);
    float cropSize[2] = {static_cast<float>(cropW), static_cast<float>(cropH)};
    TIFFSetField(tif, TIFFTAG_DEFAULTCROPSIZE, cropSize);

    if (!metaSource.opcodeList1.empty()) {
        TIFFSetField(tif, TIFFTAG_OPCODELIST1, static_cast<uint32_t>(metaSource.opcodeList1.size()), metaSource.opcodeList1.data());
    }
    if (!metaSource.opcodeList2.empty()) {
        TIFFSetField(tif, TIFFTAG_OPCODELIST2, static_cast<uint32_t>(metaSource.opcodeList2.size()), metaSource.opcodeList2.data());
    }
    if (!metaSource.opcodeList3.empty()) {
        TIFFSetField(tif, TIFFTAG_OPCODELIST3, static_cast<uint32_t>(metaSource.opcodeList3.size()), metaSource.opcodeList3.data());
    }

    if (isInt16) {
        std::vector<uint16_t> strip(static_cast<size_t>(width) * height);
        parallelFor(0, height, 32, [&](int y) {
            const float* src = cfaData.row(y);
            uint16_t* dst = strip.data() + static_cast<size_t>(y) * width;
            for (int x = 0; x < width; ++x) {
                float v = src[x];
                if (!(v > 0.f)) v = 0.f;   // also catches NaN
                if (v > 1.f) v = 1.f;
                dst[x] = static_cast<uint16_t>(v * 65535.f + 0.5f);
            }
        });
        if (TIFFWriteEncodedStrip(tif, 0, strip.data(),
                                  static_cast<tmsize_t>(strip.size() * sizeof(uint16_t))) < 0) {
            return fail("Failed to write 16-bit raw strip");
        }
    } else {
        // Tiles are packed and compressed in parallel, then appended in order as raw tiles.
        const int tilesX = (width + kTileSize - 1) / kTileSize;
        const int tilesY = (height + kTileSize - 1) / kTileSize;
        const int rowsPerBatch = std::max(1, static_cast<int>(hardwareThreads()));
        for (int ty0 = 0; ty0 < tilesY; ty0 += rowsPerBatch) {
            const int ty1 = std::min(tilesY, ty0 + rowsPerBatch);
            std::vector<std::vector<uint8_t>> encoded(static_cast<size_t>(ty1 - ty0) * tilesX);
            parallelFor(0, static_cast<int>(encoded.size()), 1, [&](int i) {
                const int tx = i % tilesX;
                const int ty = ty0 + i / tilesX;
                encoded[i] = encodeFloatTile(cfaData, width, height, tx * kTileSize, ty * kTileSize,
                                             resolvedBits, compressFloat);
            });
            for (size_t i = 0; i < encoded.size(); ++i) {
                const int tx = static_cast<int>(i) % tilesX;
                const int ty = ty0 + static_cast<int>(i) / tilesX;
                if (encoded[i].empty()) {
                    return fail("Failed to compress tile");
                }
                const uint32_t tileIndex = TIFFComputeTile(tif, static_cast<uint32_t>(tx * kTileSize),
                                                           static_cast<uint32_t>(ty * kTileSize), 0, 0);
                if (TIFFWriteRawTile(tif, tileIndex, encoded[i].data(), static_cast<tmsize_t>(encoded[i].size())) < 0) {
                    return fail("Failed to write tile at (" + std::to_string(tx * kTileSize) + ", " +
                                std::to_string(ty * kTileSize) + ")");
                }
            }
        }
    }

    if (!TIFFWriteDirectory(tif)) {
        return fail("Failed to write raw SubIFD");
    }
    TIFFClose(tif);

    if (tiffErrors.errors > 0) {
        return Status::error("libtiff reported " + std::to_string(tiffErrors.errors) +
                             " error(s) while writing " + outputPath.filename().string());
    }
    return Status::success();
}

} // namespace hdr
