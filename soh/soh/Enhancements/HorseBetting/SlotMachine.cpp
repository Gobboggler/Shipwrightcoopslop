// =========================================================================
//  SlotMachine.cpp
//  "Rich Little Cuccos" slot machine. Spawns a dealer NPC inside Talon's
//  house (adult only), opens an ImGui modal with 3 spinning reels and a
//  classic lever-pull experience.
//
//  Same architectural pattern as the horse betting: NPC is a physical
//  anchor in the world, all the gambling experience lives in ImGui.
// =========================================================================

#include "HorseBetting.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
}

namespace SlotMachine {

// ---------- constants ---------------------------------------------------
// Slot dealer NPC marker — second EN_HY in Talon's house. Variant BBA (old
// woman) → her action func is EnHy_Fidget, which idles in place without
// wandering. The upper byte (0x0200) is our sentinel tag; the lower 7 bits
// are the actual NPC variant. The bookmaker uses 0x0100 (BOJ_5).
//
// Slot dealer is a second EN_GS gossip stone. Same rationale as the
// bookmaker — see commentary in HorseBetting.cpp. Different sentinel tag in
// the upper byte so we can route A-press detection to the right menu.
constexpr int16_t kSlotActorParam     = (int16_t)0x0200;
constexpr int16_t kSlotActorParamMask = (int16_t)0xFF00;
constexpr int16_t kSlotActorParamTag  = (int16_t)0x0200;

// Stored at spawn so the proximity detector in HorseBetting_OnFrame can
// check distance without walking the actor list each frame. Reset to NULL
// in OnSceneInit so we never deref a freed actor across scenes.
Actor* gSlotDealerStone = nullptr;

// Symbol display names. Three-letter codes render reliably with ImGui's
// default font (no emoji dependency).
static const char* kSymbolNames[SYM_COUNT] = {
    "BMB",  // bomb
    "BTL",  // bottle
    "HRT",  // heart
    "RUP",  // rupee
    "CUC",  // cucco
    "TRI",  // triforce
};

// Per-symbol RGB tint for the reel display (gives visual variety).
static const float kSymbolColors[SYM_COUNT][3] = {
    {0.55f, 0.55f, 0.60f},   // bomb (dark grey)
    {0.30f, 0.85f, 0.45f},   // bottle (green tinted glass)
    {0.95f, 0.30f, 0.40f},   // heart (red-pink)
    {0.40f, 0.80f, 0.95f},   // rupee (cyan)
    {0.95f, 0.90f, 0.40f},   // cucco (yellow)
    {1.00f, 0.85f, 0.10f},   // triforce (gold)
};

// Payout multipliers for 3-of-a-kind. Calibrated so the expected value
// per spin is roughly 0.86x (14% house edge — fun but loss-leaning).
//
// EV math (uniform symbols, P(3-same of X) = 1/216 each):
//   (100 + 30 + 20 + 15 + 12 + 8) / 216 = 185 / 216 = 0.856x
static const int kThreeOfAKindPayoutMul[SYM_COUNT] = {
    /* BMB  */   8,   // smallest
    /* BTL  */  12,
    /* HRT  */  15,
    /* RUP  */  20,
    /* CUC  */  30,   // themed mid-prize
    /* TRI  */ 100,   // jackpot
};

// ---------- session singleton -------------------------------------------
Session& GetSession() {
    static Session s;
    return s;
}

// ---------- RNG ---------------------------------------------------------
static std::mt19937& RNG() {
    static std::mt19937 g{
        (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()
        ^ 0xC0FFEE
    };
    return g;
}

static int RandSymbol() {
    std::uniform_int_distribution<int> d(0, SYM_COUNT - 1);
    return d(RNG());
}

// ---------- dealer NPC spawn --------------------------------------------
// Placed next to the horse bookmaker (at -350, 0, 1200), in the ranch
// exterior near Ingo. Same actor type (EN_HY) as the bookmaker with a
// different upper-byte tag so talk-detection routes correctly.
void Spawn(PlayState* play) {
    if (!play) return;
    if (play->sceneNum != SCENE_LON_LON_RANCH) return;
    if (!LINK_IS_ADULT) return;

    // Don't double-spawn.
    if (gSlotDealerStone) return;

    const float spawnX = 1385.5f;
    const float spawnY =   30.0f;
    const float spawnZ = -2207.7f;
    const int16_t spawnYaw = 0x4000;

    Actor* a = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_GS,
                           spawnX, spawnY, spawnZ,
                           0, spawnYaw, 0, kSlotActorParam);
    if (a) {
        gSlotDealerStone = a;
        SPDLOG_INFO("[HorseBetting] Spawned slot dealer gossip stone at ({}, {}, {}). Actor ptr = {}",
                    spawnX, spawnY, spawnZ, (void*)a);
    } else {
        SPDLOG_WARN("[HorseBetting] Actor_Spawn returned NULL for slot dealer stone");
    }
}

// Compat shim — HorseBetting.cpp still calls this from its talk detection.
// With the new proximity-based flow we don't actually need the linked-list
// match anymore, but the helper is harmless and keeps the cross-file
// surface stable.
bool IsDealerActor(Actor* a) {
    return a != nullptr && a == gSlotDealerStone;
}

// ---------- audio helpers -----------------------------------------------
static void PlayLeverPull() {
    // Mechanical heavy lever pull. Used when player clicks Spin.
    Sfx_PlaySfxCentered(NA_SE_EV_METALDOOR_STOP);
}
static void PlayReelStop() {
    // Sharp click when a reel locks. One per reel.
    Sfx_PlaySfxCentered(NA_SE_SY_DECIDE);
}
static void PlayWinSmall() {
    Sfx_PlaySfxCentered(NA_SE_SY_GET_RUPY);
}
static void PlayWinBig() {
    Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
}
static void PlayLoss() {
    Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
}

// ---------- payout calculation ------------------------------------------
static int CalculatePayout(int r1, int r2, int r3, int bet, int* outMul) {
    *outMul = 0;
    if (r1 == r2 && r2 == r3) {
        *outMul = kThreeOfAKindPayoutMul[r1];
        return bet * (*outMul);
    }
    return 0;
}

// ---------- per-frame tick ----------------------------------------------
// Implements the staggered-reel-stop animation that gives slot machines
// their suspense. Reels lock at frames 60, 90, and 120 of a 140-frame spin.
void Tick() {
    auto& s = GetSession();
    s.framesInState++;

    switch (s.state) {
        case SLOT_STATE_CLOSED:
        case SLOT_STATE_IDLE:
            break;

        case SLOT_STATE_SPINNING: {
            // Reel landing schedule (in frames since spin began).
            static const int kReelLandFrame[HB_SLOT_NUM_REELS] = { 60, 90, 120 };

            // Each not-yet-landed reel cycles symbols every ~4 frames.
            for (int r = 0; r < HB_SLOT_NUM_REELS; ++r) {
                if (s.reelLanded[r]) continue;

                if (s.framesInState >= kReelLandFrame[r]) {
                    // Lock to predetermined final symbol.
                    s.currentDisplay[r] = s.reelStop[r];
                    s.reelLanded[r] = 1;
                    PlayReelStop();
                } else if ((s.framesInState % 4) == 0) {
                    s.currentDisplay[r] = RandSymbol();
                }
            }

            // All three landed → evaluate, transition to RESULT.
            if (s.reelLanded[0] && s.reelLanded[1] && s.reelLanded[2]) {
                int mul = 0;
                int payout = CalculatePayout(s.reelStop[0], s.reelStop[1],
                                             s.reelStop[2], s.betAmount, &mul);
                s.lastPayout = payout;
                s.lastWinMultiplier = mul;

                if (payout > 0) {
                    Rupees_ChangeBy(payout);
                    if (mul >= 50) PlayWinBig();
                    else           PlayWinSmall();
                } else {
                    PlayLoss();
                }

                s.state = SLOT_STATE_RESULT;
                s.framesInState = 0;
            }
            break;
        }

        case SLOT_STATE_RESULT:
            // Auto-return to idle after a beat so player can spin again.
            if (s.framesInState > 90) {
                s.state = SLOT_STATE_IDLE;
                s.framesInState = 0;
            }
            break;
    }
}

}  // namespace SlotMachine

