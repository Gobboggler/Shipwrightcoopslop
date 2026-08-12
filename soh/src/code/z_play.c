#include "global.h"
#include "vt.h"
#include "objects/gameplay_keep/gameplay_keep.h"

#include <string.h>

#include "soh/Enhancements/gameconsole.h"
#include "soh/frame_interpolation.h"
#include <overlays/actors/ovl_En_Niw/z_en_niw.h>
#include <overlays/misc/ovl_kaleido_scope/z_kaleido_scope.h>
#include "soh/Enhancements/enhancementTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/OTRGlobals.h"
#include "soh/ResourceManagerHelpers.h"
#include "soh/SaveManager.h"
#include "soh/framebuffer_effects.h"

#include <libultraship/libultraship.h>

#include <time.h>

// SoH multiplayer: Camera_BGCheck is defined in z_camera.c but isn't
// declared in any public header. GCC accepts the implicit declaration
// silently; Clang (macOS) rejects it. Forward-declare here so all three
// platforms compile.
s32 Camera_BGCheck(Camera* camera, Vec3f* from, Vec3f* to);

// SoH multiplayer: shared state for the P2 lock-on reticle. Updated
// once per frame inside the PiP block (z_play.c) with P2's current
// view-projection matrix and a TargetContext seeded from P2's
// focusActor. Then read at HUD draw time (z_parameter.c) where
// func_8002C124 is called a second time with these values overridden
// so the spinning-triangle reticle renders correctly in P2's PiP
// region using P2's projection rather than P1's. gCoopP2ReticleValid
// gates the second call — only draws when P2 actually has a target.
MtxF gCoopP2ViewProjMtxF;
TargetContext gCoopP2TargetCtx;
s32 gCoopP2ReticleValid = 0;
// SoH multiplayer: PiP stability counter — see commentary at the PiP
// block below. Lifted to file scope so the splitScreenActive check at
// the top of Play_Draw can read it and decide whether to shrink P1's
// viewport. If we shrink P1's viewport BEFORE PiP is ready to render,
// the right half of the screen flashes black during the 2-frame
// warmup. By gating both viewport-shrink AND PiP-render on the same
// counter, the right half stays as P1's view content (full-width)
// until PiP is ready, then snaps to PiP — no black flash visible.
s32 gCoopPiPStableFrames = 0;
// SoH multiplayer: tracks whether split-screen is *currently* rendering
// this frame — not just whether the LocalCoop.PiPPrototype CVar is set,
// but whether all the runtime gates pass (no cutscene, no fixed-cam,
// no transition, free-look camera setting). z_player.c reads this to
// decide how to interpret P2's control stick: when split-screen is on,
// P2's stick is relative to P2's PiP camera (so "up" means away from
// the PiP camera the player is looking at); when split-screen is off
// — even with PiPPrototype CVar on — the player sees through P1's main
// camera, so P2's stick must be interpreted relative to that camera
// or P2 controls feel rotated when the perspective shifts (cutscenes,
// shop interiors, fixed boss views, etc.).
s32 gCoopSplitScreenActive = 0;
// SoH multiplayer: when non-NULL, points to the Player actor whose head
// limb should be SKIPPED during Player_OverrideLimbDrawGameplayCommon.
// Set by the PiP block right before that view's actor-draw pass when
// the player is in first-person; cleared right after. Lets us hide a
// player's own head ONLY on their own viewport (so the head model
// doesn't clip the camera near plane / draw inside-out) while leaving
// the head visible to the other player. Vanilla relies on Camera_Subj3
// keeping the eye an explicit distance out from the head bone so the
// head naturally clips behind the near plane — we mirror that with a
// 12-unit forward nudge above, but the nudge alone leaves the back of
// the head still partly visible when P2 looks straight forward and
// can show inverted polygons when they turn around. Explicit head-
// limb suppression covers those edge cases cleanly.
Actor* gCoopHideHeadFor = NULL;
#include <assert.h>

TransitionUnk sTrnsnUnk;
s32 gTrnsnUnkState;
VisMono gPlayVisMono;
Color_RGBA8_u32 gVisMonoColor;

FaultClient D_801614B8;

s16 sTransitionFillTimer;

void* gDebugCutsceneScript = NULL;
UNK_TYPE D_8012D1F4 = 0; // unused

Input* D_8012D1F8 = NULL;

PlayState* gPlayState;
s16 firstInit = 0;
s16 gEnPartnerId;

void Play_SpawnScene(PlayState* play, s32 sceneId, s32 spawn);

// This macro prints the number "1" with a file and line number if R_ENABLE_PLAY_LOGS is enabled.
// For example, it can be used to trace the play state execution at a high level.
// SOHTODO: Revert log statements everywhere back to authentic, and deal with dynamic line/file names via macro
#define PLAY_LOG(line)                                  \
    do {                                                \
        if (1 & HREG(63)) {                             \
            LOG_NUM("1", 1 /*, "../z_play.c", line */); \
        }                                               \
    } while (0)

void enableBetaQuest();
void disableBetaQuest();

void OTRPlay_SpawnScene(PlayState* play, s32 sceneId, s32 spawn);

void Play_RequestViewpointBgCam(PlayState* play) {
    Camera_ChangeDataIdx(GET_ACTIVE_CAM(play), play->unk_1242B - 1);
}

