#include "StatusBar.h"
#include <imgui.h>
#include <format>

namespace hdr::gui {

void StatusBar::render(AppController& controller) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float height = 26.0f;

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.10f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 4.0f));

    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        size_t numGroups = controller.bracketGroups().size();
        int selIdx = controller.selectedGroupIndex();

        std::string groupText = std::format("Groups: {}", numGroups);
        if (selIdx >= 0 && selIdx < static_cast<int>(numGroups)) {
            groupText += std::format("  |  Selected: Group #{}", selIdx + 1);
        }

        std::string batchText = "Batch: Idle";
        if (controller.batchProcessor().isRunning()) {
            const auto jobs = controller.batchProcessor().getJobs();
            size_t total = jobs.size();
            size_t done = 0;
            for (const auto& j : jobs) {
                JobStatus s = j->status.load();
                if (s == JobStatus::Done || s == JobStatus::Failed || s == JobStatus::Skipped) done++;
            }
            batchText = std::format("Batch: Running ({}/{} done)", done, total);
        }

        const std::string status = controller.previewStatus();
        std::string previewText = status.empty() ? "Preview: Ready" : ("Preview: " + status);
        if (controller.isScanning()) previewText += "  |  Scanning files...";

        ImGui::Text("%s   |   %s   |   %s", groupText.c_str(), previewText.c_str(), batchText.c_str());

        // Pixel readout
        if (controller.hasHoverPixel) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.24f, 0.84f, 0.90f, 1.0f), "  |   Pixel [%d, %d]  RGB: (%.3f, %.3f, %.3f)",
                controller.hoverPixelX, controller.hoverPixelY,
                controller.hoverR, controller.hoverG, controller.hoverB);
        }

        // Right aligned output dir with click-to-open
        if (!controller.settings().lastOutputFolder.empty()) {
            std::string outText = "📁 " + controller.settings().lastOutputFolder;
            float textW = ImGui::CalcTextSize(outText.c_str()).x + 16.0f;
            float targetX = viewport->Size.x - textW - 20.0f;
            if (targetX > ImGui::GetCursorPosX() + 10.0f) {
                ImGui::SameLine(targetX);
            } else {
                ImGui::SameLine();
            }

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.77f, 0.84f, 0.25f));
            if (ImGui::SmallButton(outText.c_str())) {
                controller.openOutputFolder();
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to open output directory in Windows Explorer");
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace hdr::gui
