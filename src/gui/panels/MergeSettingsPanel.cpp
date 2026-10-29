#include "MergeSettingsPanel.h"
#include <imgui.h>

namespace hdr::gui {

void MergeSettingsPanel::render(AppController& controller) {
    ImGui::Begin("Merge Settings", nullptr, ImGuiWindowFlags_NoCollapse);

    auto& merge = controller.settings().merge;

    // Mode
    const char* modeItems[] = {"Best Exposure (Mode A)", "Weighted Average (Mode B)"};
    int currentMode = static_cast<int>(merge.mode);
    if (ImGui::Combo("Merge Mode", &currentMode, modeItems, IM_ARRAYSIZE(modeItems))) {
        merge.mode = static_cast<MergeMode>(currentMode);
        controller.triggerPreview();
    }

    // Alignment
    bool enableAlign = (merge.align == AlignMode::Translation);
    if (ImGui::Checkbox("Auto-Align Translation (Ward MTB)", &enableAlign)) {
        merge.align = enableAlign ? AlignMode::Translation : AlignMode::None;
        controller.triggerPreview();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Aligns handheld and drone hover drift using Greg Ward's\nMedian Threshold Bitmap pyramid algorithm (preserves Bayer CFA).");
    }
    if (enableAlign) {
        if (ImGui::SliderInt("Max Shift (px)", &merge.maxAlignShift, 16, 256)) {
            controller.triggerPreview();
        }
    }

    // Motion Deghosting
    const char* deghostItems[] = {"Off", "Low (Subtle / Minimal)", "Medium (Standard)", "High (Aggressive)"};
    int currentDeghost = static_cast<int>(merge.deghost);
    if (ImGui::Combo("Motion Deghosting", &currentDeghost, deghostItems, IM_ARRAYSIZE(deghostItems))) {
        merge.deghost = static_cast<DeghostLevel>(currentDeghost);
        controller.triggerPreview();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Detects moving objects (windblown trees, cars, people, water) by comparing\nphotometric radiance against reference frame, masking them to prevent ghosting artifacts.");
    }

    // Bits
    const char* bitsItems[] = {"Auto (16-bit Integer)", "16-bit Integer (Capture One)", "16-bit Float", "24-bit Float (Adobe only)", "32-bit Float"};
    int currentBits = 0;
    if (merge.bits == FloatBits::Auto) currentBits = 0;
    else if (merge.bits == FloatBits::Int16) currentBits = 1;
    else if (merge.bits == FloatBits::F16) currentBits = 2;
    else if (merge.bits == FloatBits::F24) currentBits = 3;
    else if (merge.bits == FloatBits::F32) currentBits = 4;

    if (ImGui::Combo("Output Precision", &currentBits, bitsItems, IM_ARRAYSIZE(bitsItems))) {
        if (currentBits == 0) merge.bits = FloatBits::Auto;
        else if (currentBits == 1) merge.bits = FloatBits::Int16;
        else if (currentBits == 2) merge.bits = FloatBits::F16;
        else if (currentBits == 3) merge.bits = FloatBits::F24;
        else if (currentBits == 4) merge.bits = FloatBits::F32;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Capture One cannot open floating-point DNGs.\nUse 16-bit Integer for Capture One; float for Lightroom/ACR, darktable, RawTherapee.\n"
                          "16-bit integer stores the merged range linearly, so very wide brackets lose some deep-shadow precision.");
    }

    ImGui::Separator();

    // Mask / Clip parameters
    if (ImGui::SliderFloat("Clip Threshold", &merge.clipThreshold, 0.80f, 0.99f, "%.2f")) {
        controller.triggerPreview();
    }
    if (ImGui::SliderInt("Feather Radius (quads)", &merge.featherRadius, 1, 32)) {
        controller.triggerPreview();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Blend width between exposures, in 2x2 Bayer quads (1 quad = 2 pixels).");
    }

    ImGui::Separator();

    // Output options
    ImGui::Checkbox("Embed Full-Size Preview", &merge.embedPreview);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off: only a small 256 px thumbnail is embedded (smaller files).");
    }
    ImGui::Checkbox("Write Lightroom XMP settings", &merge.writeLightroomSettings);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stores the Adjustments sliders as Lightroom/ACR develop settings (crs:*).\n"
                          "Exposure, Contrast, Highlights, Shadows, Whites, Blacks, Vibrance and Saturation are exported;\n"
                          "temperature, tint and the preview tone-map operator have no Lightroom equivalent.");
    }
    ImGui::Checkbox("Skip Existing Outputs", &merge.skipExisting);
    auto& s = controller.settings();
    if (ImGui::SliderInt("Parallel Jobs (0 = auto)", &s.threadCount, 0, 16)) {
        s.threadCount = std::clamp(s.threadCount, 0, 64);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How many brackets are merged at the same time. Each job already uses all cores;\n"
                          "more jobs mostly help with slow disks. Memory use is capped automatically.");
    }

    // Naming pattern
    char patternBuf[200];
    strncpy_s(patternBuf, merge.namePattern.c_str(), _TRUNCATE);
    if (ImGui::InputText("Naming Pattern", patternBuf, sizeof(patternBuf))) {
        merge.namePattern = patternBuf;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        controller.settings().save();
    }
    ImGui::TextDisabled("Tokens: {first}, {last}, {ref}, {index}, {n}, {iso}, {shutter}, {fnumber}, {camera}, {date}");

    ImGui::End();
}

} // namespace hdr::gui