void Play_SetViewpoint(PlayState* play, s16 viewpoint) {
    assert(viewpoint == 1 || viewpoint == 2);

    play->unk_1242B = viewpoint;

    if ((YREG(15) != 0x10) && (gSaveContext.cutsceneIndex < 0xFFF0)) {
        Audio_PlaySoundGeneral((viewpoint == 1) ? NA_SE_SY_CAMERA_ZOOM_DOWN : NA_SE_SY_CAMERA_ZOOM_UP, &gSfxDefaultPos,
                               4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
    }

    Play_RequestViewpointBgCam(play);
}

/**
 * @return true if the currently set viewpoint is the same as the one provided in the argument
 */
s32 Play_CheckViewpoint(PlayState* play, s16 viewpoint) {
    return (viewpoint == play->unk_1242B);
}

/**
 * If the scene is a shop, set the viewpoint that will set the bgCamIndex
 * to toggle the camera into a "browsing item selection" setting.
 */
void Play_SetShopBrowsingViewpoint(PlayState* play) {
    osSyncPrintf("Game_play_shop_pr_vr_switch_set()\n");

    if (YREG(15) == 0x10) {
        play->unk_1242B = 2;
    }
}

void Gameplay_SetupTransition(PlayState* play, s32 transitionType) {
    TransitionContext* transitionCtx = &play->transitionCtx;

    memset(transitionCtx, 0, sizeof(TransitionContext));

    transitionCtx->transitionType = transitionType;

    // Circle Transition Types
    if ((transitionCtx->transitionType >> 5) == 1) {
        transitionCtx->init = TransitionCircle_Init;
        transitionCtx->destroy = TransitionCircle_Destroy;
        transitionCtx->start = TransitionCircle_Start;
        transitionCtx->isDone = TransitionCircle_IsDone;
        transitionCtx->draw = TransitionCircle_Draw;
        transitionCtx->update = TransitionCircle_Update;
        transitionCtx->setType = TransitionCircle_SetType;
        transitionCtx->setColor = TransitionCircle_SetColor;
        transitionCtx->setEnvColor = TransitionCircle_SetEnvColor;
    } else {
        switch (transitionCtx->transitionType) {
            case TRANS_TYPE_TRIFORCE:
                transitionCtx->init = TransitionTriforce_Init;
                transitionCtx->destroy = TransitionTriforce_Destroy;
                transitionCtx->start = TransitionTriforce_Start;
                transitionCtx->isDone = TransitionTriforce_IsDone;
                transitionCtx->draw = TransitionTriforce_Draw;
                transitionCtx->update = TransitionTriforce_Update;
                transitionCtx->setType = TransitionTriforce_SetType;
                transitionCtx->setColor = TransitionTriforce_SetColor;
                transitionCtx->setEnvColor = NULL;
                break;

            case TRANS_TYPE_WIPE:
            case TRANS_TYPE_WIPE_FAST:
                transitionCtx->init = TransitionWipe_Init;
                transitionCtx->destroy = TransitionWipe_Destroy;
                transitionCtx->start = TransitionWipe_Start;
                transitionCtx->isDone = TransitionWipe_IsDone;
                transitionCtx->draw = TransitionWipe_Draw;
                transitionCtx->update = TransitionWipe_Update;
                transitionCtx->setType = TransitionWipe_SetType;
                transitionCtx->setColor = TransitionWipe_SetColor;
                transitionCtx->setEnvColor = NULL;
                break;

            case TRANS_TYPE_FADE_BLACK:
            case TRANS_TYPE_FADE_WHITE:
            case TRANS_TYPE_FADE_BLACK_FAST:
            case TRANS_TYPE_FADE_WHITE_FAST:
            case TRANS_TYPE_FADE_BLACK_SLOW:
            case TRANS_TYPE_FADE_WHITE_SLOW:
            case TRANS_TYPE_FADE_WHITE_CS_DELAYED:
            case TRANS_TYPE_FADE_WHITE_INSTANT:
            case TRANS_TYPE_FADE_GREEN:
            case TRANS_TYPE_FADE_BLUE:
                transitionCtx->init = TransitionFade_Init;
                transitionCtx->destroy = TransitionFade_Destroy;
                transitionCtx->start = TransitionFade_Start;
                transitionCtx->isDone = TransitionFade_IsDone;
                transitionCtx->draw = TransitionFade_Draw;
                transitionCtx->update = TransitionFade_Update;
                transitionCtx->setType = TransitionFade_SetType;
                transitionCtx->setColor = TransitionFade_SetColor;
                transitionCtx->setEnvColor = NULL;
                break;

            case TRANS_TYPE_FILL_WHITE2:
            case TRANS_TYPE_FILL_WHITE:
                play->transitionMode = TRANS_MODE_FILL_WHITE_INIT;
                break;

            case TRANS_TYPE_INSTANT:
                play->transitionMode = TRANS_MODE_INSTANT;
                break;

            case TRANS_TYPE_FILL_BROWN:
                play->transitionMode = TRANS_MODE_FILL_BROWN_INIT;
                break;

            case TRANS_TYPE_SANDSTORM_PERSIST:
                play->transitionMode = TRANS_MODE_SANDSTORM_INIT;
                break;

            case TRANS_TYPE_SANDSTORM_END:
                play->transitionMode = TRANS_MODE_SANDSTORM_END_INIT;
                break;

            case TRANS_TYPE_CS_BLACK_FILL:
                play->transitionMode = TRANS_MODE_CS_BLACK_FILL_INIT;
                break;

            default:
                Fault_AddHungupAndCrash(__FILE__, __LINE__);
                break;
        }
    }
}

void func_800BC88C(PlayState* play) {
    play->transitionCtx.transitionType = -1;
}

Gfx* Play_SetFog(PlayState* play, Gfx* gfx) {
    return Gfx_SetFog2(gfx, play->lightCtx.fogColor[0], play->lightCtx.fogColor[1], play->lightCtx.fogColor[2], 0,
                       play->lightCtx.fogNear, 1000);
}

void Play_Destroy(GameState* thisx) {
    PlayState* play = (PlayState*)thisx;
    Player* player = GET_PLAYER(play);

    GameInteractor_ExecuteOnPlayDestroy();

    play->state.gfxCtx->callback = NULL;
    play->state.gfxCtx->callbackParam = 0;

    SREG(91) = 0;
    R_PAUSE_MENU_MODE = 0;

    PreRender_Destroy(&play->pauseBgPreRender);
    Effect_DeleteAll(play);
    EffectSs_ClearAll(play);
    CollisionCheck_DestroyContext(play, &play->colChkCtx);

    if (gTrnsnUnkState == 3) {
        TransitionUnk_Destroy(&sTrnsnUnk);
        gTrnsnUnkState = 0;
    }

    if (play->transitionMode == TRANS_MODE_INSTANCE_RUNNING) {
        play->transitionCtx.destroy(&play->transitionCtx.data);
        func_800BC88C(play);
        play->transitionMode = TRANS_MODE_OFF;
    }

    ShrinkWindow_Destroy();
    TransitionFade_Destroy(&play->transitionFade);
    VisMono_Destroy(&gPlayVisMono);

    if (gSaveContext.linkAge != play->linkAgeOnLoad) {
        Inventory_SwapAgeEquipment();
        Player_SetEquipmentData(play, player);
    }

    func_80031C3C(&play->actorCtx, play);
    func_80110990(play);
    KaleidoScopeCall_Destroy(play);
    KaleidoManager_Destroy();
    ZeldaArena_Cleanup();

    Fault_RemoveClient(&D_801614B8);

    disableBetaQuest();

    gPlayState = NULL;
}

u8 CheckStoneCount() {
    u8 stoneCount = 0;

    if (CHECK_QUEST_ITEM(QUEST_KOKIRI_EMERALD)) {
        stoneCount++;
    }

    if (CHECK_QUEST_ITEM(QUEST_GORON_RUBY)) {
        stoneCount++;
    }

    if (CHECK_QUEST_ITEM(QUEST_ZORA_SAPPHIRE)) {
        stoneCount++;
    }

    return stoneCount;
}

u8 CheckMedallionCount() {
    u8 medallionCount = 0;

    if (CHECK_QUEST_ITEM(QUEST_MEDALLION_FOREST)) {
        medallionCount++;
    }

    if (CHECK_QUEST_ITEM(QUEST_MEDALLION_FIRE)) {
        medallionCount++;
    }

    if (CHECK_QUEST_ITEM(QUEST_MEDALLION_WATER)) {
        medallionCount++;
    }

    if (CHECK_QUEST_ITEM(QUEST_MEDALLION_SHADOW)) {
        medallionCount++;
    }

    if (CHECK_QUEST_ITEM(QUEST_MEDALLION_SPIRIT)) {
        medallionCount++;
    }

    if (CHECK_QUEST_ITEM(QUEST_MEDALLION_LIGHT)) {
        medallionCount++;
    }

    return medallionCount;
}

u8 CheckDungeonCount() {
    u8 dungeonCount = 0;

    if (Flags_GetEventChkInf(EVENTCHKINF_USED_DEKU_TREE_BLUE_WARP)) {
        dungeonCount++;
    }

    if (Flags_GetEventChkInf(EVENTCHKINF_USED_DODONGOS_CAVERN_BLUE_WARP)) {
        dungeonCount++;
    }

    if (Flags_GetEventChkInf(EVENTCHKINF_USED_JABU_JABUS_BELLY_BLUE_WARP)) {
        dungeonCount++;
    }

    if (Flags_GetEventChkInf(EVENTCHKINF_USED_FOREST_TEMPLE_BLUE_WARP)) {
        dungeonCount++;
    }

    if (Flags_GetEventChkInf(EVENTCHKINF_USED_FIRE_TEMPLE_BLUE_WARP)) {
        dungeonCount++;
    }

    if (Flags_GetEventChkInf(EVENTCHKINF_USED_WATER_TEMPLE_BLUE_WARP)) {
        dungeonCount++;
    }

    if (Flags_GetRandomizerInf(RAND_INF_DUNGEONS_DONE_SPIRIT_TEMPLE)) {
        dungeonCount++;
    }

    if (Flags_GetRandomizerInf(RAND_INF_DUNGEONS_DONE_SHADOW_TEMPLE)) {
        dungeonCount++;
    }

    return dungeonCount;
}

u8 CheckBridgeRewardCount() {
    u8 bridgeRewardCount = 0;

    switch (Randomizer_GetSettingValue(RSK_BRIDGE_OPTIONS)) {
        case RO_BRIDGE_WILDCARD_REWARD:
            if (Flags_GetRandomizerInf(RAND_INF_GREG_FOUND)) {
                bridgeRewardCount += 1;
            }
            break;
        case RO_BRIDGE_GREG_REWARD:
            if (Flags_GetRandomizerInf(RAND_INF_GREG_FOUND)) {
                bridgeRewardCount += 1;
            }
            break;
    }
    return bridgeRewardCount;
}

u8 CheckLACSRewardCount() {
    u8 lacsRewardCount = 0;

    switch (Randomizer_GetSettingValue(RSK_LACS_OPTIONS)) {
        case RO_LACS_WILDCARD_REWARD:
            if (Flags_GetRandomizerInf(RAND_INF_GREG_FOUND)) {
                lacsRewardCount += 1;
            }
            break;
        case RO_LACS_GREG_REWARD:
            if (Flags_GetRandomizerInf(RAND_INF_GREG_FOUND)) {
                lacsRewardCount += 1;
            }
            break;
    }
    return lacsRewardCount;
}

void Play_Init(GameState* thisx) {
    PlayState* play = (PlayState*)thisx;
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    uintptr_t zAlloc;
    uintptr_t zAllocAligned;
    size_t zAllocSize;
    Player* player;
    s32 playerStartBgCamIndex;
    s32 i;
    u8 baseSceneLayer;
    s32 pad[2];

    enableBetaQuest();

    // Properly initialize the frame counter so it doesn't use garbage data
    if (!firstInit) {
        play->gameplayFrames = 0;
        firstInit = 1;
    }

    // Invalid entrance, so immediately exit the game to opening title
    if (gSaveContext.entranceIndex == ENTR_LOAD_OPENING) {
        gSaveContext.entranceIndex = 0;
        play->state.running = false;
        SET_NEXT_GAMESTATE(&play->state, Opening_Init, OpeningContext);
        GameInteractor_ExecuteOnExitGame(gSaveContext.fileNum);
        return;
    }

    gPlayState = play;

    SystemArena_Display();

    // OTRTODO allocate double the normal amount of memory
    // This is to avoid some parts of the game, like loading actors, causing OoM
    // This is potionally unavoidable due to struct size differences, but is x2 the right amount?
    GameState_Realloc(&play->state, 0x1D4790 * 2);
    KaleidoManager_Init(play);
    View_Init(&play->view, gfxCtx);
    Audio_SetExtraFilter(0);
    Quake_Init();

    for (i = 0; i < ARRAY_COUNT(play->cameraPtrs); i++) {
        play->cameraPtrs[i] = NULL;
    }

    Camera_Init(&play->mainCamera, &play->view, &play->colCtx, play);
    Camera_ChangeStatus(&play->mainCamera, CAM_STAT_ACTIVE);

    for (i = 0; i < 3; i++) {
        Camera_Init(&play->subCameras[i], &play->view, &play->colCtx, play);
        Camera_ChangeStatus(&play->subCameras[i], CAM_STAT_UNK100);
    }

    play->cameraPtrs[MAIN_CAM] = &play->mainCamera;
    play->cameraPtrs[MAIN_CAM]->uid = 0;
    play->activeCamera = MAIN_CAM;
    func_8005AC48(&play->mainCamera, 0xFF);
    // Sram_Init(this, &this->sramCtx);
    Regs_InitData(play);
    Message_Init(play);
    GameOver_Init(play);
    SoundSource_InitAll(play);
    Effect_InitContext(play);
    EffectSs_InitInfo(play, 0x55);
    CollisionCheck_InitContext(play, &play->colChkCtx);
    AnimationContext_Reset(&play->animationCtx);
    func_8006450C(play, &play->csCtx);

    if (gSaveContext.nextCutsceneIndex != 0xFFEF) {
        gSaveContext.cutsceneIndex = gSaveContext.nextCutsceneIndex;
        gSaveContext.nextCutsceneIndex = 0xFFEF;
    }

    if (gSaveContext.cutsceneIndex == 0xFFFD) {
        gSaveContext.cutsceneIndex = 0;
    }

    if (gSaveContext.nextDayTime != 0xFFFF) {
        gSaveContext.dayTime = gSaveContext.nextDayTime;
        gSaveContext.skyboxTime = gSaveContext.nextDayTime;
    }

    if (gSaveContext.dayTime > 0xC000 || gSaveContext.dayTime < 0x4555) {
        gSaveContext.nightFlag = 1;
    } else {
        gSaveContext.nightFlag = 0;
    }

    Cutscene_HandleConditionalTriggers(play);

    if (gSaveContext.gameMode != GAMEMODE_NORMAL || gSaveContext.cutsceneIndex >= 0xFFF0) {
        gSaveContext.nayrusLoveTimer = 0;
        Magic_Reset(play);
        gSaveContext.sceneSetupIndex = SCENE_LAYER_CUTSCENE_FIRST + (gSaveContext.cutsceneIndex & 0xF);
    } else if (!LINK_IS_ADULT && IS_DAY) {
        gSaveContext.sceneSetupIndex = SCENE_LAYER_CHILD_DAY;
    } else if (!LINK_IS_ADULT && !IS_DAY) {
        gSaveContext.sceneSetupIndex = SCENE_LAYER_CHILD_NIGHT;
    } else if (LINK_IS_ADULT && IS_DAY) {
        gSaveContext.sceneSetupIndex = SCENE_LAYER_ADULT_DAY;
    } else {
        gSaveContext.sceneSetupIndex = SCENE_LAYER_ADULT_NIGHT;
    }

    // save the base scene layer (before accounting for the special cases below) to use later for the transition type
    baseSceneLayer = gSaveContext.sceneSetupIndex;

    if ((gEntranceTable[((void)0, gSaveContext.entranceIndex)].scene == SCENE_HYRULE_FIELD) && !LINK_IS_ADULT &&
        !IS_CUTSCENE_LAYER) {
        if (CHECK_QUEST_ITEM(QUEST_KOKIRI_EMERALD) && CHECK_QUEST_ITEM(QUEST_GORON_RUBY) &&
            CHECK_QUEST_ITEM(QUEST_ZORA_SAPPHIRE)) {
            gSaveContext.sceneSetupIndex = 1;
        } else {
            gSaveContext.sceneSetupIndex = 0;
        }
    } else if ((gEntranceTable[((void)0, gSaveContext.entranceIndex)].scene == SCENE_KOKIRI_FOREST) && LINK_IS_ADULT &&
               !IS_CUTSCENE_LAYER) {
        gSaveContext.sceneSetupIndex = (Flags_GetEventChkInf(EVENTCHKINF_USED_FOREST_TEMPLE_BLUE_WARP)) ? 3 : 2;
    }

    Play_SpawnScene(
        play, gEntranceTable[((void)0, gSaveContext.entranceIndex) + ((void)0, gSaveContext.sceneSetupIndex)].scene,
        gEntranceTable[((void)0, gSaveContext.sceneSetupIndex) + ((void)0, gSaveContext.entranceIndex)].spawn);

    osSyncPrintf("\nSCENE_NO=%d COUNTER=%d\n", ((void)0, gSaveContext.entranceIndex), gSaveContext.sceneSetupIndex);

#if 0
    // When entering Gerudo Valley in the credits, trigger the GC emulator to play the ending movie.
    // The emulator constantly checks whether PC is 0x81000000, so this works even though it's not a valid address.
    if ((gEntranceTable[((void)0, gSaveContext.save.entranceIndex)].sceneId == SCENE_GERUDO_VALLEY) &&
        gSaveContext.sceneLayer == 6) {
        PRINTF("エンディングはじまるよー\n"); // "The ending starts"
        ((void (*)(void))0x81000000)();
        PRINTF("出戻り？\n"); // "Return?"
    }
#endif

    Cutscene_HandleEntranceTriggers(play);
    KaleidoScopeCall_Init(play);
    func_801109B0(play);

    if (gSaveContext.nextDayTime != 0xFFFF) {
        if (gSaveContext.nextDayTime == 0x8001) {
            gSaveContext.totalDays++;
            gSaveContext.bgsDayCount++;
            gSaveContext.dogIsLost = true;

            if (Inventory_ReplaceItem(play, ITEM_WEIRD_EGG, ITEM_CHICKEN) || Inventory_HatchPocketCucco(play)) {
                GameInteractor_ExecuteOnCuccoOrChickenHatch();
                Message_StartTextbox(play, 0x3066, NULL);
            }

            gSaveContext.nextDayTime = 0xFFFE;
        } else {
            gSaveContext.nextDayTime = 0xFFFD;
        }
    }

    SREG(91) = -1;
    R_PAUSE_MENU_MODE = 0;
    PreRender_Init(&play->pauseBgPreRender);
    PreRender_SetValuesSave(&play->pauseBgPreRender, SCREEN_WIDTH, SCREEN_HEIGHT, NULL, NULL, NULL);
    PreRender_SetValues(&play->pauseBgPreRender, SCREEN_WIDTH, SCREEN_HEIGHT, NULL, NULL);
    gTrnsnUnkState = 0;
    play->transitionMode = TRANS_MODE_OFF;
    FrameAdvance_Init(&play->frameAdvCtx);
    Rand_Seed((u32)osGetTime());
    Matrix_Init(&play->state);
    play->state.main = Play_Main;
    play->state.destroy = Play_Destroy;
    play->transitionTrigger = TRANS_TRIGGER_END;
    play->unk_11E16 = 0xFF;
    play->unk_11E18 = 0;
    play->unk_11DE9 = false;

    if (gSaveContext.gameMode != GAMEMODE_TITLE_SCREEN) {
        if (gSaveContext.nextTransitionType == TRANS_NEXT_TYPE_DEFAULT) {
            play->transitionType = ENTRANCE_INFO_END_TRANS_TYPE(
                gEntranceTable[((void)0, gSaveContext.entranceIndex) + baseSceneLayer].field); // Fade In
        } else {
            play->transitionType = gSaveContext.nextTransitionType;
            gSaveContext.nextTransitionType = TRANS_NEXT_TYPE_DEFAULT;
        }
    } else {
        play->transitionType = TRANS_TYPE_FADE_BLACK_SLOW;
    }

    ShrinkWindow_Init();
    TransitionFade_Init(&play->transitionFade);
    TransitionFade_SetType(&play->transitionFade, 3);
    TransitionFade_SetColor(&play->transitionFade, RGBA8(160, 160, 160, 255));
    TransitionFade_Start(&play->transitionFade);
    VisMono_Init(&gPlayVisMono);
    gVisMonoColor.a = 0;
    Flags_UnsetAllEnv(play);

    osSyncPrintf("ZELDA ALLOC SIZE=%x\n", THA_GetSize(&play->state.tha));
    zAllocSize = THA_GetSize(&play->state.tha);
    zAlloc = (uintptr_t)GAMESTATE_ALLOC_MC(&play->state, zAllocSize);
    zAllocAligned = (zAlloc + 8) & ~0xF;
    ZeldaArena_Init((void*)zAllocAligned, zAllocSize - (zAllocAligned - zAlloc));
    // "Zelda Heap"
    osSyncPrintf("ゼルダヒープ %08x-%08x\n", zAllocAligned,
                 (u8*)zAllocAligned + zAllocSize - (s32)(zAllocAligned - zAlloc));

    Fault_AddClient(&D_801614B8, ZeldaArena_Display, NULL, NULL);

    // In order to keep masks equipped on first load, we need to pre-set the age reqs for the item and slot
    if (CVarGetInteger(CVAR_ENHANCEMENT("AdultMasks"), 0) || CVarGetInteger(CVAR_CHEAT("TimelessEquipment"), 0)) {
        for (int i = ITEM_MASK_KEATON; i <= ITEM_MASK_TRUTH; i += 1) {
            gItemAgeReqs[i] = AGE_REQ_NONE;
        }
        if (INV_CONTENT(ITEM_TRADE_CHILD) >= ITEM_MASK_KEATON && INV_CONTENT(ITEM_TRADE_CHILD) <= ITEM_MASK_TRUTH) {
            gSlotAgeReqs[SLOT_TRADE_CHILD] = AGE_REQ_NONE;
        }
    } else {
        for (int i = ITEM_MASK_KEATON; i <= ITEM_MASK_TRUTH; i += 1) {
            gItemAgeReqs[i] = AGE_REQ_CHILD;
        }
        gSlotAgeReqs[SLOT_TRADE_CHILD] = AGE_REQ_CHILD;
    }

    // Handle Rocs Feather requirement
    gItemAgeReqs[ITEM_ROCS_FEATHER] = AGE_REQ_NONE;
    gSlotAgeReqs[SLOT_NAYRUS_LOVE] = AGE_REQ_NONE;

    func_800304DC(play, &play->actorCtx, play->linkActorEntry);

    while (!func_800973FC(play, &play->roomCtx)) {
        ; // Empty Loop
    }

    player = GET_PLAYER(play);
    Camera_InitPlayerSettings(&play->mainCamera, player);
    Camera_ChangeMode(&play->mainCamera, CAM_MODE_NORMAL);

    // OTRTODO: Bounds check cameraDataList to guard against scenes spawning the player with
    // an out of bounds background camera index. This requires adding an extra field to the
    // CollisionHeader struct to save the length of cameraDataList.
    // Fixes Dodongo's Cavern blue warp crash.
    {
        CollisionHeader* colHeader = BgCheck_GetCollisionHeader(&play->colCtx, BGCHECK_SCENE);

        u8 camId = player->actor.params & 0xFF;
        // If the player's start cam is out of bounds, set it to 0xFF so it isn't used.
        if (colHeader != NULL && (camId != 0xFF) && (camId >= colHeader->cameraDataListLen)) {
            player->actor.params |= 0xFF;
        }
    }

    playerStartBgCamIndex = player->actor.params & 0xFF;
    if (playerStartBgCamIndex != 0xFF) {
        osSyncPrintf("player has start camera ID (" VT_FGCOL(BLUE) "%d" VT_RST ")\n", playerStartBgCamIndex);
        Camera_ChangeDataIdx(&play->mainCamera, playerStartBgCamIndex);
    }

    if (YREG(15) == 32) {
        play->unk_1242B = 2;
    } else if (YREG(15) == 16) {
        play->unk_1242B = 1;
    } else {
        play->unk_1242B = 0;
    }
    Interface_SetSceneRestrictions(play);
    Environment_PlaySceneSequence(play);
    gSaveContext.seqId = play->sequenceCtx.seqId;
    gSaveContext.natureAmbienceId = play->sequenceCtx.natureAmbienceId;
    func_8002DF18(play, GET_PLAYER(play));
    AnimationContext_Update(play, &play->animationCtx);
    gSaveContext.respawnFlag = 0;

    // #region SOH [Stats]
    if (gSaveContext.ship.stats.sceneNum != gPlayState->sceneNum) {
        u16 idx = gSaveContext.ship.stats.tsIdx;
        gSaveContext.ship.stats.sceneTimestamps[idx].sceneTime = gSaveContext.ship.stats.sceneTimer / 2;
        gSaveContext.ship.stats.sceneTimestamps[idx].roomTime = gSaveContext.ship.stats.roomTimer / 2;
        gSaveContext.ship.stats.sceneTimestamps[idx].scene = gSaveContext.ship.stats.sceneNum;
        gSaveContext.ship.stats.sceneTimestamps[idx].room = gSaveContext.ship.stats.roomNum;
        gSaveContext.ship.stats.sceneTimestamps[idx].isRoom =
            gPlayState->sceneNum == gSaveContext.ship.stats.sceneTimestamps[idx].scene &&
            gPlayState->roomCtx.curRoom.num != gSaveContext.ship.stats.sceneTimestamps[idx].room;
        gSaveContext.ship.stats.tsIdx++;
        gSaveContext.ship.stats.sceneTimer = 0;
        gSaveContext.ship.stats.roomTimer = 0;
    } else if (gSaveContext.ship.stats.roomNum != gPlayState->roomCtx.curRoom.num) {
        u16 idx = gSaveContext.ship.stats.tsIdx;
        gSaveContext.ship.stats.sceneTimestamps[idx].roomTime = gSaveContext.ship.stats.roomTimer / 2;
        gSaveContext.ship.stats.sceneTimestamps[idx].scene = gSaveContext.ship.stats.sceneNum;
        gSaveContext.ship.stats.sceneTimestamps[idx].room = gSaveContext.ship.stats.roomNum;
        gSaveContext.ship.stats.sceneTimestamps[idx].isRoom =
            gPlayState->sceneNum == gSaveContext.ship.stats.sceneTimestamps[idx].scene &&
            gPlayState->roomCtx.curRoom.num != gSaveContext.ship.stats.sceneTimestamps[idx].room;
        gSaveContext.ship.stats.tsIdx++;
        gSaveContext.ship.stats.roomTimer = 0;
    }

    gSaveContext.ship.stats.sceneNum = gPlayState->sceneNum;
    gSaveContext.ship.stats.roomNum = gPlayState->roomCtx.curRoom.num;
    // #endregion

#if 0
    if (R_USE_DEBUG_CUTSCENE) {
        static u64 sDebugCutsceneScriptBuf[0xA00];

        gDebugCutsceneScript = sDebugCutsceneScriptBuf;
        PRINTF("\nkawauso_data=[%x]", gDebugCutsceneScript);

        // This hardcoded ROM address extends past the end of the ROM file.
        // Presumably the ROM was larger at a previous point in development when this debug feature was used.
        DmaMgr_DmaRomToRam(0x03FEB000, gDebugCutsceneScript, sizeof(sDebugCutsceneScriptBuf));
    }
#endif

    if (CVarGetInteger(CVAR_ENHANCEMENT("IvanCoopModeEnabled"), 0)) {
        Actor_Spawn(&play->actorCtx, play, gEnPartnerId, GET_PLAYER(play)->actor.world.pos.x,
                    GET_PLAYER(play)->actor.world.pos.y + Player_GetHeight(GET_PLAYER(play)) + 5.0f,
                    GET_PLAYER(play)->actor.world.pos.z, 0, 0, 0, 1);
    }

    // nextEntranceIndex was not initialized, so the previous value was carried over during soft resets.
    gPlayState->nextEntranceIndex = gSaveContext.entranceIndex;
}

void Play_Update(PlayState* play) {
    Input* input = play->state.input;
    s32 isPaused;
    s32 pad1;

    if ((SREG(1) < 0) || (DREG(0) != 0)) {
        SREG(1) = 0;
        ZeldaArena_Display();
    }

    if ((HREG(80) == 18) && (HREG(81) < 0)) {
        u32 i;
        s32 pad2;

        HREG(81) = 0;
        osSyncPrintf("object_exchange_rom_address %u\n", gObjectTableSize);
        osSyncPrintf("RomStart RomEnd   Size\n");

        for (i = 0; i < gObjectTableSize; i++) {
            ptrdiff_t size = gObjectTable[i].vromEnd - gObjectTable[i].vromStart;

            osSyncPrintf("%08x-%08x %08x(%8.3fKB)\n", gObjectTable[i].vromStart, gObjectTable[i].vromEnd, size,
                         size / 1024.0f);
        }

        osSyncPrintf("\n");
    }

    if ((HREG(81) == 18) && (HREG(82) < 0)) {
        HREG(82) = 0;
        // ActorOverlayTable_LogPrint();
    }

    if (CVarGetInteger(CVAR_SETTING("FreeLook.Enabled"), 0) && Player_InCsMode(play)) {
        play->manualCamera = false;
    }

    gSegments[4] = VIRTUAL_TO_PHYSICAL(play->objectCtx.status[play->objectCtx.mainKeepIndex].segment);
    gSegments[5] = VIRTUAL_TO_PHYSICAL(play->objectCtx.status[play->objectCtx.subKeepIndex].segment);
    gSegments[2] = VIRTUAL_TO_PHYSICAL(play->sceneSegment);

    if (FrameAdvance_Update(&play->frameAdvCtx, &input[1])) {
        if ((play->transitionMode == TRANS_MODE_OFF) && (play->transitionTrigger != TRANS_TRIGGER_OFF)) {
            play->transitionMode = TRANS_MODE_SETUP;
        }

        // #region SOH [Stats] Gameplay stats: Count button presses
        if (!gSaveContext.ship.stats.gameComplete) {
            if (CHECK_BTN_ALL(input[0].press.button, BTN_A)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_A]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_B)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_B]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_CUP)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_CUP]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_CRIGHT)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_CRIGHT]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_CLEFT)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_CLEFT]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_CDOWN)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_CDOWN]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_DUP)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_DUP]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_DRIGHT)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_DRIGHT]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_DDOWN)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_DDOWN]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_DLEFT)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_DLEFT]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_L)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_L]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_R)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_R]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_Z)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_Z]++;
            }
            if (CHECK_BTN_ALL(input[0].press.button, BTN_START)) {
                gSaveContext.ship.stats.count[COUNT_BUTTON_PRESSES_START]++;
            }

            // Start RTA timing on first non-c-up input after intro cutscene
            if (!gSaveContext.ship.stats.firstInput && !Player_InCsMode(play) &&
                ((input[0].press.button && input[0].press.button != 0x8) || input[0].rel.stick_x != 0 ||
                 input[0].rel.stick_y != 0)) {
                gSaveContext.ship.stats.firstInput = GetUnixTimestamp();
            }
        }
        // #endregion

        if (gTrnsnUnkState != 0) {
            switch (gTrnsnUnkState) {
                case 2:
                    if (TransitionUnk_Init(&sTrnsnUnk, 10, 7) == NULL) {
                        osSyncPrintf("fbdemo_init呼出し失敗！\n"); // "fbdemo_init call failed!"
                        gTrnsnUnkState = 0;
                    } else {
                        sTrnsnUnk.zBuffer = (u16*)gZBuffer;
                        gTrnsnUnkState = 3;
                        R_UPDATE_RATE = 1;
                    }
                    break;
                case 3:
                    func_800B23E8(&sTrnsnUnk);
                    break;
            }
        }

        if ((u32)play->transitionMode != TRANS_MODE_OFF) {
            switch (play->transitionMode) {
                case TRANS_MODE_SETUP:
                    if (play->transitionTrigger != TRANS_TRIGGER_END) {
                        s16 sceneLayer = 0;
                        Interface_ChangeAlpha(1);

                        if (gSaveContext.cutsceneIndex >= 0xFFF0) {
                            sceneLayer = SCENE_LAYER_CUTSCENE_FIRST + (gSaveContext.cutsceneIndex & 0xF);
                        }

                        // fade out bgm if "continue bgm" flag is not set
                        if (!(gEntranceTable[play->nextEntranceIndex + sceneLayer].field &
                              ENTRANCE_INFO_CONTINUE_BGM_FLAG)) {
                            // "Sound initalized. 111"
                            osSyncPrintf("\n\n\nサウンドイニシャル来ました。111");
                            if ((play->transitionType < TRANS_TYPE_MAX) && !Environment_IsForcedSequenceDisabled()) {
                                // "Sound initalized. 222"
                                osSyncPrintf("\n\n\nサウンドイニシャル来ました。222");
                                func_800F6964(0x14);
                                gSaveContext.seqId = (u8)NA_BGM_DISABLED;
                                gSaveContext.natureAmbienceId = NATURE_ID_DISABLED;
                            }
                        }
                    }

                    if (!R_TRANS_DBG_ENABLED) {
                        Gameplay_SetupTransition(play, play->transitionType);
                    } else {
                        Gameplay_SetupTransition(play, R_TRANS_DBG_TYPE);
                    }

                    if (play->transitionMode >= TRANS_MODE_FILL_WHITE_INIT) {
                        // non-instance modes break out of this switch
                        break;
                    }
                    FALLTHROUGH;
                case TRANS_MODE_INSTANCE_INIT:
                    play->transitionCtx.init(&play->transitionCtx.data);

                    // Circle Transition Types
                    if ((play->transitionCtx.transitionType >> 5) == 1) {
                        play->transitionCtx.setType(&play->transitionCtx.data,
                                                    play->transitionCtx.transitionType | TC_SET_PARAMS);
                    }

                    gSaveContext.transWipeSpeed = 14;

                    if ((play->transitionCtx.transitionType == TRANS_TYPE_WIPE_FAST) ||
                        (play->transitionCtx.transitionType == TRANS_TYPE_FILL_WHITE2)) {
                        //! @bug TRANS_TYPE_FILL_WHITE2 will never reach this code.
                        //! It is a non-instance type transition which doesn't run this case.
                        gSaveContext.transWipeSpeed = 28;
                    }

                    gSaveContext.transFadeDuration = 60;

                    if ((play->transitionCtx.transitionType == TRANS_TYPE_FADE_BLACK_FAST) ||
                        (play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE_FAST)) {
                        gSaveContext.transFadeDuration = 20;
                    } else if ((play->transitionCtx.transitionType == TRANS_TYPE_FADE_BLACK_SLOW) ||
                               (play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE_SLOW)) {
                        gSaveContext.transFadeDuration = 150;
                    } else if (play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE_INSTANT) {
                        gSaveContext.transFadeDuration = 2;
                    }

                    if ((play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE) ||
                        (play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE_FAST) ||
                        (play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE_SLOW) ||
                        (play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE_CS_DELAYED) ||
                        (play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE_INSTANT)) {
                        play->transitionCtx.setColor(&play->transitionCtx.data, RGBA8(160, 160, 160, 255));

                        if (play->transitionCtx.setEnvColor != NULL) {
                            play->transitionCtx.setEnvColor(&play->transitionCtx.data, RGBA8(160, 160, 160, 255));
                        }
                    } else if (play->transitionCtx.transitionType == TRANS_TYPE_FADE_GREEN) {
                        play->transitionCtx.setColor(&play->transitionCtx.data, RGBA8(140, 140, 100, 255));

                        if (play->transitionCtx.setEnvColor != NULL) {
                            play->transitionCtx.setEnvColor(&play->transitionCtx.data, RGBA8(140, 140, 100, 255));
                        }
                    } else if (play->transitionCtx.transitionType == TRANS_TYPE_FADE_BLUE) {
                        play->transitionCtx.setColor(&play->transitionCtx.data, RGBA8(70, 100, 110, 255));

                        if (play->transitionCtx.setEnvColor != NULL) {
                            play->transitionCtx.setEnvColor(&play->transitionCtx.data, RGBA8(70, 100, 110, 255));
                        }
                    } else {
                        play->transitionCtx.setColor(&play->transitionCtx.data, RGBA8(0, 0, 0, 0));

                        if (play->transitionCtx.setEnvColor != NULL) {
                            play->transitionCtx.setEnvColor(&play->transitionCtx.data, RGBA8(0, 0, 0, 0));
                        }
                    }

                    if (play->transitionTrigger == TRANS_TRIGGER_END) {
                        play->transitionCtx.setType(&play->transitionCtx.data, 1);
                    } else {
                        play->transitionCtx.setType(&play->transitionCtx.data, 2);
                    }

                    play->transitionCtx.start(&play->transitionCtx);

                    if (play->transitionCtx.transitionType == TRANS_TYPE_FADE_WHITE_CS_DELAYED) {
                        play->transitionMode = TRANS_MODE_INSTANCE_WAIT;
                    } else {
                        play->transitionMode = TRANS_MODE_INSTANCE_RUNNING;
                    }
                    break;

                case TRANS_MODE_INSTANCE_RUNNING:
                    if (play->transitionCtx.isDone(&play->transitionCtx.data)) {
                        if (play->transitionCtx.transitionType >= TRANS_TYPE_MAX) {
                            if (play->transitionTrigger == TRANS_TRIGGER_END) {
                                play->transitionCtx.destroy(&play->transitionCtx.data);
                                func_800BC88C(play);
                                play->transitionMode = TRANS_MODE_OFF;
                            }
                        } else if (play->transitionTrigger != TRANS_TRIGGER_END) {
                            play->state.running = false;

                            if (gSaveContext.gameMode != GAMEMODE_FILE_SELECT) {
                                SET_NEXT_GAMESTATE(&play->state, Play_Init, PlayState);
                                gSaveContext.entranceIndex = play->nextEntranceIndex;

                                if (gSaveContext.minigameState == 1) {
                                    gSaveContext.minigameState = 3;
                                }
                            } else {
                                SET_NEXT_GAMESTATE(&play->state, FileChoose_Init, FileChooseContext);
                            }
                        } else {
                            play->transitionCtx.destroy(&play->transitionCtx.data);
                            func_800BC88C(play);
                            play->transitionMode = TRANS_MODE_OFF;

                            if (gTrnsnUnkState == 3) {
                                TransitionUnk_Destroy(&sTrnsnUnk);
                                gTrnsnUnkState = 0;
                                R_UPDATE_RATE = 3;
                            }

                            // Transition end for standard transitions
                            GameInteractor_ExecuteOnTransitionEndHooks(play->sceneNum);
                        }

                        play->transitionTrigger = TRANS_TRIGGER_OFF;
                    } else {
                        play->transitionCtx.update(&play->transitionCtx.data, R_UPDATE_RATE);
                    }
                    break;
            }

            // update non-instance transitions
            switch (play->transitionMode) {
                case TRANS_MODE_FILL_WHITE_INIT:
                    sTransitionFillTimer = 0;
                    play->envCtx.fillScreen = true;
                    play->envCtx.screenFillColor[0] = 160;
                    play->envCtx.screenFillColor[1] = 160;
                    play->envCtx.screenFillColor[2] = 160;

                    if (play->transitionTrigger != TRANS_TRIGGER_END) {
                        play->envCtx.screenFillColor[3] = 0;
                        play->transitionMode = TRANS_MODE_FILL_IN;
                    } else {
                        play->envCtx.screenFillColor[3] = 255;
                        play->transitionMode = TRANS_MODE_FILL_OUT;
                    }
                    break;

                case TRANS_MODE_FILL_IN:
                    play->envCtx.screenFillColor[3] = (sTransitionFillTimer / 20.0f) * 255.0f;

                    if (sTransitionFillTimer >= 20) {
                        play->state.running = false;
                        SET_NEXT_GAMESTATE(&play->state, Play_Init, PlayState);
                        gSaveContext.entranceIndex = play->nextEntranceIndex;
                        play->transitionTrigger = TRANS_TRIGGER_OFF;
                        play->transitionMode = TRANS_MODE_OFF;
                    } else {
                        sTransitionFillTimer++;
                    }
                    break;

                case TRANS_MODE_FILL_OUT:
                    play->envCtx.screenFillColor[3] = (1 - sTransitionFillTimer / 20.0f) * 255.0f;

                    if (sTransitionFillTimer >= 20) {
                        gTrnsnUnkState = 0;
                        R_UPDATE_RATE = 3;
                        play->transitionTrigger = TRANS_TRIGGER_OFF;
                        play->transitionMode = TRANS_MODE_OFF;
                        play->envCtx.fillScreen = false;
                    } else {
                        sTransitionFillTimer++;
                    }
                    break;

                case TRANS_MODE_FILL_BROWN_INIT:
                    sTransitionFillTimer = 0;
                    play->envCtx.fillScreen = true;
                    play->envCtx.screenFillColor[0] = 170;
                    play->envCtx.screenFillColor[1] = 160;
                    play->envCtx.screenFillColor[2] = 150;

                    if (play->transitionTrigger != TRANS_TRIGGER_END) {
                        play->envCtx.screenFillColor[3] = 0;
                        play->transitionMode = TRANS_MODE_FILL_IN;
                    } else {
                        play->envCtx.screenFillColor[3] = 255;
                        play->transitionMode = TRANS_MODE_FILL_OUT;
                    }
                    break;

                case TRANS_MODE_INSTANT:
                    if (play->transitionTrigger != TRANS_TRIGGER_END) {
                        play->state.running = 0;
                        SET_NEXT_GAMESTATE(&play->state, Play_Init, PlayState);
                        gSaveContext.entranceIndex = play->nextEntranceIndex;
                        play->transitionTrigger = TRANS_TRIGGER_OFF;
                        play->transitionMode = TRANS_MODE_OFF;
                    } else {
                        gTrnsnUnkState = 0;
                        R_UPDATE_RATE = 3;
                        play->transitionTrigger = TRANS_TRIGGER_OFF;
                        play->transitionMode = TRANS_MODE_OFF;
                    }
                    break;

                case TRANS_MODE_INSTANCE_WAIT:
                    if (gSaveContext.cutsceneTransitionControl != 0) {
                        play->transitionMode = TRANS_MODE_INSTANCE_RUNNING;
                    }
                    break;

                case TRANS_MODE_SANDSTORM_INIT:
                    if (play->transitionTrigger != TRANS_TRIGGER_END) {
                        play->envCtx.sandstormState = SANDSTORM_FILL;
                        play->transitionMode = TRANS_MODE_SANDSTORM;
                    } else {
                        play->envCtx.sandstormState = SANDSTORM_UNFILL;
                        play->envCtx.sandstormPrimA = 255;
                        play->envCtx.sandstormEnvA = 255;
                        play->transitionMode = TRANS_MODE_SANDSTORM;
                    }
                    break;

                case TRANS_MODE_SANDSTORM:
                    Audio_PlaySoundGeneral(NA_SE_EV_SAND_STORM - SFX_FLAG, &gSfxDefaultPos, 4,
                                           &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                                           &gSfxDefaultReverb);

                    if (play->transitionTrigger == TRANS_TRIGGER_END) {
                        if (play->envCtx.sandstormPrimA < 110) {
                            gTrnsnUnkState = 0;
                            R_UPDATE_RATE = 3;
                            play->transitionTrigger = TRANS_TRIGGER_OFF;
                            play->transitionMode = TRANS_MODE_OFF;

                            // Transition end for sandstorm effect (delayed until effect is finished)
                            GameInteractor_ExecuteOnTransitionEndHooks(play->sceneNum);
                        }
                    } else {
                        if (play->envCtx.sandstormEnvA == 255) {
                            play->state.running = false;
                            SET_NEXT_GAMESTATE(&play->state, Play_Init, PlayState);
                            gSaveContext.entranceIndex = play->nextEntranceIndex;
                            play->transitionTrigger = TRANS_TRIGGER_OFF;
                            play->transitionMode = TRANS_MODE_OFF;
                        }
                    }
                    break;

                case TRANS_MODE_SANDSTORM_END_INIT:
                    if (play->transitionTrigger == TRANS_TRIGGER_END) {
                        play->envCtx.sandstormState = SANDSTORM_DISSIPATE;
                        play->envCtx.sandstormPrimA = 255;
                        play->envCtx.sandstormEnvA = 255;
                        // "It's here!!!!!!!!!"
                        LOG_STRING("来た!!!!!!!!!!!!!!!!!!!!!");
                        play->transitionMode = TRANS_MODE_SANDSTORM_END;
                    } else {
                        play->transitionMode = TRANS_MODE_SANDSTORM_INIT;
                    }
                    break;

                case TRANS_MODE_SANDSTORM_END:
                    Audio_PlaySoundGeneral(NA_SE_EV_SAND_STORM - SFX_FLAG, &gSfxDefaultPos, 4,
                                           &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                                           &gSfxDefaultReverb);

                    if (play->transitionTrigger == TRANS_TRIGGER_END) {
                        if (play->envCtx.sandstormPrimA <= 0) {
                            gTrnsnUnkState = 0;
                            R_UPDATE_RATE = 3;
                            play->transitionTrigger = TRANS_TRIGGER_OFF;
                            play->transitionMode = TRANS_MODE_OFF;

                            // Transition end for sandstorm effect (delayed until effect is finished)
                            GameInteractor_ExecuteOnTransitionEndHooks(play->sceneNum);
                        }
                    }
                    break;

                case TRANS_MODE_CS_BLACK_FILL_INIT:
                    sTransitionFillTimer = 0;
                    play->envCtx.fillScreen = true;
                    play->envCtx.screenFillColor[0] = 0;
                    play->envCtx.screenFillColor[1] = 0;
                    play->envCtx.screenFillColor[2] = 0;
                    play->envCtx.screenFillColor[3] = 255;
                    play->transitionMode = TRANS_MODE_CS_BLACK_FILL;
                    break;

                case TRANS_MODE_CS_BLACK_FILL:
                    if (gSaveContext.cutsceneTransitionControl != 0) {
                        play->envCtx.screenFillColor[3] = gSaveContext.cutsceneTransitionControl;

                        if (gSaveContext.cutsceneTransitionControl <= 100) {
                            gTrnsnUnkState = 0;
                            R_UPDATE_RATE = 3;
                            play->transitionTrigger = TRANS_TRIGGER_OFF;
                            play->transitionMode = TRANS_MODE_OFF;
                        }
                    }
                    break;
            }
        }

        PLAY_LOG(3533);

        if (1 && (gTrnsnUnkState != 3)) {
            PLAY_LOG(3542);

            if ((gSaveContext.gameMode == GAMEMODE_NORMAL) && (play->msgCtx.msgMode == MSGMODE_NONE) &&
                (play->gameOverCtx.state == GAMEOVER_INACTIVE)) {
                KaleidoSetup_Update(play);
            }

            PLAY_LOG(3551);
            isPaused = (play->pauseCtx.state != 0) || (play->pauseCtx.debugState != 0);

            PLAY_LOG(3555);
            AnimationContext_Reset(&play->animationCtx);

            PLAY_LOG(3561);
            Object_UpdateBank(&play->objectCtx);

            PLAY_LOG(3577);

            if (!isPaused && (IREG(72) == 0)) {
                PLAY_LOG(3580);

                play->gameplayFrames++;
                func_800AA178(true);

                // Gameplay stat tracking
                if (!gSaveContext.ship.stats.gameComplete &&
                    (!IS_BOSS_RUSH || !gSaveContext.ship.quest.data.bossRush.isPaused)) {
                    gSaveContext.ship.stats.playTimer++;
                    gSaveContext.ship.stats.sceneTimer++;
                    gSaveContext.ship.stats.roomTimer++;

                    if (CVarGetInteger(CVAR_ENHANCEMENT("MMBunnyHood"), BUNNY_HOOD_VANILLA) != BUNNY_HOOD_VANILLA &&
                        Player_GetMask(play) == PLAYER_MASK_BUNNY) {
                        gSaveContext.ship.stats.count[COUNT_TIME_BUNNY_HOOD]++;
                    }
                }

                if (play->actorCtx.freezeFlashTimer && (play->actorCtx.freezeFlashTimer-- < 5)) {
                    if (GameInteractor_Should(VB_FLASH_SCREEN_FOR_FINISHING_BLOW, true)) {
                        osSyncPrintf("FINISH=%d\n", play->actorCtx.freezeFlashTimer);

                        if ((play->actorCtx.freezeFlashTimer > 0) && ((play->actorCtx.freezeFlashTimer % 2) != 0)) {
                            play->envCtx.fillScreen = true;
                            play->envCtx.screenFillColor[0] = play->envCtx.screenFillColor[1] =
                                play->envCtx.screenFillColor[2] = 150;
                            play->envCtx.screenFillColor[3] = 80;
                        } else {
                            play->envCtx.fillScreen = false;
                        }
                    }
                } else {
                    PLAY_LOG(3606);
                    func_800973FC(play, &play->roomCtx);

                    PLAY_LOG(3612);
                    CollisionCheck_AT(play, &play->colChkCtx);

                    PLAY_LOG(3618);
                    CollisionCheck_OC(play, &play->colChkCtx);

                    PLAY_LOG(3624);
                    CollisionCheck_Damage(play, &play->colChkCtx);

                    PLAY_LOG(3631);
                    CollisionCheck_ClearContext(play, &play->colChkCtx);

                    PLAY_LOG(3637);

                    if (!play->unk_11DE9) {
                        Actor_UpdateAll(play, &play->actorCtx);
                    }

                    PLAY_LOG(3643);
                    func_80064558(play, &play->csCtx);

                    PLAY_LOG(3648);
                    func_800645A0(play, &play->csCtx);

                    PLAY_LOG(3651);
                    Effect_UpdateAll(play);

                    PLAY_LOG(3657);
                    EffectSs_UpdateAll(play);

                    PLAY_LOG(3662);
                }
            } else {
                func_800AA178(false);
            }

            PLAY_LOG(3672);
            func_80095AA0(play, &play->roomCtx.curRoom, &input[1], 0);

            PLAY_LOG(3675);
            func_80095AA0(play, &play->roomCtx.prevRoom, &input[1], 1);

            PLAY_LOG(3677);

            if (play->unk_1242B != 0) {
                if (CHECK_BTN_ALL(input[0].press.button, BTN_CUP)) {
                    if ((play->pauseCtx.state != 0) || (play->pauseCtx.debugState != 0)) {
                        // "Changing viewpoint is prohibited due to the kaleidoscope"
                        osSyncPrintf(VT_FGCOL(CYAN) "カレイドスコープ中につき視点変更を禁止しております\n" VT_RST);
                    } else if (Player_InCsMode(play)) {
                        // "Changing viewpoint is prohibited during the cutscene"
                        osSyncPrintf(VT_FGCOL(CYAN) "デモ中につき視点変更を禁止しております\n" VT_RST);
                    } else if (YREG(15) == 0x10) {
                        Audio_PlaySoundGeneral(NA_SE_SY_ERROR, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                               &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
                    } else {
                        // C-Up toggle for houses, move between pivot camera and fixed camera
                        // Toggle viewpoint between VIEWPOINT_LOCKED and VIEWPOINT_PIVOT
                        Play_SetViewpoint(play, play->unk_1242B ^ 3);
                    }
                }

                Play_RequestViewpointBgCam(play);
            }

            PLAY_LOG(3708);
            SkyboxDraw_Update(&play->skyboxCtx);

            PLAY_LOG(3716);

            if ((play->pauseCtx.state != 0) || (play->pauseCtx.debugState != 0)) {
                PLAY_LOG(3721);
                KaleidoScopeCall_Update(play);
            } else if (play->gameOverCtx.state != GAMEOVER_INACTIVE) {
                PLAY_LOG(3727);
                GameOver_Update(play);
            } else {
                PLAY_LOG(3733);
                Message_Update(play);
            }

            PLAY_LOG(3737);

            PLAY_LOG(3742);
            Interface_Update(play);

            PLAY_LOG(3765);
            AnimationContext_Update(play, &play->animationCtx);

            PLAY_LOG(3771);
            SoundSource_UpdateAll(play);

            PLAY_LOG(3777);
            ShrinkWindow_Update(R_UPDATE_RATE);

            PLAY_LOG(3783);
            TransitionFade_Update(&play->transitionFade, R_UPDATE_RATE);
        } else {
            goto skip;
        }
    }

    PLAY_LOG(3799);

