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
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <algorithm>
#include <cmath>

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

        // Where inside the tab the press landed, along the bar's axis. Keeping it is
        // what makes the tab track the cursor instead of snapping its edge to it.
        double grabOffset = 0;
    };

    SDragState g_drag;

    // Set for the duration of one groupbar draw that has a drag to render.
    struct SDrawState {
        bool                          active  = false;
        bool                          stacked = false;
        double                        slotStart = 0; // along the axis, scaled monitor-local
        double                        slotEnd   = 0;
        Vector2D                      delta;
        std::vector<UP<IPassElement>> stashed;
    };

    SDrawState g_draw;

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

    // The bar reduced to the one axis tabs are laid out along, mirroring the
    // arithmetic the groupbar uses to place them.
    struct SBarAxis {
        bool   stacked = false;
        double start   = 0; // box.x or box.y
        double len     = 0; // box.w or box.h
        double tabLen  = 0; // one tab along the axis
        double step    = 0; // tabLen plus the gap between tabs
    };

    SBarAxis barAxis(const CBox& box, size_t count) {
        static auto PSTACKED   = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
        static auto PINNERGAP  = CConfigValue<Config::INTEGER>("group:groupbar:gaps_in");
        static auto POUTERGAP  = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
        static auto PKEEPUPPER = CConfigValue<Config::INTEGER>("group:groupbar:keep_upper_gap");

        const double N = std::max<size_t>(count, 1);
        SBarAxis     ax;
        ax.stacked = *PSTACKED;

        if (ax.stacked) {
            ax.start  = box.y;
            ax.len    = box.h;
            ax.tabLen = ((box.h - *POUTERGAP * *PKEEPUPPER) - *POUTERGAP * N) / N;
            ax.step   = ax.tabLen + *POUTERGAP;
        } else {
            ax.start  = box.x;
            ax.len    = box.w;
            ax.tabLen = (box.w - *PINNERGAP * (N - 1)) / N;
            ax.step   = ax.tabLen + *PINNERGAP;
        }

        return ax;
    }

    // Which tab slot a point falls in. Clamped, so a cursor past either end targets the
    // end slot -- that is what makes dragging past the bar keep working.
    int tabIndexAt(const SBarAxis& ax, size_t count, const Vector2D& point) {
        if (count == 0 || ax.step <= 0)
            return 0;

        const double ALONG = (ax.stacked ? point.y : point.x) - ax.start;
        return std::clamp((int)(ALONG / ax.step), 0, (int)count - 1);
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

    void damageBar() {
        const auto GROUP = g_drag.group.lock();
        if (!GROUP)
            return;

        if (auto* bar = groupbarOf(GROUP->current()); bar)
            bar->damageEntire();
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

            const auto AX  = barAxis(BOX, g->size());
            const int  IDX = tabIndexAt(AX, g->size(), cursor);

            // The compositor has already made the pressed tab current. If our own
            // index math disagrees, we are not reading the bar the same way it is --
            // a press on tab padding does that -- and reordering from here would move
            // the wrong tab.
            if (IDX != (int)g->getCurrentIdx())
                return;

            g_drag.armed      = true;
            g_drag.dragging   = false;
            g_drag.group      = g;
            g_drag.pressPos   = cursor;
            g_drag.grabOffset = (AX.stacked ? cursor.y : cursor.x) - (AX.start + IDX * AX.step);
            return;
        }
    }

    // The mutable box a pass element will be drawn at, for the two kinds the groupbar
    // emits. Anything else is left alone.
    CBox* boxOf(IPassElement* el) {
        if (!el)
            return nullptr;

        switch (el->type()) {
            case EK_RECT: return &((CRectPassElement*)el)->m_data.box;
            case EK_TEXTURE: return &((CTexPassElement*)el)->m_data.box;
            default: return nullptr;
        }
    }

    CBox* clipOf(IPassElement* el) {
        if (!el)
            return nullptr;

        switch (el->type()) {
            case EK_RECT: return &((CRectPassElement*)el)->m_data.clipBox;
            case EK_TEXTURE: return &((CTexPassElement*)el)->m_data.clipBox;
            default: return nullptr;
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

    // The tab is drawn under the cursor, so dropping it has to repaint the bar or it
    // would stay drawn where it was let go.
    if (WAS_DRAGGING)
        damageBar();

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
    const auto W    = GROUP->current();
    auto*      deco = groupbarOf(W);
    if (!deco) {
        reset();
        return;
    }

    const int TARGET = tabIndexAt(barAxis(decoBox(W, deco), GROUP->size()), GROUP->size(), CURSOR);

    // swapWithNext/swapWithLast wrap around at the ends of the group, so stepping one
    // slot at a time toward a clamped target is what keeps a drag past the edge from
    // teleporting the tab to the opposite end.
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

    // The tab is drawn under the cursor, so every motion has to repaint the bar --
    // nothing else changed that the compositor would damage on its own.
    deco->damageEntire();
}

bool TabDrag::beginDraw(void* deco, const PHLMONITOR& monitor) {
    g_draw = {};

    if (!g_drag.dragging || !monitor)
        return false;

    const auto GROUP = g_drag.group.lock();
    if (!GROUP || GROUP->size() < 2)
        return false;

    const auto W = GROUP->current();
    if (!W)
        return false;

    // Only the decoration actually drawing this group is intercepted; every window in
    // the group owns one, and other groups' bars must be left alone.
    auto* bar = groupbarOf(W);
    if (!bar || bar != deco)
        return false;

    const CBox BOX = decoBox(W, bar);
    const auto AX  = barAxis(BOX, GROUP->size());
    if (AX.step <= 0 || AX.tabLen <= 0)
        return false;

    const int    IDX     = (int)GROUP->getCurrentIdx();
    const auto   CURSOR  = g_pInputManager->getMouseCoordsInternal();
    const double POINTER = AX.stacked ? CURSOR.y : CURSOR.x;

    // Where the tab wants to sit, held to the bar so it cannot be dragged out of it.
    const double DESIRED = std::clamp(POINTER - g_drag.grabOffset - AX.start, 0.0, std::max(0.0, AX.len - AX.tabLen));
    const double DELTA   = DESIRED - IDX * AX.step;

    // Once the reorder has caught up with the cursor the tab is already where it
    // belongs, and there is nothing to move.
    if (std::abs(DELTA) < 0.5)
        return false;

    const double SCALE     = monitor->m_scale;
    const double ORIGIN    = AX.stacked ? monitor->m_position.y : monitor->m_position.x;
    const double FLOATOFF  = AX.stacked ? W->m_floatingOffset.y : W->m_floatingOffset.x;
    const double SLOTLOCAL = (AX.start + IDX * AX.step - ORIGIN + FLOATOFF) * SCALE;

    g_draw.active    = true;
    g_draw.stacked   = AX.stacked;
    g_draw.slotStart = SLOTLOCAL;
    g_draw.slotEnd   = SLOTLOCAL + AX.tabLen * SCALE;
    g_draw.delta     = AX.stacked ? Vector2D{0.0, DELTA * SCALE} : Vector2D{DELTA * SCALE, 0.0};

    return true;
}

bool TabDrag::stashPassElement(UP<IPassElement>& element) {
    if (!g_draw.active || !element)
        return false;

    const CBox* BOX = boxOf(element.get());
    if (!BOX)
        return false;

    // Matched on the same box that will be moved, so the test and the move cannot
    // disagree about which coordinate space they are in.
    const double CENTER = g_draw.stacked ? BOX->y + BOX->h / 2.0 : BOX->x + BOX->w / 2.0;
    if (CENTER < g_draw.slotStart || CENTER >= g_draw.slotEnd)
        return false;

    g_draw.stashed.emplace_back(std::move(element));
    return true;
}

std::vector<UP<IPassElement>> TabDrag::endDraw() {
    for (auto& el : g_draw.stashed) {
        if (auto* box = boxOf(el.get()); box)
            box->translate(g_draw.delta);

        // An unset clipBox is all-zero and means "no clipping"; a set one is in the
        // same space and would otherwise clip the tab back to where it no longer is.
        if (auto* clip = clipOf(el.get()); clip && clip->w > 0 && clip->h > 0)
            clip->translate(g_draw.delta);
    }

    auto out = std::move(g_draw.stashed);
    g_draw   = {};
    return out;
}
