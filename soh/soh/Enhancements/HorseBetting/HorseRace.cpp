// =========================================================================
//  HorseRace.cpp
//  Race logic: spawn horses, drive them along waypoints, follow leader with
//  the camera, play start cue + race BGM + win fanfare, spawn audience.
// =========================================================================

#include "HorseBetting.h"

#include <cmath>
#include <algorithm>

#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
}

// EN_HORSE internals we need to drive the gallop animation. EnHorse is just
// an Actor with extra fields appended — declared in
// soh/src/overlays/actors/ovl_En_Horse/z_en_horse.h. We don't want to
// include the full header here (it's overlay-private and would pull
// libultraship templates inside our extern "C" linkage above), so we
// forward-declare the function and define a minimal layout match for the
// fields we touch.
//
// `action` lives at offset 0x14C in EnHorse; the `EnHorseAction` enum has
// ENHORSE_ACT_MOUNTED_GALLOP = 10. We don't read the value back, only
// write, so we don't need the full enum — just a constant.
extern "C" {
    // EnHorse_StartGalloping has external linkage in z_en_horse.c (no
    // static keyword on the definition). Calling it once after spawn
    // puts the horse in ENHORSE_ACT_MOUNTED_GALLOP, sets animationIdx
    // = ENHORSE_ANIM_GALLOP, and Animation_Changes the skelAnime to
    // the gallop animation. The MountedGallop action handler then
    // maintains the gallop loop as long as speedXZ stays >= 6.0.
    void EnHorse_StartGalloping(void* this_);
}
// Offset of EnHorseAction enum in EnHorse struct. Verified against
// soh/src/overlays/actors/ovl_En_Horse/z_en_horse.h:96 (/* 0x014C */).
// Each frame we re-write action back to MOUNTED_GALLOP in case the
// MountedGallop handler transitioned us to trotting / stopping due to
// momentary speedXZ dip from collision checks.
static constexpr size_t kEnHorseActionOffset = 0x14C;
static constexpr int kEnHorseActMountedGallop = 10;
static inline void ForceHorseGallopAction(void* horseActor) {
    auto* p = reinterpret_cast<int*>(
        reinterpret_cast<char*>(horseActor) + kEnHorseActionOffset);
    *p = kEnHorseActMountedGallop;
}