skip:
    PLAY_LOG(3801);

    GameInteractor_ExecuteOnCameraState(play);

    if (!isPaused || gDbgCamEnabled) {
        s32 i;

        play->nextCamera = play->activeCamera;

        PLAY_LOG(3806);

        for (i = 0; i < NUM_CAMS; i++) {
            if ((i != play->nextCamera) && (play->cameraPtrs[i] != NULL)) {
                PLAY_LOG(3809);
                Camera_Update(play->cameraPtrs[i]);
            }
        }

        Camera_Update(play->cameraPtrs[play->nextCamera]);

        PLAY_LOG(3814);
    }

    PLAY_LOG(3816);
    Environment_Update(play, &play->envCtx, &play->lightCtx, &play->pauseCtx, &play->msgCtx, &play->gameOverCtx,
                       play->state.gfxCtx);
}

void Play_DrawOverlayElements(PlayState* play) {
    if ((play->pauseCtx.state != 0) || (play->pauseCtx.debugState != 0)) {
        KaleidoScopeCall_Draw(play);
    }

    if (gSaveContext.gameMode == GAMEMODE_NORMAL) {
        Interface_Draw(play);
    }

    Message_Draw(play);

    if (play->gameOverCtx.state != GAMEOVER_INACTIVE) {
        GameOver_FadeInLights(play);
    }
}

