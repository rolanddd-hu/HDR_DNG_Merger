#pragma once
// Bayer helpers shared by the loader, merge engine, preview pipeline and DNG writer.

#include "Parallel.h"
#include "RawImage.h"
#include <algorithm>
#include <array>
#include <vector>

namespace hdr {

// Position of R, G1, G2 and B inside a 2x2 Bayer quad.
struct QuadOffsets {
    int ry = 0, rx = 0, g1y = 0, g1x = 1, g2y = 1, g2x = 0, by = 1, bx = 1;

    static QuadOffsets from(const CfaPattern& cfa) {
        int r = -1, b = -1, g1 = -1, g2 = -1;
        for (int i = 0; i < 4; ++i) {
            const uint8_t c = cfa.c[i];
            if (c == 0) r = i;
            else if (c == 2) b = i;
            else if (g1 < 0) g1 = i;
            else g2 = i;
        }
        if (r < 0) r = 0;
        if (g1 < 0) g1 = 1;
        if (g2 < 0) g2 = 2;
        if (b < 0) b = 3;
        return {r / 2, r % 2, g1 / 2, g1 % 2, g2 / 2, g2 % 2, b / 2, b % 2};
    }
};

// Lookup tables mapping raw uint16 values to black-subtracted, white-normalised floats, one table
// per 2x2 CFA position. Replaces the per-pixel CFA lookup and division of RawImage::normalizedAt.
class NormLut {
public:
    NormLut() = default;
    explicit NormLut(const RawImage& img) {
        for (int pos = 0; pos < 4; ++pos) {
            const float bl = img.blackLevel[img.cfa.c[pos]];
            const float wl = img.whiteLevel;
            auto& t = tables_[pos];
            t.resize(65536);
            if (wl <= bl) {
                std::fill(t.begin(), t.end(), 0.f);
                continue;
            }
            const float inv = 1.f / (wl - bl);
            for (int v = 0; v < 65536; ++v) {
                t[v] = std::max(0.f, (static_cast<float>(v) - bl) * inv);
            }
        }
    }

    // Table for pixels at (y, x) with the given parity.
    const float* table(int y, int x) const { return tables_[((y & 1) << 1) | (x & 1)].data(); }
    float at(const RawImage& img, int y, int x) const { return table(y, x)[img.data.at(y, x)]; }

private:
    std::array<std::vector<float>, 4> tables_;
};

// Converts camera RGB (white-balanced via camMul) to linear sRGB with the rgbCam matrix.
inline void cameraToLinearSrgb(float r, float g, float b, const ColorInfo& color, float out[3]) {
    r *= static_cast<float>(color.camMul[0]);
    g *= static_cast<float>(color.camMul[1]);
    b *= static_cast<float>(color.camMul[2]);
    const auto& m = color.rgbCam;
    const bool hasMatrix = m[0] != 0.0 || m[4] != 0.0 || m[8] != 0.0;
    if (!hasMatrix) {
        out[0] = r;
        out[1] = g;
        out[2] = b;
    } else {
        out[0] = static_cast<float>(m[0] * r + m[1] * g + m[2] * b);
        out[1] = static_cast<float>(m[3] * r + m[4] * g + m[5] * b);
        out[2] = static_cast<float>(m[6] * r + m[7] * g + m[8] * b);
    }
    for (int i = 0; i < 3; ++i) out[i] = std::max(out[i], 0.f);
}

// Half-resolution superpixel debayer: one linear sRGB pixel per 2x2 quad.
// sample(y, x) must return the normalised CFA value at full-resolution (y, x).
template <typename SampleFn>
ImageRgbF quadsToRgb(int width, int height, const CfaPattern& cfa, const ColorInfo& color,
                     float gain, SampleFn&& sample) {
    const int halfW = width / 2;
    const int halfH = height / 2;
    ImageRgbF rgb(halfW, halfH);
    const QuadOffsets o = QuadOffsets::from(cfa);
    parallelFor(0, halfH, 16, [&](int y) {
        const int baseY = y * 2;
        for (int x = 0; x < halfW; ++x) {
            const int baseX = x * 2;
            const float r = sample(baseY + o.ry, baseX + o.rx) * gain;
            const float g = 0.5f * (sample(baseY + o.g1y, baseX + o.g1x) + sample(baseY + o.g2y, baseX + o.g2x)) * gain;
            const float b = sample(baseY + o.by, baseX + o.bx) * gain;
            cameraToLinearSrgb(r, g, b, color, rgb.pixel(y, x));
        }
    });
    return rgb;
}

}  // namespace hdr
