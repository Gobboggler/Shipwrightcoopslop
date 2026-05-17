// =========================================================================
//  HorseBetting.cpp
//  Top-level ShipInit, hook registration, NPC spawn, state machine driver.
// =========================================================================

#include "HorseBetting.h"

#include <cstring>
#include <cstdio>
#include <random>
#include <chrono>
#include <algorithm>
#include <cmath>

#include <libultraship/libultraship.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;

// Object_Spawn exists in soh/src/code/z_scene.c with external linkage, but
// its prototype is missing from soh/include/functions.h (only Object_GetIndex
// and Object_IsLoaded are declared there). Forward-declare it ourselves so
// we can preload OBJECT_OS_ANIME / OBJECT_BOJ / OBJECT_BBA on scene init.
s32 Object_Spawn(ObjectContext* objectCtx, s16 objectId);
}

namespace HorseBetting {

// ---------- session singleton --------------------------------------------
Session& GetSession() {
    static Session s;
    return s;
}

// ---------- horse name pool ----------------------------------------------
static const char* kHorseNames[] = {
    "Thunderhoof", "Moonshade",   "Epona's Cousin", "Sir Trots-a-Lot",
    "Lon Lon Lightning", "Goron Roll", "Saria's Steed", "Hyrule's Hope",
    "Stalfos Dancer", "Cuccoo Catcher", "Deku Drifter", "Bombchu",
    "Triforce Tilly", "Malon's Mishap", "Ingo's Regret", "Galloping Goron",
    "Silver Mane",   "Master Mare", "Hot to Trotter", "Field of Schemes",
};
constexpr int kNumNames = sizeof(kHorseNames) / sizeof(kHorseNames[0]);

// ---------- RNG ----------------------------------------------------------
static std::mt19937& RNG() {
    static std::mt19937 g{
        (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()
    };
    return g;
}
template <typename T>
static T RandRange(T lo, T hi) {
    if constexpr (std::is_integral_v<T>) {
        std::uniform_int_distribution<T> d(lo, hi);
        return d(RNG());
    } else {
        std::uniform_real_distribution<T> d(lo, hi);
        return d(RNG());
    }
}

// ---------- horse generation --------------------------------------------
void RegenerateHorses() {
    auto& s = GetSession();

    std::vector<int> nameIdx(kNumNames);
    for (int i = 0; i < kNumNames; ++i) nameIdx[i] = i;
    std::shuffle(nameIdx.begin(), nameIdx.end(), RNG());

    float strength[HB_NUM_HORSES];
    float total = 0.0f;
    for (int i = 0; i < HB_NUM_HORSES; ++i) {
        strength[i] = RandRange(0.5f, 1.5f);
        total += strength[i];
    }

    for (int i = 0; i < HB_NUM_HORSES; ++i) {
        HorseInfo& h = s.horses[i];
        std::strncpy(h.name, kHorseNames[nameIdx[i]], sizeof(h.name) - 1);
        h.name[sizeof(h.name) - 1] = '\0';

        h.colorR = (uint8_t)RandRange(40, 255);
        h.colorG = (uint8_t)RandRange(40, 255);
        h.colorB = (uint8_t)RandRange(40, 255);

        h.speedBase     = 4.0f + strength[i] * 2.0f;
        h.speedVariance = RandRange(0.3f, 1.2f);
        h.stamina       = RandRange(0.6f, 1.0f);

        float share = strength[i] / total;
        h.odds = (1.0f / share) * 0.85f;
        if (h.odds < 1.2f) h.odds = 1.2f;
        if (h.odds > 9.9f) h.odds = 9.9f;
    }

    for (auto& rt : s.runtimes) rt = HorseRuntime{};
    s.chosenHorse = -1;
    s.winnerIdx   = -1;
    s.betAmount   =  0;
}

// ---------- waypoint persistence ----------------------------------------
// Pre-compute cumulative arc lengths for the loaded waypoint loop so we
// can do arc-length parameterization in UpdateHorseAI. cumulativeArc[i]
// = total distance from waypoint 0 to waypoint i along the path; the
// closing segment (waypoints[N-1] → waypoints[0]) goes into arcPerLap.
//
// With this in place, distAlongPath becomes the SOLE source of truth for
// a horse's track progress: world position is derived by interpolating
// along the path at distAlongPath, so the race-tracker UI and the
// in-world positions are guaranteed consistent.
void ComputeArcLengths() {
    auto& s = GetSession();
    s.cumulativeArc.clear();
    s.arcPerLap = 0.0f;
    if (s.waypoints.size() < 2) return;

    s.cumulativeArc.reserve(s.waypoints.size() + 1);
    s.cumulativeArc.push_back(0.0f);
    for (size_t i = 1; i < s.waypoints.size(); ++i) {
        const HorseWaypoint& a = s.waypoints[i - 1];
        const HorseWaypoint& b = s.waypoints[i];
        float dx = b.x - a.x, dz = b.z - a.z;
        s.cumulativeArc.push_back(s.cumulativeArc.back()
                                  + std::sqrt(dx*dx + dz*dz));
    }
    // Add the closing segment from last waypoint back to first.
    const HorseWaypoint& last  = s.waypoints.back();
    const HorseWaypoint& first = s.waypoints.front();
    float dx = first.x - last.x, dz = first.z - last.z;
    s.arcPerLap = s.cumulativeArc.back() + std::sqrt(dx*dx + dz*dz);
}

void SaveWaypoints() {
    auto& s = GetSession();
    std::string out;
    char buf[64];
    for (auto& w : s.waypoints) {
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f,%.1f;", w.x, w.y, w.z);
        out += buf;
    }
    CVarSetString(HB_CVAR_WAYPOINTS, out.c_str());
    CVarSave();
}

// User-captured race track (Images 2 and 3 from the v13 testing session).
// These are the live coordinates captured by walking the track in-game with
// the debug overlay's "Capture player pos as waypoint" button. The route
// forms a clockwise loop around the central ranch area.
static const HorseWaypoint kIngoRouteWaypoints[] = {
    {  -207.5f, 0.0f, -1717.0f },
    {  1196.5f, 0.0f, -1703.1f },
    {  1862.8f, 0.0f,  -600.2f },
    {  1130.6f, 0.0f,   550.8f },
    {   -66.7f, 0.0f,   572.3f },
    { -1313.5f, 0.0f,   489.1f },
    { -1805.1f, 0.0f,  -905.7f },
    {  -977.3f, 0.0f, -1736.4f },
};
constexpr int kNumIngoWaypoints =
    sizeof(kIngoRouteWaypoints) / sizeof(kIngoRouteWaypoints[0]);

void LoadIngoRoute() {
    auto& s = GetSession();
    s.waypoints.clear();
    s.waypoints.reserve(kNumIngoWaypoints);
    for (int i = 0; i < kNumIngoWaypoints; ++i) {
        s.waypoints.push_back(kIngoRouteWaypoints[i]);
    }
    ComputeArcLengths();
}

void LoadWaypoints() {
    auto& s = GetSession();
    s.waypoints.clear();
    const char* raw = CVarGetString(HB_CVAR_WAYPOINTS, "");
    if (!raw || !*raw) {
        // No user-captured track saved — use Ingo's actual race route.
        LoadIngoRoute();
        return;
    }
    const char* p = raw;
    while (*p) {
        float x, y, z;
        int n = std::sscanf(p, "%f,%f,%f;", &x, &y, &z);
        if (n != 3) break;
        s.waypoints.push_back({x, y, z});
        const char* sc = std::strchr(p, ';');
        if (!sc) break;
        p = sc + 1;
    }
    ComputeArcLengths();
}

// Sheikah-style gossip stone replaces the EN_HY NPC. Reason: EN_HY's variant-
// specific dispatch (BOJ_5 / BBA / etc.) checks scene-level game flags to wire
// up its talk-handler function pointer. Our "fake" variants didn't satisfy
// those preconditions, so the post-dialog handler was a NULL pointer — pressing
// A near our NPC crashed the game with RIP=0x0 after the textbox closed.
//
// EN_GS (gossip stone) is structurally simpler — one uniform actor with no
// variant-specific code paths. Combined with a ShouldActorUpdate filter that
// suppresses its update entirely, the stone:
//   * never calls func_8002F1C4 to register itself as a talk target
//   * never sets player->talkActor (the engine offer pathway is dead)
//   * never starts a vanilla dialog
// Menu opening is driven by our own proximity + A-press detection in
// HorseBetting_OnFrame instead of riding the engine's talk system.
//
// We keep the upper-byte sentinel tag in params for "is this one of ours?"
// checks, but no longer rely on EN_HY's lower-byte variant ID. Lower byte
// is 0 (gossip stones don't use it for variant selection).
static constexpr int16_t kHbActorParam =
    (int16_t)0x0100;
static constexpr int16_t kHbActorParamMask = (int16_t)0xFF00;
static constexpr int16_t kHbActorParamTag  = (int16_t)0x0100;

// Stored on spawn so HorseBetting_OnFrame can compute proximity without
// walking the actor list every frame. Reset to NULL in OnSceneInit so it
// never references a freed actor across scenes.
Actor* gBookmakerStone = nullptr;

static void SpawnShopkeeperIfNeeded(PlayState* play) {
    if (!play) return;
    if (play->sceneNum != SCENE_LON_LON_RANCH) return;
    if (!LINK_IS_ADULT) return;

    // Don't double-spawn — if we already have a valid pointer and the actor
    // still lives in the scene, skip.
    if (gBookmakerStone) return;

    // Same ranch-entrance coords as the previous EN_HY bookmaker. Y-offset
    // slightly up because EN_GS draws from its origin (no foot anchor).
    const float spawnX = 1285.5f;
    const float spawnY =   30.0f;
    const float spawnZ = -2207.7f;
    const int16_t spawnYaw = 0x4000;

    Actor* a = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_GS,
                           spawnX, spawnY, spawnZ,
                           0, spawnYaw, 0, kHbActorParam);
    if (a) {
        gBookmakerStone = a;
        SPDLOG_INFO("[HorseBetting] Spawned bookmaker gossip stone at ({}, {}, {}). Actor ptr = {}",
                    spawnX, spawnY, spawnZ, (void*)a);
    } else {
        SPDLOG_WARN("[HorseBetting] Actor_Spawn returned NULL for bookmaker stone");
    }
}

// ---------- player-proximity menu trigger --------------------------------
// New approach: completely bypass the engine's talk system. We never let our
// stones offer themselves as talk targets (ShouldActorUpdate hook below
// suppresses their update entirely, so they can't call func_8002F1C4),
// which means player->talkActor is always NULL with respect to our stones
// and no vanilla dialog can ever be queued.
//
// Instead, each frame:
//   1. Compute distance from every player to each stone
//   2. Find the (player, stone) pair with the smallest distance under
//      a 120-unit threshold
//   3. Check whether that player's A button was pressed this frame
//   4. If yes and game state allows, open the corresponding menu
//
// Edge-debounce via sBettingPreviouslyTriggered prevents a held A from
// retriggering the menu every frame.
static bool sBettingPreviouslyTriggered = false;

template <typename Fn>
static void ForEachPlayer(PlayState* play, Fn fn) {
    if (!play) return;
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
         a != NULL; a = a->next) {
        fn((Player*)a);
    }
}