void Play_Draw(PlayState* play) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Lights* sp228;
    Vec3f sp21C;

    // #region SOH [Port] Frame buffer effects for pause menu
    // Track render size when paused and that a copy was performed
    static u32 lastPauseWidth;
    static u32 lastPauseHeight;
    static bool lastAltAssets;
    static bool hasCapturedPauseBuffer;
    bool recapturePauseBuffer = false;

    // If the size has changed, alt assets toggled, or dropped frames leading to the buffer not being copied,
    // set the prerender state back to setup to copy a new frame.
    // This requires not rendering kaleido during this copy to avoid kaleido itself being copied too.
    if ((R_PAUSE_MENU_MODE == 2 || R_PAUSE_MENU_MODE == 3) &&
        (lastPauseWidth != OTRGetGameRenderWidth() || lastPauseHeight != OTRGetGameRenderHeight() ||
         lastAltAssets != ResourceMgr_IsAltAssetsEnabled() || !hasCapturedPauseBuffer)) {
        R_PAUSE_MENU_MODE = 1;
        recapturePauseBuffer = true;
    }
    // #endregion

    OPEN_DISPS(gfxCtx);

    gSegments[4] = VIRTUAL_TO_PHYSICAL(play->objectCtx.status[play->objectCtx.mainKeepIndex].segment);
    gSegments[5] = VIRTUAL_TO_PHYSICAL(play->objectCtx.status[play->objectCtx.subKeepIndex].segment);
    gSegments[2] = VIRTUAL_TO_PHYSICAL(play->sceneSegment);

    gSPSegment(POLY_OPA_DISP++, 0x00, NULL);
    gSPSegment(POLY_XLU_DISP++, 0x00, NULL);
    gSPSegment(OVERLAY_DISP++, 0x00, NULL);

    gSPSegment(POLY_OPA_DISP++, 0x04, play->objectCtx.status[play->objectCtx.mainKeepIndex].segment);
    gSPSegment(POLY_XLU_DISP++, 0x04, play->objectCtx.status[play->objectCtx.mainKeepIndex].segment);
    gSPSegment(OVERLAY_DISP++, 0x04, play->objectCtx.status[play->objectCtx.mainKeepIndex].segment);

    gSPSegment(POLY_OPA_DISP++, 0x05, play->objectCtx.status[play->objectCtx.subKeepIndex].segment);
    gSPSegment(POLY_XLU_DISP++, 0x05, play->objectCtx.status[play->objectCtx.subKeepIndex].segment);
    gSPSegment(OVERLAY_DISP++, 0x05, play->objectCtx.status[play->objectCtx.subKeepIndex].segment);

    gSPSegment(POLY_OPA_DISP++, 0x02, play->sceneSegment);
    gSPSegment(POLY_XLU_DISP++, 0x02, play->sceneSegment);
    gSPSegment(OVERLAY_DISP++, 0x02, play->sceneSegment);

    Gfx_SetupFrame(gfxCtx, 0, 0, 0);

    if ((HREG(80) != 10) || (HREG(82) != 0)) {
        GameInteractor_ExecuteOnPlayDrawBegin();

        POLY_OPA_DISP = Play_SetFog(play, POLY_OPA_DISP);
        POLY_XLU_DISP = Play_SetFog(play, POLY_XLU_DISP);

        // SoH multiplayer split-screen: when PiP is on AND P2 exists AND
        // we're not in a cinematic state, the main view renders to the
        // LEFT HALF of the screen. The PiP block at the end of Play_Draw
        // then renders P2's view to the RIGHT HALF. After the PiP block
        // we restore to full-screen so the HUD draws across both halves
        // (each player sees their relevant half — P1 sees hearts top-left,
        // P2 sees C-buttons top-right).
        {
            // SoH multiplayer: split-screen needs to be OFF for fixed-cam
            // setups where the camera isn't attached to the player (prerendered
            // backdrops, cutscene cameras, boss intros, locked-fixed minigame
            // cameras). Everything else — diving, spiral stairs, crawlspaces,
            // shops, free-look — runs splitscreen fine because the camera
            // still tracks the player.
            //
            // History: originally this was an ALLOW-list of only
            // NORMAL/DUNGEON/HORSE settings. Too narrow — Zora's Domain spiral
            // (CAM_SET_TOWER_CLIMB), diving (CAM_SET_PIVOT_WATER_SURFACE),
            // crawlspaces, jail cells, lowering platforms, shops etc. all
            // weren't in the list, so splitscreen would flicker off whenever
            // the player crossed into one of these zones and back. Inverted
            // to a deny-list of the actual fixed/cutscene cameras: any new
            // gameplay-style camera setting added later defaults to allowing
            // splitscreen rather than disabling it.
            //
            // msgMode != NONE further covers ocarina and dialog (handled by
            // the existing splitScreenActive check below).
            Camera* coopSplitMainCam = play->cameraPtrs[MAIN_CAM];
            s32 coopSplitFreeCam = 1;
            if (coopSplitMainCam != NULL) {
                switch (coopSplitMainCam->setting) {
                    // Prerendered backdrops — camera position locked in scene
                    // data, halving the view tears the prerender.
                    case CAM_SET_PREREND_FIXED:
                    case CAM_SET_PREREND_PIVOT:
                    case CAM_SET_PREREND_SIDE_SCROLL:
                    // Generic cutscene cameras
                    case CAM_SET_CS_0:
                    case CAM_SET_CS_TWISTED_HALLWAY:
                    case CAM_SET_CS_3:
                    case CAM_SET_CS_ATTENTION:
                    case CAM_SET_CS_C:
                    case CAM_SET_SLOW_CHEST_CS:
                    case CAM_SET_TURN_AROUND:
                    case CAM_SET_FREE2:
                    case CAM_SET_SCENE_TRANSITION:
                    case CAM_SET_DOOR0:
                    case CAM_SET_START0:
                    case CAM_SET_START1:
                    // Boss intro / fixed boss cameras — these reposition far
                    // from the player or pan across the arena, splitscreen
                    // would split mid-cinematic which looks broken.
                    case CAM_SET_BOSS_GOHMA:
                    case CAM_SET_BOSS_DODONGO:
                    case CAM_SET_BOSS_BARINADE:
                    case CAM_SET_BOSS_PHANTOM_GANON:
                    case CAM_SET_BOSS_VOLVAGIA:
                    case CAM_SET_BOSS_BONGO:
                    case CAM_SET_BOSS_MORPHA:
                    case CAM_SET_BOSS_TWINROVA_PLATFORM:
                    case CAM_SET_BOSS_TWINROVA_FLOOR:
                    case CAM_SET_BOSS_GANONDORF:
                    case CAM_SET_BOSS_GANON:
                    // Fixed minigame / specific-area cameras
                    case CAM_SET_MARKET_BALCONY:
                    case CAM_SET_CHU_BOWLING:
                    case CAM_SET_FISHING:
                    case CAM_SET_FOREST_BIRDS_EYE:
                    case CAM_SET_MEADOW_BIRDS_EYE:
                    case CAM_SET_FIRE_BIRDS_EYE:
                    case CAM_SET_FIRE_PLATFORM:
                    case CAM_SET_FIRE_STAIRCASE:
                    case CAM_SET_FOREST_DEFEAT_POE:
                    case CAM_SET_BIG_OCTO:
                    case CAM_SET_JABU_TENTACLE:
                        coopSplitFreeCam = 0;
                        break;
                    default:
                        // All other settings (NORMAL0..4, DUNGEON0..2, HORSE,
                        // PIVOT_WATER_SURFACE, TOWER_CLIMB, CRAWLSPACE,
                        // PIVOT_CORNER, PIVOT_VERTICAL, PIVOT_FROM_SIDE,
                        // PIVOT_SHOP_BROWSING, PIVOT_IN_FRONT, DIRECTED_YAW,
                        // DOORC, FREE0, BEAN_*, etc.) allow splitscreen.
                        coopSplitFreeCam = 1;
                        break;
                }
            }
            s32 splitScreenActive = CVarGetInteger(CVAR_ENHANCEMENT("LocalCoop.PiPPrototype"), 0) &&
                                    !Play_InCsMode(play) &&
                                    play->pauseCtx.state == 0 &&
                                    play->pauseCtx.debugState == 0 &&
                                    play->gameOverCtx.state == GAMEOVER_INACTIVE &&
                                    play->transitionTrigger == TRANS_TRIGGER_OFF &&
                                    play->transitionMode == 0 &&
                                    play->msgCtx.msgMode == MSGMODE_NONE &&
                                    coopSplitFreeCam;
            // SoH multiplayer: stage current splitscreen state for z_player.c
            // to read. P2's stick conversion needs to know whether the
            // player is currently seeing P2's PiP camera (interpret stick
            // relative to it) or P1's main camera (interpret stick
            // relative to that). Computed once here, consumed by both
            // this gate and elsewhere — single source of truth.
            gCoopSplitScreenActive = splitScreenActive;
            if (splitScreenActive) {
                Actor* coopP = play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
                s32 coopP2Exists = 0;
                for (; coopP != NULL; coopP = coopP->next) { if (coopP->category != ACTORCAT_PLAYER || PLAYER_GET_INDEX(coopP) == 0) continue;
                    if (coopP->category == ACTORCAT_PLAYER) {
                        coopP2Exists = 1;
                        break;
                    }
                }
                if (coopP2Exists) {
                    // SoH multiplayer: shrink P1's viewport to the left
                    // half of the screen so PiP can own the right half
                    // cleanly. Gated on gCoopPiPStableFrames >= 2 so
                    // we don't shrink before PiP is ready to render —
                    // otherwise the right side flashes black during
                    // the warmup. P1 keeps full screen during the
                    // 2-frame warmup, then snaps to left-half once PiP
                    // is ready.
                    if (gCoopPiPStableFrames >= 2) {
                        Viewport leftHalf;
                        leftHalf.topY = 0;
                        leftHalf.bottomY = SCREEN_HEIGHT;
                        leftHalf.leftX = 0;
                        leftHalf.rightX = SCREEN_WIDTH / 2;
                        View_SetViewport(&play->view, &leftHalf);
                    }
                }
            }
        }

        func_800AA460(&play->view, play->view.fovy, play->view.zNear, play->lightCtx.fogFar);
        func_800AAA50(&play->view, 15);

        // Flip the projections and invert culling for the OPA and XLU display buffers
        // These manage the world and effects when we are not drawing kaleido
        if (R_PAUSE_MENU_MODE <= 1 && CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0)) {
            gSPSetExtraGeometryMode(POLY_OPA_DISP++, G_EX_INVERT_CULLING);
            gSPSetExtraGeometryMode(POLY_XLU_DISP++, G_EX_INVERT_CULLING);
            gSPMatrix(POLY_OPA_DISP++, play->view.projectionFlippedPtr, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
            gSPMatrix(POLY_XLU_DISP++, play->view.projectionFlippedPtr, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
            gSPMatrix(POLY_OPA_DISP++, play->view.viewingPtr, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
            gSPMatrix(POLY_XLU_DISP++, play->view.viewingPtr, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        }

        // The billboard matrix temporarily stores the viewing matrix
        Matrix_MtxToMtxF(&play->view.viewing, &play->billboardMtxF);
        Matrix_MtxToMtxF(&play->view.projection, &play->viewProjectionMtxF);
        Matrix_Mult(&play->viewProjectionMtxF, MTXMODE_NEW);
        // The billboard is still a viewing matrix at this stage
        Matrix_Mult(&play->billboardMtxF, MTXMODE_APPLY);
        Matrix_Get(&play->viewProjectionMtxF);
        play->billboardMtxF.mf[0][3] = play->billboardMtxF.mf[1][3] = play->billboardMtxF.mf[2][3] =
            play->billboardMtxF.mf[3][0] = play->billboardMtxF.mf[3][1] = play->billboardMtxF.mf[3][2] = 0.0f;
        // This transpose is where the viewing matrix is properly converted into a billboard matrix
        Matrix_Transpose(&play->billboardMtxF);
        play->billboardMtx =
            Matrix_MtxFToMtx(MATRIX_CHECKFLOATS(&play->billboardMtxF), Graph_Alloc(gfxCtx, sizeof(Mtx)));

        gSPSegment(POLY_OPA_DISP++, 0x01, play->billboardMtx);

        if ((HREG(80) != 10) || (HREG(92) != 0)) {
            Gfx* gfxP;
            Gfx* sp1CC = POLY_OPA_DISP;

            gfxP = Graph_GfxPlusOne(sp1CC);
            gSPDisplayList(OVERLAY_DISP++, gfxP);
            gSPGrayscale(gfxP++, false);

            if ((play->transitionMode == TRANS_MODE_INSTANCE_RUNNING) ||
                (play->transitionMode == TRANS_MODE_INSTANCE_WAIT) ||
                (play->transitionCtx.transitionType >= TRANS_TYPE_MAX)) {
                View view;

                View_Init(&view, gfxCtx);
                view.flags = 2 | 8;

                SET_FULLSCREEN_VIEWPORT(&view);

                func_800AB9EC(&view, 15, &gfxP);
                play->transitionCtx.draw(&play->transitionCtx.data, &gfxP);
            }

            TransitionFade_Draw(&play->transitionFade, &gfxP);

            if (gVisMonoColor.a > 0) {
                gPlayVisMono.vis.primColor.rgba = gVisMonoColor.rgba;
                VisMono_Draw(&gPlayVisMono, &gfxP);
            }

            gSPEndDisplayList(gfxP++);
            Graph_BranchDlist(sp1CC, gfxP);
            POLY_OPA_DISP = gfxP;
        }

        if (gTrnsnUnkState == 3) {
            Gfx* sp88 = POLY_OPA_DISP;

            TransitionUnk_Draw(&sTrnsnUnk, &sp88);
            POLY_OPA_DISP = sp88;
            goto Play_Draw_DrawOverlayElements;
        }

        PreRender_SetValues(&play->pauseBgPreRender, SCREEN_WIDTH, SCREEN_HEIGHT, gfxCtx->curFrameBuffer, gZBuffer);

        if (R_PAUSE_MENU_MODE == 2) {
            // Wait for the previous frame's display list to be processed,
            // so that `pauseBgPreRender.fbufSave` and `pauseBgPreRender.cvgSave` are filled with the appropriate
            // content and can be used by `PreRender_ApplyFilters` below.
            MsgEvent_SendNullTask();

            PreRender_Calc(&play->pauseBgPreRender);

            R_PAUSE_MENU_MODE = 3;
        } else if (R_PAUSE_MENU_MODE >= 4) {
            R_PAUSE_MENU_MODE = 0;
        }

        if (R_PAUSE_MENU_MODE == 3) {
            Gfx* gfxP = POLY_OPA_DISP;

            // SOH [Port] Draw game framebuffer using our custom handling
            // func_800C24BC(&play->pauseBgPreRender, &gfxP);
            FB_DrawFromFramebuffer(&gfxP, gPauseFrameBuffer, 255);
            POLY_OPA_DISP = gfxP;

            goto Play_Draw_DrawOverlayElements;
        }

        if ((HREG(80) != 10) || (HREG(83) != 0)) {
            if (play->skyboxId && (play->skyboxId != SKYBOX_UNSET_1D) && !play->envCtx.skyboxDisabled) {
                if ((play->skyboxId == SKYBOX_NORMAL_SKY) || (play->skyboxId == SKYBOX_CUTSCENE_MAP)) {
                    Environment_UpdateSkybox(play, play->skyboxId, &play->envCtx, &play->skyboxCtx);
                    SkyboxDraw_Draw(&play->skyboxCtx, gfxCtx, play->skyboxId, play->envCtx.skyboxBlend,
                                    play->view.eye.x, play->view.eye.y, play->view.eye.z);
                } else if (play->skyboxCtx.unk_140 == 0) {
                    SkyboxDraw_Draw(&play->skyboxCtx, gfxCtx, play->skyboxId, 0, play->view.eye.x, play->view.eye.y,
                                    play->view.eye.z);
                }
            }
        }

        if ((HREG(80) != 10) || (HREG(90) & 2)) {
            if (!play->envCtx.sunMoonDisabled) {
                Environment_DrawSunAndMoon(play);
            }
        }

        if ((HREG(80) != 10) || (HREG(90) & 1)) {
            Environment_DrawSkyboxFilters(play);
        }

        if ((HREG(80) != 10) || (HREG(90) & 4)) {
            Environment_UpdateLightningStrike(play);
            Environment_DrawLightning(play, 0);
        }

        if ((HREG(80) != 10) || (HREG(90) & 8)) {
            sp228 = LightContext_NewLights(&play->lightCtx, gfxCtx);
            Lights_BindAll(sp228, play->lightCtx.listHead, NULL);
            Lights_Draw(sp228, gfxCtx);
        }

        if ((HREG(80) != 10) || (HREG(84) != 0)) {
            if (VREG(94) == 0) {
                s32 roomDrawFlags;

                if (HREG(80) != 10) {
                    roomDrawFlags = 3;
                } else {
                    roomDrawFlags = HREG(84);
                }
                Scene_Draw(play);
                Room_Draw(play, &play->roomCtx.curRoom, roomDrawFlags & 3);
                Room_Draw(play, &play->roomCtx.prevRoom, roomDrawFlags & 3);
            }
        }

        if ((HREG(80) != 10) || (HREG(83) != 0)) {
            if ((play->skyboxCtx.unk_140 != 0) && (GET_ACTIVE_CAM(play)->setting != CAM_SET_PREREND_FIXED)) {
                Vec3f quakeOffset;

                Camera_GetSkyboxOffset(&quakeOffset, GET_ACTIVE_CAM(play));
                SkyboxDraw_Draw(&play->skyboxCtx, gfxCtx, play->skyboxId, 0, play->view.eye.x + quakeOffset.x,
                                play->view.eye.y + quakeOffset.y, play->view.eye.z + quakeOffset.z);
            }
        }

        if (play->envCtx.unk_EE[1] != 0) {
            Environment_DrawRain(play, &play->view, gfxCtx);
        }

        if ((HREG(80) != 10) || (HREG(84) != 0)) {
            Environment_FillScreen(gfxCtx, 0, 0, 0, play->unk_11E18, FILL_SCREEN_OPA);
        }

        if ((HREG(80) != 10) || (HREG(85) != 0)) {
            func_800315AC(play, &play->actorCtx);
        }

        if ((HREG(80) != 10) || (HREG(86) != 0)) {
            if (!play->envCtx.sunMoonDisabled) {
                sp21C.x = play->view.eye.x + play->envCtx.sunPos.x;
                sp21C.y = play->view.eye.y + play->envCtx.sunPos.y;
                sp21C.z = play->view.eye.z + play->envCtx.sunPos.z;
                Environment_DrawSunLensFlare(play, &play->envCtx, &play->view, gfxCtx, sp21C, 0);
            }
            Environment_DrawCustomLensFlare(play);
        }

        if ((HREG(80) != 10) || (HREG(87) != 0)) {
            if (MREG(64) != 0) {
                Environment_FillScreen(gfxCtx, MREG(65), MREG(66), MREG(67), MREG(68),
                                       FILL_SCREEN_OPA | FILL_SCREEN_XLU);
            }

            switch (play->envCtx.fillScreen) {
                case 1:
                    Environment_FillScreen(gfxCtx, play->envCtx.screenFillColor[0], play->envCtx.screenFillColor[1],
                                           play->envCtx.screenFillColor[2], play->envCtx.screenFillColor[3],
                                           FILL_SCREEN_OPA | FILL_SCREEN_XLU);
                    break;
                default:
                    break;
            }
        }

        if ((HREG(80) != 10) || (HREG(88) != 0)) {
            if (play->envCtx.sandstormState != SANDSTORM_OFF) {
                Environment_DrawSandstorm(play, play->envCtx.sandstormState);
            }
        }

        if ((HREG(80) != 10) || (HREG(93) != 0)) {
            DebugDisplay_DrawObjects(play);
        }

        if ((R_PAUSE_MENU_MODE == 1) || (gTrnsnUnkState == 1)) {
            Gfx* gfxP = OVERLAY_DISP;

            // Copy the frame buffer contents at this point in the display list to the zbuffer
            // The zbuffer must then stay untouched until unpausing
            play->pauseBgPreRender.fbuf = gfxCtx->curFrameBuffer;
            play->pauseBgPreRender.fbufSave = (u16*)gZBuffer;
            // SOH [Port] Use our custom copy method instead of the prerender system
            // func_800C1F20(&play->pauseBgPreRender, &gfxP);
            if (R_PAUSE_MENU_MODE == 1) {
                play->pauseBgPreRender.cvgSave = (u8*)gfxCtx->curFrameBuffer;
                // func_800C20B4(&play->pauseBgPreRender, &gfxP);
                R_PAUSE_MENU_MODE = 2;

                // #region SOH [Port] Custom handling for pause prerender background capture
                lastPauseWidth = OTRGetGameRenderWidth();
                lastPauseHeight = OTRGetGameRenderHeight();
                lastAltAssets = ResourceMgr_IsAltAssetsEnabled();
                hasCapturedPauseBuffer = false;

                FB_CopyToFramebuffer(&gfxP, 0, gPauseFrameBuffer, false, &hasCapturedPauseBuffer);

                // Set the state back to ready after the recapture is done
                if (recapturePauseBuffer) {
                    R_PAUSE_MENU_MODE = 3;
                }
                // #endregion
            } else {
                gTrnsnUnkState = 2;
            }
            OVERLAY_DISP = gfxP;
            play->unk_121C7 = 2;
            SREG(33) |= 1;

            // SOH [Port] Continue to render the post world for pausing to avoid flashing the HUD
            if (gTrnsnUnkState == 2) {
                goto Play_Draw_skip;
            }
        }

        // Draw Enhancements that need to be placed in the world. This happens before the PostWorldDraw
        // so that they aren't drawn when the pause menu is up (e.g. collision viewer, actor name tags)
        GameInteractor_ExecuteOnPlayDrawEnd();

    Play_Draw_DrawOverlayElements:
        if ((HREG(80) != 10) || (HREG(89) != 0)) {
            Play_DrawOverlayElements(play);
        }

        // Reset the inverted culling
        if (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0)) {
            gSPClearExtraGeometryMode(POLY_OPA_DISP++, G_EX_INVERT_CULLING);
            gSPClearExtraGeometryMode(POLY_XLU_DISP++, G_EX_INVERT_CULLING);
        }
    }

Play_Draw_skip:

    if (play->view.unk_124 != 0) {
        Camera_Update(GET_ACTIVE_CAM(play));
        func_800AB944(&play->view);
        play->view.unk_124 = 0;
        if (play->skyboxId && (play->skyboxId != SKYBOX_UNSET_1D) && !play->envCtx.skyboxDisabled) {
            SkyboxDraw_UpdateMatrix(&play->skyboxCtx, play->view.eye.x, play->view.eye.y, play->view.eye.z);
        }
    }

    Camera_Finish(GET_ACTIVE_CAM(play));

    CLOSE_DISPS(gfxCtx);

    // SoH multiplayer (Tier B Phase 1): Picture-in-picture scaffold for P2's
    // view. This prototype draws a solid magenta rectangle in the bottom-right
    // quarter of the screen when CVar is enabled AND a non-P1 player actor
    // exists in the world. Phase 2 will replace the colored rect with actual
    // scene rendering from P2's perspective — requires careful state save/
    // restore around View_Apply, viewport switching, and re-invocation of
    // Scene_Draw / Room_Draw / actor draw (func_800315AC) with P2 as the
    // camera target.
    //
    // Why phased: Play_Draw is 300+ lines of intricate state. Trying to wedge
    // a full second render pass in one shot is high-risk; the gfx command
    // pipeline can fail in ways that produce silent corruption or hangs. By
    // shipping the hook + viewport region first, we can confirm position/size
    // are right, then iterate the actual scene render against a known-good
    // viewport.
    // SoH multiplayer (Tier B Phase 2): Picture-in-picture render of P2's
    // perspective in the bottom-right quarter. After the main scene render
    // completes (full screen, P1 view) but before the HUD draws, we:
    //   1. Save the current view state (eye/at/up/viewport/fov)
    //   2. Compute a simple over-shoulder camera for P2 (200 units behind,
    //      80 units up, looking 60 units above P2's feet — no zoom, no
    //      collision check, no smoothing)
    //   3. Set the View viewport to the PiP rectangle
    //   4. Re-apply view matrices via func_800AA460 + func_800AAA50
    //   5. Re-emit scene render: skybox, scene, rooms (current + previous),
    //      actor draw all (func_800315AC)
    //   6. Restore original view state and re-apply matrices for the HUD
    //
    // Known limitations of this first pass:
    //   - Actor draw functions may have draw-time side effects (particles,
    //     sounds, frame counters) that double up with two passes per frame.
    //     If specific actors misbehave, we add per-actor skip filters.
    //   - The P2 camera is fixed-distance — no collision check, no smooth
    //     follow. Inside tight spaces P2's view may clip walls.
    //   - No lens flare, no rain, no sandstorm, no fill-screen effects in
    //     the PiP. These run once on the main pass only.
    //   - The PiP region renders OVER any HUD elements that happen to fall
    //     in the bottom-right (currently none in vanilla layout).
    if (CVarGetInteger(CVAR_ENHANCEMENT("LocalCoop.PiPPrototype"), 0)) {
        // SoH multiplayer: stricter gating for the PiP block.
        //
        // The PiP re-runs Scene_Draw + Room_Draw + actor draw to render
        // a second view from P2's perspective. Each call emits display
        // list commands referencing scene textures and per-frame buffers.
        // During and immediately after scene transitions, those resources
        // are in a half-loaded state — old textures freed, new ones not
        // fully populated. The dual render replays display list pointers
        // that no longer resolve, and the libultraship Fast3D interpreter
        // reads past valid commands into garbage memory ("Unhandled OP
        // code: 0xD5", "Texture is null" floods, eventual access
        // violation in gfx_copy_fb_handler_custom).
        //
        // Existing skip conditions handle the obvious cases (cutscenes,
        // pause, dialog, transition flags). Two additional checks:
        //   1. play->roomCtx.curRoom.segment == NULL — the room data
        //      isn't loaded; rendering it crashes.
        //   2. A "stable frame counter" — the engine signals
        //      transition done a few frames before resources are fully
        //      populated. Original window of 15 frames (~0.25 sec at
        //      60fps) was conservative — visible to the user as a
        //      noticeable black-screen-then-PiP-appears on every
        //      transition. Dropped to 2 frames (~33ms) which is the
        //      minimum we've observed where dual-render doesn't trigger
        //      garbage display lists, and short enough that the
        //      transition is essentially invisible. Combined with the
        //      "full-width P1 render during warmup" hack below (which
        //      stretches P1's view to cover the right half while we
        //      wait), the user sees no black flash at all — the right
        //      side just smoothly transitions from "P1's view content"
        //      to "P2's PiP content" when the warmup completes.
        // (Counter lives at file scope as gCoopPiPStableFrames so the
        // splitScreenActive check earlier in this function can also
        // read it.)
        // SoH multiplayer: detect fixed-camera scene areas where vanilla
        // forces a non-player-attached camera (prerendered backdrops,
        // cutscene cameras, boss intros, fixed minigame cameras). These
        // were designed around a single camera angle and PiP either
        // freezes, renders into the wrong viewport, or causes z-fighting
        // in them. We disable PiP for those settings and re-enable it
        // for everything else (NORMAL/DUNGEON/HORSE plus diving, spiral
        // stairs, crawlspaces, shops, free-look — anything still attached
        // to the player). Result: P2 stays visible in P1's shared full-
        // screen view during fixed-cam scenes; PiP returns the moment
        // the camera settings goes back to a player-tracking mode.
        //
        // Same gate also kicks in during ocarina playback (msgMode in
        // 0x09..0x25). User asked for "P2's ocarina would work if we
        // could unhide P2 just during that and switch to the same
        // shared camera": dropping into coopCinematic disables PiP
        // (= shared camera), and the corresponding ocarina exception
        // in z_player.c's P2 hide gate keeps P2 visible in that shared
        // view.
        //
        // Must match the deny-list at the start of Play_Draw exactly so
        // the two splitscreen-gate sites agree on every frame — keep
        // these two switch statements in sync.
        Camera* coopMainCamForGate = play->cameraPtrs[MAIN_CAM];
        s32 coopFreeCamSetting = 1;
        if (coopMainCamForGate != NULL) {
            switch (coopMainCamForGate->setting) {
                case CAM_SET_PREREND_FIXED:
                case CAM_SET_PREREND_PIVOT:
                case CAM_SET_PREREND_SIDE_SCROLL:
                case CAM_SET_CS_0:
                case CAM_SET_CS_TWISTED_HALLWAY:
                case CAM_SET_CS_3:
                case CAM_SET_CS_ATTENTION:
                case CAM_SET_CS_C:
                case CAM_SET_SLOW_CHEST_CS:
                case CAM_SET_TURN_AROUND:
                case CAM_SET_FREE2:
                case CAM_SET_SCENE_TRANSITION:
                case CAM_SET_DOOR0:
                case CAM_SET_START0:
                case CAM_SET_START1:
                case CAM_SET_BOSS_GOHMA:
                case CAM_SET_BOSS_DODONGO:
                case CAM_SET_BOSS_BARINADE:
                case CAM_SET_BOSS_PHANTOM_GANON:
                case CAM_SET_BOSS_VOLVAGIA:
                case CAM_SET_BOSS_BONGO:
                case CAM_SET_BOSS_MORPHA:
                case CAM_SET_BOSS_TWINROVA_PLATFORM:
                case CAM_SET_BOSS_TWINROVA_FLOOR:
                case CAM_SET_BOSS_GANONDORF:
                case CAM_SET_BOSS_GANON:
                case CAM_SET_MARKET_BALCONY:
                case CAM_SET_CHU_BOWLING:
                case CAM_SET_FISHING:
                case CAM_SET_FOREST_BIRDS_EYE:
                case CAM_SET_MEADOW_BIRDS_EYE:
                case CAM_SET_FIRE_BIRDS_EYE:
                case CAM_SET_FIRE_PLATFORM:
                case CAM_SET_FIRE_STAIRCASE:
                case CAM_SET_FOREST_DEFEAT_POE:
                case CAM_SET_BIG_OCTO:
                case CAM_SET_JABU_TENTACLE:
                    coopFreeCamSetting = 0;
                    break;
                default:
                    coopFreeCamSetting = 1;
                    break;
            }
        }
        s32 coopGateIsOcarina =
            (play->msgCtx.msgMode >= MSGMODE_OCARINA_STARTING) &&
            (play->msgCtx.msgMode <= MSGMODE_SCARECROW_RECORDING_ONGOING);
        s32 coopGateIsHidingMsg =
            (play->msgCtx.msgMode != MSGMODE_NONE) && !coopGateIsOcarina;
        s32 coopCinematic = Play_InCsMode(play) ||
                            play->pauseCtx.state != 0 ||
                            play->pauseCtx.debugState != 0 ||
                            play->gameOverCtx.state != GAMEOVER_INACTIVE ||
                            play->transitionTrigger != TRANS_TRIGGER_OFF ||
                            play->transitionMode != 0 ||
                            coopGateIsHidingMsg ||
                            coopGateIsOcarina ||
                            !coopFreeCamSetting ||
                            play->roomCtx.curRoom.segment == NULL;
        if (coopCinematic) {
            gCoopPiPStableFrames = 0;
        } else if (gCoopPiPStableFrames < 2) {
            gCoopPiPStableFrames++;
        }
        Player* coopP2 = NULL;
        if (!coopCinematic && gCoopPiPStableFrames >= 2) {
            Actor* coopP = play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
            for (; coopP != NULL; coopP = coopP->next) { if (coopP->category != ACTORCAT_PLAYER || PLAYER_GET_INDEX(coopP) == 0) continue;
                if (coopP->category == ACTORCAT_PLAYER) {
                    coopP2 = (Player*)coopP;
                    break;
                }
            }
        }
        if (coopP2 != NULL &&
            coopP2->actor.world.pos.x == coopP2->actor.world.pos.x &&
            coopP2->actor.world.pos.y == coopP2->actor.world.pos.y &&
            coopP2->actor.world.pos.z == coopP2->actor.world.pos.z) {
            // SoH multiplayer: wrap the entire PiP block in a UNIQUE
            // FrameInterpolation child so PiP's matrix recordings live
            // in their own sub-tree separate from the main render's.
            //
            // Without this isolation, every actor inside the PiP block
            // calls Actor_Draw which does RecordOpenChild(actor, 0).
            // The main render already did RecordOpenChild(actor, 0) for
            // the same actor, so PiP's call appends to the same vector
            // (now at idx 1). The matching tree-traversal interpolates
            // idx 0 (main) vs prev frame's idx 0 (main) and idx 1 (PiP)
            // vs prev frame's idx 1 (PiP) — sounds right.
            //
            // The breakage at high FPS comes from the matrix ops at the
            // PiP-block scope (view setup, scene draws, z-buffer clear,
            // billboard rebuild, etc.) being intermixed with the main
            // render's PRE-PiP ops at the same recording-tree depth.
            // The PiP ops appear AFTER all main-render ops in the
            // current frame, but the interpolation algorithm matches
            // ops by index *per Op-type*. With dozens of extra
            // MatrixMult/Translate/ToMtx ops between frames depending
            // on what PiP is doing (Navi spawning, target switching,
            // first-person aim toggling), the per-Op-type indices
            // shift — and frame N's idx 5 MatrixMult might semantically
            // be a different operation from frame N+1's idx 5, so the
            // interpolator lerps mismatched matrices into garbage.
            //
            // Sub-tree isolation fixes this: main-render ops stay at
            // the root-path level (consistent ordering), and PiP ops
            // are under a stable-keyed child (consistent ordering
            // *within* that subtree). The interpolator matches each
            // tree independently.
            //
            // Key: address of a file-scope sentinel — stable across
            // frames, distinct from any actor pointer or other engine
            // marker. The int part stays 0.
            static const u8 sCoopPipFrameInterpMarker = 0;
            FrameInterpolation_RecordOpenChild((const void*)&sCoopPipFrameInterpMarker, 0);
            // Compute over-shoulder eye/at/up for P2. The yaw used for the
            // eye offset is LERPED toward P2's facing direction rather than
            // snapping instantly — this is what makes vanilla feel like it
            // "follows" rather than being "locked behind." 10% per frame
            // gives a smooth ~10-frame catch-up that matches OoT's vanilla
            // camera responsiveness reasonably well.
            //
            // Wrap-aware lerp: subtracting two s16 yaw values and casting
            // back to s16 gives the shortest signed distance around the
            // unit circle, so this handles 0-degree wrap-around without
            // explicit modular math.
            // SoH multiplayer: P2's PiP camera geometry.
            //
            // P2 has a real Camera struct (gCoopP2CameraId), but its
            // status is intentionally NOT CAM_STAT_ACTIVE — see commentary
            // in z_player.c on P2 init. That means Camera_Update doesn't
            // recompute its eye/at/up each frame. The struct's primary
            // role is to give vanilla Camera_ChangeMode / aim / fire paths
            // a target so they don't clobber main camera state.
            //
            // Since the engine doesn't drive P2's camera, we compute its
            // eye/at/up here based on P2's camera->mode (which vanilla
            // DOES set correctly via Camera_ChangeMode(SUBCAM_ACTIVE)
            // during P2's update):
            //
            //   - CAM_MODE_BOWARROW / _SLINGSHOT / _HOOKSHOT /
            //     _FIRSTPERSON / our gCoopP2InAimMode → first-person
            //     view from P2's head, looking along yaw + pitch.
            //   - focusActor != NULL → lock-on framing: eye behind P2
            //     looking past P2 toward the locked target.
            //   - default → third-person follow (gCoopP2CameraYaw +
            //     Pitch with FreeLook right-stick handling).
            extern f32 gCoopP2CameraYaw;
            extern f32 gCoopP2CameraPitch;
            extern s32 gCoopP2InAimMode;
            extern s32 gCoopP2CameraId;
            s16 coopSmoothYaw = (s16)gCoopP2CameraYaw;
            s16 coopSmoothPitch = (s16)gCoopP2CameraPitch;

            Camera* coopP2CamRef = (gCoopP2CameraId != SUBCAM_NONE)
                                       ? play->cameraPtrs[gCoopP2CameraId]
                                       : NULL;
            s16 coopP2Mode = (coopP2CamRef != NULL) ? coopP2CamRef->mode : CAM_MODE_NORMAL;
            // SoH multiplayer: FP detection now ALSO honors P2's own
            // PLAYER_STATE1_FIRST_PERSON flag. Vanilla aim code (the
            // C-button handler → func_8083AD4C → Player_Action_8084B1D8
            // path) sets this flag on P2 the same way it sets it on
            // P1 when the player presses a ranged-weapon C-button.
            // With the activeCamera swap, that path also sets P2's
            // sub-camera mode to BOWARROW/SLINGSHOT — so we get the
            // *real* vanilla aim camera computed on P2's sub-cam,
            // which is what makes the FP framing identical to P1's
            // (head bone position, vanilla FOV pull-in, etc).
            s32 coopP2InFP = gCoopP2InAimMode ||
                             (coopP2->stateFlags1 & PLAYER_STATE1_FIRST_PERSON) ||
                             coopP2Mode == CAM_MODE_BOWARROW ||
                             coopP2Mode == CAM_MODE_SLINGSHOT ||
                             coopP2Mode == CAM_MODE_FIRSTPERSON ||
                             coopP2Mode == CAM_MODE_HOOKSHOT;

            Vec3f p2At, p2Eye, p2Up;
            f32 sinYaw = Math_SinS(coopSmoothYaw);
            f32 cosYaw = Math_CosS(coopSmoothYaw);
            f32 sinPitch = Math_SinS(coopSmoothPitch);
            f32 cosPitch = Math_CosS(coopSmoothPitch);
            if (coopP2InFP) {
                // Preferred path: use P2's sub-camera eye/at directly.
                // Vanilla aim mode (CAM_MODE_BOWARROW / _SLINGSHOT /
                // _FIRSTPERSON) writes proper eye/at onto P2's sub-
                // camera struct via Camera_Update during P2's update
                // tick (the activeCamera swap routes Camera_ChangeMode
                // / Camera_Update calls into P2's slot). Reading them
                // here gives the IDENTICAL framing P1 gets when aiming
                // a slingshot/bow — same head bone position, same
                // FOV pull-in, same vanilla aim math.
                //
                // Fallback (when sub-camera isn't initialized or its
                // eye/at hasn't been written yet): hand-rolled FP
                // framing from P2's head bone + yaw/pitch. Identical
                // to the previous implementation. Triggers for the
                // gCoopP2InAimMode custom path before vanilla aim
                // takes over.
                s32 coopUseSubCamView = 0;
                if (coopP2CamRef != NULL &&
                    (coopP2Mode == CAM_MODE_BOWARROW ||
                     coopP2Mode == CAM_MODE_SLINGSHOT ||
                     coopP2Mode == CAM_MODE_FIRSTPERSON ||
                     coopP2Mode == CAM_MODE_HOOKSHOT) &&
                    !(coopP2CamRef->eye.x != coopP2CamRef->eye.x) &&
                    !(coopP2CamRef->at.x != coopP2CamRef->at.x)) {
                    // SoH multiplayer: sub-cam eye/at sanity check.
                    //
                    // P2's sub-camera has status CAM_STAT_WAIT so the
                    // engine's Camera_Update returns early without
                    // recomputing eye/at (see z_camera.c line 7645).
                    // When P2 enters BOWARROW/SLINGSHOT/etc via the
                    // Camera_ChangeMode routing, the mode field gets
                    // set BUT eye/at remain at their last value —
                    // often (0,0,0) from initial allocation, or a
                    // stale position from before the scene change.
                    //
                    // The NaN guard above (eye.x != eye.x) only
                    // catches NaN, not zero or stale-but-finite
                    // values. A degenerate eye=(0,0,0)/at=(0,0,0)
                    // pair fed to guLookAtF produces a zero-direction
                    // matrix → empty view-projection → black screen
                    // for the entire PiP. Repro: P2 holds a C-button
                    // for slingshot, screen goes black.
                    //
                    // Reject sub-cam eye if it's too far from P2's
                    // actual position (more than ~500 units, which
                    // covers vanilla's max aim eye offset by a wide
                    // margin) — in that case the sub-cam values are
                    // stale and we fall through to the head-bone
                    // fallback below.
                    f32 coopEyeDx = coopP2CamRef->eye.x - coopP2->actor.world.pos.x;
                    f32 coopEyeDy = coopP2CamRef->eye.y - coopP2->actor.world.pos.y;
                    f32 coopEyeDz = coopP2CamRef->eye.z - coopP2->actor.world.pos.z;
                    f32 coopEyeDistSq = coopEyeDx*coopEyeDx + coopEyeDy*coopEyeDy + coopEyeDz*coopEyeDz;
                    if (coopEyeDistSq < (500.0f * 500.0f)) {
                        coopUseSubCamView = 1;
                        p2Eye = coopP2CamRef->eye;
                        p2At = coopP2CamRef->at;
                    }
                }
                if (!coopUseSubCamView) {
                    p2Eye.x = coopP2->bodyPartsPos[PLAYER_BODYPART_HEAD].x;
                    p2Eye.y = coopP2->bodyPartsPos[PLAYER_BODYPART_HEAD].y;
                    p2Eye.z = coopP2->bodyPartsPos[PLAYER_BODYPART_HEAD].z;
                    f32 coopAimDist = 300.0f;
                    p2At.x = p2Eye.x + sinYaw * cosPitch * coopAimDist;
                    p2At.y = p2Eye.y + sinPitch * coopAimDist;
                    p2At.z = p2Eye.z + cosYaw * cosPitch * coopAimDist;
                }
                // SoH multiplayer: nudge p2Eye slightly forward of the
                // head bone so the head model isn't right at the
                // camera. Without this, P2's own head model overlaps
                // the camera's near plane — parts of it that extend
                // past zNear (back of skull when looking forward,
                // front of face when looking back, etc.) clip into
                // the view as inverted/black polygons.
                //
                // Vanilla's first-person aim camera (Camera_Subj3)
                // does the same thing — the eye sits a small distance
                // out from the player rather than literally inside
                // the head. ~12 units forward roughly matches the
                // adult-Link head radius so the entire head model
                // ends up behind the camera near plane.
                {
                    f32 coopLookDx = p2At.x - p2Eye.x;
                    f32 coopLookDy = p2At.y - p2Eye.y;
                    f32 coopLookDz = p2At.z - p2Eye.z;
                    f32 coopLookLen = sqrtf(coopLookDx*coopLookDx + coopLookDy*coopLookDy + coopLookDz*coopLookDz);
                    if (coopLookLen > 1.0f) {
                        f32 coopNudge = 12.0f / coopLookLen;
                        p2Eye.x += coopLookDx * coopNudge;
                        p2Eye.y += coopLookDy * coopNudge;
                        p2Eye.z += coopLookDz * coopNudge;
                    }
                }
            } else if (coopP2->focusActor != NULL) {
                // Lock-on: at-point toward the focusActor's focus.pos
                // (e.g., enemy chest height), eye behind P2 looking past
                // P2 toward the target. This frames both P2 and the
                // target without P2's body blocking the view.
                Vec3f coopTargetPos = coopP2->focusActor->focus.pos;
                // Compute yaw from P2 toward target so the eye sits
                // BEHIND P2 along the P2→target line.
                f32 coopDx = coopTargetPos.x - coopP2->actor.world.pos.x;
                f32 coopDz = coopTargetPos.z - coopP2->actor.world.pos.z;
                f32 coopHorizDist = sqrtf(coopDx * coopDx + coopDz * coopDz);
                if (coopHorizDist < 1.0f) coopHorizDist = 1.0f;
                f32 coopDirX = coopDx / coopHorizDist;
                f32 coopDirZ = coopDz / coopHorizDist;
                // At-point is midway between P2 and target (vanilla
                // BATTLE camera does similar — keeps both visible).
                p2At.x = (coopP2->actor.world.pos.x + coopTargetPos.x) * 0.5f;
                p2At.y = (coopP2->actor.world.pos.y + coopTargetPos.y) * 0.5f + 30.0f;
                p2At.z = (coopP2->actor.world.pos.z + coopTargetPos.z) * 0.5f;
                // Eye behind P2 by ~140 units along the opposite-of-
                // target direction, raised 60.
                p2Eye.x = coopP2->actor.world.pos.x - coopDirX * 140.0f;
                p2Eye.y = coopP2->actor.world.pos.y + 80.0f;
                p2Eye.z = coopP2->actor.world.pos.z - coopDirZ * 140.0f;
            } else {
                // Third-person follow.
                p2At.x = coopP2->actor.world.pos.x;
                p2At.y = coopP2->actor.world.pos.y + 40.0f;
                p2At.z = coopP2->actor.world.pos.z;
                f32 coopHorizDist = 140.0f * cosPitch;
                f32 coopVertOffset = 50.0f - 140.0f * sinPitch;
                p2Eye.x = p2At.x - sinYaw * coopHorizDist;
                p2Eye.y = p2At.y + coopVertOffset;
                p2Eye.z = p2At.z - cosYaw * coopHorizDist;
            }
            p2Up.x = 0.0f; p2Up.y = 1.0f; p2Up.z = 0.0f;

            // Camera collision: raycast from p2At to p2Eye and clamp the eye
            // to any wall hit. Without this, the P2 split-screen camera
            // clips through walls in tight rooms and shows the inside of
            // geometry. Uses the same Camera_BGCheck function the engine's
            // vanilla camera uses for its own collision.
            // Skipped in first-person aim mode: eye is at P2's head and at
            // is projected forward; clamping the at to a wall would pull
            // the look-target toward P2, narrowing FOV strangely. Walls in
            // front of P2 are visually present anyway in first-person.
            if (!gCoopP2InAimMode) {
                Camera* coopMainCam = Play_GetCamera(play, MAIN_CAM);
                if (coopMainCam != NULL) {
                    Camera_BGCheck(coopMainCam, &p2At, &p2Eye);
                }
            }

            // Save main-pass view state.
            Vec3f savedEye = play->view.eye;
            Vec3f savedAt = play->view.lookAt;
            Vec3f savedUp = play->view.up;
            Viewport savedViewport;
            View_GetViewport(&play->view, &savedViewport);

            // PiP region: RIGHT HALF of the screen for true split-screen.
            s32 pipLeftX = SCREEN_WIDTH / 2;
            s32 pipTopY = 0;
            s32 pipRightX = SCREEN_WIDTH;
            s32 pipBottomY = SCREEN_HEIGHT;

            // Clear the z-buffer for the PiP region BEFORE re-rendering the
            // scene from P2's perspective. Without this, the main pass's z
            // values cause the PiP geometry to occlude itself against the
            // main view's terrain — visible as blocky cutouts where main
            // world geometry "shows through" the PiP. Pattern adapted from
            // Player_DrawImpl's bunny-hood ice-trap fix.
            OPEN_DISPS(gfxCtx);
            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetColorImage(POLY_OPA_DISP++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH, gZBuffer);
            gDPSetCycleType(POLY_OPA_DISP++, G_CYC_FILL);
            gDPSetRenderMode(POLY_OPA_DISP++, G_RM_NOOP, G_RM_NOOP2);
            gDPSetFillColor(POLY_OPA_DISP++,
                            (GPACK_ZDZ(G_MAXFBZ, 0) << 16) | GPACK_ZDZ(G_MAXFBZ, 0));
            gDPFillRectangle(POLY_OPA_DISP++, pipLeftX, pipTopY, pipRightX - 1, pipBottomY - 1);
            gDPPipeSync(POLY_OPA_DISP++);
            // Restore the color image to the framebuffer so subsequent
            // draws go to the screen, not the depth buffer.
            gDPSetColorImage(POLY_OPA_DISP++, G_IM_FMT_RGBA, G_IM_SIZ_16b, SCREEN_WIDTH,
                             gfxCtx->curFrameBuffer);
            gDPPipeSync(POLY_OPA_DISP++);
            CLOSE_DISPS(gfxCtx);

            // Apply P2 view + PiP viewport.
            play->view.eye = p2Eye;
            play->view.lookAt = p2At;
            play->view.up = p2Up;

            // SoH multiplayer: stage P2's view-projection matrix for
            // the actor-culling system in z_actor.c func_800315AC. The
            // engine culls actors against play->viewProjectionMtxF
            // (P1's main camera) each frame — when P1 looks away from
            // an actor, it falls out of P1's frustum and gets culled,
            // even if P2 is still looking right at it. The fix is to
            // expose P2's view-projection here so the culling pass
            // can OR-in a second frustum check against P2's view. We
            // compute the matrix the same way we project the P2
            // reticle (guLookAtF + guPerspectiveF with the half-width
            // PiP aspect), stash it as a global, and set the valid
            // flag for the culling code to consume.
            //
            // Note: gCoopP2ViewProjMtxF is staged DURING draw and
            // consumed by func_800315AC the SAME frame on the call
            // that draws PiP (func_800315AC is invoked for both the
            // main scene and the PiP re-render, in that order). For
            // the main-scene call the matrix holds last frame's value
            // — fine, P2's camera barely moves frame-to-frame, the
            // worst case is one frame of late-arriving culling, which
            // is way better than the actor never updating at all.
            {
                MtxF coopCullViewMtxF;
                MtxF coopCullProjMtxF;
                u16 coopCullPerspNorm;
                f32 coopCullAspect = (f32)(pipRightX - pipLeftX) /
                                     (f32)(pipBottomY - pipTopY);
                guLookAtF(coopCullViewMtxF.mf,
                          p2Eye.x, p2Eye.y, p2Eye.z,
                          p2At.x, p2At.y, p2At.z,
                          p2Up.x, p2Up.y, p2Up.z);
                guPerspectiveF(coopCullProjMtxF.mf, &coopCullPerspNorm,
                               play->view.fovy, coopCullAspect,
                               play->view.zNear, play->view.zFar, 1.0f);
                SkinMatrix_MtxFMtxFMult(&coopCullProjMtxF, &coopCullViewMtxF,
                                        &gCoopP2ViewProjMtxF);
                gCoopP2ReticleValid = 1;
            }

            Viewport pipVp;
            pipVp.topY = pipTopY;
            pipVp.bottomY = pipBottomY;
            pipVp.leftX = pipLeftX;
            pipVp.rightX = pipRightX;
            View_SetViewport(&play->view, &pipVp);
            func_800AA460(&play->view, play->view.fovy, play->view.zNear, play->lightCtx.fogFar);
            func_800AAA50(&play->view, 15);

            // SoH multiplayer: rebuild the billboard matrix from P2's
            // view and re-bind segment 0x01 before drawing the scene
            // from P2's perspective. Without this, billboarded sprites
            // drawn during the PiP pass (Navi, sparkles, particle
            // effects, item icons, etc.) face P1's camera instead of
            // P2's — visible to the user as billboards looking
            // "sideways" or pointing the wrong direction in P2's view.
            //
            // The billboard matrix is the view-rotation matrix with
            // translation zeroed and transposed (so it represents the
            // camera-to-world rotation — applying it to a vertex
            // un-rotates it from view space, making it face the
            // camera). Mirrors the main-pass setup at lines 1513-1526,
            // minus the viewProjectionMtxF computation (we don't need
            // to rebuild that here — we already staged P2's matrix
            // separately as gCoopP2ViewProjMtxF for actor culling).
            {
                Matrix_MtxToMtxF(&play->view.viewing, &play->billboardMtxF);
                play->billboardMtxF.mf[0][3] = play->billboardMtxF.mf[1][3] = play->billboardMtxF.mf[2][3] =
                    play->billboardMtxF.mf[3][0] = play->billboardMtxF.mf[3][1] = play->billboardMtxF.mf[3][2] = 0.0f;
                Matrix_Transpose(&play->billboardMtxF);
                play->billboardMtx =
                    Matrix_MtxFToMtx(&play->billboardMtxF, Graph_Alloc(gfxCtx, sizeof(Mtx)));
                OPEN_DISPS(gfxCtx);
                gSPSegment(POLY_OPA_DISP++, 0x01, play->billboardMtx);
                gSPSegment(POLY_XLU_DISP++, 0x01, play->billboardMtx);
                CLOSE_DISPS(gfxCtx);
            }

            // Re-emit scene contents from P2's perspective.
            if (play->skyboxId && (play->skyboxId != SKYBOX_UNSET_1D) && !play->envCtx.skyboxDisabled) {
                SkyboxDraw_Draw(&play->skyboxCtx, gfxCtx, play->skyboxId, 0,
                                p2Eye.x, p2Eye.y, p2Eye.z);
            }
            Scene_Draw(play);
            Room_Draw(play, &play->roomCtx.curRoom, 3);
            Room_Draw(play, &play->roomCtx.prevRoom, 3);
            // SoH multiplayer: P2's first-person view — hide ONLY the
            // head limb so P2 doesn't see their own head clipping into
            // their camera, but the rest of P2's body still draws
            // (player sees own torso/feet when looking down in aim
            // mode, sees own hands when looking at items, etc.). The
            // OTHER player's view of P2 (i.e., P1's main view) is
            // not affected by gCoopHideHeadFor — that gets cleared
            // immediately after this PiP draw pass, so P1 always sees
            // P2's full model including head.
            //
            // Why a single-limb hide and not the previous "null the
            // whole draw pointer" approach: per user request, both
            // players should see each other's full models, and each
            // player should see their own body except the head. The
            // null-draw approach hid the entire model on the FP
            // player's own viewport, which made P2 invisible to
            // themselves — wrong. Per-limb suppression via the
            // existing OverrideLimbDraw hook in z_player_lib.c keeps
            // the rest of the model intact.
            if (coopP2InFP && coopP2 != NULL) {
                gCoopHideHeadFor = &coopP2->actor;
            }
            // SoH multiplayer: targetCtx (Navi position, reticle center,
            // pointed/targeted actors) is GLOBAL and updated each frame
            // by Actor_UpdateAll → func_8002C7BC against GET_PLAYER(play),
            // which is always P1. When the actor draw pass runs during
            // P2's PiP rendering, Navi draws hovering over **P1's** target,
            // and so does the spinning-triangles reticle — visually it
            // looks like P2's lock-on is following P1's even when P2 is
            // locked onto a different enemy. To make P2's PiP show P2's
            // own targeting, save targetCtx, override its key fields based
            // on coopP2->focusActor for this PiP draw, then restore so
            // P1's subsequent rendering and next-frame target update are
            // unaffected.
            Actor* coopSavedArrowPtd = play->actorCtx.targetCtx.arrowPointedActor;
            Actor* coopSavedTargeted = play->actorCtx.targetCtx.targetedActor;
            Vec3f coopSavedNaviRef = play->actorCtx.targetCtx.naviRefPos;
            Vec3f coopSavedTargetCtr = play->actorCtx.targetCtx.targetCenterPos;
            u8 coopSavedActiveCat = play->actorCtx.targetCtx.activeCategory;
            if (coopP2 != NULL) {
                Actor* coopP2Target = coopP2->focusActor;
                play->actorCtx.targetCtx.arrowPointedActor = coopP2Target;
                play->actorCtx.targetCtx.targetedActor = coopP2Target;
                if (coopP2Target != NULL) {
                    play->actorCtx.targetCtx.naviRefPos = coopP2Target->focus.pos;
                    play->actorCtx.targetCtx.targetCenterPos = coopP2Target->focus.pos;
                    play->actorCtx.targetCtx.activeCategory = coopP2Target->category;
                } else {
                    // No P2 target — anchor Navi at P2 themself so the
                    // fairy follows P2 in their PiP rather than P1.
                    play->actorCtx.targetCtx.naviRefPos = coopP2->actor.world.pos;
                    play->actorCtx.targetCtx.naviRefPos.y += 40.0f;
                    play->actorCtx.targetCtx.activeCategory = ACTORCAT_PLAYER;
                }
            }
            func_800315AC(play, &play->actorCtx);
            // Restore so the subsequent frame's target update (against P1)
            // continues from the right state — and so anything else this
            // frame that reads targetCtx sees the P1-relative values again.
            play->actorCtx.targetCtx.arrowPointedActor = coopSavedArrowPtd;
            play->actorCtx.targetCtx.targetedActor = coopSavedTargeted;
            play->actorCtx.targetCtx.naviRefPos = coopSavedNaviRef;
            play->actorCtx.targetCtx.targetCenterPos = coopSavedTargetCtr;
            play->actorCtx.targetCtx.activeCategory = coopSavedActiveCat;
            // SoH multiplayer: clear head-hide pointer after P2's PiP
            // actor pass so subsequent draws (e.g. P1's main view in
            // the NEXT frame, kaleido pause-menu Link, etc.) see all
            // limbs again. Must always clear, even if we never set it
            // this frame, in case some other code path set it.
            gCoopHideHeadFor = NULL;

            // SoH multiplayer: draw a simple white crosshair at the center
            // of the PiP region while P2 is in first-person aim mode. Since
            // our camera looks straight along the aim direction (yaw +
            // pitch), screen-center is exactly where the projectile will
            // travel — the crosshair is functionally accurate, not just
            // decorative.
            //
            // Uses G_CYC_FILL mode with the framebuffer as the color image,
            // same pattern as the z-buffer clear above but writing to the
            // visible framebuffer with a white fill color (0xFFFF in 16-bit
            // RGBA = white). Two thin rectangles form a "+".
            //
            // OPEN_DISPS / CLOSE_DISPS is REQUIRED because POLY_OPA_DISP
            // expands to __gfxCtx->polyOpa.p where __gfxCtx is a local
            // declared inside OPEN_DISPS. Without the block, MSVC errors
            // (GCC also UB but happens to compile silently). Found via the
            // Windows CI failure — the install step downstream of the
            // failed compile reported a missing soh.pdb.
            // SoH multiplayer: first-person crosshair removed per user
            // request. Previously this drew a white "+" at the screen
            // center of the PiP when P2 was in aim mode. User found it
            // visually noisy and asked to remove. P2 shots still fly
            // straight along aim yaw + pitch, so screen-center is where
            // they land — the marker was just decorative. Lock-on
            // corner-bracket reticle below is preserved.
            (void)0;

            // SoH multiplayer: P2 lock-on reticle. Earlier attempts
            // tried to reuse vanilla func_8002C124's spinning-triangles
            // path with a matrix swap, but the projection produced by
            // func_8002BE04 lives in the HUD ortho coordinate space
            // (matrix x ∈ [-160, +160] = full-screen edges), while our
            // P2 view-projection projects targets for the half-width
            // PiP region — the resulting reticle landed on the wrong
            // half or didn't appear at all due to the clamping. Going
            // back to the corner-bracket approach: I project P2's
            // target manually with the same view-proj I use for the
            // PiP camera, clamp explicitly to the PiP rectangle, and
            // draw four corner brackets via gDPFillRectangle in the
            // user-chosen tunic color. Not the vanilla shape but it's
            // reliably visible and constrained to P2's half.
            if (coopP2 != NULL && coopP2->focusActor != NULL) {
                Actor* coopTarget = coopP2->focusActor;
                MtxF coopViewMtxF;
                MtxF coopProjMtxF;
                MtxF coopVpMtxF;
                u16 coopPerspNorm;
                f32 coopAspect = (f32)(pipRightX - pipLeftX) /
                                 (f32)(pipBottomY - pipTopY);
                guLookAtF(coopViewMtxF.mf,
                          p2Eye.x, p2Eye.y, p2Eye.z,
                          p2At.x, p2At.y, p2At.z,
                          p2Up.x, p2Up.y, p2Up.z);
                guPerspectiveF(coopProjMtxF.mf, &coopPerspNorm,
                               play->view.fovy, coopAspect,
                               play->view.zNear, play->view.zFar, 1.0f);
                SkinMatrix_MtxFMtxFMult(&coopProjMtxF, &coopViewMtxF,
                                        &coopVpMtxF);
                Vec3f coopWorldPos = coopTarget->focus.pos;
                Vec3f coopClipPos;
                f32 coopW;
                SkinMatrix_Vec3fMtxFMultXYZW(&coopVpMtxF, &coopWorldPos,
                                             &coopClipPos, &coopW);
                if (coopW >= 1.0f) {
                    f32 coopInvW = 1.0f / coopW;
                    f32 coopNdcX = coopClipPos.x * coopInvW;
                    f32 coopNdcY = coopClipPos.y * coopInvW;
                    // Forward declare the engine helper that stores a
                    // single triangle entry (pos.xyz + copies
                    // targetCtx->unk_44 into entry->unk_0C). Defined in
                    // z_actor.c, no header.
                    extern void func_8002BE64(TargetContext* targetCtx, s32 index,
                                              f32 arg2, f32 arg3, f32 arg4);
                    // Forward declare libultraship's aspect-ratio helper.
                    // Used below for widescreen overshoot correction.
                    extern float OTRGetAspectRatio(void);
                    // SoH multiplayer: vanilla-style spinning triangle
                    // reticle for P2, using gZTargetLockOnTriangleDL
                    // (the same display list func_8002C124 uses for
                    // P1's lock-on reticle).
                    //
                    // Coord system: writing to OVERLAY_DISP, which
                    // flushes at end of frame with the full-screen
                    // viewport. So we project the target through
                    // P2's view to NDC, then map to FULL-screen ortho
                    // with the PiP region's center+halfwidth offsets.
                    // Full-screen ortho center is (0, 0); PiP right-
                    // half center is at ortho (+80, 0) for the
                    // standard 160-pixel-wide right-half PiP.
                    //
                    // The arr_50 entry array gives the vanilla trail
                    // effect — three triangle positions with the most
                    // recent being the "live" one and the prior two
                    // trailing for the zoom-in animation. entry->
                    // unk_0C is the triangle distance-from-center,
                    // which shrinks from 500 to 80 as the lock
                    // settles, producing the iconic "zoom in to
                    // lock" visual cue.
                    extern TargetContext gCoopP2TargetCtx;
                    TargetContext* p2tc = &gCoopP2TargetCtx;
                    // SoH multiplayer: widescreen correction.
                    //
                    // libultraship's GfxSpVertex applies
                    // AdjXForAspectRatio to every clip-space X
                    // coordinate: x *= (4/3) / display_aspect. For
                    // 16:9 the factor is 0.75, for 21:9 it's ~0.57.
                    // The factor pulls vertices toward the center
                    // horizontally — same multiplier on both 3D and
                    // HUD vertices.
                    //
                    // But the 3D scene uses the PiP viewport
                    // (half-width 80 in internal coords) while our
                    // reticle uses the HUD ortho with the full-screen
                    // viewport (half-width 160). Same NDC value
                    // lands at different display positions in the
                    // two viewport mappings. Working through the
                    // math: for the reticle X to line up with the
                    // visible target X in widescreen, the per-NDC
                    // half-width factor needs to scale by the same
                    // AdjX ratio. Without this correction the
                    // reticle "overshoots evenly on both sides" of
                    // the target — its horizontal travel per unit
                    // NDC is 1/ratio times what the rendered target
                    // travels.
                    //
                    // Clamp the ratio to [0.5, 1.0]: ratios above
                    // 1.0 happen for narrower-than-4:3 displays
                    // (rare, no correction needed); ratios below
                    // 0.5 would shrink the reticle implausibly
                    // (ultra-wide 32:9 etc.) so we cap.
                    f32 coopAdjRatio = (4.0f / 3.0f) / OTRGetAspectRatio();
                    if (coopAdjRatio > 1.0f) coopAdjRatio = 1.0f;
                    if (coopAdjRatio < 0.5f) coopAdjRatio = 0.5f;
                    f32 coopPipCenterOrthoX = ((f32)(pipLeftX + pipRightX) * 0.5f) - 160.0f;
                    f32 coopPipHalfWOrtho = ((f32)(pipRightX - pipLeftX) * 0.5f) * coopAdjRatio;
                    f32 coopRetOrthoX = coopNdcX * coopPipHalfWOrtho + coopPipCenterOrthoX;
                    f32 coopPipCenterOrthoY = 120.0f - ((f32)(pipTopY + pipBottomY) * 0.5f);
                    f32 coopPipHalfHOrtho = (f32)(pipBottomY - pipTopY) * 0.5f;
                    f32 coopRetOrthoY = coopNdcY * coopPipHalfHOrtho + coopPipCenterOrthoY;

                    Color_RGB8 coopRetDef = { 0xC8, 0x14, 0x14 };
                    Color_RGB8 coopRetCol = CVarGetColor24(
                        CVAR_ENHANCEMENT("LocalCoop.P2.KokiriTunic.Value"),
                        coopRetDef);

                    // Push latest screen pos + zoom radius into the
                    // triangle entry buffer. func_8002BE64 stores
                    // pos.xyz and copies targetCtx->unk_44 → entry->
                    // unk_0C, so the entry inherits the current
                    // settle radius.
                    p2tc->unk_4C--;
                    if (p2tc->unk_4C < 0) p2tc->unk_4C = 2;
                    f32 coopVar1 = (500.0f - p2tc->unk_44) / 420.0f;
                    func_8002BE64(p2tc, p2tc->unk_4C, coopRetOrthoX, coopRetOrthoY, coopVar1);

                    // 1 triangle copy when settled (clean lock), 3
                    // during lock-on/target switch (trailing).
                    s32 coopSpB8 = (p2tc->unk_4B != 0) ? 1 : 3;
                    s32 coopAlpha = 0xFF;

                    OPEN_DISPS(gfxCtx);
                    OVERLAY_DISP = Gfx_SetupDL(OVERLAY_DISP, 0x39);
                    for (s32 coopSpB0 = 0, coopSpAC = p2tc->unk_4C;
                         coopSpB0 < coopSpB8;
                         coopSpB0++, coopSpAC = (coopSpAC + 1) % 3) {
                        TargetContextEntry* coopEntry = &p2tc->arr_50[coopSpAC];
                        if (coopEntry->unk_0C >= 500.0f) continue;

                        f32 coopVar2;
                        if (coopEntry->unk_0C <= 120.0f) {
                            coopVar2 = 0.15f;
                        } else {
                            coopVar2 = ((coopEntry->unk_0C - 120.0f) * 0.001f) + 0.15f;
                        }

                        Matrix_Translate(coopEntry->pos.x, coopEntry->pos.y, 0.0f, MTXMODE_NEW);
                        Matrix_Scale(coopVar2, 0.15f, 1.0f, MTXMODE_APPLY);

                        gDPSetPrimColor(OVERLAY_DISP++, 0, 0,
                                        coopRetCol.r, coopRetCol.g, coopRetCol.b,
                                        (u8)coopAlpha);

                        Matrix_RotateZ((p2tc->unk_4B & 0x7F) * (M_PI / 64), MTXMODE_APPLY);

                        for (s32 coopI = 0; coopI < 4; coopI++) {
                            Matrix_RotateZ(M_PI / 2, MTXMODE_APPLY);
                            Matrix_Push();
                            Matrix_Translate(coopEntry->unk_0C, coopEntry->unk_0C, 0.0f, MTXMODE_APPLY);
                            gSPMatrix(OVERLAY_DISP++, MATRIX_NEWMTX(gfxCtx),
                                      G_MTX_MODELVIEW | G_MTX_LOAD);
                            gSPDisplayList(OVERLAY_DISP++, gZTargetLockOnTriangleDL);
                            Matrix_Pop();
                        }

                        coopAlpha -= 0xFF / 3;
                        if (coopAlpha < 0) coopAlpha = 0;
                    }
                    CLOSE_DISPS(gfxCtx);
                }
            }

            // Restore main-pass view so the HUD renders against full-screen
            // viewport (each player sees the half of the HUD that corresponds
            // to their side). The "savedViewport" we captured at the start of
            // this block is the LEFT-HALF viewport from the main render — we
            // intentionally don't restore to that, we restore to full screen.
            play->view.eye = savedEye;
            play->view.lookAt = savedAt;
            play->view.up = savedUp;
            Viewport fullVp;
            fullVp.topY = 0;
            fullVp.bottomY = SCREEN_HEIGHT;
            fullVp.leftX = 0;
            fullVp.rightX = SCREEN_WIDTH;
            View_SetViewport(&play->view, &fullVp);
            func_800AA460(&play->view, play->view.fovy, play->view.zNear, play->lightCtx.fogFar);
            func_800AAA50(&play->view, 15);

            // SoH multiplayer: rebuild P1's billboard matrix and re-bind
            // segment 0x01 now that play->view.viewing is back to P1's
            // view. During the PiP block we replaced billboardMtxF with
            // P2's billboard so sprites in P2's view face P2's camera;
            // if we don't restore it here, any subsequent rendering
            // that touches segment 0x01 (item-pickup sparkles, HUD
            // overlay billboards, Navi popups during item-get cutscenes,
            // etc.) sees P2's billboard and draws facing the wrong
            // direction in P1's view. Mirror of the main-pass setup at
            // line 1513-1526 minus the viewProjectionMtxF computation.
            Matrix_MtxToMtxF(&play->view.viewing, &play->billboardMtxF);
            play->billboardMtxF.mf[0][3] = play->billboardMtxF.mf[1][3] = play->billboardMtxF.mf[2][3] =
                play->billboardMtxF.mf[3][0] = play->billboardMtxF.mf[3][1] = play->billboardMtxF.mf[3][2] = 0.0f;
            Matrix_Transpose(&play->billboardMtxF);
            play->billboardMtx =
                Matrix_MtxFToMtx(&play->billboardMtxF, Graph_Alloc(gfxCtx, sizeof(Mtx)));
            {
                OPEN_DISPS(gfxCtx);
                gSPSegment(POLY_OPA_DISP++, 0x01, play->billboardMtx);
                gSPSegment(POLY_XLU_DISP++, 0x01, play->billboardMtx);
                CLOSE_DISPS(gfxCtx);
            }
            // SoH multiplayer: close the FrameInterpolation sub-tree
            // opened at the top of this PiP block. Must be balanced
            // with the matching RecordOpenChild above — every
            // RecordOpenChild call pushes onto current_path, and
            // RecordCloseChild pops it. Unbalanced calls would corrupt
            // the recording tree for subsequent frames.
            FrameInterpolation_RecordCloseChild();
        }
    }

    Interface_DrawTotalGameplayTimer(play);
}

time_t Play_GetRealTime() {
    time_t t1, t2;
    struct tm* tms;
    time(&t1);
    tms = localtime(&t1);
    tms->tm_hour = 0;
    tms->tm_min = 0;
    tms->tm_sec = 0;
    t2 = mktime(tms);
    return t1 - t2;
}

void Play_Main(GameState* thisx) {
    PlayState* play = (PlayState*)thisx;

    if (play->envCtx.unk_EE[2] == 0 && CVarGetInteger(CVAR_GENERAL("LetItSnow"), 0)) {
        play->envCtx.unk_EE[3] = 64;
        Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_OBJECT_KANKYO, 0, 0, 0, 0, 0, 0, 3);
    }

    D_8012D1F8 = &play->state.input[0];

    DebugDisplay_Init();

    PLAY_LOG(4556);

    if ((HREG(80) == 10) && (HREG(94) != 10)) {
        HREG(81) = 1;
        HREG(82) = 1;
        HREG(83) = 1;
        HREG(84) = 3;
        HREG(85) = 1;
        HREG(86) = 1;
        HREG(87) = 1;
        HREG(88) = 1;
        HREG(89) = 1;
        HREG(90) = 15;
        HREG(91) = 1;
        HREG(92) = 1;
        HREG(93) = 1;
        HREG(94) = 10;
    }

    if ((HREG(80) != 10) || (HREG(81) != 0)) {
        Play_Update(play);
    }

    PLAY_LOG(4583);

    FrameInterpolation_StartRecord();
    Play_Draw(play);
    FrameInterpolation_StopRecord();

    PLAY_LOG(4587);

    if (CVarGetInteger(CVAR_CHEAT("TimeSync"), 0)) {
        const int maxRealDaySeconds = 86400;
        const int maxInGameDayTicks = 65536;

        int secs = (int)Play_GetRealTime();
        float percent = (float)secs / (float)maxRealDaySeconds;

        int newIngameTime = maxInGameDayTicks * percent;

        gSaveContext.dayTime = newIngameTime;
    }
}

