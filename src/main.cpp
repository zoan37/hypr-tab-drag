#include "globals.hpp"
#include "TabDrag.hpp"

#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <optional>
#include <stdexcept>

namespace {
    CFunctionHook* g_pButtonHook = nullptr;
    CFunctionHook* g_pMoveHook   = nullptr;

    using origOnMouseButton    = void (*)(void*, IPointer::SButtonEvent, SP<IPointer>);
    using origMouseMoveUnified = void (*)(void*, uint32_t, bool, bool, std::optional<Vector2D>);

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

    const std::string HASH = __hyprland_api_get_hash();

    if (HASH != HyprlandAPI::getHyprlandVersion(PHANDLE).hash)
        fail("headers were built against a different Hyprland than the one running");

    HyprlandAPI::addConfigValueV2(PHANDLE,
                                  Config::Values::makeConfigValue<Config::Values::Bool>("plugin:tab-drag:enabled", "reorder grouped windows by dragging their groupbar tabs", true,
                                                                                        Config::Values::SBoolValueOptions{}));

    void* const BUTTON_FN = findFunction("onMouseButton", "CInputManager::onMouseButton");
    void* const MOVE_FN   = findFunction("mouseMoveUnified", "CInputManager::mouseMoveUnified");

    if (!BUTTON_FN || !MOVE_FN)
        fail("could not find the input functions to hook");

    g_pButtonHook = HyprlandAPI::createFunctionHook(PHANDLE, BUTTON_FN, (void*)&hkOnMouseButton);
    g_pMoveHook   = HyprlandAPI::createFunctionHook(PHANDLE, MOVE_FN, (void*)&hkMouseMoveUnified);

    if (!g_pButtonHook->hook() || !g_pMoveHook->hook())
        fail("could not hook the input functions");

    return {"tab-drag", "Reorder groupbar tabs by dragging them, like browser tabs", "zoan37", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // The hooks are removed by Hyprland after this returns, but a gesture still holding
    // a group reference must not outlive them.
    TabDrag::reset();
}