static void CheckTalkToShopkeeper(PlayState* play) {
    if (!play) return;
    if (!gBookmakerStone && !SlotMachine::gSlotDealerStone) return;

    // Find the closest (player, stone) pair under the proximity threshold.
    Player* closestPlayer = nullptr;
    Actor*  closestStone  = nullptr;
    float   closestDist   = 120.0f;   // proximity threshold (sq below)
    float   closestDistSq = closestDist * closestDist;

    auto considerStone = [&](Player* p, Actor* stone) {
        if (!stone) return;
        float dx = p->actor.world.pos.x - stone->world.pos.x;
        float dy = p->actor.world.pos.y - stone->world.pos.y;
        float dz = p->actor.world.pos.z - stone->world.pos.z;
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < closestDistSq) {
            closestDistSq = d2;
            closestPlayer = p;
            closestStone  = stone;
        }
    };
    ForEachPlayer(play, [&](Player* p) {
        considerStone(p, gBookmakerStone);
        considerStone(p, SlotMachine::gSlotDealerStone);
    });

    if (!closestPlayer) {
        sBettingPreviouslyTriggered = false;
        return;
    }

    // Player frozen in race cutscene state — they shouldn't trigger menus.
    if (closestPlayer->stateFlags1 & PLAYER_STATE1_IN_CUTSCENE) {
        sBettingPreviouslyTriggered = false;
        return;
    }

    // Read A-press from the closest player's input slot. PLAYER_GET_INDEX
    // returns 0 for P1, 1 for P2, etc. — guarded for the >=4 case in
    // case coop ever supports a fifth controller and the input array
    // doesn't extend that far.
    int playerIdx = PLAYER_GET_INDEX(&closestPlayer->actor);
    if (playerIdx < 0 || playerIdx >= 4) playerIdx = 0;
    bool aPressed = (play->state.input[playerIdx].press.button & BTN_A) != 0;

    bool isBookmaker = (closestStone == gBookmakerStone);
    bool bookmakerIdle = (GetSession().state == HB_STATE_IDLE);
    bool slotClosed    = (SlotMachine_GetState() == SLOT_STATE_CLOSED);
    if (aPressed && !sBettingPreviouslyTriggered) {
        bool canOpen =
            ( isBookmaker && bookmakerIdle) ||
            (!isBookmaker && slotClosed && bookmakerIdle);
        if (!canOpen) return;
        sBettingPreviouslyTriggered = true;
        if (isBookmaker) {
            HorseBetting_OpenBettingUI();
        } else {
            SlotMachine_Open();
        }
    } else if (!aPressed) {
        sBettingPreviouslyTriggered = false;
    }
}