u8 PlayerGrounded(Player* player) {
    return player->actor.bgCheckFlags & 1;
}

// original name: "Game_play_demo_mode_check"
s32 Play_InCsMode(PlayState* play) {
    return (play->csCtx.state != CS_STATE_IDLE) || Player_InCsMode(play);
}

f32 func_800BFCB8(PlayState* play, MtxF* mf, Vec3f* pos) {
    CollisionPoly poly;
    f32 temp1;
    f32 temp2;
    f32 temp3;
    f32 floorY = BgCheck_AnyRaycastFloor1(&play->colCtx, &poly, pos);

    if (floorY > BGCHECK_Y_MIN) {
        f32 nx = COLPOLY_GET_NORMAL(poly.normal.x);
        f32 ny = COLPOLY_GET_NORMAL(poly.normal.y);
        f32 nz = COLPOLY_GET_NORMAL(poly.normal.z);
        s32 pad[5];

        temp1 = sqrtf(1.0f - SQ(nx));

        if (temp1 != 0.0f) {
            temp2 = ny * temp1;
            temp3 = -nz * temp1;
        } else {
            temp3 = 0.0f;
            temp2 = 0.0f;
        }

        mf->xx = temp1;
        mf->yx = -nx * temp2;
        mf->zx = nx * temp3;
        mf->xy = nx;
        mf->yy = ny;
        mf->zy = nz;
        mf->yz = temp3;
        mf->zz = temp2;
        mf->wx = 0.0f;
        mf->wy = 0.0f;
        mf->xz = 0.0f;
        mf->wz = 0.0f;
        mf->xw = pos->x;
        mf->yw = floorY;
        mf->zw = pos->z;
        mf->ww = 1.0f;
    } else {
        mf->xy = 0.0f;
        mf->zx = 0.0f;
        mf->yx = 0.0f;
        mf->xx = 0.0f;
        mf->wz = 0.0f;
        mf->xz = 0.0f;
        mf->wy = 0.0f;
        mf->wx = 0.0f;
        mf->zz = 0.0f;
        mf->yz = 0.0f;
        mf->zy = 0.0f;
        mf->yy = 1.0f;
        mf->xw = pos->x;
        mf->yw = pos->y;
        mf->zw = pos->z;
        mf->ww = 1.0f;
    }

    return floorY;
}

