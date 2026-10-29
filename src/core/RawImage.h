#pragma once
#include "Types.h"
#include <vector>
#include <array>
#include <string>

namespace hdr {

struct RawImage {
    ImageU16 data;
    int width = 0;
    int height = 0;
    CfaPattern cfa;

    std::array<float, 4> blackLevel{0.f, 0.f, 0.f, 0.f};
    float whiteLevel = 65535.f;

    ExposureInfo exposure;
    ColorInfo color;

    // DNG tags & opcodes copied or parsed
    std::vector<uint8_t> opcodeList1;
    std::vector<uint8_t> opcodeList2;
    std::vector<uint8_t> opcodeList3;

    std::array<int, 4> activeArea{0, 0, 0, 0};       // top, left, bottom, right
    std::array<int, 2> defaultCropOrigin{0, 0};     // left, top
    std::array<int, 2> defaultCropSize{0, 0};       // width, height

    std::array<float, 9> colorMatrix1{};
    std::array<float, 9> colorMatrix2{};
    int calibrationIlluminant1 = 0;
    int calibrationIlluminant2 = 0;
    std::array<float, 9> forwardMatrix1{};
    std::array<float, 9> forwardMatrix2{};
    std::vector<float> asShotNeutral; // 3 floats

    int orientation = 1;
    std::string originalFileName;

    // Helper: normalized float raw pixel value [0, 1] for channel c at (y, x)
    float normalizedAt(int y, int x) const {
        uint8_t c = cfa.at(y, x);
        float raw = static_cast<float>(data.at(y, x));
        float bl = blackLevel[c];
        float wl = whiteLevel;
        if (wl <= bl) return 0.f;
        float v = (raw - bl) / (wl - bl);
        return v < 0.f ? 0.f : v;
    }
};

} // namespace hdr
