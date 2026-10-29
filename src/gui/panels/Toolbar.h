#pragma once
#include "../AppController.h"

namespace hdr::gui {

class Toolbar {
public:
    static constexpr float kHeight = 46.0f;
    static void render(AppController& controller);
};

} // namespace hdr::gui
