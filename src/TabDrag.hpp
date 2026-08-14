#pragma once

#include <hyprland/src/devices/IPointer.hpp>

namespace TabDrag {
    // Runs before the compositor handles the button. Returns true when the event has
    // been consumed and must not be passed on.
    bool consumesButton(const IPointer::SButtonEvent& e);

    // Runs after the compositor has handled a press. The compositor makes the pressed
    // tab current, and the gesture is defined relative to that, so arming can only
    // happen once it has.
    void afterButton(const IPointer::SButtonEvent& e);

    // Runs after the compositor has processed motion, so the cursor position is current.
    void afterMouseMove();

    // Drops any in-flight gesture. Safe to call when there is none.
    void reset();
}
