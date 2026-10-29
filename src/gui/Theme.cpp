#include "Theme.h"
#include <filesystem>
#include <windows.h>

namespace hdr::gui {

void Theme::applyDarkTheme(float dpiScale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Charcoal #1b1d22 as base background
    const ImVec4 bgCharcoal(0.106f, 0.114f, 0.133f, 1.0f);
    const ImVec4 bgSurface(0.133f, 0.145f, 0.169f, 1.0f);
    const ImVec4 bgInput(0.165f, 0.180f, 0.208f, 1.0f);
    const ImVec4 bgInputHover(0.200f, 0.220f, 0.255f, 1.0f);

    // Teal #2ec4d6 accent
    const ImVec4 accentTeal(0.180f, 0.769f, 0.839f, 1.0f);
    const ImVec4 accentHover(0.240f, 0.840f, 0.900f, 1.0f);
    const ImVec4 accentActive(0.140f, 0.650f, 0.720f, 1.0f);
    const ImVec4 accentMuted(0.180f, 0.769f, 0.839f, 0.35f);

    // Text colors
    const ImVec4 textWhite(0.95f, 0.96f, 0.98f, 1.0f);
    const ImVec4 textMuted(0.62f, 0.66f, 0.72f, 1.0f);
    const ImVec4 textDisabled(0.40f, 0.44f, 0.48f, 1.0f);

    // Borders
    const ImVec4 borderDark(0.22f, 0.24f, 0.28f, 0.65f);

    colors[ImGuiCol_Text]                  = textWhite;
    colors[ImGuiCol_TextDisabled]          = textDisabled;
    colors[ImGuiCol_WindowBg]              = bgCharcoal;
    colors[ImGuiCol_ChildBg]               = bgSurface;
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.13f, 0.15f, 0.98f);
    colors[ImGuiCol_Border]                = borderDark;
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg]               = bgInput;
    colors[ImGuiCol_FrameBgHovered]        = bgInputHover;
    colors[ImGuiCol_FrameBgActive]         = accentMuted;
    colors[ImGuiCol_TitleBg]               = bgSurface;
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.15f, 0.165f, 0.195f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed]      = bgCharcoal;
    colors[ImGuiCol_MenuBarBg]             = bgSurface;
    colors[ImGuiCol_ScrollbarBg]           = bgCharcoal;
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.25f, 0.27f, 0.32f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.35f, 0.38f, 0.44f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive]   = accentTeal;
    colors[ImGuiCol_CheckMark]             = accentTeal;
    colors[ImGuiCol_SliderGrab]            = accentTeal;
    colors[ImGuiCol_SliderGrabActive]      = accentActive;
    colors[ImGuiCol_Button]                = bgInput;
    colors[ImGuiCol_ButtonHovered]         = accentMuted;
    colors[ImGuiCol_ButtonActive]          = accentActive;
    colors[ImGuiCol_Header]                = bgInput;
    colors[ImGuiCol_HeaderHovered]         = accentMuted;
    colors[ImGuiCol_HeaderActive]          = accentActive;
    colors[ImGuiCol_Separator]             = borderDark;
    colors[ImGuiCol_SeparatorHovered]      = accentHover;
    colors[ImGuiCol_SeparatorActive]       = accentActive;
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.25f, 0.27f, 0.32f, 0.35f);
    colors[ImGuiCol_ResizeGripHovered]     = accentHover;
    colors[ImGuiCol_ResizeGripActive]      = accentActive;
    colors[ImGuiCol_Tab]                   = bgSurface;
    colors[ImGuiCol_TabHovered]            = accentMuted;
    colors[ImGuiCol_TabActive]             = ImVec4(0.16f, 0.22f, 0.26f, 1.0f);
    colors[ImGuiCol_TabUnfocused]          = bgCharcoal;
    colors[ImGuiCol_TabUnfocusedActive]    = bgSurface;
    colors[ImGuiCol_DockingPreview]        = accentMuted;
    colors[ImGuiCol_DockingEmptyBg]        = bgCharcoal;
    colors[ImGuiCol_PlotLines]             = accentTeal;
    colors[ImGuiCol_PlotLinesHovered]      = accentHover;
    colors[ImGuiCol_PlotHistogram]         = accentTeal;
    colors[ImGuiCol_PlotHistogramHovered]  = accentHover;
    colors[ImGuiCol_TableHeaderBg]         = bgInput;
    colors[ImGuiCol_TableBorderStrong]     = borderDark;
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.20f, 0.22f, 0.25f, 0.5f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    colors[ImGuiCol_TextSelectedBg]        = accentMuted;
    colors[ImGuiCol_DragDropTarget]        = accentTeal;
    colors[ImGuiCol_NavHighlight]          = accentTeal;
    colors[ImGuiCol_NavWindowingHighlight] = accentTeal;
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.65f);

    // Layout metrics
    style.WindowPadding    = ImVec2(10.0f, 10.0f);
    style.FramePadding     = ImVec2(8.0f, 5.0f);
    style.CellPadding      = ImVec2(6.0f, 5.0f);
    style.ItemSpacing      = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.TouchExtraPadding= ImVec2(0.0f, 0.0f);
    style.IndentSpacing    = 18.0f;
    style.ScrollbarSize    = 12.0f;
    style.GrabMinSize      = 10.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize  = 1.0f;
    style.PopupBorderSize  = 1.0f;
    style.FrameBorderSize  = 1.0f;
    style.TabBorderSize    = 0.0f;

    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;

    if (dpiScale > 1.0f) {
        style.ScaleAllSizes(dpiScale);
    }
}

void Theme::setupFonts(float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();
    float fontSize = 16.0f * (dpiScale > 0.0f ? dpiScale : 1.0f);

    // Try system Segoe UI on Windows for crisp modern UI, fallback to default font
    char winDir[MAX_PATH];
    if (GetWindowsDirectoryA(winDir, MAX_PATH) > 0) {
        std::string segoePath = std::string(winDir) + "\\Fonts\\segoeui.ttf";
        if (std::filesystem::exists(segoePath)) {
            io.Fonts->AddFontFromFileTTF(segoePath.c_str(), fontSize);
            return;
        }
    }

    // Default font
    ImFontConfig config;
    config.SizePixels = fontSize;
    io.Fonts->AddFontDefault(&config);
}

} // namespace hdr::gui
