#include "Toolbar.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>
#include <vector>
#include <string>
#include "../../core/FileScanner.h"
#include "../../core/PathUtil.h"

namespace hdr::gui {

namespace {

// NFD filter spec ("dng,cr2,...") built from the scanner's extension list.
const char* rawFilterSpec() {
    static const std::string spec = [] {
        std::string out;
        for (const auto& ext : FileScanner::supportedExtensions()) {
            if (!out.empty()) out += ',';
            out += ext.substr(1);
        }
        return out;
    }();
    return spec.c_str();
}

void openFilesDialog(AppController& controller) {
    const nfdu8filteritem_t filter[1] = {{"Raw Images", rawFilterSpec()}};
    const nfdpathset_t* pathSet = nullptr;
    const std::string& last = controller.settings().lastInputFolder;
    if (NFD_OpenDialogMultipleU8(&pathSet, filter, 1, last.empty() ? nullptr : last.c_str()) == NFD_OKAY && pathSet) {
        nfdpathsetsize_t count = 0;
        NFD_PathSet_GetCount(pathSet, &count);
        std::vector<fs::path> paths;
        for (nfdpathsetsize_t i = 0; i < count; ++i) {
            nfdu8char_t* p = nullptr;
            NFD_PathSet_GetPathU8(pathSet, i, &p);
            if (p) {
                paths.push_back(pathFromUtf8(p));
                NFD_PathSet_FreePathU8(p);
            }
        }
        NFD_PathSet_Free(pathSet);
        controller.addFiles(paths);
    }
}

void openFolderDialog(AppController& controller) {
    nfdu8char_t* outFolder = nullptr;
    const std::string& last = controller.settings().lastInputFolder;
    if (NFD_PickFolderU8(&outFolder, last.empty() ? nullptr : last.c_str()) == NFD_OKAY && outFolder) {
        const fs::path folder = pathFromUtf8(outFolder);
        NFD_FreePathU8(outFolder);
        controller.addFolder(folder);
    }
}

} // namespace

void Toolbar::render(AppController& controller) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Add Files...")) {
                openFilesDialog(controller);
            }
            if (ImGui::MenuItem("Add Folder...")) {
                openFolderDialog(controller);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear All")) {
                controller.clearAll();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                controller.requestExit = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Auto-Detect Groups")) {
                controller.autoDetectGrouping();
            }
            if (ImGui::MenuItem("Reset Tone Controls")) {
                controller.resetToneParams();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Fit to Window", nullptr, &controller.fitToWindow);
            ImGui::MenuItem("Clipping Warnings", nullptr, &controller.showClipping);
            ImGui::MenuItem("Before / After", nullptr, &controller.showBeforeAfter);
            ImGui::Separator();
            if (ImGui::MenuItem("Show List Thumbnails", nullptr, &controller.settings().showListThumbnails)) {
                controller.settings().save();
            }
            if (ImGui::BeginMenu("Thumbnail Size", controller.settings().showListThumbnails)) {
                if (ImGui::MenuItem("Small (48x30)", nullptr, controller.settings().listThumbnailSize == ListThumbnailSize::Small)) {
                    controller.settings().listThumbnailSize = ListThumbnailSize::Small;
                    controller.settings().save();
                }
                if (ImGui::MenuItem("Medium (240x150)", nullptr, controller.settings().listThumbnailSize == ListThumbnailSize::Medium)) {
                    controller.settings().listThumbnailSize = ListThumbnailSize::Medium;
                    controller.settings().save();
                }
                if (ImGui::MenuItem("Large (480x300)", nullptr, controller.settings().listThumbnailSize == ListThumbnailSize::Large)) {
                    controller.settings().listThumbnailSize = ListThumbnailSize::Large;
                    controller.settings().save();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                controller.resetLayoutRequested = true;
            }
            ImGui::EndMenu();
        }

        static bool showAboutModal = false;
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About HDR DNG Merger")) {
                showAboutModal = true;
            }
            ImGui::EndMenu();
        }

        // Right-aligned version tag in the menu bar
        const char* verStr = "v" HDR_VERSION_STRING;
        float verWidth = ImGui::CalcTextSize(verStr).x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - verWidth - 16.0f);
        ImGui::TextDisabled("%s", verStr);

        ImGui::EndMainMenuBar();

        if (showAboutModal) {
            ImGui::OpenPopup("About HDR DNG Merger");
        }
        if (ImGui::BeginPopupModal("About HDR DNG Merger", &showAboutModal, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(ImVec4(0.24f, 0.84f, 0.90f, 1.0f), "HDR DNG Merger");
            ImGui::SameLine();
            ImGui::Text("v%s", HDR_VERSION_STRING);
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("A RAW-to-RAW HDR merger for bracketed drone and camera exposures.");
            ImGui::Text("Merges mosaic Bayer sensor data directly into floating-point or 16-bit DNG.");
            ImGui::Spacing();
            ImGui::Text("License: GNU General Public License v3.0 (GPL-3.0)");
            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                showAboutModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Dedicated fixed Top Toolbar window
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, kHeight));

    ImGuiWindowFlags toolbarFlags = ImGuiWindowFlags_NoDecoration |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.13f, 0.15f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##TopToolbar", nullptr, toolbarFlags)) {

    if (ImGui::Button("Add Files...")) {
        openFilesDialog(controller);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Folder...")) {
        openFolderDialog(controller);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        controller.clearAll();
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    ImGui::Text("Frames/Bracket:");
    ImGui::SameLine();
    int n = controller.settings().framesPerBracket;
    ImGui::SetNextItemWidth(105.0f);
    if (ImGui::InputInt("##FramesPerBracket", &n, 1, 1)) {
        n = std::clamp(n, 2, 9);
        controller.regroup(n);
        controller.settings().save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Number of exposures per bracket group (e.g. 3, 5, or 7 for DJI AEB series)");
    }

    ImGui::SameLine();
    if (ImGui::Button("Auto-Group")) {
        controller.autoDetectGrouping();
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // Output folder (read-only display) + browse
    std::string outDirText = controller.settings().lastOutputFolder;
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##OutFolder", outDirText.data(), outDirText.size() + 1, ImGuiInputTextFlags_ReadOnly);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Current output folder: %s", controller.settings().lastOutputFolder.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        nfdu8char_t* outFolder = nullptr;
        const char* defaultPath = controller.settings().lastOutputFolder.empty() ? nullptr : controller.settings().lastOutputFolder.c_str();
        if (NFD_PickFolderU8(&outFolder, defaultPath) == NFD_OKAY && outFolder) {
            controller.settings().lastOutputFolder = reinterpret_cast<const char*>(outFolder);
            NFD_FreePathU8(outFolder);
            controller.settings().save();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Folder")) {
        controller.openOutputFolder();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open output folder in Windows Explorer");
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    bool isRunning = controller.batchProcessor().isRunning();
    bool hasBrackets = !controller.bracketGroups().empty();
    if (!isRunning) {
        ImGui::BeginDisabled(!hasBrackets);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.55f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.70f, 0.45f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.45f, 0.30f, 1.0f));
        if (ImGui::Button("Start Batch") && hasBrackets) {
            controller.startBatch();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Cancel Batch")) {
            controller.cancelBatch();
        }
        ImGui::PopStyleColor(3);
    }

    } // End if (Begin)
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

} // namespace hdr::gui
