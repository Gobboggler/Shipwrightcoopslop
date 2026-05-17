// =========================================================================
//  SlotMachineUI.cpp
//  ImGui modal for "Rich Little Cuccos". Three animated reels, lever-pull
//  spin button, payout table, themed colors.
//
//  Drawn from the OnGameFrameUpdate hook (same pattern as HorseBettingUI).
// =========================================================================

#include "HorseBetting.h"

#include <cstdio>
#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
}

namespace SlotMachine {

// Mirror of the symbol tables from SlotMachine.cpp. Duplicated here rather
// than exported to keep the UI file self-contained for ImGui-only edits.
static const char* kUiSymbolNames[SYM_COUNT] = {
    "Bomb", "Bottle", "Heart", "Rupee", "Cucco", "Triforce",
};
static const float kUiSymbolColors[SYM_COUNT][3] = {
    {0.20f, 0.20f, 0.25f},   // Bomb — dark
    {0.30f, 0.85f, 0.45f},   // Bottle — green
    {0.95f, 0.30f, 0.40f},   // Heart — red
    {0.40f, 0.80f, 0.95f},   // Rupee — blue
    {0.95f, 0.90f, 0.40f},   // Cucco — yellow
    {1.00f, 0.85f, 0.10f},   // Triforce — gold
};
static const char* kUiSymbolLabels[SYM_COUNT] = {
    "Bomb", "Bottle", "Heart", "Rupee", "Cucco", "Triforce",
};
static const int kUiPayoutTable[SYM_COUNT] = {
    8, 12, 15, 20, 30, 100,
};

// Draw an item-icon glyph using ImGui DrawList primitives, centered in the
// rect (top-left, bottom-right corners). The N64 item textures themselves
// aren't accessible from ImGui (they live in OTR archives and target the
// game's gfx pipeline, not ImGui's), so we approximate each item with a
// recognizable vector shape in its themed color.
static void DrawIconBomb(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    float cx = (tl.x + br.x) * 0.5f;
    float cy = (tl.y + br.y) * 0.5f;
    float r  = std::min(br.x - tl.x, br.y - tl.y) * 0.30f;
    // Body
    dl->AddCircleFilled(ImVec2(cx, cy + r * 0.15f), r, IM_COL32(25, 25, 35, 255));
    dl->AddCircle(ImVec2(cx, cy + r * 0.15f), r, IM_COL32(180, 180, 200, 255), 16, 2.0f);
    // Fuse
    dl->AddLine(ImVec2(cx, cy - r * 0.85f),
                ImVec2(cx + r * 0.3f, cy - r * 1.4f),
                IM_COL32(170, 130, 70, 255), 3.0f);
    // Spark
    dl->AddCircleFilled(ImVec2(cx + r * 0.3f, cy - r * 1.4f), r * 0.20f,
                        IM_COL32(255, 200, 60, 255));
}

static void DrawIconBottle(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    float cx = (tl.x + br.x) * 0.5f;
    float cy = (tl.y + br.y) * 0.5f;
    float h  = (br.y - tl.y) * 0.65f;
    float w  = h * 0.55f;
    // Body
    dl->AddRectFilled(ImVec2(cx - w * 0.5f, cy - h * 0.2f),
                      ImVec2(cx + w * 0.5f, cy + h * 0.55f),
                      IM_COL32(70, 200, 110, 255), 4.0f);
    dl->AddRect(ImVec2(cx - w * 0.5f, cy - h * 0.2f),
                ImVec2(cx + w * 0.5f, cy + h * 0.55f),
                IM_COL32(30, 100, 50, 255), 4.0f, 0, 2.0f);
    // Neck
    dl->AddRectFilled(ImVec2(cx - w * 0.25f, cy - h * 0.5f),
                      ImVec2(cx + w * 0.25f, cy - h * 0.2f),
                      IM_COL32(180, 180, 200, 255));
    // Cap
    dl->AddRectFilled(ImVec2(cx - w * 0.30f, cy - h * 0.55f),
                      ImVec2(cx + w * 0.30f, cy - h * 0.42f),
                      IM_COL32(150, 60, 30, 255));
}

static void DrawIconHeart(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    float cx = (tl.x + br.x) * 0.5f;
    float cy = (tl.y + br.y) * 0.5f;
    float r  = std::min(br.x - tl.x, br.y - tl.y) * 0.22f;
    ImU32 red = IM_COL32(230, 50, 70, 255);
    // Two lobes
    dl->AddCircleFilled(ImVec2(cx - r, cy - r * 0.4f), r, red);
    dl->AddCircleFilled(ImVec2(cx + r, cy - r * 0.4f), r, red);
    // Point
    ImVec2 p0(cx - r * 1.6f, cy);
    ImVec2 p1(cx + r * 1.6f, cy);
    ImVec2 p2(cx, cy + r * 2.0f);
    dl->AddTriangleFilled(p0, p1, p2, red);
}

static void DrawIconRupee(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    float cx = (tl.x + br.x) * 0.5f;
    float cy = (tl.y + br.y) * 0.5f;
    float w  = (br.x - tl.x) * 0.30f;
    float h  = (br.y - tl.y) * 0.35f;
    ImU32 green = IM_COL32(60, 220, 100, 255);
    ImU32 hi    = IM_COL32(180, 255, 180, 255);
    // Diamond shape
    ImVec2 top(cx, cy - h);
    ImVec2 right(cx + w, cy);
    ImVec2 bot(cx, cy + h);
    ImVec2 left(cx - w, cy);
    dl->AddQuadFilled(top, right, bot, left, green);
    // Inner highlight
    dl->AddLine(top, right, hi, 2.0f);
    dl->AddLine(top, left, hi, 2.0f);
}

static void DrawIconCucco(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    float cx = (tl.x + br.x) * 0.5f;
    float cy = (tl.y + br.y) * 0.5f;
    float r  = std::min(br.x - tl.x, br.y - tl.y) * 0.28f;
    // Body
    dl->AddCircleFilled(ImVec2(cx, cy + r * 0.2f), r,
                        IM_COL32(245, 240, 225, 255));
    // Wing/wing shadow
    dl->AddCircleFilled(ImVec2(cx + r * 0.4f, cy + r * 0.3f), r * 0.5f,
                        IM_COL32(220, 200, 170, 255));
    // Comb (red triangle on top)
    ImVec2 c0(cx - r * 0.3f, cy - r * 0.7f);
    ImVec2 c1(cx + r * 0.3f, cy - r * 0.7f);
    ImVec2 c2(cx, cy - r * 1.15f);
    dl->AddTriangleFilled(c0, c1, c2, IM_COL32(220, 60, 50, 255));
    // Beak
    ImVec2 b0(cx - r * 0.45f, cy - r * 0.2f);
    ImVec2 b1(cx - r * 0.85f, cy - r * 0.05f);
    ImVec2 b2(cx - r * 0.45f, cy + r * 0.05f);
    dl->AddTriangleFilled(b0, b1, b2, IM_COL32(255, 180, 60, 255));
    // Eye
    dl->AddCircleFilled(ImVec2(cx - r * 0.25f, cy - r * 0.3f), r * 0.10f,
                        IM_COL32(0, 0, 0, 255));
}

static void DrawIconTriforce(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    float cx = (tl.x + br.x) * 0.5f;
    float cy = (tl.y + br.y) * 0.5f;
    float side = std::min(br.x - tl.x, br.y - tl.y) * 0.45f;
    float h = side * std::sqrt(3.0f) * 0.5f;
    ImU32 gold = IM_COL32(255, 220, 50, 255);
    ImU32 outline = IM_COL32(150, 100, 0, 255);

    // Three small triangles forming the Triforce
    // Top triangle
    ImVec2 t1(cx, cy - h);
    ImVec2 t2(cx - side * 0.5f, cy);
    ImVec2 t3(cx + side * 0.5f, cy);
    dl->AddTriangleFilled(t1, t2, t3, gold);
    dl->AddTriangle(t1, t2, t3, outline, 1.5f);
    // Bottom-left triangle
    ImVec2 bl1(cx - side * 0.5f, cy);
    ImVec2 bl2(cx - side, cy + h);
    ImVec2 bl3(cx, cy + h);
    dl->AddTriangleFilled(bl1, bl2, bl3, gold);
    dl->AddTriangle(bl1, bl2, bl3, outline, 1.5f);
    // Bottom-right triangle
    ImVec2 br1(cx + side * 0.5f, cy);
    ImVec2 br2(cx + side, cy + h);
    ImVec2 br3(cx, cy + h);
    dl->AddTriangleFilled(br1, br2, br3, gold);
    dl->AddTriangle(br1, br2, br3, outline, 1.5f);
}

// Dispatch by symbol id.
static void DrawSymbolIcon(ImDrawList* dl, int symbol, ImVec2 tl, ImVec2 br) {
    switch (symbol) {
        case SYM_BOMB:     DrawIconBomb(dl, tl, br); break;
        case SYM_BOTTLE:   DrawIconBottle(dl, tl, br); break;
        case SYM_HEART:    DrawIconHeart(dl, tl, br); break;
        case SYM_RUPEE:    DrawIconRupee(dl, tl, br); break;
        case SYM_CUCCO:    DrawIconCucco(dl, tl, br); break;
        case SYM_TRIFORCE: DrawIconTriforce(dl, tl, br); break;
        default: break;
    }
}

// Render a single reel cell. Item icon centered on a tinted background.
static void DrawReel(int symbol, bool spinning, int reelIdx) {
    if (symbol < 0 || symbol >= SYM_COUNT) symbol = 0;

    ImVec4 bg(kUiSymbolColors[symbol][0],
              kUiSymbolColors[symbol][1],
              kUiSymbolColors[symbol][2], 1.0f);

    // Slightly desaturate while spinning for a "motion blur" feel.
    if (spinning) {
        bg.x = bg.x * 0.5f + 0.35f;
        bg.y = bg.y * 0.5f + 0.35f;
        bg.z = bg.z * 0.5f + 0.35f;
    }

    char id[16];
    std::snprintf(id, sizeof(id), "##reel%d", reelIdx);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::BeginChild(id, ImVec2(110, 110), true,
                      ImGuiWindowFlags_NoScrollbar);

    // Draw the icon centered using DrawList. The child window's screen
    // rect bounds the icon.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wpos = ImGui::GetWindowPos();
    ImVec2 wsize = ImGui::GetWindowSize();
    ImVec2 inner_tl(wpos.x + 8.0f, wpos.y + 8.0f);
    ImVec2 inner_br(wpos.x + wsize.x - 8.0f, wpos.y + wsize.y - 8.0f);
    DrawSymbolIcon(dl, symbol, inner_tl, inner_br);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawWindow() {
    auto& s = GetSession();
    if (s.state == SLOT_STATE_CLOSED) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings;

    // Gold-on-burgundy color theme for that gambling-parlor feel.
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.30f, 0.05f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.45f, 0.08f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.15f, 0.05f, 0.07f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(1.00f, 0.80f, 0.10f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

    if (ImGui::Begin("Rich Little Cuccos##slot_machine", nullptr, flags)) {
        // ----- Title banner -----
        ImGui::SetWindowFontScale(1.8f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.10f, 1.0f));
        const char* banner = "*** RICH LITTLE CUCCOS ***";
        float bannerWidth = ImGui::CalcTextSize(banner).x;
        ImGui::SetCursorPosX((520.0f - bannerWidth) * 0.5f);
        ImGui::TextUnformatted(banner);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ----- Reels -----
        bool spinning = (s.state == SLOT_STATE_SPINNING);
        // Center the three reel cells horizontally (3 * 110 + 2 * 18 spacing).
        const float reelRowWidth = 3 * 110.0f + 2 * 18.0f;
        ImGui::SetCursorPosX((520.0f - reelRowWidth) * 0.5f);
        for (int r = 0; r < HB_SLOT_NUM_REELS; ++r) {
            bool reelSpinning = spinning && !s.reelLanded[r];
            DrawReel(s.currentDisplay[r], reelSpinning, r);
            if (r < HB_SLOT_NUM_REELS - 1) {
                ImGui::SameLine(0.0f, 18.0f);
            }
        }
        ImGui::Spacing();
        ImGui::Spacing();

        // ----- Bet controls -----
        ImGui::Text("Your rupees: %d", gSaveContext.rupees);
        int maxBet = (gSaveContext.rupees < HB_SLOT_MAX_BET)
                        ? (int)gSaveContext.rupees : HB_SLOT_MAX_BET;
        if (maxBet < 1) maxBet = 1;
        if (s.betAmount > maxBet) s.betAmount = maxBet;
        if (s.betAmount < 1)      s.betAmount = 1;

        ImGui::BeginDisabled(spinning);
        ImGui::SliderInt("Bet (rupees)", &s.betAmount, 1, maxBet);
        ImGui::EndDisabled();

        ImGui::Spacing();

        // ----- Spin / Close buttons -----
        bool canSpin = (s.state == SLOT_STATE_IDLE) &&
                       (s.betAmount >= 1) &&
                       (s.betAmount <= (int)gSaveContext.rupees);
        ImGui::BeginDisabled(!canSpin);
        if (ImGui::Button(spinning ? "Spinning..." : "PULL THE LEVER!",
                          ImVec2(-1, 44))) {
            SlotMachine_Spin(s.betAmount);
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(spinning);
        if (ImGui::Button("Leave the parlor", ImVec2(-1, 0))) {
            SlotMachine_Close();
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();

        // ----- Result line -----
        if (s.state == SLOT_STATE_RESULT) {
            if (s.lastPayout > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
                ImGui::SetWindowFontScale(1.3f);
                ImGui::Text("WIN! %dx -- %d rupees!",
                            s.lastWinMultiplier, s.lastPayout);
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                ImGui::Text("No match. Try again, pal.");
                ImGui::PopStyleColor();
            }
        } else if (s.state == SLOT_STATE_IDLE) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Pull the lever to spin!");
        } else if (spinning) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.1f, 1.0f),
                "*** clack clack clack ***");
        }

        // ----- Payout table (collapsible) -----
        if (ImGui::CollapsingHeader("Payout table")) {
            ImGui::Text("3 of a kind pays bet * multiplier:");
            // Iterate symbols in descending payout order: TRI, CUC, RUP, HRT, BTL, BMB
            static const int kDispOrder[SYM_COUNT] = {
                SYM_TRIFORCE, SYM_CUCCO, SYM_RUPEE,
                SYM_HEART, SYM_BOTTLE, SYM_BOMB
            };
            for (int i = 0; i < SYM_COUNT; ++i) {
                int sym = kDispOrder[i];
                ImVec4 col(kUiSymbolColors[sym][0],
                           kUiSymbolColors[sym][1],
                           kUiSymbolColors[sym][2], 1.0f);
                ImGui::ColorButton("##paycolor", col,
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                    ImVec2(16, 16));
                ImGui::SameLine();
                ImGui::Text("%-8s x%d  (%s %s %s)",
                            kUiSymbolLabels[sym],
                            kUiPayoutTable[sym],
                            kUiSymbolNames[sym],
                            kUiSymbolNames[sym],
                            kUiSymbolNames[sym]);
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

}  // namespace SlotMachine