namespace HorseBetting {

// Portable Pi. M_PI isn't standard C++ and MSVC doesn't expose it through
// <cmath> by default (only when _USE_MATH_DEFINES is set BEFORE the include),
// which trips up the Windows CI build even though GCC/Clang on Linux/macOS
// pass silently. Hardcoded constant sidesteps the whole mess.
constexpr float kPi = 3.14159265358979323846f;

// ---------- spawn horses --------------------------------------------------
// ACTOR_EN_HORSE with params = 1 is the regular wild/roaming horse — same
// kind that walks around inside the Lon Lon stables. Importantly:
//   - No Ingo riding on the back (that's only params=3, "Ingo's race horse")
//   - Uses OBJECT_HORSE (already loaded in the ranch by vanilla rideable
//     horses), so no extra Object_Spawn is needed.
//   - ENHORSE_FLAG_7 is set (wandering AI). We override world.pos every
//     frame, so the AI's idea of where the horse is gets clobbered each
//     tick — but the animation system (driven by speedXZ which we also
//     set) plays correctly.
//
// Per-horse color: applied via Actor_SetColorFilter to RED/BLUE/WHITE tints
// so the player can visually distinguish horses on the track. Default
// brown-Epona for any horse whose HorseInfo color is closest to vanilla.
static constexpr int16_t kHbRaceHorseParams = 0x0001;

// Map a horse's HorseInfo RGB into Actor_SetColorFilter's three available
// tint families. From z_actor.c:2782, the encoded colors are:
//   colorFlag & 0x8000  -> white (R = G = B)
//   colorFlag & 0x4000  -> red   (R only)
//   neither             -> blue  (B only)
// Returns (colorFlag, intensity 0-255). Intensity 0 means "leave horse
// untouched" — default brown Epona. Brown horses are still distinct from
// the tinted ones, so we use that for ~one horse per race.
static void PickHorseTint(const HorseInfo& h, int16_t* flag, uint8_t* intensity) {
    int r = h.colorR, g = h.colorG, b = h.colorB;
    int maxc = std::max({r, g, b});
    int minc = std::min({r, g, b});
    int range = maxc - minc;

    // Near-gray and bright: white tint.
    if (range < 30 && maxc > 180) {
        *flag = (int16_t)0x8000;  // white
        *intensity = 200;
        return;
    }
    // Near-gray and dim, or near brown: no tint (default brown Epona).
    if (range < 30) {
        *flag = 0; *intensity = 0;
        return;
    }
    // Red-dominant.
    if (r > g + 20 && r > b + 20) {
        *flag = (int16_t)0x4000;
        *intensity = (uint8_t)std::min(255, 120 + range);
        return;
    }
    // Blue-dominant.
    if (b > r && b > g - 20) {
        *flag = 0;   // blue path in the colorFilter decoder
        *intensity = (uint8_t)std::min(255, 120 + range);
        return;
    }
    // Greenish / mixed — default no tint.
    *flag = 0; *intensity = 0;
}

// Per-horse perpendicular offset from the centerline (in world units).
// 6 horses spaced ~60 units apart, centered. This is a constant per-horse
// visual offset — distAlongPath stays in sync with track progress.
static constexpr float kHbLaneSpacing = 60.0f;
static float LaneOffsetFor(int horseIdx) {
    return (horseIdx - (HB_NUM_HORSES - 1) * 0.5f) * kHbLaneSpacing;
}

static void SpawnHorses(PlayState* play) {
    auto& s = GetSession();
    if (s.waypoints.empty()) {
        SPDLOG_WARN("[HorseBetting] SpawnHorses: waypoints empty, bailing");
        return;
    }
    SPDLOG_INFO("[HorseBetting] SpawnHorses: spawning {} horses", HB_NUM_HORSES);

    // Compute path direction at waypoint 0 → waypoint 1 for the initial
    // perpendicular offset. This way horses spawn side-by-side facing
    // forward, not piled on top of each other.
    const HorseWaypoint& start = s.waypoints[0];
    const HorseWaypoint& next  = s.waypoints.size() > 1 ? s.waypoints[1] : start;
    float pdx = next.x - start.x, pdz = next.z - start.z;
    float plen = std::sqrt(pdx*pdx + pdz*pdz);
    if (plen < 0.001f) plen = 1.0f;
    float ndx = pdx / plen, ndz = pdz / plen;
    float perpX =  ndz, perpZ = -ndx;   // right-hand perpendicular
    int16_t startYaw = (int16_t)((std::atan2(pdx, pdz) / kPi) * 0x8000);

    int spawnedOk = 0, spawnedFail = 0;
    for (int i = 0; i < HB_NUM_HORSES; ++i) {
        HorseRuntime& rt = s.runtimes[i];
        rt = HorseRuntime{};
        // distAlongPath = 0 for all horses — they all start at the same
        // track progress, just laterally separated. As speeds diverge the
        // tracker UI and the visual gap stay perfectly in sync.
        rt.distAlongPath = 0.0f;
        rt.lap           = 0;
        rt.waypointIdx   = 0;
        rt.finished      = false;
        rt.actorPtr      = nullptr;

        float laneOff = LaneOffsetFor(i);
        rt.x = start.x + perpX * laneOff;
        rt.y = start.y;
        rt.z = start.z + perpZ * laneOff;
        rt.yaw = startYaw;

        // Alternate horse skin between brown (Epona, params bit 0x8000 clear)
        // and white Gerudo horse (HNI, bit set). Gives 6 horses two visually
        // distinct skins (3 brown + 3 white) since per-horse colorFilter
        // tinting doesn't work on EN_HORSE — its draw fn doesn't honor
        // Actor_SetColorFilter. The race tracker UI handles the remaining
        // identification with colored dots.
        int16_t skinBit = (i & 1) ? (int16_t)0x8000 : (int16_t)0;
        int16_t spawnParams = kHbRaceHorseParams | skinBit;
        Actor* a = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_HORSE,
                               rt.x, rt.y, rt.z,
                               0, startYaw, 0, spawnParams);
        if (a) {
            spawnedOk++;
            rt.actorPtr = a;
            SPDLOG_INFO("[HorseBetting] Horse {} spawned at ({:.1f}, {:.1f}, {:.1f}) params=0x{:04X} ptr={}",
                        i, rt.x, rt.y, rt.z, (uint16_t)spawnParams, (void*)a);
            // Per-horse color tint kept as best-effort: most likely a no-op
            // on EN_HORSE but harmless. If a future SoH change adds color-
            // filter support to EnHorse_Draw, this will start working.
            int16_t flag = 0; uint8_t intensity = 0;
            PickHorseTint(s.horses[i], &flag, &intensity);
            if (intensity > 0) {
                Actor_SetColorFilter(a, flag, intensity, 0, 0xFFFE);
            }
            // Kick the horse into MOUNTED_GALLOP action so it plays the
            // run animation. Without this call EN_HORSE defaults to
            // ENHORSE_ACT_IDLE (params=1 wandering wild horse) and just
            // stands there — speedXZ alone doesn't trigger the gallop
            // animation. EnHorse_StartGalloping sets action = MOUNTED_
            // GALLOP, animationIdx = ANIM_GALLOP, and Animation_Changes
            // the skelAnime. Subsequent frames the MountedGallop handler
            // maintains the loop as long as speedXZ stays >= 6.0.
            EnHorse_StartGalloping(a);
        } else {
            spawnedFail++;
            SPDLOG_WARN("[HorseBetting] Horse {} spawn FAILED at ({:.1f}, {:.1f}, {:.1f}) params=0x{:04X} — out of actor slots? bad params? object not loaded?",
                        i, rt.x, rt.y, rt.z, (uint16_t)spawnParams);
        }
    }
    SPDLOG_INFO("[HorseBetting] SpawnHorses done: {} ok / {} failed", spawnedOk, spawnedFail);
}

