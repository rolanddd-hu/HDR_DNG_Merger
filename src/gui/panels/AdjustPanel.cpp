#include "AdjustPanel.h"
#include "../TonePresets.h"
#include <imgui.h>
#include <implot.h>

namespace hdr::gui {

static bool sliderResetable(const char* label, float* v, float v_min, float v_max, float default_val = 0.0f, const char* format = "%.1f") {
    bool changed = ImGui::SliderFloat(label, v, v_min, v_max, format);
    if (ImGui::IsItemHovered()) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            *v = default_val;
            changed = true;
        }
        ImGui::SetTooltip("Double-click: reset to %.1f\nCtrl+Click: type value directly", default_val);
    }
    return changed;
}

void AdjustPanel::render(AppController& controller) {
    ImGui::Begin("Adjustments", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::TextDisabled("Preview & Lightroom metadata (non-destructive)");
    ImGui::Spacing();

    // 1. Histogram Plot
    const auto& hist = controller.histogram();
    if (ImPlot::BeginPlot("##Histogram", ImVec2(-1, 130), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxesLimits(0, 255, 0, 1.05, ImPlotCond_Always);

        if (!hist.r.empty() && !hist.xValues.empty()) {
            ImPlot::SetNextFillStyle(ImVec4(0.95f, 0.25f, 0.25f, 0.20f));
            ImPlot::PlotShaded("R##fill", hist.xValues.data(), hist.r.data(), HistogramData::kNumBins);
            ImPlot::SetNextLineStyle(ImVec4(0.95f, 0.25f, 0.25f, 0.85f), 1.5f);
            ImPlot::PlotLine("R", hist.xValues.data(), hist.r.data(), HistogramData::kNumBins);

            ImPlot::SetNextFillStyle(ImVec4(0.25f, 0.90f, 0.35f, 0.20f));
            ImPlot::PlotShaded("G##fill", hist.xValues.data(), hist.g.data(), HistogramData::kNumBins);
            ImPlot::SetNextLineStyle(ImVec4(0.25f, 0.90f, 0.35f, 0.85f), 1.5f);
            ImPlot::PlotLine("G", hist.xValues.data(), hist.g.data(), HistogramData::kNumBins);

            ImPlot::SetNextFillStyle(ImVec4(0.30f, 0.55f, 0.95f, 0.20f));
            ImPlot::PlotShaded("B##fill", hist.xValues.data(), hist.b.data(), HistogramData::kNumBins);
            ImPlot::SetNextLineStyle(ImVec4(0.30f, 0.55f, 0.95f, 0.85f), 1.5f);
            ImPlot::PlotLine("B", hist.xValues.data(), hist.b.data(), HistogramData::kNumBins);

            ImPlot::SetNextLineStyle(ImVec4(0.85f, 0.85f, 0.85f, 0.45f), 1.0f);
            ImPlot::PlotLine("Luma", hist.xValues.data(), hist.luma.data(), HistogramData::kNumBins);
        }
        ImPlot::EndPlot();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset All Sliders", ImVec2(-1, 0))) {
        controller.resetToneParams();
    }
    ImGui::Separator();

    // 2. Dynamic Range & Bracket Info
    int gIdx = controller.selectedGroupIndex();
    const auto& groups = controller.bracketGroups();
    if (gIdx >= 0 && gIdx < static_cast<int>(groups.size())) {
        const auto& grp = groups[gIdx];
        if (ImGui::CollapsingHeader("Dynamic Range & Bracket Info", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Bracket:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.18f, 0.77f, 0.84f, 1.0f), "%s (%zu frames)", grp.name.c_str(), grp.files.size());

            ImGui::Text("Bracket EV Span:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "%.2f EV", grp.evSpan);

            double estDR = 10.8 + grp.evSpan;
            ImGui::Text("Est. Scene DR:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.35f, 1.0f), "~%.1f EV (+%.2f EV boost)", estDR, grp.evSpan);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Approximate dynamic range: drone sensor baseline (~10.8 EV) + bracket expansion (+%.2f EV)", grp.evSpan);
            }

            if (grp.files.size() >= 2) {
                double minExp = grp.files[0].info.exposureTime;
                double maxExp = grp.files[0].info.exposureTime;
                for (const auto& f : grp.files) {
                    minExp = std::min(minExp, f.info.exposureTime);
                    maxExp = std::max(maxExp, f.info.exposureTime);
                }
                ImGui::TextDisabled("Exposure range: 1/%.0fs to 1/%.0fs",
                    (minExp > 0 ? 1.0 / minExp : 0.0),
                    (maxExp > 0 ? 1.0 / maxExp : 0.0));
            }
            ImGui::Spacing();
        }
    }

    auto& params = controller.toneParams();

    // 3. Presets
    if (ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("PresetsSection");
        TonePreset activePreset = matchingTonePreset(params);
        int activeIdx = static_cast<int>(activePreset);

        constexpr int kNumPresets = static_cast<int>(TonePreset::Count);
        const char* presetItems[kNumPresets + 1];
        for (int i = 0; i < kNumPresets; ++i) {
            presetItems[i] = tonePresetName(static_cast<TonePreset>(i));
        }
        presetItems[kNumPresets] = "Custom";

        int comboSelection = (activeIdx < kNumPresets) ? activeIdx : kNumPresets;
        if (ImGui::Combo("Preset", &comboSelection, presetItems, kNumPresets + 1)) {
            if (comboSelection < kNumPresets) {
                params = tonePreset(static_cast<TonePreset>(comboSelection));
            }
        }
        if (ImGui::IsItemHovered()) {
            TonePreset p = (comboSelection < kNumPresets)
                ? static_cast<TonePreset>(comboSelection) : TonePreset::Default;
            ImGui::SetTooltip("%s", tonePresetTooltip(p));
        }

        // Quick preset buttons arranged in 2 responsive rows
        ImGui::Spacing();
        ImGui::PushID("QuickPresets");

        float availW = ImGui::GetContentRegionAvail().x;
        float spacing = 6.0f;

        // Row 1: 4 buttons
        float btnW1 = std::max(50.0f, (availW - 3.0f * spacing) / 4.0f);
        if (ImGui::Button("Natural", ImVec2(btnW1, 0))) params = tonePreset(TonePreset::Default);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tonePresetTooltip(TonePreset::Default));
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Max DR", ImVec2(btnW1, 0))) params = tonePreset(TonePreset::MaxDynamicRange);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tonePresetTooltip(TonePreset::MaxDynamicRange));
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Shadows", ImVec2(btnW1, 0))) params = tonePreset(TonePreset::ShadowReveal);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tonePresetTooltip(TonePreset::ShadowReveal));
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Highlights", ImVec2(btnW1, 0))) params = tonePreset(TonePreset::HighlightRecovery);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tonePresetTooltip(TonePreset::HighlightRecovery));

        // Row 2: 3 buttons
        float btnW2 = std::max(60.0f, (availW - 2.0f * spacing) / 3.0f);
        if (ImGui::Button("Landscape", ImVec2(btnW2, 0))) params = tonePreset(TonePreset::LandscapeHDR);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tonePresetTooltip(TonePreset::LandscapeHDR));
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Sunset", ImVec2(btnW2, 0))) params = tonePreset(TonePreset::SunsetGoldenHour);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tonePresetTooltip(TonePreset::SunsetGoldenHour));
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("B&W", ImVec2(btnW2, 0))) params = tonePreset(TonePreset::BlackAndWhiteHDR);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tonePresetTooltip(TonePreset::BlackAndWhiteHDR));
        ImGui::PopID(); // QuickPresets

        ImGui::Spacing();
        ImGui::PopID(); // PresetsSection
    }

    // 4. Exposure & Light
    if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("LightSection");
        sliderResetable("Exposure", &params.exposure, -5.0f, 5.0f, 0.0f, "%+.2f EV");
        sliderResetable("Highlights", &params.highlights, -100.0f, 100.0f, 0.0f, "%+.0f");
        sliderResetable("Shadows", &params.shadows, -100.0f, 100.0f, 0.0f, "%+.0f");
        sliderResetable("Whites", &params.whites, -100.0f, 100.0f, 0.0f, "%+.0f");
        sliderResetable("Blacks", &params.blacks, -100.0f, 100.0f, 0.0f, "%+.0f");
        sliderResetable("Contrast", &params.contrast, -100.0f, 100.0f, 0.0f, "%+.0f");
        ImGui::PopID(); // LightSection
    }

    // 5. Color
    if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("ColorSection");
        sliderResetable("Temp", &params.temperature, -100.0f, 100.0f, 0.0f, "%+.0f");
        sliderResetable("Tint", &params.tint, -100.0f, 100.0f, 0.0f, "%+.0f");
        sliderResetable("Vibrance", &params.vibrance, -100.0f, 100.0f, 0.0f, "%+.0f");
        sliderResetable("Saturation", &params.saturation, -100.0f, 100.0f, 0.0f, "%+.0f");
        ImGui::PopID(); // ColorSection
    }

    // 6. Tone Mapping
    if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("ToneMappingSection");
        constexpr int kNumOps = static_cast<int>(ToneMapOp::Count);
        const char* toneMapItems[kNumOps];
        for (int i = 0; i < kNumOps; ++i) {
            toneMapItems[i] = toneMapOpName(static_cast<ToneMapOp>(i));
        }
        int currentTm = static_cast<int>(params.toneMap);
        if (currentTm < 0 || currentTm >= kNumOps) currentTm = 0;
        if (ImGui::Combo("Operator", &currentTm, toneMapItems, kNumOps)) {
            params.toneMap = static_cast<ToneMapOp>(currentTm);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", toneMapOpTooltip(static_cast<ToneMapOp>(currentTm)));
        }
        ImGui::PopID(); // ToneMappingSection
    }

    ImGui::End();
}

} // namespace hdr::gui
