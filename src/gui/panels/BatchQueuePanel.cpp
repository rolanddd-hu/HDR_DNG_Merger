#include "BatchQueuePanel.h"
#include <imgui.h>
#include <format>

namespace hdr::gui {

void BatchQueuePanel::render(AppController& controller) {
    ImGui::Begin("Batch Queue", nullptr, ImGuiWindowFlags_NoCollapse);

    const auto jobs = controller.batchProcessor().getJobs();  // snapshot
    if (jobs.empty()) {
        ImGui::TextDisabled("No jobs in queue. Click 'Start Batch' to begin merging all brackets.");
        ImGui::End();
        return;
    }

    // Overall batch progress summary
    size_t totalJobs = jobs.size();
    size_t doneJobs = 0;
    size_t failedJobs = 0;
    float sumProgress = 0.0f;
    for (const auto& job : jobs) {
        JobStatus st = job->status.load();
        if (st == JobStatus::Done) doneJobs++;
        else if (st == JobStatus::Failed) failedJobs++;
        sumProgress += job->progress.load();
    }
    float overallProgress = totalJobs > 0 ? (sumProgress / totalJobs) : 0.0f;

    ImGui::Text("Batch Progress: %zu of %zu completed (%.0f%%)", doneJobs, totalJobs, overallProgress * 100.0f);
    if (failedJobs > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.25f, 0.25f, 1.0f), "| %zu failed", failedJobs);
    }
    if (!controller.batchProcessor().isRunning() && totalJobs > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("📁 Open Output Folder")) {
            controller.openOutputFolder();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Finished")) {
            controller.batchProcessor().clearFinished();
        }
    }
    ImGui::ProgressBar(overallProgress, ImVec2(-1.0f, 6.0f), "");
    ImGui::Spacing();

    if (ImGui::BeginTable("##BatchQueueTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Output File", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Stage / Time", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        for (const auto& job : jobs) {
            ImGui::TableNextRow();

            // Col 0: ID
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", job->id);

            // Col 1: Group
            ImGui::TableSetColumnIndex(1);
            std::string grpName = !job->group.name.empty() ? job->group.name : std::format("Group #{}", job->group.id);
            ImGui::Selectable(std::format("{} ({})##row_{}", grpName, job->group.files.size(), job->id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

            // Double click row to preview this group
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                const auto& grps = controller.bracketGroups();
                for (size_t gi = 0; gi < grps.size(); ++gi) {
                    if (grps[gi].id == job->group.id) {
                        controller.selectGroup(static_cast<int>(gi));
                        controller.selectFrame(-1);
                        break;
                    }
                }
            }

            // Col 2: Output File
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", job->outputPath.filename().string().c_str());

            // Col 3: Status
            ImGui::TableSetColumnIndex(3);
            JobStatus status = job->status.load();
            const std::string errorMessage = job->errorMessage();
            if (status == JobStatus::Done) {
                ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.3f, 1.0f), "Done");
            } else if (status == JobStatus::Running) {
                ImGui::TextColored(ImVec4(0.18f, 0.769f, 0.839f, 1.0f), "Running");
            } else if (status == JobStatus::Failed) {
                ImGui::TextColored(ImVec4(0.9f, 0.25f, 0.25f, 1.0f), "Failed");
                if (ImGui::IsItemHovered() && !errorMessage.empty()) {
                    ImGui::SetTooltip("Error: %s", errorMessage.c_str());
                }
            } else if (status == JobStatus::Cancelled) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Cancelled");
            } else if (status == JobStatus::Skipped) {
                ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.2f, 1.0f), "Skipped");
            } else {
                ImGui::TextDisabled("Queued");
            }

            // Col 4: Progress
            ImGui::TableSetColumnIndex(4);
            float prog = job->progress.load();
            ImGui::ProgressBar(prog, ImVec2(-1.0f, 0.0f));

            // Col 5: Stage / Time
            ImGui::TableSetColumnIndex(5);
            const std::string stage = job->stage();
            if (status == JobStatus::Running && !stage.empty()) {
                ImGui::TextUnformatted(stage.c_str());
            } else if (status == JobStatus::Done) {
                ImGui::Text("%.1f s", job->elapsedSeconds.load());
            } else if (status == JobStatus::Failed && !errorMessage.empty()) {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", errorMessage.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", errorMessage.c_str());
                }
            } else {
                ImGui::Text("-");
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace hdr::gui