// ---------- camera --------------------------------------------------------
// Cinematic camera: tracks the leader at all times, but periodically
// switches between "shots" so the player gets a TV-broadcast feel:
//   - Shot 0: chase-cam from behind (default)
//   - Shot 1: side-on, 90° to the right of motion
//   - Shot 2: high overhead at a 45° angle
//   - Shot 3: opposing-side view, 90° to the left
// Switches every ~4 seconds during RACING. Smooth interpolation between
// shots prevents jarring snaps.
static int FindLeader() {
    auto& s = GetSession();
    int best = 0;
    float bestDist = -1.0f;
    for (int i = 0; i < HB_NUM_HORSES; ++i) {
        if (s.runtimes[i].distAlongPath > bestDist) {
            bestDist = s.runtimes[i].distAlongPath;
            best = i;
        }
    }
    return best;
}

static void FollowLeaderCamera(PlayState* play) {
    if (!play) return;
    auto& s = GetSession();
    int leader = FindLeader();
    const HorseRuntime& rt = s.runtimes[leader];

    Camera* cam = play->cameraPtrs[play->activeCamera];
    if (!cam) return;

    // Lock the camera into FREE0 mode (CAM_SET_FREE0 = 0x21, comment in
    // soh/include/z64camera.h:58: "Full manual control is given over the
    // camera"). Vanilla's camera state machine normally tracks the focus
    // actor (Link) and recomputes eye/at from `setting`+`mode` each
    // frame — that overwrites any direct eye/at writes immediately, which
    // is why the previous version of this function appeared to do nothing.
    //
    // FREE0 short-circuits the state machine: vanilla code reads eye/at
    // and uses them directly without computing new values. We re-check
    // the setting every frame in case anything tries to swap us back
    // (scene transitions, cutscene triggers, etc.).
    if (cam->setting != CAM_SET_FREE0) {
        Camera_ChangeSetting(cam, CAM_SET_FREE0);
    }

    // Switch shots every 240 frames (4s @60fps) during the actual race.
    // In COUNTDOWN, stick to shot 0. In FINISHED, hold the last shot for
    // a nice winner-pose moment.
    int shot = 0;
    if (s.state == HB_STATE_RACING) {
        shot = (s.framesInState / 240) % 4;
    } else if (s.state == HB_STATE_FINISHED) {
        // Hold whatever shot was active when the race ended.
        shot = (s.framesInState / 240) % 4;
    }

    // Compute the leader's facing yaw as a unit vector.
    float yawRad = (float)rt.yaw / 0x8000 * kPi;
    float fx = std::sin(yawRad);
    float fz = std::cos(yawRad);
    // Right-perpendicular (relative to facing direction).
    float rx =  fz;
    float rz = -fx;

    float eyeX = rt.x, eyeY = rt.y, eyeZ = rt.z;
    float atX  = rt.x, atY  = rt.y + 30.0f, atZ = rt.z;

    switch (shot) {
        case 0:  // Chase cam from behind the leader
            eyeX = rt.x - fx * 250.0f;
            eyeY = rt.y + 120.0f;
            eyeZ = rt.z - fz * 250.0f;
            break;
        case 1:  // Side-on right
            eyeX = rt.x + rx * 280.0f;
            eyeY = rt.y +  80.0f;
            eyeZ = rt.z + rz * 280.0f;
            break;
        case 2:  // High overhead at angle
            eyeX = rt.x - fx * 180.0f;
            eyeY = rt.y + 260.0f;
            eyeZ = rt.z - fz * 180.0f;
            break;
        case 3:  // Side-on left
            eyeX = rt.x - rx * 280.0f;
            eyeY = rt.y +  80.0f;
            eyeZ = rt.z - rz * 280.0f;
            break;
    }

    // Smooth the camera (lerp from current to target) so shot transitions
    // don't snap instantly. Faster lerp during a shot, slower (gentler)
    // pull-in when switching shots — gives the cinematic glide feel.
    const float lerp = 0.12f;
    cam->eye.x = cam->eye.x + (eyeX - cam->eye.x) * lerp;
    cam->eye.y = cam->eye.y + (eyeY - cam->eye.y) * lerp;
    cam->eye.z = cam->eye.z + (eyeZ - cam->eye.z) * lerp;
    cam->at.x  = cam->at.x  + (atX  - cam->at.x)  * lerp;
    cam->at.y  = cam->at.y  + (atY  - cam->at.y)  * lerp;
    cam->at.z  = cam->at.z  + (atZ  - cam->at.z)  * lerp;
    cam->eyeNext = cam->eye;
}

