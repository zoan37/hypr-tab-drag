#pragma once

#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include <vector>

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

    // --- rendering ---------------------------------------------------------------
    //
    // The groupbar draws every tab in one call, so there is no seam to slide a single
    // tab through. Instead the pass elements it emits are intercepted: those belonging
    // to the dragged tab are held back, moved under the cursor, and re-emitted after
    // the rest of the bar, which also puts them on top.

    // Called at the start of the groupbar's draw. Returns true if this decoration is
    // the one drawing a drag in progress, in which case endDraw must follow.
    bool                       beginDraw(void* deco, const PHLMONITOR& monitor);

    // Returns the held-back elements, already moved under the cursor.
    std::vector<UP<IPassElement>> endDraw();

    // Returns true if the element belongs to the dragged tab, taking ownership of it.
    bool                       stashPassElement(UP<IPassElement>& element);
}