// ---------- C entry points ----------------------------------------------
extern "C" {

void SlotMachine_Init(void) {
    // No persistent state to load; everything regenerates per session.
    // Hook registration happens via HorseBetting's existing hooks — we
    // just expose entry points it can call.
}

void SlotMachine_SpawnDealerIfNeeded(PlayState* play) {
    SlotMachine::Spawn(play);
}

void SlotMachine_OnFrame(void) {
    SlotMachine::Tick();
}

void SlotMachine_Open(void) {
    auto& s = SlotMachine::GetSession();
    if (s.state != SLOT_STATE_CLOSED && s.state != SLOT_STATE_IDLE) return;
    s.state = SLOT_STATE_IDLE;
    s.framesInState = 0;
}

void SlotMachine_Close(void) {
    auto& s = SlotMachine::GetSession();
    if (s.state == SLOT_STATE_SPINNING) return;  // can't bail mid-spin
    s.state = SLOT_STATE_CLOSED;
    s.framesInState = 0;
}

void SlotMachine_Spin(int betAmount) {
    auto& s = SlotMachine::GetSession();
    if (s.state != SLOT_STATE_IDLE) return;

    if (betAmount < 1) betAmount = 1;
    if (betAmount > HB_SLOT_MAX_BET) betAmount = HB_SLOT_MAX_BET;
    if (gSaveContext.rupees < betAmount) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }

    Rupees_ChangeBy(-betAmount);
    s.betAmount = betAmount;

    // Predetermine the final symbols at spin start; visual animation just
    // hides them until the landing frame. This is how real slot machines
    // work too — the result is locked the moment you pull the lever.
    for (int r = 0; r < HB_SLOT_NUM_REELS; ++r) {
        s.reelStop[r] = SlotMachine::RandSymbol();
        s.reelLanded[r] = 0;
        s.currentDisplay[r] = SlotMachine::RandSymbol();
    }
    s.lastPayout = 0;
    s.lastWinMultiplier = 0;
    s.state = SLOT_STATE_SPINNING;
    s.framesInState = 0;
    SlotMachine::PlayLeverPull();
}

SlotMachineState SlotMachine_GetState(void) {
    return SlotMachine::GetSession().state;
}

bool SlotMachine_TalkActorIsDealer(void* actor) {
    return SlotMachine::IsDealerActor((Actor*)actor);
}

}  // extern "C"
