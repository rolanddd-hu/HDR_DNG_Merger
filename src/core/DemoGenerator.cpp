#include "DemoGenerator.h"
#include "RawLoader.h"
#include "DngWriter.h"
#include "MetadataCopier.h"
#include "MergeJob.h"
#include "BracketGrouper.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <iostream>

namespace hdr {

DemoGenResult DemoGenerator::generateBracket(
    const fs::path& templatePath,
    const fs::path& outputDir,
    std::function<void(float, const std::string&)> progress
) {
    DemoGenResult res;
    if (progress) progress(0.05f, "Loading template DNG metadata...");

    auto [sFull, origImg] = RawLoader::loadFull(templatePath);
    if (!sFull) {
        res.status = Status::error("Failed to load template file: " + sFull.message);
        return res;
    }

    int W = origImg.width;
    int H = origImg.height;
    if (W <= 0 || H <= 0) {
        res.status = Status::error("Invalid dimensions in template file");
        return res;
    }

    fs::create_directories(outputDir);

    if (progress) progress(0.15f, "Synthesizing 6-stop HDR radiance scene...");

    // White balance neutral gains for raw synthesis
    float wbR = (origImg.asShotNeutral.size() >= 3 && origImg.asShotNeutral[0] > 0.f)
                ? origImg.asShotNeutral[0] : 0.488f;
    float wbG = 1.0f;
    float wbB = (origImg.asShotNeutral.size() >= 3 && origImg.asShotNeutral[2] > 0.f)
                ? origImg.asShotNeutral[2] : 0.547f;

    // Radiance scene buffer (R, G, B)
    std::vector<float> radR(static_cast<size_t>(W) * H, 0.20f);
    std::vector<float> radG(static_cast<size_t>(W) * H, 0.20f);
    std::vector<float> radB(static_cast<size_t>(W) * H, 0.20f);

    // 1. Room background (warm neutral walls, midtone radiance 0.22)
    for (int y = 0; y < H; ++y) {
        float ny = static_cast<float>(y) / H;
        for (int x = 0; x < W; ++x) {
            size_t idx = static_cast<size_t>(y) * W + x;
            radR[idx] = 0.24f - 0.05f * ny;
            radG[idx] = 0.22f - 0.05f * ny;
            radB[idx] = 0.20f - 0.04f * ny;
        }
    }

    // 2. Color chart on left wall (x: 200..1400, y: 200..1000)
    int chartX0 = static_cast<int>(W * 0.06f);
    int chartY0 = static_cast<int>(H * 0.10f);
    int patchW = static_cast<int>(W * 0.07f);
    int patchH = static_cast<int>(H * 0.12f);
    int gap = static_cast<int>(W * 0.02f);

    struct ColorPatch { float r, g, b; };
    ColorPatch patches[6] = {
        {0.70f, 0.10f, 0.10f}, // Red
        {0.10f, 0.65f, 0.10f}, // Green
        {0.10f, 0.20f, 0.75f}, // Blue
        {0.75f, 0.70f, 0.10f}, // Yellow
        {0.10f, 0.65f, 0.70f}, // Cyan
        {0.65f, 0.10f, 0.65f}  // Magenta
    };

    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            int pIndex = row * 3 + col;
            int px0 = chartX0 + col * (patchW + gap);
            int py0 = chartY0 + row * (patchH + gap);
            for (int py = py0; py < py0 + patchH && py < H; ++py) {
                for (int px = px0; px < px0 + patchW && px < W; ++px) {
                    size_t idx = static_cast<size_t>(py) * W + px;
                    radR[idx] = patches[pIndex].r;
                    radG[idx] = patches[pIndex].g;
                    radB[idx] = patches[pIndex].b;
                }
            }
        }
    }

    // 3. Dark shadow bookcase at bottom (x: 100..3900, y: 1350..2150)
    // Deep shadows: base radiance 0.012 (4 stops below midtone)
    int shX0 = static_cast<int>(W * 0.05f);
    int shX1 = static_cast<int>(W * 0.95f);
    int shY0 = static_cast<int>(H * 0.62f);
    int shY1 = static_cast<int>(H * 0.95f);
    for (int y = shY0; y < shY1 && y < H; ++y) {
        for (int x = shX0; x < shX1 && x < W; ++x) {
            size_t idx = static_cast<size_t>(y) * W + x;
            // Vertical book slats pattern
            int stripe = (x / 40) % 6;
            float stripeMult = 0.6f + 0.15f * stripe; // 0.007 to 0.018
            radR[idx] = 0.012f * stripeMult;
            radG[idx] = 0.011f * stripeMult;
            radB[idx] = 0.014f * stripeMult;
        }
    }

    // 4. THE EXTREME HIGHLIGHT WINDOW (x: 1700..3850, y: 120..1250)
    // Radiance ranges from 2.0 up to 8.0 (1 to 3 stops above mid clip)
    int winX0 = static_cast<int>(W * 0.44f);
    int winX1 = static_cast<int>(W * 0.96f);
    int winY0 = static_cast<int>(H * 0.06f);
    int winY1 = static_cast<int>(H * 0.58f);
    int winW = winX1 - winX0;
    int winH = winY1 - winY0;

    int sunCenterX = static_cast<int>(winX0 + winW * 0.65f);
    int sunCenterY = static_cast<int>(winY0 + winH * 0.40f);
    float sunRadius = winH * 0.28f;

    for (int y = winY0; y < winY1 && y < H; ++y) {
        float ny = static_cast<float>(y - winY0) / winH; // 0 = top, 1 = bottom
        for (int x = winX0; x < winX1 && x < W; ++x) {
            float nx = static_cast<float>(x - winX0) / winW;
            size_t idx = static_cast<size_t>(y) * W + x;

            // Window frame border (dark wood border)
            if (x < winX0 + 20 || x > winX1 - 20 || y < winY0 + 20 || y > winY1 - 20) {
                radR[idx] = 0.05f;
                radG[idx] = 0.04f;
                radB[idx] = 0.03f;
                continue;
            }

            // Sunset vertical sky radiance: golden hour sky (rad 2.5 to 5.5)
            float baseRad = 2.5f + 3.0f * (1.0f - std::abs(ny - 0.5f) * 2.0f);
            float r = baseRad * 1.35f;
            float g = baseRad * 0.75f;
            float b = baseRad * 0.25f + 0.8f * (1.0f - ny);

            // Glowing Sun disc (radiance rises up to 8.0)
            float dx = static_cast<float>(x - sunCenterX);
            float dy = static_cast<float>(y - sunCenterY);
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < sunRadius) {
                float sunFactor = 1.0f - (dist / sunRadius);
                float sunRad = 3.5f + 4.5f * (sunFactor * sunFactor); // up to 8.0
                r += sunRad * 1.4f;
                g += sunRad * 1.0f;
                b += sunRad * 0.6f;
            }

            // Mountain silhouette along bottom of window
            float mtnY = winY0 + winH * (0.80f + 0.12f * std::sin(nx * 12.0f) + 0.06f * std::cos(nx * 27.0f));
            if (y > mtnY) {
                // Silhouetted mountain: dark foreground against sunset
                r = 0.15f;
                g = 0.12f;
                b = 0.18f;
            }

            // High-Contrast Highlight Recovery Target Circles inside window
            float c1Dist = std::sqrt(std::pow(x - (winX0 + winW * 0.22f), 2.0f) + std::pow(y - (winY0 + winH * 0.42f), 2.0f));
            if (c1Dist > 50.f && c1Dist < 120.f) {
                int ring = static_cast<int>(c1Dist / 15.0f);
                if (ring % 2 == 0) {
                    // Dark contrasting concentric target lines (rad 0.8 vs 5.0)
                    r *= 0.15f;
                    g *= 0.15f;
                    b *= 0.15f;
                }
            }

            // Step wedges across top of window:
            // 4 steps with distinct radiances: +1 EV (2.0), +1.8 EV (3.5), +2.3 EV (5.0), +3.0 EV (7.5)
            if (y >= winY0 + 40 && y <= winY0 + 130 && x >= winX0 + 50 && x <= winX0 + winW * 0.85f) {
                float stepNorm = static_cast<float>(x - (winX0 + 50)) / (winW * 0.85f - 50);
                int stepIdx = std::clamp(static_cast<int>(stepNorm * 4.0f), 0, 3);
                float stepRad[4] = {2.0f, 3.5f, 5.0f, 7.5f};
                float sR = stepRad[stepIdx];
                // Invert every 20 pixels to create high-frequency test bar pattern
                bool bar = ((x / 20) % 2 == 0);
                if (bar) sR *= 0.25f; // High contrast test bars
                r = sR * 1.2f;
                g = sR * 0.9f;
                b = sR * 0.4f;
            }

            radR[idx] = r;
            radG[idx] = g;
            radB[idx] = b;
        }
    }

    // Helper lambda to sample CFA raw value at (y, x) given an exposure multiplier
    auto sampleCfa = [&](int y, int x, float expMult) -> float {
        size_t idx = static_cast<size_t>(y) * W + x;
        uint8_t c = origImg.cfa.at(y, x);
        float radiance = 0.f;
        float wbGain = 1.f;
        if (c == 0) {
            radiance = radR[idx];
            wbGain = wbR;
        } else if (c == 1 || c == 3) {
            radiance = radG[idx];
            wbGain = wbG;
        } else if (c == 2) {
            radiance = radB[idx];
            wbGain = wbB;
        }
        float rawVal = radiance * wbGain * expMult;
        return std::clamp(rawVal, 0.0f, 1.0f);
    };

    // 5. Generate 3 Bracketed Frames:
    // Frame 1: DARK   (1/4000s, scale = 0.125, -3 EV)
    // Frame 2: MID    (1/500s,  scale = 1.000,  0 EV reference)
    // Frame 3: BRIGHT (1/60s,   scale = 8.000, +3 EV)
    fs::path darkPath = outputDir / "DEMO_001_DARK.dng";
    fs::path midPath = outputDir / "DEMO_002_MID.dng";
    fs::path brightPath = outputDir / "DEMO_003_BRIGHT.dng";
    fs::path hdrPath = outputDir / "DEMO_HDR.dng";

    DngWriteParams writeParams;
    writeParams.bits = FloatBits::Int16; // 16-bit int for full Capture One compatibility
    writeParams.compress = false;
    writeParams.embedPreview = true;

    // --- Write Frame 1 (DARK) ---
    if (progress) progress(0.35f, "Generating DEMO_001_DARK.dng (1/4000s, -3 EV)...");
    {
        ImageF cfa(W, H);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                cfa.at(y, x) = sampleCfa(y, x, 0.125f);
            }
        }
        auto ws = DngWriter::writeFloatDng(darkPath, cfa, W, H, origImg, writeParams);
        if (!ws) return {Status::error("Failed to write dark frame: " + ws.message)};
        MetadataCopier::copyMetadata(templatePath, darkPath);
        MetadataCopier::updateExifExposure(darkPath, 1, 4000, "2026:10:06 12:00:00");
    }

    // --- Write Frame 2 (MID) ---
    if (progress) progress(0.55f, "Generating DEMO_002_MID.dng (1/500s, 0 EV)...");
    {
        ImageF cfa(W, H);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                cfa.at(y, x) = sampleCfa(y, x, 1.0f);
            }
        }
        auto ws = DngWriter::writeFloatDng(midPath, cfa, W, H, origImg, writeParams);
        if (!ws) return {Status::error("Failed to write mid frame: " + ws.message)};
        MetadataCopier::copyMetadata(templatePath, midPath);
        MetadataCopier::updateExifExposure(midPath, 1, 500, "2026:10:06 12:00:01");
    }

    // --- Write Frame 3 (BRIGHT) ---
    if (progress) progress(0.75f, "Generating DEMO_003_BRIGHT.dng (1/60s, +3 EV)...");
    {
        ImageF cfa(W, H);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                cfa.at(y, x) = sampleCfa(y, x, 8.0f);
            }
        }
        auto ws = DngWriter::writeFloatDng(brightPath, cfa, W, H, origImg, writeParams);
        if (!ws) return {Status::error("Failed to write bright frame: " + ws.message)};
        MetadataCopier::copyMetadata(templatePath, brightPath);
        MetadataCopier::updateExifExposure(brightPath, 1, 60, "2026:10:06 12:00:02");
    }

    // 6. Merge the 3 frames into DEMO_HDR.dng
    if (progress) progress(0.85f, "Merging bracket into DEMO_HDR.dng...");
    {
        auto scanned = FileScanner::scanPath(outputDir);
        // Only keep the 3 demo files
        std::vector<FileEntry> demoFiles;
        for (const auto& fe : scanned) {
            if (fe.path == darkPath || fe.path == midPath || fe.path == brightPath) {
                demoFiles.push_back(fe);
            }
        }
        FileScanner::sortEntries(demoFiles);
        auto grouping = BracketGrouper::groupFixedN(demoFiles, 3);
        if (grouping.groups.empty()) {
            return {Status::error("Failed to group demo bracket files")};
        }

        MergeSettings mergeSettings;
        mergeSettings.bits = FloatBits::Int16;
        mergeSettings.align = AlignMode::None;
        mergeSettings.clipThreshold = 0.92f;
        mergeSettings.featherRadius = 12;
        mergeSettings.referenceFrame = 1; // DEMO_MID is the reference frame

        auto mergeStatus = MergeJob::execute(grouping.groups[0], hdrPath, mergeSettings, nullptr);
        if (!mergeStatus) {
            return {Status::error("Failed to merge demo bracket: " + mergeStatus.message)};
        }
    }

    if (progress) progress(1.0f, "Demo bracket generated successfully!");

    res.status = Status::success();
    res.darkPath = darkPath;
    res.midPath = midPath;
    res.brightPath = brightPath;
    res.hdrPath = hdrPath;
    res.evSpan = 6.0;
    return res;
}

} // namespace hdr
