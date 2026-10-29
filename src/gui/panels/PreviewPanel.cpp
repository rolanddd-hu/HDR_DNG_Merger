#include "PreviewPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <format>
#include <algorithm>

namespace hdr::gui {

void PreviewPanel::render(AppController& controller) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Preview", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();

    // 1. Controls Bar
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild("PreviewControls", ImVec2(0, 38), false, ImGuiWindowFlags_NoScrollbar);

    int gIdx = controller.selectedGroupIndex();
    const auto& groups = controller.bracketGroups();

    if (gIdx >= 0 && gIdx < static_cast<int>(groups.size())) {
        const auto& grp = groups[gIdx];

        // Merged Button
        bool isMerged = (controller.selectedFrameIndex() == -1);
        if (isMerged) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.769f, 0.839f, 0.6f));
        if (ImGui::Button("Merged [HDR]")) {
            controller.selectFrame(-1);
        }
        if (isMerged) ImGui::PopStyleColor();

        int refIdx = grp.getResolvedReferenceIndex();
        double refExposure = 1.0;
        if (refIdx >= 0 && refIdx < static_cast<int>(grp.files.size())) {
            refExposure = grp.files[refIdx].info.relativeExposure();
        }

        // Individual frames
        for (size_t fIdx = 0; fIdx < grp.files.size(); ++fIdx) {
            ImGui::SameLine();
            bool isFrame = (controller.selectedFrameIndex() == static_cast<int>(fIdx));
            if (isFrame) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.769f, 0.839f, 0.6f));
            
            double relEv = 0.0;
            double curExp = grp.files[fIdx].info.relativeExposure();
            if (curExp > 0.0 && refExposure > 0.0) {
                relEv = std::log2(curExp / refExposure);
            }
            std::string btnLabel;
            if (static_cast<int>(fIdx) == refIdx) {
                btnLabel = std::format("F{} [Ref]", fIdx + 1);
            } else {
                btnLabel = std::format("F{} ({:+.1f} EV)", fIdx + 1, relEv);
            }

            if (ImGui::Button(btnLabel.c_str())) {
                controller.selectFrame(static_cast<int>(fIdx));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Frame %zu: %s\nExposure: %s\nRelative EV: %+.2f EV",
                    fIdx + 1, grp.files[fIdx].path.filename().string().c_str(),
                    grp.files[fIdx].info.formatExposure().c_str(), relEv);
            }
            if (isFrame) ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        if (ImGui::Button("Fit [F]")) {
            controller.fitToWindow = true;
            controller.zoomLevel = 1.0f;
            controller.panOffsetX = 0.0f;
            controller.panOffsetY = 0.0f;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit to view (shortcut: F)");

        ImGui::SameLine();
        if (ImGui::Button("1:1 [1]")) {
            controller.fitToWindow = false;
            controller.zoomLevel = 1.0f;
            controller.panOffsetX = 0.0f;
            controller.panOffsetY = 0.0f;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("100%% pixel scale (shortcut: 1)");

        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%%", controller.zoomLevel * 100.0f);

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        ImGui::Checkbox("Clipping [C]", &controller.showClipping);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle highlight/shadow clipping overlay (shortcut: C)");

        ImGui::SameLine();
        ImGui::Checkbox("Before / After [B]", &controller.showBeforeAfter);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle side-by-side comparison (shortcut: B)");

        if (controller.hasMotionMask()) {
            ImGui::SameLine();
            ImGui::Checkbox("Motion [M]", &controller.showMotionMask);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Overlays detected moving regions in semi-transparent magenta (shortcut: M).");
            }
        }

        if (controller.hasSourceMap() && isMerged) {
            ImGui::SameLine();
            ImGui::Checkbox("Source Map [S]", &controller.showSourceMap);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Visualizes with false colors which exposure contributed each pixel region (shortcut: S).");
            }
        }

        if (controller.isPreviewLoading()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.9f, 1.0f), "  Loading...");
        }
    } else {
        ImGui::TextDisabled("Select a bracket to preview.");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    // 2. Viewport Area
    ImVec2 viewSize = ImGui::GetContentRegionAvail();
    if (viewSize.x <= 10.0f || viewSize.y <= 10.0f) {
        ImGui::End();
        return;
    }

    ImGui::BeginChild("PreviewCanvas", viewSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);

    GlTexture& tex = controller.displayTexture();
    if (tex.isValid()) {
        float imgW = static_cast<float>(tex.width());
        float imgH = static_cast<float>(tex.height());

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        ImVec2 mouseRel(mousePos.x - canvasPos.x, mousePos.y - canvasPos.y);

        float displayW = imgW * controller.zoomLevel;
        float displayH = imgH * controller.zoomLevel;

        if (controller.fitToWindow) {
            float scale = std::min(viewSize.x / imgW, viewSize.y / imgH);
            displayW = imgW * scale;
            displayH = imgH * scale;
            controller.zoomLevel = scale;
            controller.panOffsetX = (viewSize.x - displayW) * 0.5f;
            controller.panOffsetY = (viewSize.y - displayH) * 0.5f;
        }

        // Before / After Split divider interaction
        static bool isDraggingSplit = false;
        if (controller.showBeforeAfter && controller.beforeTexture().isValid()) {
            float splitScreenX = canvasPos.x + controller.panOffsetX + displayW * controller.splitPosition;
            float imgScreenYMin = canvasPos.y + controller.panOffsetY;
            float imgScreenYMax = imgScreenYMin + displayH;

            bool isOverDivider = (std::abs(mousePos.x - splitScreenX) <= 12.0f &&
                                  mousePos.y >= imgScreenYMin && mousePos.y <= imgScreenYMax);

            if (isOverDivider) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    isDraggingSplit = true;
                }
            }

            if (isDraggingSplit) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float relX = (mousePos.x - (canvasPos.x + controller.panOffsetX)) / std::max(displayW, 1.0f);
                    controller.splitPosition = std::clamp(relX, 0.02f, 0.98f);
                } else {
                    isDraggingSplit = false;
                }
            }
        } else {
            isDraggingSplit = false;
        }

        // Handle Mouse Wheel Zoom anchored at cursor
        if (ImGui::IsWindowHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                float fitScale = std::min(viewSize.x / imgW, viewSize.y / imgH);
                float oldZoom = controller.zoomLevel;
                float zoomFactor = (wheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
                float newZoom = oldZoom * zoomFactor;

                if (newZoom <= fitScale) {
                    // Do not allow zooming out smaller than the fit view
                    controller.zoomLevel = fitScale;
                    controller.fitToWindow = true;
                    displayW = imgW * fitScale;
                    displayH = imgH * fitScale;
                    controller.panOffsetX = (viewSize.x - displayW) * 0.5f;
                    controller.panOffsetY = (viewSize.y - displayH) * 0.5f;
                } else {
                    newZoom = std::min(newZoom, 20.0f);
                    float ratio = newZoom / oldZoom;

                    // Keep image point under cursor invariant
                    controller.panOffsetX = mouseRel.x - (mouseRel.x - controller.panOffsetX) * ratio;
                    controller.panOffsetY = mouseRel.y - (mouseRel.y - controller.panOffsetY) * ratio;
                    controller.zoomLevel = newZoom;
                    controller.fitToWindow = false;

                    displayW = imgW * newZoom;
                    displayH = imgH * newZoom;
                }
            }

            // Mouse Drag Pan (strictly disabled while dragging split divider or when fitted)
            if (!isDraggingSplit && !controller.fitToWindow) {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    ImVec2 delta = ImGui::GetIO().MouseDelta;
                    controller.panOffsetX += delta.x;
                    controller.panOffsetY += delta.y;
                }
            }
        }

        ImVec2 pMin(canvasPos.x + controller.panOffsetX, canvasPos.y + controller.panOffsetY);
        ImVec2 pMax(pMin.x + displayW, pMin.y + displayH);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(canvasPos, ImVec2(canvasPos.x + viewSize.x, canvasPos.y + viewSize.y), true);

        // Dark background for canvas
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + viewSize.x, canvasPos.y + viewSize.y), IM_COL32(20, 22, 26, 255));

        if (!controller.showBeforeAfter || !controller.beforeTexture().isValid()) {
            // Normal display
            drawList->AddImage(
                static_cast<ImTextureID>(tex.id()),
                pMin, pMax,
                ImVec2(0, 0), ImVec2(1, 1)
            );
        } else {
            // Before / After Split View
            float splitX = pMin.x + displayW * controller.splitPosition;

            // Draw "Before" on left half
            drawList->PushClipRect(pMin, ImVec2(splitX, pMax.y), true);
            drawList->AddImage(
                static_cast<ImTextureID>(controller.beforeTexture().id()),
                pMin, pMax,
                ImVec2(0, 0), ImVec2(1, 1)
            );
            drawList->PopClipRect();

            // Draw "After" on right half
            drawList->PushClipRect(ImVec2(splitX, pMin.y), pMax, true);
            drawList->AddImage(
                static_cast<ImTextureID>(tex.id()),
                pMin, pMax,
                ImVec2(0, 0), ImVec2(1, 1)
            );
            drawList->PopClipRect();

            // Divider Line
            drawList->AddLine(ImVec2(splitX, pMin.y), ImVec2(splitX, pMax.y), IM_COL32(46, 196, 214, 255), 2.0f);

            // Labels
            drawList->AddText(ImVec2(pMin.x + 10, pMin.y + 10), IM_COL32(255, 255, 255, 220), "BEFORE");
            drawList->AddText(ImVec2(splitX + 10, pMin.y + 10), IM_COL32(46, 196, 214, 220), "AFTER");
        }

        // Motion Mask overlay
        if (controller.showMotionMask && controller.motionTexture().isValid()) {
            drawList->AddImage(
                static_cast<ImTextureID>(controller.motionTexture().id()),
                pMin, pMax,
                ImVec2(0, 0), ImVec2(1, 1)
            );
        }

        // Source Frame Map overlay
        if (controller.showSourceMap && controller.sourceTexture().isValid()) {
            drawList->AddImage(
                static_cast<ImTextureID>(controller.sourceTexture().id()),
                pMin, pMax,
                ImVec2(0, 0), ImVec2(1, 1)
            );

            // Legend at bottom right of canvas
            if (gIdx >= 0 && gIdx < static_cast<int>(groups.size())) {
                const auto& grp = groups[gIdx];
                static const ImU32 legColors[] = {
                    IM_COL32(40, 130, 240, 240),  // Frame 0: Blue
                    IM_COL32(46, 204, 113, 240),  // Frame 1: Green
                    IM_COL32(240, 160, 20, 240),  // Frame 2: Amber
                    IM_COL32(160, 60, 220, 240),  // Frame 3: Purple
                    IM_COL32(235, 60, 60, 240),   // Frame 4: Red
                    IM_COL32(20, 200, 200, 240),  // Frame 5: Cyan
                    IM_COL32(240, 220, 40, 240)   // Frame 6: Yellow
                };
                constexpr size_t numLegCols = sizeof(legColors) / sizeof(legColors[0]);

                float legW = 160.0f;
                float legH = grp.files.size() * 18.0f + 24.0f;
                float legX = canvasPos.x + viewSize.x - legW - 12.0f;
                float legY = canvasPos.y + viewSize.y - legH - 12.0f;

                drawList->AddRectFilled(ImVec2(legX, legY), ImVec2(legX + legW, legY + legH), IM_COL32(15, 17, 21, 210), 4.0f);
                drawList->AddRect(ImVec2(legX, legY), ImVec2(legX + legW, legY + legH), IM_COL32(50, 55, 65, 200), 4.0f);
                drawList->AddText(ImVec2(legX + 8.0f, legY + 5.0f), IM_COL32(200, 205, 215, 255), "Source Frame Map");

                for (size_t fi = 0; fi < grp.files.size(); ++fi) {
                    ImU32 col = legColors[fi % numLegCols];
                    float cy = legY + 22.0f + fi * 18.0f;
                    drawList->AddRectFilled(ImVec2(legX + 8.0f, cy + 2.0f), ImVec2(legX + 20.0f, cy + 14.0f), col, 2.0f);
                    std::string label = std::format("Frame {}", fi + 1);
                    drawList->AddText(ImVec2(legX + 26.0f, cy), IM_COL32(240, 240, 240, 255), label.c_str());
                }
            }
        }

        // Hovered pixel readout
        if (mousePos.x >= pMin.x && mousePos.x <= pMax.x &&
            mousePos.y >= pMin.y && mousePos.y <= pMax.y &&
            displayW > 0.0f && displayH > 0.0f) {
            float normX = (mousePos.x - pMin.x) / displayW;
            float normY = (mousePos.y - pMin.y) / displayH;
            const auto& img = controller.currentLinearImage();
            if (!img.empty()) {
                int px = std::clamp(static_cast<int>(normX * img.width), 0, img.width - 1);
                int py = std::clamp(static_cast<int>(normY * img.height), 0, img.height - 1);
                const float* rgb = img.pixel(py, px);
                controller.hasHoverPixel = true;
                controller.hoverPixelX = px * 2; // raw sensor coordinates
                controller.hoverPixelY = py * 2;
                controller.hoverR = rgb[0];
                controller.hoverG = rgb[1];
                controller.hoverB = rgb[2];
            }
        } else {
            controller.hasHoverPixel = false;
        }

        // Dim overlay & spinner if preview is computing in background
        if (controller.isPreviewLoading()) {
            drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + viewSize.x, canvasPos.y + viewSize.y), IM_COL32(10, 12, 16, 140));
            float time = static_cast<float>(ImGui::GetTime());
            ImVec2 center(canvasPos.x + viewSize.x * 0.5f, canvasPos.y + viewSize.y * 0.5f);
            for (int i = 0; i < 8; ++i) {
                float angle = time * 4.0f + i * (3.14159f * 2.0f / 8.0f);
                float r = 20.0f;
                float alpha = (i + 1) / 8.0f;
                ImVec2 pt(center.x + std::cos(angle) * r, center.y + std::sin(angle) * r);
                drawList->AddCircleFilled(pt, 3.5f, IM_COL32(46, 196, 214, static_cast<int>(alpha * 255)));
            }
            std::string loadTxt = "Rendering HDR Preview...";
            ImVec2 txtSz = ImGui::CalcTextSize(loadTxt.c_str());
            drawList->AddText(ImVec2(center.x - txtSz.x * 0.5f, center.y + 30.0f), IM_COL32(220, 240, 255, 230), loadTxt.c_str());
        }

        drawList->PopClipRect();

        // Canvas keyboard shortcuts
        bool isCanvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        bool isCanvasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if ((isCanvasHovered || isCanvasFocused) && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                controller.fitToWindow = true;
                controller.zoomLevel = 1.0f;
                controller.panOffsetX = 0.0f;
                controller.panOffsetY = 0.0f;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_1)) {
                controller.fitToWindow = false;
                controller.zoomLevel = 1.0f;
                controller.panOffsetX = 0.0f;
                controller.panOffsetY = 0.0f;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_B)) {
                controller.showBeforeAfter = !controller.showBeforeAfter;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_C)) {
                controller.showClipping = !controller.showClipping;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_M) && controller.hasMotionMask()) {
                controller.showMotionMask = !controller.showMotionMask;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_S) && controller.hasSourceMap()) {
                controller.showSourceMap = !controller.showSourceMap;
            }
        }
    } else {
        ImVec2 center(ImGui::GetCursorScreenPos().x + viewSize.x * 0.5f, ImGui::GetCursorScreenPos().y + viewSize.y * 0.5f);
        ImGui::SetCursorPos(ImVec2(viewSize.x * 0.5f - 80, viewSize.y * 0.5f - 10));
        if (controller.isPreviewLoading()) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.9f, 1.0f), "Loading preview...");
        } else {
            ImGui::TextDisabled("No preview loaded.");
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace hdr::gui
