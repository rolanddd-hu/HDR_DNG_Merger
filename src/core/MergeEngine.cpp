#include "MergeEngine.h"
#include "CfaUtil.h"
#include "ExposureEstimator.h"
#include "ImageAligner.h"
#include "Parallel.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <spdlog/spdlog.h>

namespace hdr {

namespace {

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 3x3 dilation of a quad mask (reads `src`, writes `dst`; both qw*qh).
void dilate3x3(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, int qw, int qh) {
    dst.assign(src.size(), 0);
    parallelFor(0, qh, 64, [&](int qy) {
        const int y0 = std::max(0, qy - 1), y1 = std::min(qh - 1, qy + 1);
        for (int qx = 0; qx < qw; ++qx) {
            const int x0 = std::max(0, qx - 1), x1 = std::min(qw - 1, qx + 1);
            uint8_t v = 0;
            for (int y = y0; y <= y1 && !v; ++y) {
                const uint8_t* row = src.data() + static_cast<size_t>(y) * qw;
                for (int x = x0; x <= x1; ++x) {
                    if (row[x]) { v = 1; break; }
                }
            }
            dst[static_cast<size_t>(qy) * qw + qx] = v;
        }
    });
}

} // namespace

void MergeEngine::boxBlur2D(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int r) {
    if (r <= 0) {
        dst = src;
        return;
    }
    const size_t size = static_cast<size_t>(w) * h;
    std::vector<float> tmp(size);

    // Horizontal pass (rows in parallel)
    parallelFor(0, h, 64, [&](int y) {
        const float* s = src.data() + static_cast<size_t>(y) * w;
        float* t = tmp.data() + static_cast<size_t>(y) * w;
        double sum = 0.0;
        for (int x = 0; x <= std::min(r, w - 1); ++x) sum += s[x];
        for (int x = 0; x < w; ++x) {
            const int lo = std::max(0, x - r), hi = std::min(w - 1, x + r);
            t[x] = static_cast<float>(sum / (hi - lo + 1));
            const int xOut = x - r, xIn = x + r + 1;
            if (xOut >= 0) sum -= s[xOut];
            if (xIn < w) sum += s[xIn];
        }
    });

    // Vertical pass (column blocks in parallel, walking rows for cache-friendly access)
    dst.resize(size);
    constexpr int kBlock = 64;
    const int blocks = (w + kBlock - 1) / kBlock;
    parallelFor(0, blocks, 1, [&](int b) {
        const int x0 = b * kBlock;
        const int bw = std::min(w, x0 + kBlock) - x0;
        std::vector<double> sum(bw, 0.0);
        for (int y = 0; y <= std::min(r, h - 1); ++y) {
            const float* t = tmp.data() + static_cast<size_t>(y) * w + x0;
            for (int i = 0; i < bw; ++i) sum[i] += t[i];
        }
        for (int y = 0; y < h; ++y) {
            const int lo = std::max(0, y - r), hi = std::min(h - 1, y + r);
            const double inv = 1.0 / (hi - lo + 1);
            float* d = dst.data() + static_cast<size_t>(y) * w + x0;
            for (int i = 0; i < bw; ++i) d[i] = static_cast<float>(sum[i] * inv);
            const int yOut = y - r, yIn = y + r + 1;
            if (yOut >= 0) {
                const float* t = tmp.data() + static_cast<size_t>(yOut) * w + x0;
                for (int i = 0; i < bw; ++i) sum[i] -= t[i];
            }
            if (yIn < h) {
                const float* t = tmp.data() + static_cast<size_t>(yIn) * w + x0;
                for (int i = 0; i < bw; ++i) sum[i] += t[i];
            }
        }
    });
}

MergeResult MergeEngine::merge(
    const std::vector<const RawImage*>& frames,
    std::vector<double> exposures,
    const MergeSettings& settings,
    ProgressFn progress,
    std::stop_token stopToken
) {
    MergeResult res;
    auto cancelled = [&] {
        res.status = Status::error("Cancelled");
        return res;
    };

    // ---- Validation (callers may hand us anything, e.g. a group joined across cameras) ----
    if (frames.empty()) {
        res.status = Status::error("No frames to merge");
        return res;
    }
    const size_t numFrames = frames.size();
    if (numFrames > 255) {
        res.status = Status::error("Too many frames in one bracket (max 255)");
        return res;
    }
    const int width = frames[0]->width;
    const int height = frames[0]->height;
    if (width < 2 || height < 2) {
        res.status = Status::error("Image too small to merge");
        return res;
    }
    for (const RawImage* f : frames) {
        if (!f || f->width != width || f->height != height ||
            f->data.width != width || f->data.height != height) {
            res.status = Status::error("Image dimensions mismatch in bracket" +
                                       (f ? ": " + f->originalFileName : std::string()));
            return res;
        }
        if (f->cfa.c != frames[0]->cfa.c) {
            res.status = Status::error("CFA pattern mismatch in bracket: " + f->originalFileName);
            return res;
        }
    }
    if (!exposures.empty()) {
        if (exposures.size() != numFrames) {
            res.status = Status::error("Exposure count does not match frame count");
            return res;
        }
        for (double e : exposures) {
            if (!(e > 0.0) || !std::isfinite(e)) {
                res.status = Status::error("Invalid relative exposure");
                return res;
            }
        }
    }

    res.width = width;
    res.height = height;
    if (progress) progress(0.05f, "Initializing merge grid...");
    if (stopToken.stop_requested()) return cancelled();

    // Reference frame (default: middle exposure). Before estimation, order by EXIF priors.
    std::vector<double> ordering = exposures;
    if (ordering.empty()) {
        for (const RawImage* f : frames) ordering.push_back(f->exposure.relativeExposure());
    }
    std::vector<size_t> orderDesc(numFrames);
    std::iota(orderDesc.begin(), orderDesc.end(), size_t{0});
    std::stable_sort(orderDesc.begin(), orderDesc.end(), [&](size_t a, size_t b) {
        return ordering[a] > ordering[b];
    });

    int refIdx = settings.referenceFrame;
    if (refIdx < 0 || refIdx >= static_cast<int>(numFrames)) {
        refIdx = static_cast<int>(orderDesc[numFrames / 2]);
    }

    // Alignment: compute even Bayer shifts relative to reference frame
    std::vector<ImageShift> shifts(numFrames, {0, 0});
    if (settings.align == AlignMode::Translation && numFrames > 1) {
        if (progress) progress(0.08f, "Aligning bracket exposures (Ward MTB)...");
        shifts = ImageAligner::alignBracket(frames, refIdx, settings.maxAlignShift);
    }
    if (stopToken.stop_requested()) return cancelled();

    res.shifts.resize(numFrames);
    for (size_t i = 0; i < numFrames; ++i) {
        res.shifts[i] = {shifts[i].dx, shifts[i].dy};
    }

    // Exposure estimation on the aligned frames
    if (exposures.empty()) {
        if (progress) progress(0.15f, "Estimating relative exposures...");
        exposures = ExposureEstimator::estimateExposures(frames, shifts);
        std::stable_sort(orderDesc.begin(), orderDesc.end(), [&](size_t a, size_t b) {
            return exposures[a] > exposures[b];
        });
    }
    res.exposures = exposures;
    const size_t darkestFrameIdx = orderDesc.back();

    // Region covered by every frame after alignment
    {
        int x0 = 0, y0 = 0, x1 = width, y1 = height;
        for (const auto& s : shifts) {
            x0 = std::max(x0, -s.dx);
            y0 = std::max(y0, -s.dy);
            x1 = std::min(x1, width - s.dx);
            y1 = std::min(y1, height - s.dy);
        }
        if (x1 <= x0 || y1 <= y0) {
            x0 = 0, y0 = 0, x1 = width, y1 = height;
        }
        res.validRect = {x0, y0, x1, y1};
    }

    std::vector<NormLut> luts;
    luts.reserve(numFrames);
    std::vector<float> invExposure(numFrames);
    for (size_t i = 0; i < numFrames; ++i) {
        luts.emplace_back(*frames[i]);
        invExposure[i] = static_cast<float>(1.0 / exposures[i]);
    }

    // 2x2 quad dimensions
    const int qw = width / 2;
    const int qh = height / 2;
    const size_t numQuads = static_cast<size_t>(qw) * qh;

    // Quad value (max and mean of the 4 CFA samples) of frame i at output quad (qx, qy),
    // or false when the shifted quad falls outside the frame.
    auto sampleQuad = [&](size_t i, int qx, int qy, float& vMax, float& vMean) {
        const int srcQx = qx + shifts[i].dx / 2;
        const int srcQy = qy + shifts[i].dy / 2;
        if (srcQy < 0 || srcQy >= qh || srcQx < 0 || srcQx >= qw) return false;
        const RawImage& img = *frames[i];
        const int by = srcQy * 2, bx = srcQx * 2;
        const float a = luts[i].at(img, by, bx);
        const float b = luts[i].at(img, by, bx + 1);
        const float c = luts[i].at(img, by + 1, bx);
        const float d = luts[i].at(img, by + 1, bx + 1);
        vMax = std::max(std::max(a, b), std::max(c, d));
        vMean = 0.25f * (a + b + c + d);
        return true;
    };

    // Saturated (or unavailable) quad masks per frame, dilated by one quad
    if (progress) progress(0.20f, "Detecting highlights & clipping...");
    std::vector<std::vector<uint8_t>> clipMasks(numFrames);
    for (size_t i = 0; i < numFrames; ++i) {
        std::vector<uint8_t> raw(numQuads, 0);
        parallelFor(0, qh, 32, [&](int qy) {
            for (int qx = 0; qx < qw; ++qx) {
                float vMax = 0.f, vMean = 0.f;
                const bool inside = sampleQuad(i, qx, qy, vMax, vMean);
                raw[static_cast<size_t>(qy) * qw + qx] = (!inside || vMax > settings.clipThreshold) ? 1 : 0;
            }
        });
        dilate3x3(raw, clipMasks[i], qw, qh);
    }
    if (stopToken.stop_requested()) return cancelled();

    // Motion deghosting: detect radiance divergence against reference frame
    if (settings.deghost != DeghostLevel::Off && numFrames > 1) {
        if (progress) progress(0.35f, "Detecting motion and ghosting...");
        float deghostThreshold = 0.45f; // Medium
        if (settings.deghost == DeghostLevel::Low) {
            deghostThreshold = 0.65f;
        } else if (settings.deghost == DeghostLevel::High) {
            deghostThreshold = 0.28f;
        }

        std::vector<uint8_t> motion(numQuads, 0);
        for (size_t i = 0; i < numFrames; ++i) {
            if (static_cast<int>(i) == refIdx) continue;
            parallelFor(0, qh, 32, [&](int qy) {
                for (int qx = 0; qx < qw; ++qx) {
                    const size_t qIdx = static_cast<size_t>(qy) * qw + qx;
                    if (clipMasks[refIdx][qIdx] || clipMasks[i][qIdx]) continue;
                    float refMax, refMean, srcMax, srcMean;
                    if (!sampleQuad(refIdx, qx, qy, refMax, refMean) || !sampleQuad(i, qx, qy, srcMax, srcMean)) continue;
                    const double radRef = refMean * invExposure[refIdx];
                    const double radSrc = srcMean * invExposure[i];
                    const double baseline = std::max(radRef, 0.02);
                    if (std::abs(radSrc - radRef) / baseline > deghostThreshold) {
                        motion[qIdx] = 1;
                    }
                }
            });
        }

        // Dilate motion mask and exclude non-reference frames from motion areas
        dilate3x3(motion, res.motionMask, qw, qh);
        parallelFor(0, qh, 64, [&](int qy) {
            for (int qx = 0; qx < qw; ++qx) {
                const size_t qIdx = static_cast<size_t>(qy) * qw + qx;
                if (!res.motionMask[qIdx]) continue;
                for (size_t i = 0; i < numFrames; ++i) {
                    if (static_cast<int>(i) != refIdx) clipMasks[i][qIdx] = 1;
                }
            }
        });
    }

    if (stopToken.stop_requested()) return cancelled();

    // ---- Per-quad raw weights ----
    if (progress) progress(0.40f, "Computing exposure selection map...");
    std::vector<std::vector<float>> rawWeights(numFrames, std::vector<float>(numQuads, 0.f));
    res.sourceMap.assign(numQuads, static_cast<uint8_t>(darkestFrameIdx));
    const bool weighted = (settings.mode == MergeMode::WeightedAverage);
    const float clipLo = std::max(0.05f, settings.clipThreshold - 0.15f);

    parallelFor(0, qh, 32, [&](int qy) {
        for (int qx = 0; qx < qw; ++qx) {
            const size_t q = static_cast<size_t>(qy) * qw + qx;
            if (!weighted) {
                // Mode A: brightest non-clipped frame (reference in motion areas)
                size_t best = darkestFrameIdx;
                if (!res.motionMask.empty() && res.motionMask[q] && !clipMasks[refIdx][q]) {
                    best = static_cast<size_t>(refIdx);
                } else {
                    for (size_t idx : orderDesc) {
                        if (!clipMasks[idx][q]) {
                            best = idx;
                            break;
                        }
                    }
                }
                rawWeights[best][q] = 1.0f;
                res.sourceMap[q] = static_cast<uint8_t>(best);
            } else {
                // Mode B: every usable frame contributes, weighted by exposure (shot-noise SNR)
                // and a hat function that fades out near black and near the clip threshold.
                float bestW = 0.f;
                for (size_t i = 0; i < numFrames; ++i) {
                    if (clipMasks[i][q]) continue;
                    float vMax, vMean;
                    if (!sampleQuad(i, qx, qy, vMax, vMean)) continue;
                    const float hat = smoothstep(0.0f, 0.02f, vMean) *
                                      (1.0f - smoothstep(clipLo, settings.clipThreshold, vMax));
                    const float w = hat * static_cast<float>(exposures[i]);
                    rawWeights[i][q] = w;
                    if (w > bestW) {
                        bestW = w;
                        res.sourceMap[q] = static_cast<uint8_t>(i);
                    }
                }
            }
        }
    });
    clipMasks.clear();
    clipMasks.shrink_to_fit();

    if (stopToken.stop_requested()) return cancelled();

    if (progress) progress(0.55f, "Feathering selection transitions...");
    std::vector<std::vector<float>> weights(numFrames);
    for (size_t i = 0; i < numFrames; ++i) {
        boxBlur2D(rawWeights[i], weights[i], qw, qh, settings.featherRadius);
        rawWeights[i] = {};
    }

    // Normalize weights
    parallelFor(0, qh, 64, [&](int qy) {
        for (int qx = 0; qx < qw; ++qx) {
            const size_t q = static_cast<size_t>(qy) * qw + qx;
            float sum = 0.f;
            for (size_t i = 0; i < numFrames; ++i) sum += weights[i][q];
            if (sum > 1e-6f) {
                const float inv = 1.0f / sum;
                for (size_t i = 0; i < numFrames; ++i) weights[i][q] *= inv;
            } else {
                for (size_t i = 0; i < numFrames; ++i) weights[i][q] = 0.f;
                weights[darkestFrameIdx][q] = 1.0f;
            }
        }
    });

    if (stopToken.stop_requested()) return cancelled();

    if (progress) progress(0.75f, "Blending floating-point radiance...");
    res.cfa = ImageF(width, height);

    // Radiance reconstruction with alignment shifts applied
    parallelFor(0, height, 16, [&](int y) {
        if (stopToken.stop_requested()) return;
        const int qy = std::min(y / 2, qh - 1);
        const size_t quadRowOffset = static_cast<size_t>(qy) * qw;
        float* out = res.cfa.row(y);

        for (int x = 0; x < width; ++x) {
            const int qx = std::min(x / 2, qw - 1);
            const size_t qIdx = quadRowOffset + qx;

            float radiance = 0.f;
            float weightSum = 0.f;

            for (size_t i = 0; i < numFrames; ++i) {
                const float w = weights[i][qIdx];
                if (w > 1e-5f) {
                    const int srcY = y + shifts[i].dy;
                    const int srcX = x + shifts[i].dx;
                    if (srcY >= 0 && srcY < height && srcX >= 0 && srcX < width) {
                        radiance += w * luts[i].at(*frames[i], srcY, srcX) * invExposure[i];
                        weightSum += w;
                    }
                }
            }

            if (weightSum > 1e-5f) {
                radiance /= weightSum;
            } else {
                const int rY = std::clamp(y + shifts[refIdx].dy, 0, height - 1);
                const int rX = std::clamp(x + shifts[refIdx].dx, 0, width - 1);
                radiance = luts[refIdx].at(*frames[refIdx], rY, rX) * invExposure[refIdx];
            }

            out[x] = std::clamp(radiance, 0.f, 1.f);
        }
    });
    if (stopToken.stop_requested()) return cancelled();

    // Calculate baseline exposure
    const double eRef = exposures[refIdx];
    const double eDark = exposures[darkestFrameIdx]; // which is 1.0
    res.baselineExposure = (eRef > 0.0 && eDark > 0.0) ? std::log2(eRef / eDark) : 0.0;

    if (progress) progress(1.0f, "Merge complete");
    res.status = Status::success();
    return res;
}

MergeResult MergeEngine::merge(
    const std::vector<RawImage>& frames,
    const std::vector<double>& exposures,
    const MergeSettings& settings,
    ProgressFn progress,
    std::stop_token stopToken
) {
    std::vector<const RawImage*> ptrs;
    ptrs.reserve(frames.size());
    for (const auto& f : frames) ptrs.push_back(&f);
    return merge(ptrs, exposures, settings, std::move(progress), stopToken);
}

} // namespace hdr
