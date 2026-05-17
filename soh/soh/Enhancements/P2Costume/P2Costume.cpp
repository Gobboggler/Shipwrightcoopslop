// =========================================================================
//  P2Costume.cpp
//  Per-player tunic color override so P2 looks visually distinct from P1.
//
//  How it works:
//    SoH already exposes a Vanilla Behavior hook (VB_APPLY_TUNIC_COLOR) at
//    the exact gDPSetEnvColor call that tints Link's tunic in z_player_lib.c.
//    The hook receives Player* and Color_RGB8* — perfect for per-player
//    overrides. When the hook fires for P2 (PLAYER_GET_INDEX != 0), we
//    swap the color in place to match the user-selected preset.
//
//  Why not full model swap:
//    SoH's alt-assets system is global — toggling it swaps Link's model
//    for BOTH players. There's no per-actor resource-swap path without
//    much deeper rendering-pipeline work. Tunic + the existing color
//    filter tint already gives strong visual differentiation, so that's
//    where v1 ships.
// =========================================================================

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/MenuTypes.h"
#include "soh/SohGui/UIWidgets.hpp"

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
}

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
#include "macros.h"
}

// ---------- CVars --------------------------------------------------------
#define P2C_CVAR_PRESET    "gEnhancements.LocalCoop.P2Costume"
#define P2C_CVAR_CUSTOM_R  "gEnhancements.LocalCoop.P2CostumeCustomR"
#define P2C_CVAR_CUSTOM_G  "gEnhancements.LocalCoop.P2CostumeCustomG"
#define P2C_CVAR_CUSTOM_B  "gEnhancements.LocalCoop.P2CostumeCustomB"

// ---------- Preset table -------------------------------------------------
// Indices must match menu combobox order.
enum P2CostumePreset : int {
    P2C_PRESET_VANILLA   = 0,
    P2C_PRESET_DARK_LINK = 1,
    P2C_PRESET_SHEIKAH   = 2,
    P2C_PRESET_GERUDO    = 3,
    P2C_PRESET_GORON_ISH = 4,
    P2C_PRESET_ZORA_ISH  = 5,
    P2C_PRESET_KOKIRI_DARK = 6,
    P2C_PRESET_CUSTOM    = 7,
    P2C_PRESET_COUNT
};

struct PresetColor {
    const char* name;
    uint8_t r, g, b;
};

static const PresetColor kPresets[P2C_PRESET_COUNT] = {
    { "Vanilla (no change)", 0x00, 0x00, 0x00 },   // unused — vanilla branch returns early
    { "Dark Link",           0x18, 0x18, 0x18 },   // near-black
    { "Sheikah",             0x60, 0x70, 0x88 },   // gray-blue
    { "Gerudo",              0xB0, 0x30, 0x40 },   // crimson-red
    { "Goron-ish",           0xC0, 0x55, 0x20 },   // burnt orange
    { "Zora-ish",            0x40, 0x80, 0xC0 },   // ocean blue
    { "Dark Kokiri",         0x28, 0x60, 0x30 },   // dark green
    { "Custom",              0x80, 0x80, 0x80 },   // overridden by Custom CVars
};

// ---------- Hook ---------------------------------------------------------
//
// VB_APPLY_TUNIC_COLOR is invoked once per Player draw, just before the
// gDPSetEnvColor call. Its args are (Player*, Color_RGB8*).
//
// Behaviour:
//   - P1 (index 0): always vanilla, never touch.
//   - P2+, preset == Vanilla: don't touch — engine uses vanilla tunic color.
//   - P2+, any other preset: override using the RGB slider CVars.
//
// The RGB slider CVars are the source of truth for the color. Picking a
// non-Vanilla preset from the menu pre-fills those sliders with that
// preset's baseline RGB (via Callback in the menu), and the user can then
// drag the sliders to tweak from there. The hook never reads the preset
// list directly — it just gates on Vanilla vs. anything-else.
static void RegisterP2CostumeHook() {
    COND_VB_SHOULD(VB_APPLY_TUNIC_COLOR, true, {
        Player*     player = va_arg(args, Player*);
        Color_RGB8* color  = va_arg(args, Color_RGB8*);

        if (player == nullptr || color == nullptr) return;
        if (PLAYER_GET_INDEX(&player->actor) == 0) return;   // P1 untouched

        int preset = CVarGetInteger(P2C_CVAR_PRESET, P2C_PRESET_VANILLA);
        if (preset <= P2C_PRESET_VANILLA || preset >= P2C_PRESET_COUNT) return;

        // Sliders are always the source of truth. Pull current RGB.
        color->r = (uint8_t)CVarGetInteger(P2C_CVAR_CUSTOM_R, 0x80);
        color->g = (uint8_t)CVarGetInteger(P2C_CVAR_CUSTOM_G, 0x80);
        color->b = (uint8_t)CVarGetInteger(P2C_CVAR_CUSTOM_B, 0x80);
    });
}

// ---------- ShipInit -----------------------------------------------------
static void RegisterP2Costume() {
    CVarRegisterInteger(P2C_CVAR_PRESET, P2C_PRESET_VANILLA);
    CVarRegisterInteger(P2C_CVAR_CUSTOM_R, 0x80);
    CVarRegisterInteger(P2C_CVAR_CUSTOM_G, 0x80);
    CVarRegisterInteger(P2C_CVAR_CUSTOM_B, 0x80);
    RegisterP2CostumeHook();
}
static RegisterShipInitFunc initP2Costume(RegisterP2Costume, { P2C_CVAR_PRESET });