// Freeze every player in cutscene state for the duration of the race. Used
// during PREPARING, COUNTDOWN, RACING, FINISHED so neither P1 nor P2 can
// wander off-camera or steal input.
void LockAllPlayersForRace(PlayState* play) {
    ForEachPlayer(play, [](Player* p) {
        p->stateFlags1 |= PLAYER_STATE1_IN_CUTSCENE;
    });
}

// Defensive cleanup when the race is done. The natural scene transition back
// to Talon's house usually clears this, but coop's auto-resync logic doesn't
// always rebuild state flags, so we explicitly drop the flag.
void UnlockAllPlayersAfterRace(PlayState* play) {
    ForEachPlayer(play, [](Player* p) {
        p->stateFlags1 &= ~PLAYER_STATE1_IN_CUTSCENE;
    });
}

// ---------- top-level hook glue -----------------------------------------
static bool sHooksRegistered = false;

static void RegisterHooks() {
    if (sHooksRegistered) return;
    sHooksRegistered = true;

    auto* gi = GameInteractor::Instance;

    gi->RegisterGameHook<GameInteractor::OnSceneInit>(
        [](int16_t sceneNum) { HorseBetting_OnSceneInit(sceneNum); });

    gi->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        []() { HorseBetting_OnFrame(); });

    // ----- Sheikah stone freeze: never let our stones update -----------
    // Our gambling NPCs are now EN_GS gossip stones. Their vanilla update
    // function calls func_8002F1C4 to register the actor as a talk target
    // when the player is in range — which sets player->talkActor to the
    // stone and queues a vanilla "Listen" dialog when A is pressed.
    //
    // We suppress EN_GS's update entirely for our specific stones (matched
    // by stored pointer + sentinel tag). With the update skipped:
    //   * func_8002F1C4 is never called → player->talkActor stays NULL
    //     with respect to our stones → vanilla never opens a dialog
    //   * the stone is still DRAWN every frame (draw is independent of
    //     update), so visually it's a normal Sheikah stone
    //   * collision is set up once in Init and persists — player still
    //     bumps into it like a solid stone
    //
    // Menu opening is driven by proximity + raw A-press detection in
    // HorseBetting_OnFrame, not by any vanilla talk path.
    gi->RegisterGameHookForID<GameInteractor::ShouldActorUpdate>(
        ACTOR_EN_GS,
        [](void* actorPtr, bool* shouldUpdate) {
            Actor* a = (Actor*)actorPtr;
            const bool isOurs =
                (a == gBookmakerStone) ||
                (a == SlotMachine::gSlotDealerStone) ||
                ((a->params & kHbActorParamMask) == kHbActorParamTag) ||
                ((a->params & 0xFF00) == 0x0200);   // slot dealer tag inlined
            if (!isOurs) return;
            // Defensive: also clear textId so even if the update did slip
            // through (e.g., during the one-frame window before our hook
            // is registered), the actor has no text to display.
            a->textId = 0;
            *shouldUpdate = false;
        });
}

}  // namespace HorseBetting