void* Play_LoadFile(PlayState* play, RomFile* file) {
    size_t size;
    void* allocp;

    size = file->vromEnd - file->vromStart;
    allocp = GAMESTATE_ALLOC_MC(&play->state, size);
    DmaMgr_SendRequest1(allocp, file->vromStart, size, __FILE__, __LINE__);

    return allocp;
}

void Play_InitEnvironment(PlayState* play, s16 skyboxId) {
    Skybox_Init(&play->state, &play->skyboxCtx, skyboxId);
    Environment_Init(play, &play->envCtx, 0);
}

void Play_InitScene(PlayState* play, s32 spawn) {
    play->curSpawn = spawn;

    play->linkActorEntry = NULL;
    play->unk_11DFC = NULL;
    play->setupEntranceList = NULL;
    play->setupExitList = NULL;
    play->cUpElfMsgs = NULL;
    play->setupPathList = NULL;

    play->numSetupActors = 0;

    Object_InitBank(play, &play->objectCtx);
    LightContext_Init(play, &play->lightCtx);
    TransitionActor_InitContext(&play->state, &play->transiActorCtx);
    func_80096FD4(play, &play->roomCtx.curRoom);
    YREG(15) = 0;
    gSaveContext.worldMapArea = 0;
    Scene_ExecuteCommands(play, play->sceneSegment);
    Play_InitEnvironment(play, play->skyboxId);
}