// ---------- horse AI per-frame -------------------------------------------
//
// Arc-length parameterization model:
//   - rt.distAlongPath is the cumulative track distance the horse has
//     covered. It's the SOLE source of truth for the horse's track
//     progress, and it's exactly what the race tracker UI displays.
//   - world (x, z) is computed deterministically from distAlongPath by
//     interpolating along the path segments, plus a constant perpendicular
//     lane offset for visual separation.
//   - Yaw is computed from the current segment's direction.
//
// Result: where the horse is on the track ALWAYS matches what the UI
// tracker says. A horse at 50% progress is visually at the midpoint of
// the lap, regardless of speed, lane offset, or starting position.

static void HorseAtArc(int horseIdx, float lapArc,
                       float* outX, float* outY, float* outZ, int16_t* outYaw) {
    auto& s = GetSession();
    if (s.waypoints.empty() || s.arcPerLap <= 0.0f) {
        *outX = *outY = *outZ = 0.0f; *outYaw = 0;
        return;
    }
    // Wrap lapArc into [0, arcPerLap) defensively.
    if (lapArc < 0.0f) lapArc = 0.0f;
    if (lapArc >= s.arcPerLap) lapArc = std::fmod(lapArc, s.arcPerLap);

    // Find the segment whose [cumArc[i], cumArc[i+1]) contains lapArc.
    // cumArc has N entries (one per waypoint). The closing segment runs
    // from cumArc[N-1] to arcPerLap (back to waypoint 0).
    int N = (int)s.waypoints.size();
    int seg = N - 1;   // default: closing segment
    float segStart = s.cumulativeArc[N - 1];
    float segEnd   = s.arcPerLap;
    for (int i = 0; i < N - 1; ++i) {
        if (lapArc >= s.cumulativeArc[i] && lapArc < s.cumulativeArc[i + 1]) {
            seg = i;
            segStart = s.cumulativeArc[i];
            segEnd   = s.cumulativeArc[i + 1];
            break;
        }
    }
    const HorseWaypoint& a = s.waypoints[seg];
    const HorseWaypoint& b = s.waypoints[(seg + 1) % N];
    float segLen = segEnd - segStart;
    float t = (segLen > 0.001f) ? (lapArc - segStart) / segLen : 0.0f;

    // Path direction along this segment.
    float dx = b.x - a.x;
    float dz = b.z - a.z;
    float dlen = std::sqrt(dx*dx + dz*dz);
    if (dlen < 0.001f) dlen = 1.0f;
    float ndx = dx / dlen, ndz = dz / dlen;
    // Right-hand perpendicular.
    float perpX =  ndz, perpZ = -ndx;

    float laneOff = LaneOffsetFor(horseIdx);
    *outX = a.x + dx * t + perpX * laneOff;
    *outY = a.y + (b.y - a.y) * t;
    *outZ = a.z + dz * t + perpZ * laneOff;
    *outYaw = (int16_t)((std::atan2(dx, dz) / kPi) * 0x8000);
}

