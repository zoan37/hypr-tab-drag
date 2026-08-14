#include "TabDrag.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>

#include <algorithm>

using namespace Desktop::View;

namespace {
    constexpr uint32_t BTN_LEFT_CODE = 272;

    // Pointer travel, in layout px, before a press turns into a drag. Below this the
    // press stays a plain click, so tab switching is unaffected.
    constexpr double DRAG_THRESHOLD = 4.0;

    // swapWithNext/swapWithLast move one slot per call, so a drag across the bar needs
    // at most size() of them. The cap only matters if a swap ever fails to move
    // current, which would otherwise spin forever.
    constexpr int MAX_SWAPS_PER_MOTION = 64;

    struct SDragState {
        bool       armed    = false;
        bool       dragging = false;
        WP<CGroup> group;
        Vector2D   pressPos;
    };

    SDragState g_drag;

    bool       enabled() {
        static auto PENABLED = CConfigValue<Config::BOOL>("plugin:tab-drag:enabled");
        return *PENABLED;
    }

    // The decoration's box in layout coordinates. getWindowDecorationBox omits the
    // workspace render offset, which is non-zero mid workspace-slide, so add it back
    // the way the groupbar's own assignedBoxGlobal does.
    CBox decoBox(const PHLWINDOW& w, IHyprWindowDecoration* deco) {
        CBox box = g_pDecorationPositioner->getWindowDecorationBox(deco);

        if (w->m_workspace && !w->m_pinned)
            box.translate(w->m_workspace->m_renderOffset->value());

        return box.round();
    }

    // Which tab slot a point falls in, mirroring the arithmetic the groupbar uses to
    // lay the tabs out. Clamped, so a cursor past either end targets the end slot --
    // that is what makes dragging past the bar keep working.
    int tabIndexAt(const CBox& box, size_t count, const Vector2D& point) {
        static auto PSTACKED   = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
        static auto PINNERGAP  = CConfigValue<Config::INTEGER>("group:groupbar:gaps_in");
        static auto POUTERGAP  = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
        static auto PKEEPUPPER = CConfigValue<Config::INTEGER>("group:groupbar:keep_upper_gap");

        if (count == 0)
            return 0;

        const float N = count;
        int         idx;

        if (*PSTACKED) {
            const float ROW = ((box.h - *POUTERGAP * *PKEEPUPPER) - *POUTERGAP * N) / N + *POUTERGAP;
            idx             = ROW > 0 ? (int)((point.y - box.y) / ROW) : 0;
        } else {
            const float COL = (box.w - *PINNERGAP * (N - 1)) / N + *PINNERGAP;
            idx             = COL > 0 ? (int)((point.x - box.x) / COL) : 0;
        }

        return std::clamp(idx, 0, (int)count - 1);
    }

    IHyprWindowDecoration* groupbarOf(const PHLWINDOW& w) {
        return w ? w->getDecorationByType(DECORATION_GROUPBAR) : nullptr;
    }

    // SUPER + drag on a tab moves the whole window out of the group, and that gesture
    // starts from the same press this one would. It wins: reordering tabs underneath a
    // window being torn out would fight it.
    bool windowDragInProgress() {
        if (!g_layoutManager)
            return false;

        const auto& CONTROLLER = g_layoutManager->dragController();
        return CONTROLLER && CONTROLLER->mode() != MBIND_INVALID;
    }

    // Finds the groupbar under the cursor and arms the gesture on it.
    void tryArm(const Vector2D& cursor) {
        if (windowDragInProgress())
            return;

        for (auto& gref : groups()) {
            auto g = gref.lock();
            if (!g || g->size() < 2)
                continue;

            const auto W = g->current();
            if (!W || !W->m_workspace || !W->m_workspace->isVisible())
                continue;

            auto* deco = groupbarOf(W);
            if (!deco)
                continue;

            const CBox BOX = decoBox(W, deco);
            if (!BOX.containsPoint(cursor))
                continue;

            // The compositor has already made the pressed tab current. If our own
            // index math disagrees, we are not reading the bar the same way it is --
            // a press on tab padding does that -- and reordering from here would move
            // the wrong tab.
            if (tabIndexAt(BOX, g->size(), cursor) != (int)g->getCurrentIdx())
                return;

            g_drag.armed    = true;
            g_drag.dragging = false;
            g_drag.group    = g;
            g_drag.pressPos = cursor;
            return;
        }
    }
}

void TabDrag::reset() {
    g_drag = {};
}

bool TabDrag::consumesButton(const IPointer::SButtonEvent& e) {
    if (!g_drag.armed)
        return false;

    // Any other button arriving mid-gesture means something else is starting; drop the
    // gesture rather than reordering underneath it.
    if (e.button != BTN_LEFT_CODE) {
        reset();
        return false;
    }

    if (e.state != WL_POINTER_BUTTON_STATE_RELEASED)
        return false;

    // The press was consumed by the groupbar, so a release that ended a real drag must
    // be consumed too -- by then the cursor may be over a client that never saw the
    // press. A release that never became a drag is left alone: that press was an
    // ordinary tab click and its release belongs to the normal path.
    const bool WAS_DRAGGING = g_drag.dragging;
    reset();
    return WAS_DRAGGING;
}

void TabDrag::afterButton(const IPointer::SButtonEvent& e) {
    if (e.button != BTN_LEFT_CODE || e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;

    if (!enabled())
        return;

    // A press always starts fresh. If one is somehow still armed, it lost its release.
    reset();

    tryArm(g_pInputManager->getMouseCoordsInternal());
}

void TabDrag::afterMouseMove() {
    if (!g_drag.armed)
        return;

    const auto GROUP = g_drag.group.lock();
    if (!GROUP || GROUP->size() < 2) {
        reset();
        return;
    }

    // A window drag can begin after the press -- a mousebind held down late, or a
    // dispatcher -- so it has to be rechecked, not just tested when arming.
    if (windowDragInProgress()) {
        reset();
        return;
    }

    const auto CURSOR = g_pInputManager->getMouseCoordsInternal();

    if (!g_drag.dragging) {
        if (CURSOR.distance(g_drag.pressPos) < DRAG_THRESHOLD)
            return;

        g_drag.dragging = true;
    }

    // Re-read the geometry every motion: the window can move or resize mid-drag, and a
    // stale box would map the cursor to the wrong slot.
    const auto W = GROUP->current();
    auto*      deco = groupbarOf(W);
    if (!deco) {
        reset();
        return;
    }

    const int TARGET = tabIndexAt(decoBox(W, deco), GROUP->size(), CURSOR);

    // swapWithNext/swapWithLast wrap around at the ends, so stepping one slot at a time
    // toward a clamped target is what keeps a drag past the edge from teleporting the
    // tab to the opposite end.
    for (int i = 0; i < MAX_SWAPS_PER_MOTION; ++i) {
        const int CUR = (int)GROUP->getCurrentIdx();

        if (CUR < TARGET)
            GROUP->swapWithNext();
        else if (CUR > TARGET)
            GROUP->swapWithLast();
        else
            break;

        if ((int)GROUP->getCurrentIdx() == CUR)
            break;
    }
}
