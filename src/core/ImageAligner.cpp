#include "ImageAligner.h"
#include "CfaUtil.h"
#include "Parallel.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <format>
#include <limits>
#include <spdlog/spdlog.h>

namespace hdr {

namespace {

constexpr float kLogStops = 10.0f;  // stops of scene range mapped onto the 8-bit luma

// Downsample RawImage 2x2 Bayer quads into an 8-bit log-luminance image of size (W/2, H/2).
// A log encoding keeps the darkest bracket frame from collapsing into a handful of levels,
// which would make its median threshold bitmap meaningless.
std::vector<uint8_t> createQuadLuma(const RawImage& img, int& outW, int& outH) {
    outW = img.width / 2;
    outH = img.height / 2;
    std::vector<uint8_t> luma(static_cast<size_t>(outW) * outH, 0);
    const NormLut lut(img);
    const float minLum = std::exp2(-kLogStops);
    const int w = outW;

    parallelFor(0, outH, 32, [&](int qy) {
        const int baseY = qy * 2;
        const float* t0 = lut.table(baseY, 0);
        const float* t1 = lut.table(baseY, 1);
        const float* t2 = lut.table(baseY + 1, 0);
        const float* t3 = lut.table(baseY + 1, 1);
        const uint16_t* row0 = img.data.row(baseY);
        const uint16_t* row1 = img.data.row(baseY + 1);
        uint8_t* dst = luma.data() + static_cast<size_t>(qy) * w;
        for (int qx = 0; qx < w; ++qx) {
            const int baseX = qx * 2;
            const float avg = 0.25f * (t0[row0[baseX]] + t1[row0[baseX + 1]] + t2[row1[baseX]] + t3[row1[baseX + 1]]);
            const float l = (std::log2(std::max(avg, minLum)) + kLogStops) / kLogStops;
            dst[qx] = static_cast<uint8_t>(std::clamp(l * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    });
    return luma;
}

// Downsample a grayscale image by 2x in each dimension
std::vector<uint8_t> downsample2x(const std::vector<uint8_t>& src, int sw, int sh, int& dw, int& dh) {
    dw = sw / 2;
    dh = sh / 2;
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh, 0);

    for (int y = 0; y < dh; ++y) {
        const size_t sy = static_cast<size_t>(y) * 2;
        for (int x = 0; x < dw; ++x) {
            const size_t sx = static_cast<size_t>(x) * 2;
            int sum = src[sy * sw + sx] +
                      src[sy * sw + sx + 1] +
                      src[(sy + 1) * sw + sx] +
                      src[(sy + 1) * sw + sx + 1];
            dst[static_cast<size_t>(y) * dw + x] = static_cast<uint8_t>((sum + 2) / 4);
        }
    }
    return dst;
}

// Compute median value of an 8-bit image using histogram
uint8_t computeMedian(const std::vector<uint8_t>& img) {
    size_t hist[256] = {0};
    for (uint8_t val : img) {
        hist[val]++;
    }
    size_t half = img.size() / 2;
    size_t count = 0;
    for (int i = 0; i < 256; ++i) {
        count += hist[i];
        if (count >= half) {
            return static_cast<uint8_t>(i);
        }
    }
    return 128;
}

struct MtbLevel {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> bitmap;    // 1 if pixel >= median, else 0
    std::vector<uint8_t> exclusion; // 1 if |pixel - median| > noiseTolerance, else 0
};

// Build Median Threshold Bitmap and Exclusion Mask for one level
MtbLevel buildMtb(const std::vector<uint8_t>& img, int w, int h, int noiseTolerance = 4) {
    MtbLevel lvl;
    lvl.w = w;
    lvl.h = h;
    lvl.bitmap.resize(img.size());
    lvl.exclusion.resize(img.size());

    uint8_t med = computeMedian(img);

    for (size_t i = 0; i < img.size(); ++i) {
        uint8_t v = img[i];
        lvl.bitmap[i] = (v >= med) ? 1 : 0;
        int diff = std::abs(static_cast<int>(v) - static_cast<int>(med));
        lvl.exclusion[i] = (diff > noiseTolerance) ? 1 : 0;
    }
    return lvl;
}

} // namespace

ImageShift ImageAligner::alignBayer(
    const RawImage& target,
    const RawImage& reference,
    int maxShift
) {
    if (target.width != reference.width || target.height != reference.height ||
        target.width < 2 || target.height < 2) {
        return {0, 0};
    }

    int qw = 0, qh = 0;
    auto tgtLuma0 = createQuadLuma(target, qw, qh);
    auto refLuma0 = createQuadLuma(reference, qw, qh);

    int maxQuadShift = std::max(1, maxShift / 2);

    // Build pyramid
    std::vector<std::vector<uint8_t>> tgtPyramid = { tgtLuma0 };
    std::vector<std::vector<uint8_t>> refPyramid = { refLuma0 };
    std::vector<int> pyramidW = { qw };
    std::vector<int> pyramidH = { qh };

    int curW = qw, curH = qh;
    while (curW > 32 && curH > 32 && (1 << tgtPyramid.size()) <= maxQuadShift * 2) {
        int nw = 0, nh = 0;
        auto nextTgt = downsample2x(tgtPyramid.back(), curW, curH, nw, nh);
        auto nextRef = downsample2x(refPyramid.back(), curW, curH, nw, nh);
        tgtPyramid.push_back(std::move(nextTgt));
        refPyramid.push_back(std::move(nextRef));
        pyramidW.push_back(nw);
        pyramidH.push_back(nh);
        curW = nw;
        curH = nh;
    }

    int numLevels = static_cast<int>(tgtPyramid.size());

    // Build MTB representations for each level
    std::vector<MtbLevel> tgtMtb(numLevels);
    std::vector<MtbLevel> refMtb(numLevels);
    for (int i = 0; i < numLevels; ++i) {
        tgtMtb[i] = buildMtb(tgtPyramid[i], pyramidW[i], pyramidH[i]);
        refMtb[i] = buildMtb(refPyramid[i], pyramidW[i], pyramidH[i]);
    }

    // Coarse-to-fine search
    int shiftX = 0;
    int shiftY = 0;

    for (int lvl = numLevels - 1; lvl >= 0; --lvl) {
        shiftX *= 2;
        shiftY *= 2;

        const auto& tMtb = tgtMtb[lvl];
        const auto& rMtb = refMtb[lvl];
        int lw = tMtb.w;
        int lh = tMtb.h;

        int bestDx = 0;
        int bestDy = 0;
        double minErrRate = std::numeric_limits<double>::infinity();

        int step = (lvl == 0 && lw > 1200) ? 2 : 1;

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int cx = shiftX + dx;
                int cy = shiftY + dy;

                int y0 = std::max(0, -cy);
                int y1 = std::min(lh, lh - cy);
                int x0 = std::max(0, -cx);
                int x1 = std::min(lw, lw - cx);
                if (y0 >= y1 || x0 >= x1) continue;

                int64_t diffCount = 0;
                int64_t validCount = 0;
                for (int y = y0; y < y1; y += step) {
                    const size_t refRowOffset = static_cast<size_t>(y) * lw;
                    const size_t tgtRowOffset = static_cast<size_t>(y + cy) * lw;

                    const uint8_t* pRefB = rMtb.bitmap.data() + refRowOffset;
                    const uint8_t* pRefX = rMtb.exclusion.data() + refRowOffset;
                    const uint8_t* pTgtB = tMtb.bitmap.data() + tgtRowOffset;
                    const uint8_t* pTgtX = tMtb.exclusion.data() + tgtRowOffset;

                    for (int x = x0; x < x1; x += step) {
                        if (pRefX[x] && pTgtX[x + cx]) {
                            validCount++;
                            if (pRefB[x] != pTgtB[x + cx]) {
                                diffCount++;
                            }
                        }
                    }
                }

                if (validCount > 64) {
                    double errRate = static_cast<double>(diffCount) / validCount;
                    if (errRate < minErrRate) {
                        minErrRate = errRate;
                        bestDx = dx;
                        bestDy = dy;
                    }
                }
            }
        }

        shiftX += bestDx;
        shiftY += bestDy;
    }

