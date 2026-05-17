// =========================================================================
//  HorseBettingMenu.cpp
//  Adds an "Enhancements > Horse Betting" sidebar entry with toggles for
//  the mod, the debug window, and quick test buttons.
//
//  Uses RegisterMenuInitFunc so the entries get added after SoH's main menu
//  pages are built. Zero edits to existing SoH files; no conflict surface
//  with the coop mod.
// =========================================================================

#include "HorseBetting.h"

#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/MenuTypes.h"
#include "soh/SohGui/UIWidgets.hpp"

#include <libultraship/bridge/consolevariablebridge.h>

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
}

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

using namespace UIWidgets;

static void AddHorseBettingMenu() {
    auto menu = SohGui::mSohMenu;
    if (!menu) return;

    // Sidebar entry under the existing "Enhancements" section.
    // Columns = 2 (left: feature toggles, right: diagnostics).
    menu->AddSidebarEntry("Enhancements", "Horse Betting", 2);

    WidgetPath path = { "Enhancements", "Horse Betting", SECTION_COLUMN_1 };

    // ----- Column 1: feature toggles -----
    menu->AddWidget(path, "Lon Lon Gambling", WIDGET_SEPARATOR_TEXT);

    menu->AddWidget(path, "Enable Horse Betting Mod", WIDGET_CVAR_CHECKBOX)
        .CVar(HB_CVAR_ENABLED)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Spawns the bookmaker and slot dealer NPCs at Lon Lon Ranch "
            "(adult timeline only), near where Ingo stands.\n\n"
            "If the NPCs are already in the scene you'll need to leave and "
            "re-enter the ranch for changes to take effect."));

    menu->AddWidget(path, "How to use", WIDGET_SEPARATOR_TEXT);
    menu->AddWidget(path,
        "Adult Link only. Walk into Lon Lon Ranch (the main outdoor area).\n"
        "Two NPCs spawn near Ingo:\n"
        "  - Rich businessman  ->  Horse-race betting\n"
        "  - Old woman         ->  Rich Little Cuccos slot machine",
        WIDGET_TEXT);

    // ----- Column 2: diagnostics -----
    path.column = SECTION_COLUMN_2;

    menu->AddWidget(path, "Diagnostics", WIDGET_SEPARATOR_TEXT);

    menu->AddWidget(path, "Show Debug Window", WIDGET_CVAR_CHECKBOX)
        .CVar(HB_CVAR_DEBUG)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Shows a debug overlay with current scene, player position, "
            "and waypoint capture tools.\n\n"
            "Useful for verifying the mod is loaded — if this checkbox "
            "exists in the menu, the mod is in your binary."));

    menu->AddWidget(path, "Testing", WIDGET_SEPARATOR_TEXT);

    menu->AddWidget(path, "Open Betting Menu", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip(
            "Force-opens the horse-betting modal without needing to talk "
            "to the bookmaker. Useful for testing UI."))
        .Callback([](WidgetInfo& info) {
            HorseBetting_OpenBettingUI();
        });

    menu->AddWidget(path, "Open Slot Machine", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip(
            "Force-opens the Rich Little Cuccos slot machine modal."))
        .Callback([](WidgetInfo& info) {
            SlotMachine_Open();
        });
}

static RegisterMenuInitFunc initHorseBettingMenu(AddHorseBettingMenu);