void Play_SpawnScene(PlayState* play, s32 sceneId, s32 spawn) {
    uint8_t mqMode = CVarGetInteger(CVAR_GENERAL("BetterDebugWarpScreenMQMode"), WARP_MODE_OVERRIDE_OFF);
    int16_t mqModeScene = CVarGetInteger(CVAR_GENERAL("BetterDebugWarpScreenMQModeScene"), -1);
    if (mqMode != WARP_MODE_OVERRIDE_OFF && sceneId != mqModeScene) {
        CVarClear(CVAR_GENERAL("BetterDebugWarpScreenMQMode"));
        CVarClear(CVAR_GENERAL("BetterDebugWarpScreenMQModeScene"));
    }

    OTRPlay_SpawnScene(play, sceneId, spawn);
}

void func_800C016C(PlayState* play, Vec3f* src, Vec3f* dest) {
    f32 w;

    Matrix_Mult(&play->viewProjectionMtxF, MTXMODE_NEW);
    Matrix_MultVec3f(src, dest);

    w = play->viewProjectionMtxF.ww + (play->viewProjectionMtxF.wx * src->x + play->viewProjectionMtxF.wy * src->y +
                                       play->viewProjectionMtxF.wz * src->z);

    dest->x = (SCREEN_WIDTH / 2) + ((dest->x / w) * (SCREEN_WIDTH / 2));
    dest->y = (SCREEN_HEIGHT / 2) - ((dest->y / w) * (SCREEN_HEIGHT / 2));
}