// ---------- C entry points -----------------------------------------------
extern "C" {

void HorseBetting_Init(void) {
    CVarRegisterInteger(HB_CVAR_ENABLED, 1);
    CVarRegisterInteger(HB_CVAR_DEBUG, 0);
    CVarRegisterString(HB_CVAR_WAYPOINTS, "");
    HorseBetting::LoadWaypoints();
    HorseBetting::RegenerateHorses();
    HorseBetting::RegisterHooks();
    SlotMachine_Init();
}

void HorseBetting_OnSceneInit(int16_t sceneNum) {
    auto& s = HorseBetting::GetSession();
    s.audienceSpawned = false;

    // Scene transition: clear stored Actor* pointers to our spawned stones.
    // The actors themselves are destroyed by the engine across scene change;
    // holding stale pointers would cause use-after-free when the proximity
    // detector tries to read their world.pos. The next scene tick will
    // spawn fresh stones (if this is the ranch and player is adult) and
    // re-store new valid pointers.
    HorseBetting::gBookmakerStone = nullptr;
    SlotMachine::gSlotDealerStone = nullptr;

    if (sceneNum == SCENE_LON_LON_RANCH) {
        SPDLOG_INFO("[HorseBetting] OnSceneInit fired for Lon Lon Ranch (0x{:X})", sceneNum);
        // Preload OBJECT_GS (gossip stone object). Without it, EnGs_Init
        // would call Actor_Kill on itself when the object bank lookup fails
        // and our stones vanish silently. Idempotent — duplicate calls are
        // skipped via Object_GetIndex check.
        if (gPlayState) {
            auto preload = [](int16_t objId, const char* tag) {
                if (Object_GetIndex(&gPlayState->objectCtx, objId) < 0) {
                    s32 idx = Object_Spawn(&gPlayState->objectCtx, objId);
                    SPDLOG_INFO("[HorseBetting] Preloaded {} (objId=0x{:X}, slot={})", tag, objId, idx);
                } else {
                    SPDLOG_INFO("[HorseBetting] {} already loaded (objId=0x{:X})", tag, objId);
                }
            };
            preload(OBJECT_GS, "GS (gossip stones)");
        } else {
            SPDLOG_WARN("[HorseBetting] OnSceneInit: gPlayState is NULL, can't preload");
        }
        // Stone spawn is no longer attempted here. We retry every frame in
        // HorseBetting_OnFrame so timing issues (object DMA still loading,
        // bank not ready, etc.) can recover instead of being a one-shot fail.
    }
}

void HorseBetting_OnFrame(void) {
    auto& s = HorseBetting::GetSession();
    s.framesInState++;

    // Retry NPC spawn every ~30 frames (0.5s) while in the ranch as adult.
    // SpawnShopkeeperIfNeeded short-circuits if our NPC already exists, so
    // this is cheap and self-healing — if a spawn ever dies, next 0.5s
    // we try again. Diagnostics on every successful spawn.
    if (gPlayState && gPlayState->sceneNum == SCENE_LON_LON_RANCH && LINK_IS_ADULT
        && (s.framesInState % 30) == 0) {
        HorseBetting::SpawnShopkeeperIfNeeded(gPlayState);
        SlotMachine_SpawnDealerIfNeeded(gPlayState);
    }

    // Per-frame dialog check whenever idle and at the ranch.
    if (s.state == HB_STATE_IDLE && gPlayState &&
        gPlayState->sceneNum == SCENE_LON_LON_RANCH) {
        HorseBetting::CheckTalkToShopkeeper(gPlayState);
    }

    switch (s.state) {
        case HB_STATE_IDLE:
        case HB_STATE_BETTING:
            break;
        case HB_STATE_PREPARING:
        case HB_STATE_COUNTDOWN:
        case HB_STATE_RACING:
        case HB_STATE_FINISHED:
            HorseBetting::TickRace();
            break;
        case HB_STATE_RETURNING:
            s.state = HB_STATE_IDLE;
            s.framesInState = 0;
            break;
        case HB_STATE_PAYOUT:
            break;
    }

    SlotMachine_OnFrame();

    // NOTE: UI drawing has been moved to HorseBetting::MasterUIWindow.
    // OnGameFrameUpdate fires during game logic, BEFORE ImGui::NewFrame, so
    // calling ImGui::Begin from here is a silent no-op. Real ImGui draws
    // happen via the Ship::GuiWindow dispatch path during the GUI render pass.
}

bool HorseBetting_IsRaceActive(void) {
    auto st = HorseBetting::GetSession().state;
    return st == HB_STATE_PREPARING ||
           st == HB_STATE_COUNTDOWN ||
           st == HB_STATE_RACING    ||
           st == HB_STATE_FINISHED  ||
           st == HB_STATE_RETURNING;
}

HorseBettingState HorseBetting_GetState(void) {
    return HorseBetting::GetSession().state;
}

void HorseBetting_OpenBettingUI(void) {
    auto& s = HorseBetting::GetSession();
    if (s.state != HB_STATE_IDLE) return;
    s.state = HB_STATE_BETTING;
    s.framesInState = 0;
}

void HorseBetting_CancelBetting(void) {
    auto& s = HorseBetting::GetSession();
    if (s.state != HB_STATE_BETTING) return;
    s.state = HB_STATE_IDLE;
    s.framesInState = 0;
}

void HorseBetting_DismissResult(void) {
    auto& s = HorseBetting::GetSession();
    if (s.state != HB_STATE_PAYOUT) return;
    s.state = HB_STATE_IDLE;
    s.framesInState = 0;
    HorseBetting::RegenerateHorses();
}

void HorseBetting_BeginRace(int horseIdx, int betAmount) {
    auto& s = HorseBetting::GetSession();
    if (s.state != HB_STATE_BETTING) return;

    if (gSaveContext.rupees < betAmount) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }
    Rupees_ChangeBy(-betAmount);

    s.chosenHorse = horseIdx;
    s.betAmount   = betAmount;
    HorseBetting::StartRace();
}

}  // extern "C"

