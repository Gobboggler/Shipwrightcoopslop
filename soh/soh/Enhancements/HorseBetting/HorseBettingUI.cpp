// =========================================================================
//  HorseBettingUI.cpp
//  ImGui windows: betting modal, countdown / result overlays, debug tuning.
//  All drawn from OnGameFrameUpdate (same pattern as ValueViewer).
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

namespace HorseBetting {

// ---------- player-facing betting modal ----------------------------------
static int sUiBetAmount = 10;

void DrawBettingWindow() {
    auto& s = GetSession();
    if (s.state != HB_STATE_BETTING) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 500), ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("Lon Lon Bookmaker", nullptr, flags)) {
        ImGui::TextWrapped(
            "\"Place yer bets, friend. Each horse's odds tell ya the payout.\n"
            " Pick a runner and how many rupees yer puttin' down.\"");
        ImGui::Separator();

        ImGui::Text("Your rupees: %d", gSaveContext.rupees);
        int maxBet = (gSaveContext.rupees < HB_MAX_BET)
                        ? (int)gSaveContext.rupees : HB_MAX_BET;
        if (maxBet < 1) maxBet = 1;
        if (sUiBetAmount > maxBet) sUiBetAmount = maxBet;

        ImGui::SliderInt("Bet (rupees)", &sUiBetAmount, 1, maxBet);
        ImGui::Separator();

        ImGui::Text("Today's runners:");
        ImGui::BeginChild("horse_list", ImVec2(0, 280), true);
        for (int i = 0; i < HB_NUM_HORSES; ++i) {
            const HorseInfo& h = s.horses[i];

            ImVec4 col = ImVec4(h.colorR / 255.0f,
                                h.colorG / 255.0f,
                                h.colorB / 255.0f, 1.0f);
            ImGui::PushID(i);
            ImGui::ColorButton("##swatch", col,
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                ImVec2(28, 28));
            ImGui::SameLine();

            char label[160];
            float potentialWin = sUiBetAmount * h.odds;
            std::snprintf(label, sizeof(label),
                          "%-18s  %.1fx   (pays %.0f)",
                          h.name, h.odds, potentialWin);

            bool selected = (s.chosenHorse == i);
            if (ImGui::Selectable(label, selected, 0, ImVec2(0, 28))) {
                s.chosenHorse = i;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Speed: %.1f +/- %.1f", h.speedBase, h.speedVariance);
                ImGui::Text("Stamina: %.0f%%", h.stamina * 100.0f);
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::Separator();

        bool canBet = (s.chosenHorse >= 0) &&
                      (sUiBetAmount >= 1) &&
                      (sUiBetAmount <= (int)gSaveContext.rupees);

        if (!canBet) ImGui::BeginDisabled();
        if (ImGui::Button("Place bet and start the race!", ImVec2(-1, 32))) {
            HorseBetting_BeginRace(s.chosenHorse, sUiBetAmount);
        }
        if (!canBet) ImGui::EndDisabled();

        if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
            HorseBetting_CancelBetting();
        }
    }
    ImGui::End();
}

// ---------- countdown overlay --------------------------------------------
void DrawCountdownOverlay() {
    auto& s = GetSession();
    if (s.state != HB_STATE_COUNTDOWN) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##hb_countdown", nullptr, flags)) {
        const char* txt = (s.countdown == 3) ? "3" :
                          (s.countdown == 2) ? "2" :
                          (s.countdown == 1) ? "1" : "GO!";
        ImGui::SetWindowFontScale(8.0f);
        ImVec4 color = (s.countdown <= 0)
            ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
            : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImGui::TextColored(color, "%s", txt);
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
}