static void UpdateHorseAI(HorseRuntime& rt, const HorseInfo& info, float jitter) {
    auto& s = GetSession();
    if (rt.finished) return;
    if (s.waypoints.empty() || s.arcPerLap <= 0.0f) return;

    // Per-horse index so we can fetch the correct lane offset.
    int idx = (int)(&rt - &s.runtimes[0]);

    // Stamina fades speed across laps.
    float lapFatigue = 1.0f - (rt.lap * (1.0f - info.stamina) * 0.25f);
    float baseSpeed  = info.speedBase * lapFatigue;
    float speed = baseSpeed + jitter * info.speedVariance;
    if (speed < 0.5f) speed = 0.5f;
    rt.curSpeed = speed;

    // Single state-advance: track-progress increment.
    rt.distAlongPath += speed;

    // Lap detection. Once we've covered enough total arc for HB_NUM_LAPS,
    // the horse is done.
    int newLap = (int)(rt.distAlongPath / s.arcPerLap);
    if (newLap > rt.lap) rt.lap = newLap;
    if (rt.lap >= HB_NUM_LAPS) {
        rt.finished = true;
        // Snap to finish-line position for a clean stop.
        rt.distAlongPath = HB_NUM_LAPS * s.arcPerLap;
    }

    // Derive world position from arc length within current lap.
    float lapArc = std::fmod(rt.distAlongPath, s.arcPerLap);
    HorseAtArc(idx, lapArc, &rt.x, &rt.y, &rt.z, &rt.yaw);
    rt.waypointIdx = 0;   // kept for any legacy reads; not used internally

    // Push transform into the spawned actor. NULL-tolerant.
    if (rt.actorPtr) {
        Actor* a = (Actor*)rt.actorPtr;
        a->world.pos.x = rt.x;
        a->world.pos.y = rt.y;
        a->world.pos.z = rt.z;
        a->shape.rot.y = rt.yaw;
        a->world.rot.y = rt.yaw;
        // speedXZ must stay >= 6.0 for the MountedGallop action handler
        // to keep us in MOUNTED_GALLOP. Below 6 it transitions to trotting
        // (via EnHorse_StartTrotting), below 3 it stops. We clamp the
        // visible speed at 6.5 minimum so the animation never falls out
        // of gallop, even if `info.speedBase` is tuned slow.
        float visSpeed = rt.curSpeed * 0.8f;  // tame the per-tick distance
        if (visSpeed < 6.5f) visSpeed = 6.5f;
        a->speedXZ = visSpeed;
        // Re-force the action every frame. The MountedGallop handler in
        // EN_HORSE will occasionally transition us out (e.g., on a
        // collision detect or if `EnHorse_PlayerCanMove` returns false
        // because there's no real rider). Stomping the action back to
        // MOUNTED_GALLOP keeps the gallop loop alive.
        ForceHorseGallopAction(a);
    }
}

// ---------- find leader ---------------------------------------------------
// ---------- audio helpers -------------------------------------------------
// Per code_800EC960.c: Audio_QueueSeqCmd with high nibble 0x0 = play seq.
static void PlayStartingGun() { Sfx_PlaySfxCentered(NA_SE_EV_BOMB_BOUND); }
static void StartRaceMusic()  { Audio_QueueSeqCmd(0x00000000 | (uint16_t)NA_BGM_HORSE); }
static void StartGoalMusic()  { Audio_QueueSeqCmd(0x00000000 | (uint16_t)NA_BGM_HORSE_GOAL); }
static void StopMusic()       { Audio_QueueSeqCmd(NA_BGM_STOP); }

// ---------- state transitions --------------------------------------------
void StartRace() {
    auto& s = GetSession();
    // NPCs and race both live at the ranch — no scene transition needed.
    // The TickRace handler picks up PREPARING and spawns horses next frame.
    s.state = HB_STATE_PREPARING;
    s.framesInState = 0;
}

void EndRace() {
    auto& s = GetSession();

    int order[HB_NUM_HORSES];
    for (int i = 0; i < HB_NUM_HORSES; ++i) order[i] = i;
    std::sort(order, order + HB_NUM_HORSES, [&s](int a, int b){
        return s.runtimes[a].distAlongPath > s.runtimes[b].distAlongPath;
    });
    for (int i = 0; i < HB_NUM_HORSES; ++i) {
        s.runtimes[order[i]].finalPlacement = i + 1;
    }
    s.winnerIdx = order[0];

    if (s.chosenHorse == s.winnerIdx) {
        int payout = (int)(s.betAmount * s.horses[s.chosenHorse].odds);
        Rupees_ChangeBy(payout);
    }

    StartGoalMusic();
}

