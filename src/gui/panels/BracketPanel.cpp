#include "BracketPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <format>
#include <cmath>

namespace hdr::gui {

void BracketPanel::render(AppController& controller) {
    ImGui::Begin("Brackets", nullptr, ImGuiWindowFlags_NoCollapse);

    const auto& groups = controller.bracketGroups();
    if (groups.empty()) {
        ImGui::TextDisabled("No images loaded.");
        ImGui::Spacing();
        ImGui::TextWrapped("• Drag & drop raw files or folders anywhere onto the window\n• Or use 'Add Files...' / 'Add Folder...' in the toolbar above.");
        ImGui::End();
        return;
    }

    ImGui::Text("Total Brackets: %zu", groups.size());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 95.0f);
    if (ImGui::SmallButton("View Options...")) {
        ImGui::OpenPopup("BracketViewOptions");
    }
    if (ImGui::BeginPopup("BracketViewOptions")) {
        if (ImGui::Checkbox("Show Thumbnails", &controller.settings().showListThumbnails)) {
            controller.settings().save();
        }
        ImGui::Separator();
        ImGui::BeginDisabled(!controller.settings().showListThumbnails);
        ImGui::TextDisabled("Thumbnail Size:");
        int currentSize = static_cast<int>(controller.settings().listThumbnailSize);
        if (ImGui::RadioButton("Small (48x30)", &currentSize, static_cast<int>(ListThumbnailSize::Small))) {
            controller.settings().listThumbnailSize = ListThumbnailSize::Small;
            controller.settings().save();
        }
        if (ImGui::RadioButton("Medium (240x150)", &currentSize, static_cast<int>(ListThumbnailSize::Medium))) {
            controller.settings().listThumbnailSize = ListThumbnailSize::Medium;
            controller.settings().save();
        }
        if (ImGui::RadioButton("Large (480x300)", &currentSize, static_cast<int>(ListThumbnailSize::Large))) {
            controller.settings().listThumbnailSize = ListThumbnailSize::Large;
            controller.settings().save();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::Separator();

    const bool showThumbs = controller.settings().showListThumbnails;
    const bool textBelow = showThumbs && (controller.settings().listThumbnailSize != ListThumbnailSize::Small);
    float thumbW = 48.0f;
    float thumbH = 30.0f;
    switch (controller.settings().listThumbnailSize) {
        case ListThumbnailSize::Small:
            thumbW = 48.0f;
            thumbH = 30.0f;
            break;
        case ListThumbnailSize::Medium:
            thumbW = 240.0f;
            thumbH = 150.0f;
            break;
        case ListThumbnailSize::Large:
            thumbW = 480.0f;
            thumbH = 300.0f;
            break;
    }

    for (size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
        const auto& grp = groups[gIdx];
        bool isSelected = (controller.selectedGroupIndex() == static_cast<int>(gIdx));

        // Status badge color & symbol
        ImVec4 statusColor(0.25f, 0.85f, 0.35f, 1.0f); // Ok = bright green
        const char* statusIcon = "●";
        if (grp.status == BracketGroup::Status::Warning) {
            statusColor = ImVec4(0.95f, 0.75f, 0.20f, 1.0f); // Warning = amber
            statusIcon = "▲";
        } else if (grp.status == BracketGroup::Status::Error) {
            statusColor = ImVec4(0.92f, 0.25f, 0.25f, 1.0f); // Error = red
            statusIcon = "■";
        }

        std::string spanStr;
        if (!grp.files.empty()) {
            std::string firstStem = grp.files.front().path.stem().string();
            std::string lastStem = grp.files.back().path.stem().string();
            if (grp.files.size() > 1 && firstStem != lastStem) {
                spanStr = std::format("{}..{}", firstStem, lastStem);
            } else {
                spanStr = firstStem;
            }
        } else {
            spanStr = std::format("Group #{}", gIdx + 1);
        }

        std::string headerLabel = std::format("{} {} · {:.1f} EV ({} frames)", statusIcon, spanStr, grp.evSpan, grp.files.size());

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;

        ImGui::PushID(static_cast<int>(gIdx));
        ImGuiID nodeId = ImGui::GetID("##node");

        float groupStartY = ImGui::GetCursorPosY();
        ImVec2 groupScreenPos = ImGui::GetCursorScreenPos();
        float frameH = ImGui::GetFrameHeight();
        float cardH = (!textBelow) ? (showThumbs ? std::max(thumbH, frameH) : frameH)
                                   : (thumbH + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeightWithSpacing() + 6.0f);

        float cardLeft = ImGui::GetWindowPos().x + 4.0f;
        float cardRight = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize - 4.0f;
        ImVec2 cardMin(cardLeft, groupScreenPos.y - 2.0f);
        ImVec2 cardMax(cardRight, cardMin.y + cardH);

        bool isGroupOnlySelected = (isSelected && controller.selectedFrameIndex() == -1);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (isGroupOnlySelected) {
            dl->AddRectFilled(cardMin, cardMax, IM_COL32(42, 48, 58, 255), 4.0f);
            dl->AddRect(cardMin, cardMax, IM_COL32(66, 150, 250, 200), 4.0f, 0, 1.5f);
        }

        if (textBelow) {
            ImGui::SetCursorPosY(groupStartY + std::max(0.0f, (thumbH - frameH) * 0.5f));
        }

        ImGui::PushStyleColor(ImGuiCol_Text, statusColor);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        bool open = ImGui::TreeNodeEx("##node", flags);
        bool arrowHovered = ImGui::IsItemHovered();
        ImGui::PopItemFlag();
        ImGui::PopStyleColor();

        GlTexture* groupThumb = nullptr;
        bool gThumbClicked = false;
        bool gThumbHovered = false;

        if (showThumbs) {
            ImGui::SameLine(0.0f, 4.0f);
            if (textBelow) {
                ImGui::SetCursorPosY(groupStartY);
            }
            float thumbStartX = ImGui::GetCursorPosX();
            groupThumb = controller.getGroupThumbnail(grp);
            ImVec2 gThumbPos = ImGui::GetCursorScreenPos();

            gThumbClicked = ImGui::InvisibleButton("##gThumb", ImVec2(thumbW, thumbH));
            gThumbHovered = ImGui::IsItemHovered();

            ImU32 borderColor = isGroupOnlySelected ? IM_COL32(66, 150, 250, 255) : IM_COL32(70, 75, 85, 180);
            float borderThickness = isGroupOnlySelected ? 2.0f : 1.0f;

            if (groupThumb && groupThumb->isValid()) {
                dl->AddImageRounded(
                    static_cast<ImTextureID>(groupThumb->id()),
                    gThumbPos, ImVec2(gThumbPos.x + thumbW, gThumbPos.y + thumbH),
                    ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32_WHITE, 3.0f
                );
                dl->AddRect(gThumbPos, ImVec2(gThumbPos.x + thumbW, gThumbPos.y + thumbH), borderColor, 3.0f, 0, borderThickness);
            } else {
                dl->AddRectFilled(gThumbPos, ImVec2(gThumbPos.x + thumbW, gThumbPos.y + thumbH), IM_COL32(32, 34, 40, 255), 3.0f);
                dl->AddRect(gThumbPos, ImVec2(gThumbPos.x + thumbW, gThumbPos.y + thumbH), borderColor, 3.0f, 0, borderThickness);
            }

            if (!textBelow) {
                ImGui::SameLine(0.0f, 6.0f);
            } else {
                ImGui::SetCursorPosX(thumbStartX);
            }
        } else {
            ImGui::SameLine();
        }

        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));

        float itemH = (!textBelow && showThumbs) ? thumbH : 0.0f;
        bool gTextClicked = ImGui::Selectable(headerLabel.c_str(), false, 0, ImVec2(0, itemH));
        bool gTextHovered = ImGui::IsItemHovered();

        bool gCardHovered = ImGui::IsMouseHoveringRect(cardMin, cardMax, true);
        bool gCardClicked = gCardHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !arrowHovered;

        if (gTextClicked || gThumbClicked || gCardClicked) {
            controller.selectGroup(static_cast<int>(gIdx));
            controller.selectFrame(-1);
            ImGui::SetKeyboardFocusHere(-1);
        }
        if (ImGui::IsItemFocused() && !isGroupOnlySelected && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            controller.selectGroup(static_cast<int>(gIdx));
            controller.selectFrame(-1);
        }

        ImGui::PopStyleColor(4);

        if (textBelow) {
            ImGui::Spacing();
        }

        // Enter key or Right/Left arrows to expand/collapse group
        bool isGroupFocused = ImGui::IsItemFocused() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        bool enterPressed = isGroupFocused && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
        bool rightPressed = isGroupFocused && ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        bool leftPressed = isGroupFocused && ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        bool doubleClicked = (gCardHovered || gThumbHovered || gTextHovered) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !arrowHovered;

        if (enterPressed || doubleClicked) {
            ImGui::TreeNodeSetOpen(nodeId, !open);
        } else if (rightPressed && !open) {
            ImGui::TreeNodeSetOpen(nodeId, true);
        } else if (leftPressed && open) {
            ImGui::TreeNodeSetOpen(nodeId, false);
        }

        if (ImGui::IsItemHovered() || gThumbHovered) {
            if (!groupThumb) {
                groupThumb = controller.getGroupThumbnail(grp);
            }
            ImGui::BeginTooltip();
            if (groupThumb && groupThumb->isValid()) {
                float aspect = (groupThumb->height() > 0) ? (static_cast<float>(groupThumb->width()) / groupThumb->height()) : 1.5f;
                float prevH = 100.0f;
                float prevW = prevH * aspect;
                ImGui::Image(static_cast<ImTextureID>(groupThumb->id()), ImVec2(prevW, prevH));
                ImGui::Spacing();
            }
            ImGui::TextUnformatted(std::format("Bracket: {}\nΔ {:.1f} EV spread | Time span: {:.1f}s", grp.name, grp.evSpan, grp.timeSpan).c_str());
            if (!grp.statusMessage.empty()) {
                ImGui::TextColored(statusColor, "Status: %s", grp.statusMessage.c_str());
            }
            ImGui::EndTooltip();
        }

        // Group context menu
        if (ImGui::BeginPopupContextItem("GroupContext")) {
            if (ImGui::MenuItem("Merge this bracket now")) {
                controller.mergeSingleGroup(gIdx);
            }
            ImGui::Separator();
            if (gIdx + 1 < groups.size()) {
                if (ImGui::MenuItem("Join with Next Group")) {
                    controller.joinWithNext(gIdx);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Show Thumbnails", nullptr, &controller.settings().showListThumbnails)) {
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
            ImGui::EndPopup();
        }

        if (open) {
            ImGui::Indent(20.0f);

            int refIdx = grp.getResolvedReferenceIndex();
            double refExposure = 1.0;
            if (refIdx >= 0 && refIdx < static_cast<int>(grp.files.size())) {
                refExposure = grp.files[refIdx].info.relativeExposure();
            }

            for (size_t fIdx = 0; fIdx < grp.files.size(); ++fIdx) {
                const auto& file = grp.files[fIdx];
                bool isFrameSelected = (isSelected && controller.selectedFrameIndex() == static_cast<int>(fIdx));
                bool isRef = (refIdx == static_cast<int>(fIdx));

                std::string fname = file.path.filename().string();
                std::string frameText;
                
                double relEv = 0.0;
                double curExp = file.info.relativeExposure();
                if (curExp > 0.0 && refExposure > 0.0) {
                    relEv = std::log2(curExp / refExposure);
                }

                if (file.info.exposureTime > 0.0) {
                    if (file.info.exposureTime < 0.5) {
                        double invSpeed = 1.0 / file.info.exposureTime;
                        frameText = fname + " - 1/" + std::to_string(static_cast<int>(std::round(invSpeed))) + "s, ISO " +
                                    std::to_string(static_cast<int>(std::round(file.info.iso))) + " (" +
                                    std::format("{:+.1f}", relEv) + " EV)";
                    } else {
                        frameText = fname + " - " + std::format("{:.1f}s", file.info.exposureTime) + ", ISO " +
                                    std::to_string(static_cast<int>(std::round(file.info.iso))) + " (" +
                                    std::format("{:+.1f}", relEv) + " EV)";
                    }
                } else {
                    frameText = fname;
                }

                if (isRef) {
                    frameText += " [REF]";
                }

                ImGui::PushID(static_cast<int>(fIdx));

                float frameStartY = ImGui::GetCursorPosY();
                ImVec2 frameScreenPos = ImGui::GetCursorScreenPos();
                float chkH = ImGui::GetFrameHeight();
                float fCardH = (!textBelow) ? (showThumbs ? std::max(thumbH, chkH) : chkH)
                                            : (thumbH + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeightWithSpacing() + 6.0f);

                float fCardLeft = frameScreenPos.x - 4.0f;
                float fCardRight = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize - 4.0f;
                ImVec2 fCardMin(fCardLeft, frameScreenPos.y - 2.0f);
                ImVec2 fCardMax(fCardRight, fCardMin.y + fCardH);

                if (isFrameSelected) {
                    dl->AddRectFilled(fCardMin, fCardMax, IM_COL32(42, 48, 58, 255), 4.0f);
                    dl->AddRect(fCardMin, fCardMax, IM_COL32(66, 150, 250, 200), 4.0f, 0, 1.5f);
                }

                if (textBelow) {
                    // Align checkbox vertically in the middle next to the thumbnail
                    ImGui::SetCursorPosY(frameStartY + std::max(0.0f, (thumbH - chkH) * 0.5f));
                }

                // Frame active checkbox (mouse-clickable, excluded from keyboard nav focus)
                bool isEnabled = file.enabled;
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                if (ImGui::Checkbox("##enabled", &isEnabled)) {
                    controller.setFrameEnabled(gIdx, fIdx, isEnabled);
                }
                ImGui::PopItemFlag();
                bool chkHovered = ImGui::IsItemHovered();
                if (chkHovered) {
                    ImGui::SetTooltip(isEnabled ? "Included in merge (uncheck to exclude)" : "Excluded from merge (check to include)");
                }

                GlTexture* frameThumb = nullptr;
                bool fThumbClicked = false;
                bool fThumbHovered = false;

                if (showThumbs) {
                    ImGui::SameLine(0.0f, 4.0f);
                    if (textBelow) {
                        ImGui::SetCursorPosY(frameStartY);
                    }
                    float fThumbStartX = ImGui::GetCursorPosX();
                    frameThumb = controller.getThumbnail(file.path);
                    ImVec2 fThumbPos = ImGui::GetCursorScreenPos();

                    fThumbClicked = ImGui::InvisibleButton("##fThumb", ImVec2(thumbW, thumbH));
                    fThumbHovered = ImGui::IsItemHovered();

                    ImU32 fBorderColor = isFrameSelected ? IM_COL32(66, 150, 250, 255) : IM_COL32(70, 75, 85, 180);
                    float fBorderThickness = isFrameSelected ? 2.0f : 1.0f;

                    if (frameThumb && frameThumb->isValid()) {
                        dl->AddImageRounded(
                            static_cast<ImTextureID>(frameThumb->id()),
                            fThumbPos, ImVec2(fThumbPos.x + thumbW, fThumbPos.y + thumbH),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, 3.0f
                        );
                        dl->AddRect(fThumbPos, ImVec2(fThumbPos.x + thumbW, fThumbPos.y + thumbH), fBorderColor, 3.0f, 0, fBorderThickness);
                    } else {
                        dl->AddRectFilled(fThumbPos, ImVec2(fThumbPos.x + thumbW, fThumbPos.y + thumbH), IM_COL32(32, 34, 40, 255), 3.0f);
                        dl->AddRect(fThumbPos, ImVec2(fThumbPos.x + thumbW, fThumbPos.y + thumbH), fBorderColor, 3.0f, 0, fBorderThickness);
                    }

                    if (!textBelow) {
                        ImGui::SameLine(0.0f, 6.0f);
                    } else {
                        ImGui::SetCursorPosX(fThumbStartX);
                    }
                } else {
                    ImGui::SameLine();
                }

                if (!file.enabled) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    frameText += " [EXCLUDED]";
                }

                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));

                float fItemH = (!textBelow && showThumbs) ? thumbH : 0.0f;
                bool fTextClicked = ImGui::Selectable(frameText.c_str(), false, 0, ImVec2(0, fItemH));
                bool fTextHovered = ImGui::IsItemHovered();

                bool fCardHovered = ImGui::IsMouseHoveringRect(fCardMin, fCardMax, true);
                bool fCardClicked = fCardHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !chkHovered;

                if (fTextClicked || fThumbClicked || fCardClicked) {
                    controller.selectGroup(static_cast<int>(gIdx));
                    controller.selectFrame(static_cast<int>(fIdx));
                    ImGui::SetKeyboardFocusHere(-1);
                }
                if (ImGui::IsItemFocused() && !isFrameSelected && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                    controller.selectGroup(static_cast<int>(gIdx));
                    controller.selectFrame(static_cast<int>(fIdx));
                }

                ImGui::PopStyleColor(4);

                if (textBelow) {
                    ImGui::Spacing();
                }

                if (fThumbHovered || fTextHovered || (fCardHovered && !chkHovered)) {
                    if (!frameThumb) {
                        frameThumb = controller.getThumbnail(file.path);
                    }
                    ImGui::BeginTooltip();
                    if (frameThumb && frameThumb->isValid()) {
                        float aspect = (frameThumb->height() > 0) ? (static_cast<float>(frameThumb->width()) / frameThumb->height()) : 1.5f;
                        float prevH = 100.0f;
                        float prevW = prevH * aspect;
                        ImGui::Image(static_cast<ImTextureID>(frameThumb->id()), ImVec2(prevW, prevH));
                        ImGui::Spacing();
                    }
                    ImGui::Text("%s", fname.c_str());
                    if (isRef) {
                        ImGui::TextColored(ImVec4(0.3f, 0.75f, 1.0f, 1.0f), "[Reference Frame]");
                    }
                    if (file.info.exposureTime > 0.0) {
                        ImGui::Text("Exposure: %s", file.info.formatExposure().c_str());
                        if (file.info.fNumber > 0.0) {
                            ImGui::Text("Aperture: f/%.1f", file.info.fNumber);
                        }
                        ImGui::Text("Relative EV: %+.2f EV", relEv);
                    }
                    ImGui::EndTooltip();
                }

                if (!file.enabled) {
                    ImGui::PopStyleColor();
                }

                if (ImGui::BeginPopupContextItem("FrameContext")) {
                    if (file.enabled) {
                        if (!isRef && ImGui::MenuItem("Set as Reference Frame")) {
                            controller.setReferenceFrame(gIdx, fIdx);
                        }
                        if (ImGui::MenuItem("Exclude Frame from Merge")) {
                            controller.setFrameEnabled(gIdx, fIdx, false);
                        }
                    } else {
                        if (ImGui::MenuItem("Include Frame in Merge")) {
                            controller.setFrameEnabled(gIdx, fIdx, true);
                        }
                    }
                    if (fIdx > 0 && ImGui::MenuItem("Split Group Here")) {
                        controller.splitGroup(gIdx, fIdx);
                    }
                    if (ImGui::MenuItem("Delete Frame from List")) {
                        controller.excludeFrame(gIdx, fIdx);
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }
            ImGui::Unindent(20.0f);
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace hdr::gui