// ---------- race visualization (during RACING + FINISHED states) ---------
//
// Compact HUD mini-tracker pinned to the bottom-left of the screen so the
// player can see all six horses' positions at a glance without it covering
// the main view of the on-track action. Each horse is a colored dot moving
// along its lane.
void DrawRaceVisualization() {
    auto& s = GetSession();
    if (s.state != HB_STATE_RACING && s.state != HB_STATE_FINISHED) return;

    // Bottom-left corner, comfortable margin
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos = ImVec2(vp->Pos.x + 24.0f,
                        vp->Pos.y + vp->Size.y - 24.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(360.0f, 196.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.70f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.05f, 0.02f, 0.70f));
    if (ImGui::Begin("##hb_race_viz", nullptr, flags)) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "RACE TRACKER");
        ImGui::Separator();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 lane_origin = ImGui::GetCursorScreenPos();
        const float lane_width = 340.0f;
        const float lane_height = 18.0f;
        const float lane_pad = 2.0f;
        const float track_pad = 48.0f;
        const float track_len = lane_width - track_pad - 16.0f;

        // Total race distance: HB_NUM_LAPS laps of arcPerLap (computed
        // once in HorseBetting::ComputeArcLengths). This is the SAME
        // total UpdateHorseAI uses for lap detection, so progress fraction
        // here matches in-world position exactly.
        float total = s.arcPerLap * (float)HB_NUM_LAPS;
        if (total < 1.0f) total = 1.0f;

        for (int i = 0; i < HB_NUM_HORSES; ++i) {
            const HorseInfo& h = s.horses[i];
            const HorseRuntime& rt = s.runtimes[i];

            float y0 = lane_origin.y + i * (lane_height + lane_pad);
            float y1 = y0 + lane_height;

            // Lane background
            ImU32 bg = (i & 1) ? IM_COL32(40, 30, 20, 200) : IM_COL32(55, 40, 28, 200);
            dl->AddRectFilled(ImVec2(lane_origin.x, y0),
                              ImVec2(lane_origin.x + lane_width, y1), bg);

            // Short name label (truncated)
            char nameBuf[10];
            std::snprintf(nameBuf, sizeof(nameBuf), "%-7.7s", h.name);
            dl->AddText(ImVec2(lane_origin.x + 4.0f, y0 + 3.0f),
                        IM_COL32(220, 220, 200, 255), nameBuf);

            // Track line
            float track_x0 = lane_origin.x + track_pad;
            float track_x1 = track_x0 + track_len;
            float track_y  = (y0 + y1) * 0.5f;
            dl->AddLine(ImVec2(track_x0, track_y),
                        ImVec2(track_x1, track_y),
                        IM_COL32(80, 60, 40, 255), 1.5f);

            // Progress 0..1
            float progress = rt.distAlongPath / total;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;

            float hx = track_x0 + progress * track_len;
            ImU32 horseColor = IM_COL32(h.colorR, h.colorG, h.colorB, 255);

            // Horse dot — gold ring if this is the player's pick.
            dl->AddCircleFilled(ImVec2(hx, track_y), 6.0f, horseColor);
            if (i == s.chosenHorse) {
                dl->AddCircle(ImVec2(hx, track_y), 8.5f,
                              IM_COL32(255, 220, 80, 255), 12, 2.0f);
            }
        }

        ImGui::Dummy(ImVec2(lane_width,
                            HB_NUM_HORSES * (lane_height + lane_pad)));
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// ---------- result overlay (during FINISHED, on-track) -------------------
void DrawResultOverlay() {
    auto& s = GetSession();
    if (s.state != HB_STATE_FINISHED) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos = vp->GetCenter();
    pos.y -= 200.0f;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.6f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##hb_result", nullptr, flags)) {
        const HorseInfo& winner = s.horses[s.winnerIdx];
        ImGui::SetWindowFontScale(2.0f);
        ImGui::Text("Winner: %s !", winner.name);
        if (s.chosenHorse == s.winnerIdx) {
            int payout = (int)(s.betAmount * winner.odds);
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
                "You win %d rupees!", payout);
        } else {
            const HorseInfo& yours = s.horses[s.chosenHorse];
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
                "%s came in %d.", yours.name,
                s.runtimes[s.chosenHorse].finalPlacement);
        }
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
}

// ---------- payout modal (back at Talon's) -------------------------------
void DrawPayoutOverlay() {
    auto& s = GetSession();
    if (s.state != HB_STATE_PAYOUT) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 200), ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("Lon Lon Bookmaker", nullptr, flags)) {
        if (s.chosenHorse == s.winnerIdx) {
            const HorseInfo& winner = s.horses[s.winnerIdx];
            int payout = (int)(s.betAmount * winner.odds);
            ImGui::TextWrapped(
                "\"Now THAT'S what I call a winning bet!\n"
                "  %s came through for ya — %d rupees, all yours!\"",
                winner.name, payout);
        } else {
            ImGui::TextWrapped(
                "\"Tough luck, friend. The track's a fickle mistress.\n"
                "  Better luck next time, pal!\"");
        }
        ImGui::Separator();
        if (ImGui::Button("Thanks, pal.", ImVec2(-1, 32))) {
            HorseBetting_DismissResult();
        }
    }
    ImGui::End();
}

// ---------- debug / tuning panel -----------------------------------------
void DrawDebugWindow() {
    if (!CVarGetInteger(HB_CVAR_DEBUG, 0)) return;

    if (!ImGui::Begin("Horse Betting (Debug)")) {
        ImGui::End();
        return;
    }

    auto& s = GetSession();

    ImGui::Text("State: %d   Frames in state: %d",
                (int)s.state, s.framesInState);
    if (gPlayState) ImGui::Text("Scene: 0x%X", gPlayState->sceneNum);

    if (gPlayState) {
        Player* p = GET_PLAYER(gPlayState);
        if (p) {
            ImGui::Text("Player: %.1f, %.1f, %.1f",
                p->actor.world.pos.x, p->actor.world.pos.y, p->actor.world.pos.z);
        }
    }

    ImGui::Separator();
    ImGui::Text("Waypoints: %d", (int)s.waypoints.size());
    if (ImGui::Button("Capture player pos as waypoint")) {
        if (gPlayState) {
            Player* p = GET_PLAYER(gPlayState);
            if (p) {
                s.waypoints.push_back({
                    p->actor.world.pos.x,
                    p->actor.world.pos.y,
                    p->actor.world.pos.z
                });
                ComputeArcLengths();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { s.waypoints.clear(); ComputeArcLengths(); }
    ImGui::SameLine();
    if (ImGui::Button("Save")) SaveWaypoints();
    ImGui::SameLine();
    if (ImGui::Button("Reset to Ingo route")) LoadIngoRoute();

    ImGui::BeginChild("wp_list", ImVec2(0, 140), true);
    for (size_t i = 0; i < s.waypoints.size(); ++i) {
        ImGui::Text("[%2zu] (%7.1f, %7.1f, %7.1f)",
            i, s.waypoints[i].x, s.waypoints[i].y, s.waypoints[i].z);
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Re-roll horses")) RegenerateHorses();
    ImGui::SameLine();
    if (ImGui::Button("Force start race")) StartRace();
    ImGui::SameLine();
    if (ImGui::Button("Force end race")) {
        if (s.state == HB_STATE_RACING) {
            for (auto& rt : s.runtimes) rt.lap = HB_NUM_LAPS;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to idle")) {
        s.state = HB_STATE_IDLE;
        s.framesInState = 0;
    }

    ImGui::End();
}

}  // namespace HorseBetting
