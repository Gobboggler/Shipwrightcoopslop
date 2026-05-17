#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// ----- TUNING CONSTANTS ---------------------------------------------------
// Most of these have a sensible default but are exposed in the debug UI
// (Cheats -> Horse Betting Debug) for in-game adjustment without a rebuild.
// Track waypoints in particular WILL need to be captured in your build.

#define HB_NUM_HORSES         6
#define HB_NUM_LAPS           2
#define HB_MAX_BET          500
#define HB_NUM_AUDIENCE      10

// CVar names (Ship's persistence layer)
#define HB_CVAR_ENABLED       "gEnhancements.HorseBetting.Enabled"
#define HB_CVAR_DEBUG         "gEnhancements.HorseBetting.Debug"
#define HB_CVAR_WAYPOINTS     "gEnhancements.HorseBetting.Waypoints"

// ----- TYPES --------------------------------------------------------------

typedef enum {
    HB_STATE_IDLE,        // No race active. NPC is in Talon's house.
    HB_STATE_BETTING,     // ImGui modal showing horses + odds.
    HB_STATE_PREPARING,   // Scene transition to ranch + horse/audience spawn.
    HB_STATE_COUNTDOWN,   // 3..2..1.. overlay before "go".
    HB_STATE_RACING,      // Horses running, camera follows leader.
    HB_STATE_FINISHED,    // Race ended, result overlay, music fanfare.
    HB_STATE_RETURNING,   // Deprecated; unused since NPCs moved to ranch.
    HB_STATE_PAYOUT,      // Result modal at NPC.
} HorseBettingState;

typedef struct {
    char     name[24];
    uint8_t  colorR, colorG, colorB;
    float    speedBase;        // average units/frame
    float    speedVariance;    // +/- jitter
    float    stamina;          // 0..1, fades speed late race
    float    odds;             // multiplier on bet
} HorseInfo;

typedef struct {
    float    x, y, z;
} HorseWaypoint;

typedef struct {
    int      waypointIdx;
    int      lap;
    float    distAlongPath;    // monotonic, used for placement
    float    curSpeed;
    float    x, y, z;
    int16_t  yaw;
    void*    actorPtr;         // Actor*; set when spawned
    bool     finished;
    int      finalPlacement;   // 1..N
} HorseRuntime;

// ----- C-CALLABLE BRIDGE --------------------------------------------------
void HorseBetting_Init(void);
void HorseBetting_OnSceneInit(int16_t sceneNum);
void HorseBetting_OnFrame(void);
bool HorseBetting_IsRaceActive(void);
HorseBettingState HorseBetting_GetState(void);
void HorseBetting_BeginRace(int horseIdx, int betAmount);
void HorseBetting_OpenBettingUI(void);
void HorseBetting_CancelBetting(void);
void HorseBetting_DismissResult(void);

// ----- SLOT MACHINE ("Rich Little Cuccos") -------------------------------
// Lives in the same Talon's house scene as the bookmaker. Spawns its own
// dealer NPC (a second EN_HY with a different upper-byte tag) across the
// room from the horse-betting dealer.

#define HB_SLOT_NUM_REELS       3
#define HB_SLOT_NUM_SYMBOLS     6
#define HB_SLOT_MAX_BET       100
#define HB_SLOT_SPIN_FRAMES   140   // total spin animation duration

typedef enum {
    SLOT_STATE_CLOSED,    // dealer not engaged
    SLOT_STATE_IDLE,      // modal open, awaiting bet
    SLOT_STATE_SPINNING,  // reels animating
    SLOT_STATE_RESULT,    // showing payout for ~90 frames
} SlotMachineState;

void SlotMachine_Init(void);
void SlotMachine_SpawnDealerIfNeeded(struct PlayState* play);
void SlotMachine_OnFrame(void);
void SlotMachine_Open(void);
void SlotMachine_Close(void);
void SlotMachine_Spin(int betAmount);
SlotMachineState SlotMachine_GetState(void);
bool SlotMachine_TalkActorIsDealer(void* actor);

#ifdef __cplusplus
}

#include <array>
#include <vector>
#include <string>

// Forward declare the engine's PlayState (defined in z64.h). The lock/unlock
// helpers take it by pointer so we don't need to drag z64.h into this header.
struct PlayState;
// Same for Actor — used by SlotMachine::gSlotDealerStone below.
struct Actor;

namespace HorseBetting {

struct Session {
    HorseBettingState     state = HB_STATE_IDLE;
    std::array<HorseInfo, HB_NUM_HORSES>    horses{};
    std::array<HorseRuntime, HB_NUM_HORSES> runtimes{};
    std::vector<HorseWaypoint>              waypoints{};

    // Arc-length cache. cumulativeArc[i] = distance traveled from waypoint
    // 0 to waypoint i (inclusive). cumulativeArc[N] = total lap length.
    // Recomputed by ComputeArcLengths() whenever `waypoints` changes.
    std::vector<float>    cumulativeArc{};
    float                 arcPerLap = 0.0f;

    int       chosenHorse     = -1;
    int       betAmount       =  0;
    int       framesInState   =  0;
    int       countdown       =  3;
    int       winnerIdx       = -1;
    bool      audienceSpawned = false;
};

Session& GetSession();

void RegenerateHorses();
void LoadWaypoints();
void LoadIngoRoute();
void SaveWaypoints();
void ComputeArcLengths();   // (re)populate Session::cumulativeArc + arcPerLap
void DrawDebugWindow();
void DrawBettingWindow();
void DrawCountdownOverlay();
void DrawRaceVisualization();
void DrawResultOverlay();
void DrawPayoutOverlay();
void TickRace();
void StartRace();
void EndRace();
void ReturnToTalon();

// Coop-aware helpers — walk all ACTOR_PLAYER instances. Safe in single-player.
void LockAllPlayersForRace(PlayState* play);
void UnlockAllPlayersAfterRace(PlayState* play);

}  // namespace HorseBetting

namespace SlotMachine {

// Symbol IDs. Indexed into name/payout tables in SlotMachine.cpp.
enum Symbol : int {
    SYM_BOMB    = 0,   // common
    SYM_BOTTLE  = 1,
    SYM_HEART   = 2,
    SYM_RUPEE   = 3,
    SYM_CUCCO   = 4,
    SYM_TRIFORCE= 5,   // rare jackpot
    SYM_COUNT   = 6,
};

struct Session {
    SlotMachineState state = SLOT_STATE_CLOSED;
    int   betAmount     = 10;
    int   framesInState =  0;
    int   reelStop[HB_SLOT_NUM_REELS] = {0, 0, 0};   // final symbol per reel
    int   reelLanded[HB_SLOT_NUM_REELS] = {0, 0, 0}; // 1 once that reel stops
    int   currentDisplay[HB_SLOT_NUM_REELS] = {0, 0, 0};  // visible symbol now
    int   lastPayout    =  0;     // rupees, 0 = lost
    int   lastWinMultiplier = 0;  // 0 if loss, else payout multiplier
};

Session& GetSession();

void DrawWindow();   // ImGui modal

// Stored at Spawn() so HorseBetting's proximity detector can compute the
// distance to the slot-dealer stone without walking the actor list every
// frame. Reset to nullptr in HorseBetting_OnSceneInit on scene change.
extern Actor* gSlotDealerStone;

}  // namespace SlotMachine

#endif  // __cplusplus