s16 Play_CreateSubCamera(PlayState* play) {
    s16 i;

    for (i = SUBCAM_FIRST; i < NUM_CAMS; i++) {
        if (play->cameraPtrs[i] == NULL) {
            break;
        }
    }

    if (i == NUM_CAMS) {
        osSyncPrintf(VT_COL(RED, WHITE) "camera control: error: fulled sub camera system area\n" VT_RST);
        return SUBCAM_NONE;
    }

    osSyncPrintf("camera control: " VT_BGCOL(CYAN) " " VT_COL(WHITE, BLUE) " create new sub camera [%d] " VT_BGCOL(
                     CYAN) " " VT_RST "\n",
                 i);

    play->cameraPtrs[i] = &play->subCameras[i - SUBCAM_FIRST];
    Camera_Init(play->cameraPtrs[i], &play->view, &play->colCtx, play);
    play->cameraPtrs[i]->thisIdx = i;

    return i;
}

s16 Play_GetActiveCamId(PlayState* play) {
    return play->activeCamera;
}

s16 Play_ChangeCameraStatus(PlayState* play, s16 camId, s16 status) {
    s16 camIdx = (camId == SUBCAM_ACTIVE) ? play->activeCamera : camId;

    if (status == CAM_STAT_ACTIVE) {
        play->activeCamera = camIdx;
    }

    return Camera_ChangeStatus(play->cameraPtrs[camIdx], status);
}

void Play_ClearCamera(PlayState* play, s16 camId) {
    s16 camIdx = (camId == SUBCAM_ACTIVE) ? play->activeCamera : camId;

    if (camIdx == MAIN_CAM) {
        osSyncPrintf(VT_COL(RED, WHITE) "camera control: error: never clear camera !!\n" VT_RST);
    }

    if (play->cameraPtrs[camIdx] != NULL) {
        Camera_ChangeStatus(play->cameraPtrs[camIdx], CAM_STAT_UNK100);
        play->cameraPtrs[camIdx] = NULL;
        osSyncPrintf("camera control: " VT_BGCOL(CYAN) " " VT_COL(WHITE, BLUE) " clear sub camera [%d] " VT_BGCOL(
                         CYAN) " " VT_RST "\n",
                     camIdx);
    } else {
        osSyncPrintf(VT_COL(RED, WHITE) "camera control: error: camera No.%d already cleared\n" VT_RST, camIdx);
    }
}

void Play_ClearAllSubCameras(PlayState* play) {
    s16 i;

    for (i = SUBCAM_FIRST; i < NUM_CAMS; i++) {
        if (play->cameraPtrs[i] != NULL) {
            Play_ClearCamera(play, i);
        }
    }

    play->activeCamera = MAIN_CAM;

    // SoH multiplayer: scene transitions clear all sub-cameras here, but
    // our P2 sub-camera id is held in a global (gCoopP2CameraId) that
    // doesn't get notified. Without this reset, gCoopP2CameraId points
    // at a now-NULL slot, the next P2 Player_UpdateCommon swaps
    // activeCamera to it, and Camera_ChangeMode dereferences NULL.
    // Concrete repro: entering the Gohma boss room from the Deku Tree
    // tunnel — scene-transition wipes sub-cams, first frame of the new
    // scene's Player_Update for P2 hits a NULL camera and crashes in
    // Camera_ChangeModeFlags. Resetting to SUBCAM_NONE here makes the
    // next P2 spawn re-allocate a fresh sub-camera (the existing
    // SUBCAM_NONE check at the spawn site in z_player.c handles this).
    extern s32 gCoopP2CameraId;
    gCoopP2CameraId = SUBCAM_NONE;
    // Also invalidate the staged P2 view-projection matrix — it points
    // at the previous scene's geometry. The culling code in z_actor.c
    // gates on gCoopP2ReticleValid; clearing it here means the first
    // frame of the new scene won't try to test actors against a stale
    // matrix from the wrong scene.
    gCoopP2ReticleValid = 0;
}

Camera* Play_GetCamera(PlayState* play, s16 camId) {
    s16 camIdx = (camId == SUBCAM_ACTIVE) ? play->activeCamera : camId;

    return play->cameraPtrs[camIdx];
}

s32 Play_CameraSetAtEye(PlayState* play, s16 camId, Vec3f* at, Vec3f* eye) {
    s32 ret = 0;
    s16 camIdx = (camId == SUBCAM_ACTIVE) ? play->activeCamera : camId;
    Camera* camera = play->cameraPtrs[camIdx];
    Player* player;

    ret |= Camera_SetParam(camera, 1, at);
    ret <<= 1;
    ret |= Camera_SetParam(camera, 2, eye);

    camera->dist = Math3D_Vec3f_DistXYZ(at, eye);

    player = camera->player;
    if (player != NULL) {
        camera->posOffset.x = at->x - player->actor.world.pos.x;
        camera->posOffset.y = at->y - player->actor.world.pos.y;
        camera->posOffset.z = at->z - player->actor.world.pos.z;
    } else {
        camera->posOffset.x = camera->posOffset.y = camera->posOffset.z = 0.0f;
    }

    camera->atLERPStepScale = 0.01f;

    return ret;
}

s32 Play_CameraSetAtEyeUp(PlayState* play, s16 camId, Vec3f* at, Vec3f* eye, Vec3f* up) {
    s32 ret = 0;
    s16 camIdx = (camId == SUBCAM_ACTIVE) ? play->activeCamera : camId;
    Camera* camera = play->cameraPtrs[camIdx];
    Player* player;

    ret |= Camera_SetParam(camera, 1, at);
    ret <<= 1;
    ret |= Camera_SetParam(camera, 2, eye);
    ret <<= 1;
    ret |= Camera_SetParam(camera, 4, up);

    camera->dist = Math3D_Vec3f_DistXYZ(at, eye);

    player = camera->player;
    if (player != NULL) {
        camera->posOffset.x = at->x - player->actor.world.pos.x;
        camera->posOffset.y = at->y - player->actor.world.pos.y;
        camera->posOffset.z = at->z - player->actor.world.pos.z;
    } else {
        camera->posOffset.x = camera->posOffset.y = camera->posOffset.z = 0.0f;
    }

    camera->atLERPStepScale = 0.01f;

    return ret;
}

s32 Play_CameraSetFov(PlayState* play, s16 camId, f32 fov) {
    s32 ret = Camera_SetParam(play->cameraPtrs[camId], 0x20, &fov) & 1;

    return ret;
}

s32 Play_SetCameraRoll(PlayState* play, s16 camId, s16 roll) {
    s16 camIdx = (camId == SUBCAM_ACTIVE) ? play->activeCamera : camId;
    Camera* camera = play->cameraPtrs[camIdx];

    camera->roll = roll;

    return 1;
}

void Play_CopyCamera(PlayState* play, s16 camId1, s16 camId2) {
    s16 camIdx2 = (camId2 == SUBCAM_ACTIVE) ? play->activeCamera : camId2;
    s16 camIdx1 = (camId1 == SUBCAM_ACTIVE) ? play->activeCamera : camId1;

    Camera_Copy(play->cameraPtrs[camIdx1], play->cameraPtrs[camIdx2]);
}

s32 func_800C0808(PlayState* play, s16 camId, Player* player, s16 setting) {
    Camera* camera;
    s16 camIdx = (camId == SUBCAM_ACTIVE) ? play->activeCamera : camId;

    camera = play->cameraPtrs[camIdx];
    Camera_InitPlayerSettings(camera, player);
    return Camera_ChangeSetting(camera, setting);
}

s32 Play_CameraChangeSetting(PlayState* play, s16 camId, s16 setting) {
    return Camera_ChangeSetting(Play_GetCamera(play, camId), setting);
}

void func_800C08AC(PlayState* play, s16 camId, s16 arg2) {
    s16 camIdx = (camId == SUBCAM_ACTIVE) ? play->activeCamera : camId;
    s16 i;

    Play_ClearCamera(play, camIdx);

    for (i = SUBCAM_FIRST; i < NUM_CAMS; i++) {
        if (play->cameraPtrs[i] != NULL) {
            osSyncPrintf(
                VT_COL(RED, WHITE) "camera control: error: return to main, other camera left. %d cleared!!\n" VT_RST,
                i);
            Play_ClearCamera(play, i);
        }
    }

    if (arg2 <= 0) {
        Play_ChangeCameraStatus(play, MAIN_CAM, CAM_STAT_ACTIVE);
        play->cameraPtrs[MAIN_CAM]->childCamIdx = play->cameraPtrs[MAIN_CAM]->parentCamIdx = SUBCAM_FREE;
    } else {
        OnePointCutscene_Init(play, 1020, arg2, NULL, MAIN_CAM);
    }
}

s16 Play_CameraGetUID(PlayState* play, s16 camId) {
    Camera* camera = play->cameraPtrs[camId];

    if (camera != NULL) {
        return camera->uid;
    } else {
        return -1;
    }
}

s16 func_800C09D8(PlayState* play, s16 camId, s16 arg2) {
    Camera* camera = play->cameraPtrs[camId];

    if (camera != NULL) {
        return 0;
    } else if (camera->uid != arg2) {
        return 0;
    } else if (camera->status != CAM_STAT_ACTIVE) {
        return 2;
    } else {
        return 1;
    }
}

void Play_SaveSceneFlags(PlayState* play) {
    SavedSceneFlags* savedSceneFlags = &gSaveContext.sceneFlags[play->sceneNum];

    savedSceneFlags->chest = play->actorCtx.flags.chest;
    savedSceneFlags->swch = play->actorCtx.flags.swch;
    savedSceneFlags->clear = play->actorCtx.flags.clear;
    savedSceneFlags->collect = play->actorCtx.flags.collect;
}

void Play_SetRespawnData(PlayState* play, s32 respawnMode, s16 entranceIndex, s32 roomIndex, s32 playerParams,
                         Vec3f* pos, s16 yaw) {
    RespawnData* respawnData = &gSaveContext.respawn[respawnMode];

    respawnData->entranceIndex = entranceIndex;
    respawnData->roomIndex = roomIndex;
    respawnData->pos = *pos;
    respawnData->yaw = yaw;
    respawnData->playerParams = playerParams;
    respawnData->tempSwchFlags = play->actorCtx.flags.tempSwch;
    respawnData->tempCollectFlags = play->actorCtx.flags.tempCollect;
}

void Play_SetupRespawnPoint(PlayState* play, s32 respawnMode, s32 playerParams) {
    Player* player = GET_PLAYER(play);
    s32 entranceIndex;
    s8 roomIndex;

    if ((play->sceneNum != SCENE_FAIRYS_FOUNTAIN) && (play->sceneNum != SCENE_GROTTOS)) {
        roomIndex = play->roomCtx.curRoom.num;
        entranceIndex = gSaveContext.entranceIndex;
        Play_SetRespawnData(play, respawnMode, entranceIndex, roomIndex, playerParams, &player->actor.world.pos,
                            player->actor.shape.rot.y);
    }
}

void Play_TriggerVoidOut(PlayState* play) {
    gSaveContext.respawn[RESPAWN_MODE_DOWN].tempSwchFlags = play->actorCtx.flags.tempSwch;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].tempCollectFlags = play->actorCtx.flags.tempCollect;
    gSaveContext.respawnFlag = 1;
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->nextEntranceIndex = gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex;
    play->transitionType = TRANS_TYPE_FADE_BLACK;
}

// SoH multiplayer: reload the current entrance with a fast fade. Useful for
// iterating on co-op spawn behavior without going to the title screen.
void Play_TriggerSceneReload(PlayState* play) {
    gSaveContext.respawn[RESPAWN_MODE_DOWN].tempSwchFlags = play->actorCtx.flags.tempSwch;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].tempCollectFlags = play->actorCtx.flags.tempCollect;
    gSaveContext.respawnFlag = 0;
    play->nextEntranceIndex = gSaveContext.entranceIndex;
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
}

void Play_LoadToLastEntrance(PlayState* play) {
    gSaveContext.respawnFlag = -1;
    play->transitionTrigger = TRANS_TRIGGER_START;

    if ((play->sceneNum == SCENE_GANONS_TOWER_COLLAPSE_INTERIOR) ||
        (play->sceneNum == SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR) ||
        (play->sceneNum == SCENE_INSIDE_GANONS_CASTLE_COLLAPSE) || (play->sceneNum == SCENE_GANON_BOSS)) {
        play->nextEntranceIndex = ENTR_GANONS_TOWER_COLLAPSE_EXTERIOR_0;
        Item_Give(play, ITEM_SWORD_MASTER);
    } else if ((gSaveContext.entranceIndex == ENTR_HYRULE_FIELD_11) ||
               (gSaveContext.entranceIndex == ENTR_HYRULE_FIELD_12) ||
               (gSaveContext.entranceIndex == ENTR_HYRULE_FIELD_13) ||
               (gSaveContext.entranceIndex == ENTR_HYRULE_FIELD_15)) {
        play->nextEntranceIndex = ENTR_HYRULE_FIELD_CENTER_EXIT;
    } else {
        play->nextEntranceIndex = gSaveContext.entranceIndex;
    }

    play->transitionType = TRANS_TYPE_FADE_BLACK;
}

void Play_TriggerRespawn(PlayState* play) {
    Play_SetupRespawnPoint(play, RESPAWN_MODE_DOWN, 0xDFF);
    Play_LoadToLastEntrance(play);
}

s32 func_800C0CB8(PlayState* play) {
    return (play->roomCtx.curRoom.meshHeader->base.type != 1) && (YREG(15) != 0x20) && (YREG(15) != 0x30) &&
           (YREG(15) != 0x40) && (play->sceneNum != SCENE_CASTLE_COURTYARD_GUARDS_DAY);
}

s32 FrameAdvance_IsEnabled(PlayState* play) {
    return !!play->frameAdvCtx.enabled;
}

s32 func_800C0D34(PlayState* play, Actor* actor, s16* yaw) {
    TransitionActorEntry* transitionActor;
    s32 frontRoom;

    if (actor->category != ACTORCAT_DOOR) {
        return 0;
    }

    transitionActor = &play->transiActorCtx.list[(u16)actor->params >> 10];
    frontRoom = transitionActor->sides[0].room;

    if (frontRoom == transitionActor->sides[1].room) {
        return 0;
    }

    if (frontRoom == actor->room) {
        *yaw = actor->shape.rot.y;
    } else {
        *yaw = actor->shape.rot.y + 0x8000;
    }

    return 1;
}

s32 func_800C0DB4(PlayState* play, Vec3f* pos) {
    WaterBox* waterBox;
    CollisionPoly* poly;
    Vec3f waterSurfacePos;
    s32 bgId;

    waterSurfacePos = *pos;

    if (WaterBox_GetSurface1(play, &play->colCtx, waterSurfacePos.x, waterSurfacePos.z, &waterSurfacePos.y,
                             &waterBox) == true &&
        pos->y < waterSurfacePos.y &&
        BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &waterSurfacePos) != BGCHECK_Y_MIN) {
        return true;
    } else {
        return false;
    }
}

void Play_PerformSave(PlayState* play) {
    if (play != NULL && gSaveContext.fileNum != 0xFF) {
        Play_SaveSceneFlags(play);
        gSaveContext.savedSceneNum = play->sceneNum;

        // Track values from temp B
        uint8_t prevB = gSaveContext.equips.buttonItems[0];
        uint8_t prevStatus = gSaveContext.buttonStatus[0];

        // Replicate the B button restore from minigames/epona that kaleido does
        if (gSaveContext.equips.buttonItems[0] == ITEM_SLINGSHOT || gSaveContext.equips.buttonItems[0] == ITEM_BOW ||
            gSaveContext.equips.buttonItems[0] == ITEM_BOMBCHU ||
            gSaveContext.equips.buttonItems[0] == ITEM_FISHING_POLE ||
            (gSaveContext.equips.buttonItems[0] == ITEM_NONE && !Flags_GetInfTable(INFTABLE_SWORDLESS))) {

            gSaveContext.equips.buttonItems[0] = gSaveContext.buttonStatus[0];
            Interface_RandoRestoreSwordless();
        }

        Save_SaveFile();

        // Restore temp B values back
        gSaveContext.equips.buttonItems[0] = prevB;
        gSaveContext.buttonStatus[0] = prevStatus;
    }
}
