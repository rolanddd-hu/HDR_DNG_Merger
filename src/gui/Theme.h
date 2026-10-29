#pragma once
#include <imgui.h>

namespace hdr::gui {

class Theme {
public:
    static void applyDarkTheme(float dpiScale = 1.0f);
    static void setupFonts(float dpiScale = 1.0f);
};

} // namespace hdr::gui