// ---------- GUI window registration --------------------------------------
// SoH's ImGui frame is created during the render pass, not during the game
// logic pass. The canonical SoH pattern is to subclass Ship::GuiWindow and
// register the instance with the GUI manager — its Draw() method then runs
// inside an active ImGui frame.
//
// We override Draw() (instead of DrawElement()) so we can render *multiple*
// independent ImGui windows from one registered instance, rather than being
// wrapped in a single ImGui::Begin/End by GuiWindow's default Draw().

#include <memory>
#include <libultraship/libultraship.h>

namespace HorseBetting {

class MasterUIWindow : public Ship::GuiWindow {
public:
    MasterUIWindow()
        : Ship::GuiWindow("", "HorseBetting Master UI") {
        Show();   // always-on; individual windows self-gate by state/CVar
    }

    // Override Draw() to skip GuiWindow's auto-wrap and dispatch to all our
    // conditional draw helpers. Each helper does its own ImGui::Begin/End
    // and returns early when its state doesn't warrant rendering.
    void Draw() override {
        DrawBettingWindow();
        DrawCountdownOverlay();
        DrawRaceVisualization();
        DrawResultOverlay();
        DrawPayoutOverlay();
        DrawDebugWindow();
        SlotMachine::DrawWindow();
    }

    // Pure-virtual stubs from GuiElement — we don't use them because we
    // override Draw() directly.
    void DrawElement() override {}
    void InitElement() override {}
    void UpdateElement() override {}
};

}  // namespace HorseBetting

static std::shared_ptr<HorseBetting::MasterUIWindow> sMasterUIWindow;

static void RegisterHorseBettingUI() {
    if (sMasterUIWindow) return;   // idempotent
    sMasterUIWindow = std::make_shared<HorseBetting::MasterUIWindow>();
    auto ctx = Ship::Context::GetInstance();
    if (!ctx) return;
    auto window = ctx->GetWindow();
    if (!window) return;
    auto gui = window->GetGui();
    if (!gui) return;
    gui->AddGuiWindow(sMasterUIWindow);
}

// ---------- ShipInit registration ----------------------------------------
static void RegisterHorseBettingEnhancement() {
    if (!CVarGetInteger(HB_CVAR_ENABLED, 1)) return;
    HorseBetting_Init();
    RegisterHorseBettingUI();
}
static RegisterShipInitFunc initHorseBetting(
    RegisterHorseBettingEnhancement,
    { HB_CVAR_ENABLED });