// ---------- Menu page ----------------------------------------------------
using namespace UIWidgets;

// Writes the given preset's baseline RGB into the slider CVars. Called from
// the combobox Callback whenever the user picks a new preset.
static void LoadPresetIntoSliders(int preset) {
    if (preset <= P2C_PRESET_VANILLA || preset >= P2C_PRESET_COUNT) return;
    if (preset == P2C_PRESET_CUSTOM) return;   // leave sliders untouched
    const PresetColor& p = kPresets[preset];
    CVarSetInteger(P2C_CVAR_CUSTOM_R, p.r);
    CVarSetInteger(P2C_CVAR_CUSTOM_G, p.g);
    CVarSetInteger(P2C_CVAR_CUSTOM_B, p.b);
    Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

static void AddP2CostumeMenu() {
    auto menu = SohGui::mSohMenu;
    if (!menu) return;

    menu->AddSidebarEntry("Enhancements", "P2 Costume", 2);

    WidgetPath path = { "Enhancements", "P2 Costume", SECTION_COLUMN_1 };

    // ----- Column 1: preset picker -----
    menu->AddWidget(path, "P2 Visual Differentiation", WIDGET_SEPARATOR_TEXT);

    menu->AddWidget(path,
        "Recolors Player 2's tunic to make them visually distinct from P1.\n"
        "Pick a preset for a starting color, then nudge the RGB sliders to\n"
        "the right for fine-tuning. Selecting Vanilla disables the override\n"
        "entirely.\n\n"
        "Combine with the Local Co-op color filter (Blue / Red / Gray) for\n"
        "even stronger differentiation — the two systems stack cleanly.",
        WIDGET_TEXT);

    static const std::map<int32_t, const char*> kPresetNames = {
        { P2C_PRESET_VANILLA,      "Vanilla (no override)"   },
        { P2C_PRESET_DARK_LINK,    "Dark Link"               },
        { P2C_PRESET_SHEIKAH,      "Sheikah"                 },
        { P2C_PRESET_GERUDO,       "Gerudo"                  },
        { P2C_PRESET_GORON_ISH,    "Goron-ish"               },
        { P2C_PRESET_ZORA_ISH,     "Zora-ish"                },
        { P2C_PRESET_KOKIRI_DARK,  "Dark Kokiri"             },
        { P2C_PRESET_CUSTOM,       "Custom (sliders only)"   },
    };

    menu->AddWidget(path, "P2 Tunic Preset", WIDGET_CVAR_COMBOBOX)
        .CVar(P2C_CVAR_PRESET)
        .Callback([](WidgetInfo& info) {
            // Whenever the user picks a preset, copy its baseline RGB into
            // the slider CVars so the sliders show the chosen color and
            // the user can tweak from there. Custom and Vanilla don't
            // preload anything.
            int newPreset = CVarGetInteger(P2C_CVAR_PRESET, P2C_PRESET_VANILLA);
            LoadPresetIntoSliders(newPreset);
        })
        .Options(ComboboxOptions()
            .Tooltip("Pick which costume color P2 wears. Selecting a preset "
                     "fills the RGB sliders with that color — use the sliders "
                     "to tweak from there. Affects only P2; P1 always uses "
                     "the vanilla tunic color.")
            .ComboMap(kPresetNames)
            .DefaultIndex(P2C_PRESET_VANILLA));

    // ----- Column 2: RGB sliders (always active when preset != Vanilla) -----
    path.column = SECTION_COLUMN_2;

    menu->AddWidget(path, "RGB (live)", WIDGET_SEPARATOR_TEXT);

    menu->AddWidget(path,
        "These sliders are the source of truth for P2's tunic color\n"
        "whenever any non-Vanilla preset is selected. Drag freely.",
        WIDGET_TEXT);

    menu->AddWidget(path, "Red", WIDGET_CVAR_SLIDER_INT)
        .CVar(P2C_CVAR_CUSTOM_R)
        .Options(IntSliderOptions()
            .Tooltip("Red channel of P2's tunic color (0-255).")
            .DefaultValue(0x80)
            .Min(0).Max(255));

    menu->AddWidget(path, "Green", WIDGET_CVAR_SLIDER_INT)
        .CVar(P2C_CVAR_CUSTOM_G)
        .Options(IntSliderOptions()
            .Tooltip("Green channel of P2's tunic color (0-255).")
            .DefaultValue(0x80)
            .Min(0).Max(255));

    menu->AddWidget(path, "Blue", WIDGET_CVAR_SLIDER_INT)
        .CVar(P2C_CVAR_CUSTOM_B)
        .Options(IntSliderOptions()
            .Tooltip("Blue channel of P2's tunic color (0-255).")
            .DefaultValue(0x80)
            .Min(0).Max(255));

    menu->AddWidget(path, "Limitations", WIDGET_SEPARATOR_TEXT);
    menu->AddWidget(path,
        "Only the tunic is recolored. Hat, skin, and hair stay vanilla\n"
        "because SoH doesn't expose per-actor render hooks for those\n"
        "limbs. Full character model swap (different skeleton entirely)\n"
        "is a much bigger project — coming in a future patch if requested.",
        WIDGET_TEXT);
}

static RegisterMenuInitFunc initP2CostumeMenu(AddP2CostumeMenu);
