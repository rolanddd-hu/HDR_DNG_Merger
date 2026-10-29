#include "ExposureEstimator.h"
#include "CfaUtil.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <spdlog/spdlog.h>

namespace hdr {

std::vector<double> ExposureEstimator::estimateExposures(const std::vector<const RawImage*>& frames,
                                                         const std::vector<ImageShift>& shifts) {
    const size_t n = frames.size();
    if (n == 0) return {};
    if (n == 1) return {1.0};

    auto shiftOf = [&](size_t i) { return i < shifts.size() ? shifts[i] : ImageShift{}; };

    // Calculate prior relative exposures from EXIF
    std::vector<double> priors(n);
    for (size_t i = 0; i < n; ++i) {
        priors[i] = frames[i]->exposure.relativeExposure();
        if (priors[i] <= 0.0) priors[i] = 1.0;
    }

    // Sort order from darkest to brightest
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return priors[a] < priors[b];
    });

    const int width = frames[0]->width;
    const int height = frames[0]->height;

    std::vector<NormLut> luts;
    luts.reserve(n);
    for (const auto* f : frames) luts.emplace_back(*f);

    // Sample one green site per sampled quad (every second quad in each direction). Stepping in
    // whole pixels by 4 would always land on the same CFA colour, which is red or blue for
    // RGGB/BGGR sensors.
    const QuadOffsets o = QuadOffsets::from(frames[0]->cfa);

    std::vector<double> sortedExposures(n, 1.0);
    for (size_t idx = 0; idx + 1 < n; ++idx) {
        const size_t idxA = order[idx];     // darker frame
        const size_t idxB = order[idx + 1]; // brighter frame

        double priorRatio = priors[idxB] / priors[idxA];
        if (priorRatio <= 0.0) priorRatio = 1.0;

        const RawImage& imgA = *frames[idxA];
        const RawImage& imgB = *frames[idxB];
        const ImageShift sA = shiftOf(idxA);
        const ImageShift sB = shiftOf(idxB);

        std::vector<float> sampleRatios;
        sampleRatios.reserve(static_cast<size_t>(width / 4 + 1) * (height / 4 + 1));

        for (int qy = 0; qy + 1 < height; qy += 4) {
            const int y = qy + o.g1y;
            const int yA = y + sA.dy, yB = y + sB.dy;
            if (yA < 0 || yA >= height || yB < 0 || yB >= height) continue;
            for (int qx = 0; qx + 1 < width; qx += 4) {
                const int x = qx + o.g1x;
                const int xA = x + sA.dx, xB = x + sB.dx;
                if (xA < 0 || xA >= width || xB < 0 || xB >= width) continue;

                const float va = luts[idxA].at(imgA, yA, xA);
                const float vb = luts[idxB].at(imgB, yB, xB);
                if (va >= 0.05f && va <= 0.85f && vb >= 0.05f && vb <= 0.85f) {
                    sampleRatios.push_back(vb / va);
                }
            }
        }

        double refinedRatio = priorRatio;
        if (sampleRatios.size() >= 500) {
            const size_t mid = sampleRatios.size() / 2;
            std::nth_element(sampleRatios.begin(), sampleRatios.begin() + mid, sampleRatios.end());
            const double medianRatio = sampleRatios[mid];

            // Verify median is within 1 EV (factor of 0.5 to 2.0) of prior ratio
            const double dev = medianRatio / priorRatio;
            if (dev >= 0.5 && dev <= 2.0) {
                refinedRatio = medianRatio;
            } else {
                spdlog::warn("Exposure ratio for frames {} and {} deviated significantly ({:.2f} vs prior {:.2f}); using prior",
                    imgA.originalFileName, imgB.originalFileName, medianRatio, priorRatio);
            }
        } else {
            spdlog::info("Only {} shared well-exposed samples for frames {} and {}; using EXIF exposure ratio {:.2f}",
                sampleRatios.size(), imgA.originalFileName, imgB.originalFileName, priorRatio);
        }

        sortedExposures[idx + 1] = sortedExposures[idx] * refinedRatio;
    }

    // Map sortedExposures back to original frame indices
    std::vector<double> result(n, 1.0);
    for (size_t idx = 0; idx < n; ++idx) {
        result[order[idx]] = sortedExposures[idx];
    }

    return result;
}

std::vector<double> ExposureEstimator::estimateExposures(const std::vector<RawImage>& frames,
                                                         const std::vector<ImageShift>& shifts) {
    std::vector<const RawImage*> ptrs;
    ptrs.reserve(frames.size());
    for (const auto& f : frames) ptrs.push_back(&f);
    return estimateExposures(ptrs, shifts);
}

} // namespace hdr