// Cleans up the race: kills any spawned horse actors so they don't linger
// in the ranch after the race ends, unlocks all players, transitions
// FINISHED → PAYOUT in place (no scene change — we're already where the
// NPCs are).
void ReturnToTalon() {
    auto& s = GetSession();
    StopMusic();

    // Despawn every horse we successfully spawned. NULL-guarded entries
    // (failed Actor_Spawn) are skipped silently.
    for (auto& rt : s.runtimes) {
        if (rt.actorPtr) {
            Actor_Kill((Actor*)rt.actorPtr);
            rt.actorPtr = nullptr;
        }
    }

    // Restore the camera to NORMAL0 so the player doesn't end up stuck in
    // FREE0 (manual-control) mode after the race — FREE0 freezes the
    // camera wherever we last placed it, which would be highly disorienting
    // on return to gameplay. NORMAL0 is the default scene camera setting
    // that resumes following Link.
    if (gPlayState) {
        Camera* cam = gPlayState->cameraPtrs[gPlayState->activeCamera];
        if (cam && cam->setting == CAM_SET_FREE0) {
            Camera_ChangeSetting(cam, CAM_SET_NORMAL0);
        }
    }

    // Coop-aware: clear cutscene lock on every Player.
    UnlockAllPlayersAfterRace(gPlayState);

    s.state = HB_STATE_PAYOUT;
    s.framesInState = 0;
}

// ---------- per-frame tick -----------------------------------------------
//
// Race spawns six ACTOR_EN_HORSE actors at the starting line and drives
// their positions each frame from our internal HorseRuntime structs.
// The horses visibly run on the Lon Lon track. Camera follows the leader.
// The HorseBettingUI race overlay also draws a HUD mini-tracker so the
// player can see who's leading from any camera angle.
//
// IMPORTANT: spawn params MUST be 0x0003 (race variant). params=0 caused
// the RIP=0x0 crash in v9 — EnHorse_Init's default path doesn't set up the
// update/draw function pointers correctly without a known variant.
void TickRace() {
    auto& s = GetSession();
    PlayState* play = gPlayState;
    if (!play) return;

    switch (s.state) {
        case HB_STATE_PREPARING: {
            if (play->sceneNum != SCENE_LON_LON_RANCH) return;

            // Spawn six race horses on the starting line. NULL-tolerant —
            // any that fail to spawn still tick as runtime-only entries.
            SpawnHorses(play);

            // Coop-aware: freeze every Player actor so neither P1 nor P2
            // can wander during the race countdown.
            LockAllPlayersForRace(play);

            s.state = HB_STATE_COUNTDOWN;
            s.framesInState = 0;
            s.countdown = 3;
            break;
        }

        case HB_STATE_COUNTDOWN: {
            if (s.framesInState > 0 && (s.framesInState % 60) == 0) {
                s.countdown--;
                if (s.countdown > 0) {
                    Sfx_PlaySfxCentered(NA_SE_SY_FOUND);
                } else {
                    PlayStartingGun();
                    StartRaceMusic();
                    s.state = HB_STATE_RACING;
                    s.framesInState = 0;
                }
            }
            FollowLeaderCamera(play);
            break;
        }

        case HB_STATE_RACING: {
            float jitter = std::sin(s.framesInState * 0.1f) +
                           std::cos(s.framesInState * 0.073f);
            for (int i = 0; i < HB_NUM_HORSES; ++i) {
                UpdateHorseAI(s.runtimes[i], s.horses[i], jitter);
            }
            FollowLeaderCamera(play);

            for (int i = 0; i < HB_NUM_HORSES; ++i) {
                if (s.runtimes[i].finished) {
                    EndRace();
                    s.state = HB_STATE_FINISHED;
                    s.framesInState = 0;
                    break;
                }
            }
            break;
        }

        case HB_STATE_FINISHED: {
            FollowLeaderCamera(play);
            if (s.framesInState > 60 * 4) {     // 4s of victory fanfare
                ReturnToTalon();   // in-place cleanup → PAYOUT state
            }
            break;
        }

        default:
            break;
    }
}

}  // namespace HorseBetting
