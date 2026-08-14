#include "globals.hpp"
#include "TabDrag.hpp"

#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

#include <format>
#include <optional>
#include <stdexcept>

namespace {
    CFunctionHook* g_pButtonHook = nullptr;
    CFunctionHook* g_pMoveHook   = nullptr;
    CFunctionHook* g_pDrawHook   = nullptr;
    CFunctionHook* g_pPassHook   = nullptr;

    using origOnMouseButton    = void (*)(void*, IPointer::SButtonEvent, SP<IPointer>);
    using origMouseMoveUnified = void (*)(void*, uint32_t, bool, bool, std::optional<Vector2D>);
    using origGroupBarDraw     = void (*)(void*, PHLMONITOR, const float&);
    using origAddPassElement   = void (*)(void*, UP<IPassElement>&&);

    void hkOnMouseButton(void* thisptr, IPointer::SButtonEvent e, SP<IPointer> pointer) {
        if (TabDrag::consumesButton(e))
            return;

        ((origOnMouseButton)g_pButtonHook->m_original)(thisptr, e, pointer);

        TabDrag::afterButton(e);
    }

    void hkMouseMoveUnified(void* thisptr, uint32_t time, bool refocus, bool mouse, std::optional<Vector2D> overridePos) {
        ((origMouseMoveUnified)g_pMoveHook->m_original)(thisptr, time, refocus, mouse, overridePos);

        // An overridden position is not the pointer moving -- touch and the
        // compositor's own refocus calls come through here too.
        if (!overridePos.has_value())
            TabDrag::afterMouseMove();
    }

    // The groupbar draws all of its tabs in one call, so the tab being dragged is moved
    // by intercepting the pass elements that call emits rather than by changing the
    // drawing itself. Re-emitting them afterwards also puts the tab on top of its
    // neighbours, which is what it has to be while it slides over them.
    void hkGroupBarDraw(void* thisptr, PHLMONITOR monitor, const float& a) {
        const bool INTERCEPT = TabDrag::beginDraw(thisptr, monitor);

        ((origGroupBarDraw)g_pDrawHook->m_original)(thisptr, monitor, a);

        if (!INTERCEPT)
            return;

        for (auto& element : TabDrag::endDraw())
            ((origAddPassElement)g_pPassHook->m_original)(g_pHyprRenderer.get(), std::move(element));
    }

    void hkAddPassElement(void* thisptr, UP<IPassElement>&& element) {
        if (TabDrag::stashPassElement(element))
            return;

        ((origAddPassElement)g_pPassHook->m_original)(thisptr, std::move(element));
    }

    // findFunctionsByName matches on the unqualified name, so overloads and same-named
    // methods on other classes come back too. Pick by the demangled signature.
    void* findFunction(const std::string& name, const std::string& demangledContains) {
        for (const auto& MATCH : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
            if (MATCH.demangled.contains(demangledContains))
                return MATCH.address;
        }

        return nullptr;
    }

    [[noreturn]] void fail(const std::string& reason) {
        HyprlandAPI::addNotification(PHANDLE, "[tab-drag] " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[tab-drag] " + reason);
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // _get_client_hash() is inline in the headers, so it is baked into the plugin at
    // build time; _get_hash() is exported by the compositor. Both cover the Hyprland
    // commit *and* the versions of aquamarine/hyprutils/hyprgraphics/hyprcursor/
    // hyprlang, so this catches a dependency bump as well as a compositor one.
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    const std::string SERVER_HASH = __hyprland_api_get_hash();

    if (CLIENT_HASH != SERVER_HASH)
        fail(std::format("built against a different Hyprland than the one running\nplugin: {}\nrunning: {}", CLIENT_HASH, SERVER_HASH));

    HyprlandAPI::addConfigValueV2(PHANDLE,
                                  Config::Values::makeConfigValue<Config::Values::Bool>("plugin:tab-drag:enabled", "reorder grouped windows by dragging their groupbar tabs", true,
                                                                                        Config::Values::SBoolValueOptions{}));

    void* const BUTTON_FN = findFunction("onMouseButton", "CInputManager::onMouseButton");
    void* const MOVE_FN   = findFunction("mouseMoveUnified", "CInputManager::mouseMoveUnified");
    void* const DRAW_FN   = findFunction("draw", "CHyprGroupBarDecoration::draw");
    void* const PASS_FN   = findFunction("addPassElement", "IHyprRenderer::addPassElement");

    if (!BUTTON_FN || !MOVE_FN)
        fail("could not find the input functions to hook");

    if (!DRAW_FN || !PASS_FN)
        fail("could not find the rendering functions to hook");

    g_pButtonHook = HyprlandAPI::createFunctionHook(PHANDLE, BUTTON_FN, (void*)&hkOnMouseButton);
    g_pMoveHook   = HyprlandAPI::createFunctionHook(PHANDLE, MOVE_FN, (void*)&hkMouseMoveUnified);
    g_pDrawHook   = HyprlandAPI::createFunctionHook(PHANDLE, DRAW_FN, (void*)&hkGroupBarDraw);
    g_pPassHook   = HyprlandAPI::createFunctionHook(PHANDLE, PASS_FN, (void*)&hkAddPassElement);

    if (!g_pButtonHook->hook() || !g_pMoveHook->hook() || !g_pDrawHook->hook() || !g_pPassHook->hook())
        fail("could not hook the compositor functions");

    return {"tab-drag", "Reorder groupbar tabs by dragging them, like browser tabs", "zoan37", "0.2.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // The hooks are removed by Hyprland after this returns, but a gesture still holding
    // a group reference must not outlive them.
    TabDrag::reset();
}
