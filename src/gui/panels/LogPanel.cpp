#include "LogPanel.h"
#include "../../core/Log.h"
#include <imgui.h>
#include <cstdint>
#include <string>
#include <vector>

namespace hdr::gui {

void LogPanel::render() {
    ImGui::Begin("Log", nullptr, ImGuiWindowFlags_NoCollapse);

    static bool autoScroll = true;
    static char filterBuf[128] = "";

    // Lines are only copied from the sink when it changed, and only visible rows are drawn.
    static std::vector<std::string> lines;
    static std::vector<int> visible;
    static uint64_t cachedVersion = UINT64_MAX;
    static std::string cachedFilter;

    auto sink = Logger::getRingBuffer();

    if (ImGui::Button("Clear")) {
        if (sink) sink->clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##LogFilter", "Filter...", filterBuf, sizeof(filterBuf));

    ImGui::Separator();

    if (sink && (sink->version() != cachedVersion || cachedFilter != filterBuf)) {
        cachedVersion = sink->version();
        cachedFilter = filterBuf;
        lines = sink->getLines();
        visible.clear();
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (cachedFilter.empty() || lines[i].find(cachedFilter) != std::string::npos) {
                visible.push_back(i);
            }
        }
    }

    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const std::string& line = lines[visible[row]];
            // Simple color coding based on level tag
            if (line.find("[error]") != std::string::npos || line.find("[critical]") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.3f, 1.0f), "%s", line.c_str());
            } else if (line.find("[warning]") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f), "%s", line.c_str());
            } else if (line.find("[debug]") != std::string::npos || line.find("[trace]") != std::string::npos) {
                ImGui::TextDisabled("%s", line.c_str());
            } else {
                ImGui::TextUnformatted(line.c_str());
            }
        }
    }

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace hdr::gui