    // Quad shift to full raw shift (always even, preserving Bayer mosaic)
    int rawShiftX = shiftX * 2;
    int rawShiftY = shiftY * 2;

    // Clamp to the largest even value within maxShift
    const int evenMax = (std::max(0, maxShift) / 2) * 2;
    rawShiftX = std::clamp(rawShiftX, -evenMax, evenMax);
    rawShiftY = std::clamp(rawShiftY, -evenMax, evenMax);

    return {rawShiftX, rawShiftY};
}

std::vector<ImageShift> ImageAligner::alignBracket(
    const std::vector<const RawImage*>& frames,
    int referenceIndex,
    int maxShift,
    ProgressFn progress
) {
    const size_t n = frames.size();
    if (n == 0) return {};
    if (n == 1) return {{0, 0}};

    if (referenceIndex < 0 || referenceIndex >= static_cast<int>(n)) {
        referenceIndex = static_cast<int>(n / 2);
    }

    if (progress) progress(0.05f, std::format("Aligning {} frames...", n - 1));

    std::vector<ImageShift> shifts(n, {0, 0});
    const RawImage& refFrame = *frames[referenceIndex];

    parallelFor(0, static_cast<int>(n), 1, [&](int i) {
        if (i == referenceIndex) return;
        shifts[i] = alignBayer(*frames[i], refFrame, maxShift);
        spdlog::info("Frame {} aligned to ref {}: shift dx={} px, dy={} px",
            frames[i]->originalFileName.empty() ? std::to_string(i) : frames[i]->originalFileName,
            referenceIndex, shifts[i].dx, shifts[i].dy);
    });

    return shifts;
}

std::vector<ImageShift> ImageAligner::alignBracket(
    const std::vector<RawImage>& frames,
    int referenceIndex,
    int maxShift,
    ProgressFn progress
) {
    std::vector<const RawImage*> ptrs;
    ptrs.reserve(frames.size());
    for (const auto& f : frames) ptrs.push_back(&f);
    return alignBracket(ptrs, referenceIndex, maxShift, progress);
}

} // namespace hdr
