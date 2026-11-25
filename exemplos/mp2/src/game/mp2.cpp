// Max Payne 2: what src/common needs from the game. No hooks on the game's functions yet: each one goes here (or in
// its own file in this folder) with the address, what the function does and how that was found (docs/MP2-notes.md).
#include <string>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace game {

// the game picks its language from the console's settings: the menu's row offers English only
bool HasLanguages() { return false; }
bool LanguageAvailable(const std::string& code) { return code == "int"; }
bool HudShown() { return true; }

// the OLED model's darker darks (src/d3d/d3d8.cpp, SendGamma): the option on, or on AUTO an OLED console out of the
// dock. Kept from the base as it is: it changes the picture only when the player turns it on (AUTO is the default
// there; this port's default is off, see options.h)
bool OledBoost() {
    uint32_t mode = Opt().oled;
    if (mode) return mode == 2;
#ifdef __SWITCH__
    static const bool oled = [] {
        SetSysProductModel model = SetSysProductModel_Invalid;
        if (R_SUCCEEDED(setsysInitialize())) {
            setsysGetProductModel(&model);
            setsysExit();
        }
        LOGI("oled: product model %d", int(model));
        return model == SetSysProductModel_Aula;
    }();
    return oled && appletGetOperationMode() == AppletOperationMode_Handheld;
#else
    return false;
#endif
}

}  // namespace game
