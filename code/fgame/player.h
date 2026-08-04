/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// player.h: Class definition of the player.

#pragma once

#include "g_local.h"
#include "vector.h"
#include "entity.h"
#include "weapon.h"
#include "sentient.h"
#include "navigate.h"
#include "misc.h"
#include "bspline.h"
#include "camera.h"
#include "specialfx.h"
#include "characterstate.h"
#include "actor.h"
#include "vehicle.h"
#include "dm_manager.h"
#include "scriptdelegate.h"

extern Event EV_Player_EndLevel;
extern Event EV_Player_GiveCheat;
extern Event EV_Player_GodCheat;
extern Event EV_Player_NoTargetCheat;
extern Event EV_Player_NoClipCheat;
extern Event EV_Player_GameVersion;
extern Event EV_Player_Fov;
extern Event EV_Player_WhatIs;
extern Event EV_Player_Respawn;
extern Event EV_Player_WatchActor;
extern Event EV_Player_StopWatchingActor;
extern Event EV_Player_DoStats;
extern Event EV_Player_EnterIntermission;
extern Event EV_GetViewangles;
extern Event EV_SetViewangles;
extern Event EV_Player_AutoJoinDMTeam;
extern Event EV_Player_JoinDMTeam;
extern Event EV_Player_Respawn;
extern Event EV_Player_PrimaryDMWeapon;
extern Event EV_Player_StuffText;

enum painDirection_t {
    PAIN_NONE,
    PAIN_FRONT,
    PAIN_LEFT,
    PAIN_RIGHT,
    PAIN_REAR
};

typedef enum {
    ANIMSLOT_PAIN     = 4,
    ANIMSLOT_TESTANIM = 7
} playerAnimSlot_t;

typedef enum {
    PVT_NONE_SET,
    PVT_ALLIED_START,
    PVT_ALLIED_AIRBORNE,
    PVT_ALLIED_MANON,
    PVT_ALLIED_SAS,
    PVT_ALLIED_PILOT,
    PVT_ALLIED_ARMY,
    PVT_ALLIED_RANGER,
    PVT_ALLIED_AMERICAN,
    PVT_ALLIED_BRITISH,
    PVT_ALLIED_RUSSIAN,
    PVT_ALLIED_END,
    PVT_AXIS_START,
    PVT_AXIS_AXIS1,
    PVT_AXIS_GERMAN = PVT_AXIS_AXIS1,
    PVT_AXIS_AXIS2,
    PVT_AXIS_ITALIAN = PVT_AXIS_AXIS2,
    PVT_AXIS_AXIS3,
    PVT_AXIS_AXIS4,
    PVT_AXIS_AXIS5,
    PVT_AXIS_END
} voicetype_t;

typedef enum jailstate_e {
    JAILSTATE_NONE,
    JAILSTATE_ESCAPE,
    JAILSTATE_ASSIST_ESCAPE
} jailstate_t;

typedef enum nationality_e {
    NA_NONE,
    NA_AMERICAN,
    NA_BRITISH,
    NA_RUSSIAN,
    NA_GERMAN,
    NA_ITALIAN
} nationality_t;

typedef void (Player::*movecontrolfunc_t)(void);

typedef struct vma_s {
    str   name;
    float speed;
} vma_t;

#define MAX_SPEED_MULTIPLIERS 4
#define MAX_ANIM_SLOT         16
#define MAX_TRAILS            2

class Player : public Sentient
{
    friend class Camera;
    friend class Vehicle;
    friend class TurretGun;
    friend class VehicleTurretGun;

private:
    static Condition<Player> m_conditions[];
    static movecontrolfunc_t MoveStartFuncs[];

    StateMap *statemap_Legs;
    StateMap *statemap_Torso;

    State *currentState_Legs;
    State *currentState_Torso;

    str   last_torso_anim_name;
    str   last_leg_anim_name;
    str   partAnim[2];
    int   m_iPartSlot[2];
    float m_fPartBlends[2];
    str   partOldAnim[2];
    float partBlendMult[2];
    str   m_sPainAnim;
    float m_fPainBlend;

    bool                     animdone_Legs;
    bool                     animdone_Torso;
    bool                     animdone_Pain;
    Container<Conditional *> legs_conditionals;
    Container<Conditional *> torso_conditionals;
    Conditional             *m_pLegsPainCond;
    Conditional             *m_pTorsoPainCond;

    float m_fLastDeltaTime;

    movecontrol_t movecontrol;
    int           m_iMovePosFlags;
    int           last_camera_type;

    Vector oldvelocity;
    Vector old_v_angle;
    Vector oldorigin;
    float  animspeed;
    float  airspeed;
    Vector m_vPushVelocity;

    // blend
    float    blend[4];
    float    fov;
    float    selectedfov;
    qboolean m_iInZoomMode;

    // aiming direction
    Vector v_angle;

    int buttons;
    int new_buttons;
    int server_new_buttons;

    float respawn_time;
    int   last_attack_button;

    // damage blend
    float  damage_blood;
    float  damage_alpha;
    Vector damage_blend;
    Vector damage_from;
    Vector damage_angles;
    float  damage_count;
    float  damage_yaw;
    float  next_painsound_time;
    str    waitForState;

    SafePtr<Camera> camera;
    SafePtr<Camera> actor_camera;
    SimpleActorPtr  actor_to_watch;

    qboolean actor_camera_right;
    qboolean starting_actor_camera_right;

    // music stuff
    int   music_current_mood;
    int   music_fallback_mood;
    float music_current_volume;
    float music_saved_volume;
    float music_volume_fade_time;

    int   reverb_type;
    float reverb_level;

    qboolean        gibbed;
    float           pain;
    painDirection_t pain_dir;
    meansOfDeath_t  pain_type;
    int             pain_location;
    bool            take_pain;
    int             nextpaintime;
    float           m_fMineDist;
    float           m_fMineCheckTime;
    str             m_sDmPrimary;
    bool            m_bIsInJail;
    bool            knockdown;

    bool   canfall;
    bool   falling;
    int    feetfalling;
    Vector falldir;

    bool mediumimpact;
    bool hardimpact;

    qboolean music_forced;

    usercmd_t  last_ucmd;
    usereyes_t last_eyeinfo;

    // movement variables
    float  animheight;
    Vector yaw_forward;
    Vector yaw_left;

    SafePtr<Entity> atobject;
    float           atobject_dist;
    Vector          atobject_dir;

    SafePtr<Entity> toucheduseanim;
    int             useanim_numloops;
    SafePtr<Entity> useitem_in_use;

    float move_left_vel;
    float move_right_vel;
    float move_backward_vel;
    float move_forward_vel;
    float move_up_vel;
    float move_down_vel;
    int   moveresult;

    float damage_multiplier;

    //
    // Talking
    //
    voicetype_t m_voiceType;
    float       m_fTalkTime;

    int num_deaths;
    int num_kills;
    int num_won_matches;
    int num_lost_matches;
    int num_team_kills;
    int m_iLastNumTeamKills;

    bool m_bTempSpectator;
    bool m_bSpectator;
    bool m_bSpectatorSwitching;
    bool m_bAllowFighting;
    bool m_bReady;
    int  m_iPlayerSpectating;

    teamtype_t             dm_team;
    class SafePtr<DM_Team> current_team;
    float                  m_fTeamSelectTime;
    class PlayerStart     *m_pLastSpawnpoint;

    //
    // Vote
    ///
    bool  voted;
    int   votecount;
    float m_fLastVoteTime;
    float m_fNextVoteOptionTime;

    float           m_fWeapSelectTime;
    float           fAttackerDispTime;
    SafePtr<Entity> pAttackerDistPointer;

    int         m_iInfoClient;
    int         m_iInfoClientHealth;
    float       m_fInfoClientTime;
    bool        m_bDeathSpectator;
    jailstate_t m_jailstate;

#ifdef OPM_FEATURES
    bool m_bShowingHint;
#endif

public:
    MulticastDelegate<void(const str& text)> delegate_stufftext;
    MulticastDelegate<void()>                delegate_spawned;

    static ScriptDelegate scriptDelegate_connected;
    static ScriptDelegate scriptDelegate_disconnecting;
    static ScriptDelegate scriptDelegate_spawned;
    static ScriptDelegate scriptDelegate_damage;
    static ScriptDelegate scriptDelegate_kill;
    static ScriptDelegate scriptDelegate_textMessage;

public:
    int m_iNumObjectives;
    int m_iObjectivesCompleted;

    str m_sPerferredWeaponOverride;

    float  m_fHealRate;
    float  m_fRecoilTarget;   // HZM coop - accumulated view-recoil pitch (deg) from firing, decays to 0
    float  m_fRecoilApplied;  // HZM coop - recoil pitch currently folded into delta_angles (for recovery)
    Vector m_vViewPos;
    Vector m_vViewAng;
    Vector mvTrail[MAX_TRAILS];
    Vector mvTrailEyes[MAX_TRAILS];
    int    mCurTrailOrigin;
    int    mLastTrailTime;
    int    m_iNumHitsTaken;
    int    m_iNumEnemiesKilled;
    int    m_iNumObjectsDestroyed;
    int    m_iNumShotsFired;
    int    m_iNumHits;
    int    m_iNumHeadShots;
    int    m_iNumTorsoShots;
    int    m_iNumLeftLegShots;
    int    m_iNumRightLegShots;
    int    m_iNumGroinShots;
    int    m_iNumLeftArmShots;
    int    m_iNumRightArmShots;

    float m_fLastSprintTime;
    // HZM coop - SPRINT stamina (seconds of sprint remaining). Drains while sprinting, regens otherwise.
    // m_bCoopSprinting = the per-frame "is actually sprinting right now" flag (set in ClientMove).
    float m_fCoopStamina;
    bool  m_bCoopSprinting;
    // HZM coop [user 2026-08-02] - LOW-HEALTH LIMP (bug-1291). m_bCoopLimping is the per-frame "is
    // limping right now" flag, set in TickLimp and read by BOTH the COOP_LIMPING statemap conditional
    // (3P body) and the ClientMove speed clamp. m_iCoopLimpSent is the last coop_limpView value
    // stuffed to the OWNING client (-1 = never sent) so the camera limp is driven by the SERVER's
    // decision rather than each client recomputing a threshold - same channel dbno.scr uses for
    // coop_dbnoView, and the reason a server admin turning coop_limp off actually turns it off
    // everywhere instead of only on the body.
    bool  m_bCoopLimping;
    bool  m_bCoopWounded; // HZM coop - bug-1324: health-fraction test WITHOUT the ground term; gates sprint
    int   m_iCoopLimpSent;
    // HZM coop - client is in the 3P over-the-shoulder AIM stage (a pure client concept: cg_adsStage
    // cvar + camera envelopes), mirrored to the server via the u_shoulderaim userinfo key so the
    // aimed-walk slowdown applies ONLY to the shoulder stage (FP irons keep normal ADS speed).
    bool  m_bCoopShoulderAim;
    bool  m_bCoopView3p; // HZM coop - client renders third person (u_view3p userinfo mirror; manned-turret world-gun visibility)
    bool  m_bCoopGearLoop; // HZM coop - gear-rattle loop currently playing (follows sprint state)
    // HZM coop - seconds of CONTINUOUS sprinting so far (accumulates while sprinting, resets to 0 the moment
    // sprint stops). On the stop transition, if it reached coop_sprintBreathTime we play an out-of-breath pant.
    float m_fCoopSprintDur;
    // HZM coop - TAKE COVER [214]. Script requests the pose via the coop_setcover event (bind ->
    // name-append bus 26 -> coop_mod/takecover.scr); TickCoopCover() validates it per frame with
    // world traces (standing back-to-wall / crouched low-front) and feeds the results to the
    // COOP_COVER / COOP_COVER_LOW / COOP_BLINDFIRE statemap conditions. The engine - not the
    // statemap or script - is the single authority for dropping out (movement, jump, death,
    // mount, geometry gone), so the .st "!" exits restore the default states cleanly.
    // HZM coop [user 08-02]: DBNO state published to the engine so turret/vehicle mount can
    // refuse a downed player. Mirrors the coop_setcover setter directly below.
    bool  m_bCoopDbno;
    bool  m_bCoopCoverRequested; // player asked for cover (toggled by coop_setcover)
    bool  m_bCoopCoverWall;      // requested + standing back-to-wall pose valid this frame
    bool  m_bCoopCoverLow;       // requested + crouched low-cover pose valid this frame
    bool  m_bCoopBlindfire;      // covered + fire held + blindfire-capable weapon this frame
    Vector m_vCoopCoverNormal;   // anchored cover OUT normal (wall: away from wall; low: back at player) [215]
    int    m_iCoopCoverSide;     // 1 = opening LEFT of the pose, -1 = RIGHT [215]
    bool   m_bCoopCoverPeek;     // RMB peek-aim from cover (real aiming, cover held) [215]
    float  m_fCoopVehTurretTime; // level.time stamp while manning a VEHICLE turret (jeep .30cal pose) [219]
    float  m_fCoopProbeTime;     // GUNNERPROBE diagnostic throttle [221 - REMOVE after bug-309 closes]
    int    m_iCoopSpeedBase;     // SPEEDPROBE: ps.speed before the ADS/weapon mults [222 - REMOVE with probe]
    int    m_iCoopVarCoverLast;  // last coop_incover entity-var value pushed to script (change-gated) [235]
    // HZM coop - LOBBY INPUT bridge: while in the pre-mission lobby, read A/D (rightmove) + F (BUTTON_USE)
    // from the usercmd and publish self.coop_lobbyInput (31=next uniform, 32=prev, 33=ready) so the lobby
    // script reacts with NO client key binds - works for every client (host + remote), nothing to restore.
    bool   m_bCoopLobbyInputOn;   // enabled by the coop_lobbyinput script event for the duration of the lobby
    int    m_iCoopLobbyRightPrev; // sign of last frame's rightmove (0/-1/1) for one-action-per-tap edge detect
    bool   m_bCoopLobbyUsePrev;   // BUTTON_USE state last frame (F press edge)
    // HZM coop - LOBBY CURSOR bridge: derive a screen cursor from usercmd view-angle deltas + read the left
    // mouse button, so the lobby (and future loadout / skill-tree UIs) can have CLICKABLE buttons.
    bool   m_bCoopLobbyCursorOn;  // enabled by the coop_lobbycursor script event
    bool   m_bCoopLobbyCurInit;   // seed prev-angles on the first frame so the cursor doesn't jump
    int    m_iCoopLobbyYawPrev;   // last frame's ucmd yaw (short) for the delta
    int    m_iCoopLobbyPitchPrev; // last frame's ucmd pitch (short) for the delta
    float  m_fCoopLobbyCurX;      // cursor position, virtual 640x480
    float  m_fCoopLobbyCurY;
    bool   m_bCoopLobbyAtkPrev;   // BUTTON_ATTACKLEFT last frame (left-click press edge)
    // HZM coop - BOT COMBAT DRIVE (dev/test only, coop_botInput): server-side usercmd injection so a
    // connected client auto-aims + fires + advances on the nearest visible German. Lets the 4 test
    // clients hold a real firefight UNATTENDED (the AI genuinely engages them - real bullets, LOS,
    // retaliation - which the script damage-sim can't produce). Cvar default 0 = pure vanilla.
    SafePtr<Sentient> m_pCoopBotTarget;   // cached target enemy (revalidated on the retarget tick)
    int               m_iCoopBotRetarget; // level.inttime of the next allowed target rescan (ms)
    Vector m_vCoopCoverBaseOrg;  // pose position at cover entry (peek slides away from it and back) [216]
    float  m_fCoopPeekFrac;      // 0..1 eased peek step-out fraction [216]
    float m_fCoopCoverBadTime;   // level.time the pose first went invalid (grace before drop)
    bool  m_bHasJumped;
    float m_fLastInvulnerableTime;
    int   m_iInvulnerableTimeRemaining;
    float m_fInvulnerableTimeElapsed;
    float m_fSpawnTimeLeft;
    bool  m_bWaitingForRespawn;
    bool  m_bShouldRespawn;

    //
    // Added in OPM
    //
    str               m_sVision;    // current vision
    str               m_sStateFile; // custom statefile
    bool              m_bFrozen;    // if player is frozen
    float             speed_multiplier[MAX_SPEED_MULTIPLIERS];
    ScriptThreadLabel m_killedLabel;
    bool              m_bConnected;
    str               m_lastcommand;

    //
    // View model animation
    //
    bool                animDoneVM;
    con_map<str, vma_t> vmalist;
    str                 m_sVMAcurrent;
    str                 m_sVMcurrent;
    float               m_fVMAtime;
    dtiki_t            *m_fpsTiki;

private:
    int m_iInstantMessageTime;
    int m_iTextChatTime;

public:
    qboolean CondTrue(Conditional& condition);
    qboolean CondChance(Conditional& condition);
    qboolean CondHealth(Conditional& condition);
    qboolean CondPain(Conditional& condition);
    qboolean CondBlocked(Conditional& condition);
    qboolean CondOnGround(Conditional& condition);
    qboolean CondHasWeapon(Conditional& condition);
    qboolean CondNewWeapon(Conditional& condition);
    qboolean CondImmediateSwitch(Conditional& condition);
    qboolean CondUseWeapon(Conditional& condition);
    qboolean CondUseWeaponClass(Conditional& condition);
    qboolean CondWeaponActive(Conditional& condition);
    qboolean CondWeaponClassActive(Conditional& condition);
    qboolean CondWeaponCurrentFireAnim(Conditional& condition); // mohaas
    qboolean CondWeaponReadyToFire(Conditional& condition);
    qboolean CondWeaponClassReadyToFire(Conditional& condition);
    qboolean CondUsingVehicle(Conditional& condition);
    qboolean CondVehicleType(Conditional& condition);
    qboolean CondIsPassenger(Conditional& condition);
    qboolean CondIsDriver(Conditional& condition);
    qboolean CondUsingTurret(Conditional& condition);
    qboolean CondIsEscaping(Conditional& condition);        // mohaab
    qboolean CondAbleToDefuse(Conditional& condition);      // mohaab
    qboolean CondCanPlaceLandmine(Conditional& condition);  // mohaab
    qboolean CondOnLandmine(Conditional& condition);        // mohaab
    qboolean CondNearLandmine(Conditional& condition);      // mohaab
    void     MeasureLandmineDistances();                    // mohaab
    qboolean CondIsAssistingEscape(Conditional& condition); // mohaab
    qboolean CondTurretType(Conditional& condition);
    qboolean CondWeaponReadyToFireNoSound(Conditional& condition);
    qboolean CondPutAwayMain(Conditional& condition);
    qboolean CondPutAwayOffHand(Conditional& condition);
    qboolean CondAnyWeaponActive(Conditional& condition);
    qboolean CondAttackBlocked(Conditional& condition);
    qboolean CondBlockDelay(Conditional& condition);
    qboolean CondMuzzleClear(Conditional& condition);
    qboolean CondWeaponHasAmmo(Conditional& condition);
    qboolean CondWeaponHasAmmoInClip(Conditional& condition);
    qboolean CondReload(Conditional& condition);
    qboolean CondWeaponsHolstered(Conditional& condition);
    qboolean CondWeaponIsItem(Conditional& condition);
    qboolean CondNewWeaponIsItem(Conditional& condition);
    qboolean CondSemiAuto(Conditional& condition);
    qboolean CondMinChargeTime(Conditional& condition);
    qboolean CondMaxChargeTime(Conditional& condition);
    qboolean CondPositionType(Conditional& condition);
    qboolean CondMovementType(Conditional& condition);
    qboolean CondRun(Conditional& condition);
    qboolean CondUse(Conditional& condition);
    qboolean CondTurnLeft(Conditional& condition);
    qboolean CondTurnRight(Conditional& condition);
    qboolean CondForward(Conditional& condition);
    qboolean CondBackward(Conditional& condition);
    qboolean CondStrafeLeft(Conditional& condition);
    qboolean CondStrafeRight(Conditional& condition);
    qboolean CondJump(Conditional& condition);
    qboolean CondCrouch(Conditional& condition);
    qboolean CondJumpFlip(Conditional& condition);
    qboolean CondAnimDoneLegs(Conditional& condition);
    qboolean CondAnimDoneTorso(Conditional& condition);
    qboolean CondActionAnimDone(Conditional& condition);
    qboolean CondCanTurn(Conditional& condition);
    qboolean CondLeftVelocity(Conditional& condition);
    qboolean CondRightVelocity(Conditional& condition);
    qboolean CondBackwardVelocity(Conditional& condition);
    qboolean CondForwardVelocity(Conditional& condition);
    qboolean CondUpVelocity(Conditional& condition);
    qboolean CondDownVelocity(Conditional& condition);
    qboolean CondHasVelocity(Conditional& condition);
    qboolean Cond22DegreeSlope(Conditional& condition);
    qboolean Cond45DegreeSlope(Conditional& condition);
    qboolean CondRightLegHigh(Conditional& condition);
    qboolean CondLeftLegHigh(Conditional& condition);
    qboolean CondCanFall(Conditional& condition);
    qboolean CondAtDoor(Conditional& condition);
    qboolean CondFalling(Conditional& condition);
    qboolean CondMediumImpact(Conditional& condition);
    qboolean CondHardImpact(Conditional& condition);
    qboolean CondDead(Conditional& condition);
    qboolean CondPainType(Conditional& condition);
    qboolean CondPainDirection(Conditional& condition);
    qboolean CondPainLocation(Conditional& condition);
    qboolean CondPainThreshold(Conditional& condition);
    qboolean CondKnockDown(Conditional& condition);
    qboolean CondLegsState(Conditional& condition);
    qboolean CondTorsoState(Conditional& condition);
    qboolean CondAtUseAnim(Conditional& condition);
    qboolean CondTouchUseAnim(Conditional& condition);
    qboolean CondUseAnimFinished(Conditional& condition);
    qboolean CondAtUseObject(Conditional& condition);
    qboolean CondLoopUseObject(Conditional& condition);
    qboolean CondPush(Conditional& condition);
    qboolean CondPull(Conditional& condition);
    qboolean CondLadder(Conditional& condition);
    qboolean CondLookingUp(Conditional& condition);
    qboolean CondTopOfLadder(Conditional& condition);
    qboolean CondOnLadder(Conditional& condition);
    qboolean CondCanClimbUpLadder(Conditional& condition);
    qboolean CondCanClimbDownLadder(Conditional& condition);
    qboolean CondCanGetOffLadderTop(Conditional& condition);
    qboolean CondCanGetOffLadderBottom(Conditional& condition);
    qboolean CondCanStand(Conditional& condition);
    qboolean CondFacingUpSlope(Conditional& condition);
    qboolean CondFacingDownSlope(Conditional& condition);
    qboolean CondSolidForward(Conditional& condition);
    qboolean CondStateName(Conditional& condition);
    qboolean CondGroundEntity(Conditional& condition);
    qboolean CondCheckHeight(Conditional& condition);
    qboolean CondViewInWater(Conditional& condition);
    qboolean CondDuckedViewInWater(Conditional& condition);
    qboolean CondCheckMovementSpeed(Conditional& condition); // mohaas
    qboolean CondAttackPrimary(Conditional& condition);
    qboolean CondAttackSecondary(Conditional& condition);
    qboolean CondAttackButtonPrimary(Conditional& condition);
    qboolean CondAttackButtonSecondary(Conditional& condition);
    qboolean CondCoopAds(Conditional& condition); // HZM coop - aim down sights (dedicated bind)
    qboolean CondCoopSprinting(Conditional& condition); // HZM coop - sprinting this frame (legs statemap)
    qboolean CondCoopLimping(Conditional& condition);   // HZM coop - low-health limp this frame (legs statemap)
    qboolean CondCoopCover(Conditional& condition);     // HZM coop - standing back-to-wall cover pose valid (TickCoopCover)
    qboolean CondCoopCoverLow(Conditional& condition);  // HZM coop - crouched low-cover pose valid (TickCoopCover)
    qboolean CondCoopCoverPeek(Conditional& condition);      // HZM coop - RMB peek-aim from cover [215]
    qboolean CondCoopShoulderAim(Conditional& condition);    // HZM coop - 3P shoulder-aim stage live [231]
    qboolean CondCoopCoverOpenRight(Conditional& condition); // HZM coop - opening is RIGHT of the wall pose [215]
    qboolean CondCoopOnTurret(Conditional& condition);        // HZM coop - raw m_pTurret check (manning pose; vanilla edges need !HAS_WEAPON) [216]
    qboolean CondCoopBlindfire(Conditional& condition); // HZM coop - covered + fire held -> blind-fire states

    //
    // Added in OPM
    //====
    qboolean CondClientCommand(Conditional& condition);
    qboolean CondVariable(Conditional& condition);
    qboolean CondAnimDoneVM(Conditional& condition);
    qboolean CondVMAnim(Conditional& condition);
    //====

    // movecontrol functions
    void StartPush(void);
    void StartClimbLadder(void);
    void StartUseAnim(void);
    void StartLoopUseAnim(void);
    void SetupUseObject(void);

    void StartUseObject(Event *ev);
    void FinishUseObject(Event *ev);
    void FinishUseAnim(Event *ev);
    void Turn(Event *ev);
    void TurnUpdate(Event *ev);
    void TurnLegs(Event *ev);

    CLASS_PROTOTYPE(Player);

    Player();
    ~Player();
    void Init(void);

    void InitSound(void);
    void InitEdict(void);
    void InitClient(void);
    void InitPhysics(void);
    void InitPowerups(void);
    void InitWorldEffects(void);
    void InitMaxAmmo(void);
    void InitWeapons(void);
    void InitView(void);
    void InitModel(void);
    void InitState(void);
    void InitHealth(void);
    void InitInventory(void);
    void InitDeathmatch(void);
    void InitStats(void);
    //=======
    // Added in 2.30
    bool QueryLandminesAllowed() const;
    void EnsurePlayerHasAllowedWeapons();
    void EquipWeapons();
    //=======
    void EquipWeapons_ver8();
    void ChooseSpawnPoint(void);

    void EndLevel(Event *ev);
    void Respawn(Event *ev);

    void SetDeltaAngles(void) override;
    void setAngles(Vector ang) override;
    void SetViewangles(Event *ev);
    void GetViewangles(Event *ev);

    void DoUse(Event *ev);
    void Obituary(Entity *attacker, Entity *inflictor, int meansofdeath, int iLocation);
    void Killed(Event *ev) override;
    void Dead(Event *ev);
    void Pain(Event *ev);

    // ladder stuff
    void AttachToLadder(Event *ev);
    void UnattachFromLadder(Event *ev);
    void TweakLadderPos(Event *ev);
    void EnsureOverLadder(Event *ev);
    void EnsureForwardOffLadder(Event *ev);
    void TouchStuff(pmove_t *pm);

    //====
    // Added in 2.30
    void EventForceLandmineMeasure(Event *ev) override;
    str  GetCurrentDMWeaponType() const;
    //====

    void     GetMoveInfo(pmove_t *pm);
    void     SetMoveInfo(pmove_t *pm, usercmd_t *ucmd);
    pmtype_t GetMovePlayerMoveType(void);
    void     ClientMove(usercmd_t *ucmd);
    void     VehicleMove(usercmd_t *ucmd);
    void     TurretMove(usercmd_t *ucmd);
    void     CheckMoveFlags(void);
    void     ClientInactivityTimer(void);
    void     ClientThink(void) override;
    void     UpdateEnemies(void);

    void InitLegsStateTable(void);
    void InitTorsoStateTable(void);
    void LoadStateTable(void);
    void ResetState(Event *ev);
    void EvaluateState(State *forceTorso = NULL, State *forceLegs = NULL);

    void     CheckGround(void) override;
    void     UpdateViewAngles(usercmd_t *cmd);
    qboolean AnimMove(Vector& move, Vector *endpos = NULL);
    qboolean TestMove(Vector& pos, Vector *endpos = NULL);
    qboolean CheckMove(Vector& move, Vector *endpos = NULL);

    float CheckMoveDist(Vector& delta);
    float TestMoveDist(Vector& pos);

    void EndAnim_Legs(Event *ev);
    void EndAnim_Torso(Event *ev);
    void EndAnim_Pain(Event *ev);
    void SetPartAnim(const char *anim, bodypart_t slot = legs);
    void StopPartAnimating(bodypart_t part);
    void PausePartAnim(bodypart_t part);
    int  CurrentPartAnim(bodypart_t part) const;
    void AdjustAnimBlends(void);
    void PlayerAnimDelta(float *vDelta);
    void TouchedUseAnim(Entity *ent);

    void SelectPreviousItem(Event *ev);
    void SelectNextItem(Event *ev);
    void SelectPreviousWeapon(Event *ev);
    void SelectNextWeapon(Event *ev);
    void DropCurrentWeapon(Event *ev);
    void PlayerReload(Event *ev);
    void EventCorrectWeaponAttachments(Event *ev);

    void GiveCheat(Event *ev);
    void GiveWeaponCheat(Event *ev);
    void GiveAllCheat(Event *ev);
    void GiveNewWeaponsCheat(Event *ev); // Added in 2.0
    void GodCheat(Event *ev);
    void FullHeal(Event *ev);
    void NoTargetCheat(Event *ev);
    void NoclipCheat(Event *ev);
    void Kill(Event *ev);
    void SpawnEntity(Event *ev);
    void SpawnActor(Event *ev);
    void ListInventoryEvent(Event *ev);
    void EventGetIsEscaping(Event *ev);
    void EventJailEscapeStop(Event *ev);
    void EventJailAssistEscape(Event *ev);
    void EventJailEscape(Event *ev);
    void EventTeleport(Event *ev);
    void EventFace(Event *ev);
    void EventCoord(Event *ev);
    void EventTestAnim(Event *ev);

    void GameVersion(Event *ev);

    void EventSetSelectedFov(Event *ev);
    void SetSelectedFov(float newFov);
    void GetPlayerView(Vector *pos, Vector *angle);

    float CalcRoll(void);
    void  WorldEffects(void);
    void  AddBlend(float r, float g, float b, float a);
    void  CalcBlend(void) override;
    void  DamageFeedback(void);

    void CopyStats(Player *player);
    void UpdateStats(void);
    void UpdateMusic(void);
    void UpdateReverb(void);
    void UpdateMisc(void);

    void SetReverb(str type, float level);
    void SetReverb(int type, float level);
    void SetReverb(Event *ev);

    Camera *CurrentCamera(void);
    void    SetCamera(Camera *ent, float switchTime);
    void    CameraCut(void);
    void    CameraCut(Camera *ent);

    void SetPlayerView(
        Camera *camera,
        Vector  position,
        float   cameraoffset,
        Vector  ang,
        Vector  vel,
        float   camerablend[4],
        float   camerafov
    );
    void SetupView(void);

    void ProcessPmoveEvents(int event);

    void PlayerAngles(void);
    void FinishMove(void);
    void EndFrame(void) override;

    void   TestThread(Event *ev);
    Vector EyePosition(void) override;
    Vector GunTarget(bool bNoCollision, const vec3_t position, const vec3_t forward) override;

    void GotKill(Event *ev);
    void SetPowerupTimer(Event *ev);
    void UpdatePowerupTimer(Event *ev);

    void WhatIs(Event *ev);
    void ActorInfo(Event *ev);
    void Taunt(Event *ev);

    void ChangeMusic(const char *current, const char *fallback, qboolean force);
    void ChangeMusicVolume(float volume, float fade_time);
    void RestoreMusicVolume(float fade_time);

    void Archive(Archiver& arc) override;
    void ArchivePersistantData(Archiver& arc);

    void KillEnt(Event *ev);
    void RemoveEnt(Event *ev);
    void KillClass(Event *ev);
    void RemoveClass(Event *ev);

    void addOrigin(Vector org) override;

    void Jump(Event *ev);
    void JumpXY(Event *ev);

    void SetViewAngles(Vector angles) override;
    void AddViewRecoil(float fPitch); // HZM coop - add an upward view-recoil kick (deg) on weapon fire
    void SetTargetViewAngles(Vector angles) override;

    Vector GetViewAngles(void) override { return v_angle; };

    // HZM coop - expose the last command buttons so server-side weapon code can detect aim/breath input.
    int GetLastButtons(void) const { return last_ucmd.buttons; };

    void  SetFov(float newFov);
    float GetFov() const;

    void     ToggleZoom(int iZoom);
    void     ZoomOff(void);
    void     ZoomOffEvent(Event *ev);
    qboolean IsZoomed(void);
    void     SafeZoomed(Event *ev);
    void     DumpState(Event *ev);
    void     ForceLegsState(Event *ev);
    void     ForceTorsoState(Event *ev);

    Vector GetAngleToTarget(Entity *ent, str tag, float yawclamp, float pitchclamp, Vector baseangles);

    void AutoAim(void);
    void AcquireTarget(void);
    void RemoveTarget(Entity *ent_to_remove);

    void DebugWeaponTags(int controller_tag, Weapon *weapon, str weapon_tagname);
    void NextPainTime(Event *ev);
    void SetTakePain(Event *ev);

    void SetMouthAngle(Event *ev);

    void EnterVehicle(Event *ev);
    void ExitVehicle(Event *ev);
    void EnterTurret(TurretGun *ent);
    void EnterTurret(Event *ev);
    void ExitTurret(void);
    void ExitTurret(Event *ev);
    void Holster(Event *ev);
    void HolsterToggle(Event *ev);
    void CoopLobbyPose(Event *ev);   // HZM coop lobby: freeze player as a parade-rest mannequin
    void CoopLobbyUnpose(Event *ev); // HZM coop lobby: release the mannequin freeze
    void CoopLobbyRepose(Event *ev); // HZM coop lobby: atomically swap the frozen idle pose
    void CoopLobbyCycleAnim(Event *ev); // HZM coop lobby DEV: cycle candidate idle poses to ID one
    void CoopLobbyHoldPose(Event *ev);  // HZM coop lobby: per-frame re-assert of the hands-on-hips pose
    void CoopLobbyInput(Event *ev);     // HZM coop lobby: enable/disable reading A/D/F from the usercmd
    void TickCoopLobbyInput(void);      // HZM coop lobby: per-frame A/D/F -> self.coop_lobbyInput (no binds)
    void CoopLobbyCursor(Event *ev);    // HZM coop lobby: enable/disable the mouse-cursor bridge
    void TickCoopLobbyCursor(void);     // HZM coop lobby: per-frame mouse -> self.coop_lobbyCurX/Y + coop_lobbyClick

    void            RemoveFromVehiclesAndTurretsInternal(void); // Added in 2.30
    void            RemoveFromVehiclesAndTurrets(void);
    void            WatchActor(Event *ev);
    void            StopWatchingActor(Event *ev);
    painDirection_t Pain_string_to_int(str pain);

    inline Vector GetVAngles(void) { return v_angle; }

    void GetStateAnims(Container<const char *> *c) override;
    void VelocityModified(void) override;
    int  GetKnockback(int original_knockback, qboolean blocked);
    int  GetMoveResult(void);
    void ReceivedItem(Item *item) override;
    void RemovedItem(Item *item) override;
    void AmmoAmountChanged(Ammo *ammo, int inclip = 0) override;

    void WaitForState(Event *ev);
    void SkipCinematic(Event *ev);
    void SetDamageMultiplier(Event *ev);
    void LogStats(Event *ev);
    void Loaded(void);
    void PlayerShowModel(Event *ev);
    void showModel(void) override;
    void ResetHaveItem(Event *ev);
    void ModifyHeight(Event *ev);
    void ModifyHeightFloat(Event *ev); // Added in mohaab 2.40
    void SetMovePosFlags(Event *ev);
    void GetPositionForScript(Event *ev);
    void GetMovementForScript(Event *ev);
    void EventStuffText(Event *ev);
    void EventSetVoiceType(Event *ev);
    void GetTeamDialogPrefix(str& outPrefix);
    void PlayInstantMessageSound(const char *name);
    void EventDMMessage(Event *ev);
    //====
    // Added in 2.30
    str GetBattleLanguageCondition() const;
    str GetBattleLanguageDirection() const;
    str GetBattleLanguageLocation() const;
    str GetBattleLanguageLocalFolks();
    str GetBattleLanguageWeapon() const;
    str GetBattleLanguageDistance() const;
    str GetBattleLanguageDistanceMeters(float dist) const;
    str GetBattleLanguageDistanceFeet(float dist) const;
    str GetBattleLanguageTarget() const;
    str TranslateBattleLanguageTokens(const char *string);
    //====
    void       EventIPrint(Event *ev);
    void       EventGetUseHeld(Event *ev);
    void       EventGetFireHeld(Event *ev);
    void       EventGetPrimaryFireHeld(Event *ev);
    void       EventGetSecondaryFireHeld(Event *ev);
    void       EventGetCoopAdsHeld(Event *ev); // HZM coop - aim-down-sights button held (for ads.scr move slowdown)
    void       EventCoopKillWall(Event *ev);   // HZM coop - wall probe v5: live kill of aimed invisible brush (killwall)
    void       EventCoopMarkWall(Event *ev);   // HZM coop - wall probe v5: forensic aim-trace report (markwall)
    void       EventCoopSetDbno(Event *ev);    // HZM coop [user 08-02] - DBNO state on/off (coop_setdbno, from dbno.scr)
    void       EventCoopSetCover(Event *ev);   // HZM coop - take-cover request on/off (coop_setcover, from takecover.scr)
    void       EventGetCoopCover(Event *ev);   // HZM coop - cover state getter (coop_incover: 0/1/2/3)
    void       Score(Event *ev);
    void       Join_DM_Team(Event *ev);
    void       Auto_Join_DM_Team(Event *ev);
    void       Leave_DM_Team(Event *ev);
    teamtype_t GetTeam() const;
    void       SetTeam(teamtype_t team);
    bool       IsSpectator(void);
    void       BeginFight(void);
    void       EndFight(void);
    void       Spectator(void);
    void       Spectator(Event *ev);
    void       SetPlayerSpectate(bool bNext);
    void       SetPlayerSpectateRandom(void); // Added in 2.0
    bool       IsValidSpectatePlayer(Player *pPlayer);
    void       GetSpectateFollowOrientation(Player *pPlayer, Vector& vPos, Vector& vAng);
    void       UpdateStatus(const char *s);
    void       SetDM_Team(DM_Team *team);
    DM_Team   *GetDM_Team();
    void       WarpToPoint(Entity *spawnpoint);

    void ArmorDamage(Event *ev) override;
    void HUDPrint(const char *s);
    void Disconnect(void);

    // team stuff
    int  GetNumKills() const;
    int  GetNumDeaths() const;
    void AddKills(int num);
    void AddDeaths(int num);
    void CallVote(Event *ev);
    void Vote(Event *ev);
    void RetrieveVoteOptions(Event *ev); // Added in 2.0
    void EventPrimaryDMWeapon(Event *ev);
    void EventSecondaryDMWeapon(Event *ev);
    void ResetScore();
    void DeadBody(Event *ev);
    void Gib();
    void WonMatch(void);
    void LostMatch(void);
    int  GetMatchesWon() const;
    int  GetMatchesLost() const;

    //====
    // Added in 2.30
    void GetIsSpectator(Event *ev);
    void EventSetInJail(Event *ev);
    bool IsInJail() const;
    void EventGetInJail(Event *ev);
    void GetNationalityPrefix(Event *ev);
    //====
    void GetIsDisguised(Event *ev);
    void GetHasDisguise(Event *ev);
    void SetHasDisguise(Event *ev);
    void SetObjectiveCount(Event *ev);

    void EventDMDeathDrop(Event *ev);
    void EventStopwatch(Event *ev);
    void KilledPlayerInDeathmatch(Player *killed, meansOfDeath_t meansofdeath);
    void SetStopwatch(int iDuration, stopWatchType_t type = SWT_NORMAL);
    void BeginTempSpectator(void);
    void EndSpectator(void);

    PlayerStart *GetLastSpawnpoint() const { return m_pLastSpawnpoint; }

    void Stats(Event *ev);
    void ArmWithWeapons(Event *ev);              // Added in 2.30
    void EventGetCurrentDMWeaponType(Event *ev); // Added in 2.30
    void PhysicsOn(Event *ev);
    void PhysicsOff(Event *ev);

    void Think() override;
    bool IsReady(void) const;

    void EventGetReady(Event *ev);
    void EventSetReady(Event *ev);
    void EventSetNotReady(Event *ev);
    void EventGetDMTeam(Event *ev);
    void EventGetNetName(Event *ev); // Added in 2.30
    void EventSetViewModelAnim(Event *ev);
    void EventEnterIntermission(Event *ev);
    void EventSetPerferredWeapon(Event *ev);

    bool BlocksAIMovement();

    //====
    // Added in 2.0
    void  TickSprint();
    float GetRunSpeed() const;
    // HZM coop - is the player currently sprinting this frame (read by cgame-independent consumers if needed)
    bool  IsCoopSprinting() const { return m_bCoopSprinting; }
    // HZM coop [user 2026-08-02] - LOW-HEALTH LIMP: decides m_bCoopLimping and stuffs coop_limpView
    // to the owning client. Called from ClientMove immediately BEFORE TickSprint, because TickSprint
    // reads m_bCoopLimping to suppress sprinting while wounded.
    void  TickLimp();
    void  EventCoopLimpTest(Event *ev);   // HZM coop DEV - set health to a fraction of max
    bool  IsCoopLimping() const { return m_bCoopLimping; }
    // HZM coop - TAKE COVER: per-frame pose validation (called from ClientThink next to TickSprint)
    void  TickCoopCover();
    // HZM coop - BOT COMBAT DRIVE (dev/test, coop_botInput): overwrite this frame's usercmd (aim +
    // fire + advance on the nearest visible German) so a connected client fights unattended. No-op
    // unless coop_botInput is set; never enabled in shipped cfgs.
    void  CoopBotDrive(usercmd_t *ucmd);
    // HZM coop - read by Weapon::Shoot (blind-fire spread penalty) and Weapon::GetMuzzlePosition
    // (raise the fire origin over low cover while blind-firing)
    bool  IsCoopBlindfiring() const { return m_bCoopBlindfire; }
    bool  IsCoopDbno() const { return m_bCoopDbno; }   // HZM coop [user 08-02]
    bool  IsCoopCoverLow() const { return m_bCoopCoverLow; }
    bool  IsCoopCoverWall() const { return m_bCoopCoverWall; }
    int   GetCoopCoverSide() const { return m_iCoopCoverSide; }
    const Vector& GetCoopCoverNormal() const { return m_vCoopCoverNormal; }
    void  FireWeapon(int number, firemode_t mode) override;
    void  SetInvulnerable();
    void  TickInvulnerable();
    void  SetVulnerable();
    bool  IsInvulnerable();
    void  CancelInvulnerable();
    void  InitInvulnerable();
    void  TickTeamSpawn();
    bool  ShouldForceSpectatorOnDeath() const;
    bool  HasVehicle() const override;
    void  setContentsSolid() override;
    void  UserSelectWeapon(bool bWait);
    void  PickWeaponEvent(Event *ev);
    bool  AllowTeamRespawn() const;
    void  EventUseWeaponClass(Event *ev) override;
    void  EventAddKills(Event *ev);
    bool  CanKnockback(float minHealth) const;
    //====

    //====
    // Added in 2.30
    void EventKillAxis(Event *ev);
    void EventGetTurret(Event *ev);
    void EventGetVehicle(Event *ev);
    //====

    void FindAlias(str& output, str name, AliasListNode_t **node);

    bool HasVotedYes() const;
    bool HasVotedNo() const;

    //=============================
    // Added in OPM
    //=============================

    void     ResetClient();
    qboolean CheckCanSwitchTeam(teamtype_t team);

    qboolean     ViewModelAnim(str anim, qboolean force_restart, qboolean bFullAnim);
    virtual void Spawned(void);

    void AddDeaths(Event *ev);
    void AdminRights(Event *ev);
    void BindWeap(Event *ev);
    void Dive(Event *ev);
    void EventSetTeam(Event *ev);
    void FreezeControls(Event *ev);
    void GetConnState(Event *ev);
    void GetDamageMultiplier(Event *ev);
    void GetKills(Event *ev);
    void GetDeaths(Event *ev);
    void GetKillHandler(Event *ev);
    void GetMoveSpeedScale(Event *ev);
    void GetLegsState(Event *ev);
    void GetStateFile(Event *ev);
    void GetTorsoState(Event *ev);
    void GetUserInfo(Event *ev);
    void Inventory(Event *ev);
    void InventorySet(Event *ev);
    void IsAdmin(Event *ev);
    void JoinDMTeamReal(Event *ev);
    void JoinDMTeam(Event *ev);
    void LeanLeftHeld(Event *ev);
    void LeanRightHeld(Event *ev);
    void PlayLocalSound(Event *ev);
    void RunHeld(Event *ev);
    void SecFireHeld(Event *ev);
    void SetAnimSpeed(Event *ev);
    void SetKillHandler(Event *ev);
    void SetSpeed(Event *ev);
    void SetStateFile(Event *ev);
    void HideEntity(Event *ev);
    void ShowEntity(Event *ev);
    void StopLocalSound(Event *ev);
    void Userinfo(Event *ev);

    void InitModelFps();
    void ThinkFPS();
    void EventGetViewModelAnim(Event *ev);
    void EventGetViewModelAnimFinished(Event *ev);
    void EventGetViewModelAnimValid(Event *ev);

#ifdef OPM_FEATURES
    void EventEarthquake(Event *ev);
    void SetClientFlag(Event *ev);
    void SetEntityShader(Event *ev);
    void SetLocalSoundRate(Event *ev);
    void SetVMASpeed(Event *ev);
    void VisionGetNaked(Event *ev);
    void VisionSetBlur(Event *ev);
    void VisionSetNaked(Event *ev);
#endif

    void     Postthink() override;
    void     GibEvent(Event *ev);
    qboolean canUse();
    qboolean canUse(Entity *entity, bool requiresLookAt);
    int      getUseableEntities(int *touch, int maxcount, bool requiresLookAt = true);
};

inline void Player::Archive(Archiver& arc)
{
    str tempStr;

    Sentient::Archive(arc);

    arc.ArchiveInteger(&m_iPartSlot[0]);
    arc.ArchiveInteger(&m_iPartSlot[1]);

    arc.ArchiveFloat(&m_fPartBlends[0]);
    arc.ArchiveFloat(&m_fPartBlends[1]);
    arc.ArchiveFloat(&partBlendMult[0]);
    arc.ArchiveFloat(&partBlendMult[1]);

    arc.ArchiveString(&last_torso_anim_name);
    arc.ArchiveString(&last_leg_anim_name);
    arc.ArchiveString(&partAnim[0]);
    arc.ArchiveString(&partAnim[1]);
    arc.ArchiveString(&partOldAnim[0]);
    arc.ArchiveString(&partOldAnim[1]);

    arc.ArchiveString(&m_sPerferredWeaponOverride);

    arc.ArchiveBool(&animdone_Legs);
    arc.ArchiveBool(&animdone_Torso);

    arc.ArchiveInteger(&m_iMovePosFlags);
    ArchiveEnum(movecontrol, movecontrol_t);
    arc.ArchiveInteger(&last_camera_type);

    arc.ArchiveVector(&oldvelocity);
    arc.ArchiveVector(&old_v_angle);
    arc.ArchiveVector(&oldorigin);
    arc.ArchiveFloat(&animspeed);
    arc.ArchiveFloat(&airspeed);

    arc.ArchiveVector(&m_vPushVelocity);

    arc.ArchiveRaw(blend, sizeof(blend));
    arc.ArchiveFloat(&fov);
    arc.ArchiveFloat(&selectedfov);
    arc.ArchiveInteger(&m_iInZoomMode);

    arc.ArchiveVector(&v_angle);
    arc.ArchiveVector(&m_vViewPos);
    arc.ArchiveVector(&m_vViewAng);

    arc.ArchiveInteger(&buttons);
    arc.ArchiveInteger(&new_buttons);
    arc.ArchiveInteger(&server_new_buttons);
    arc.ArchiveFloat(&respawn_time);

    arc.ArchiveInteger(&last_attack_button);

    arc.ArchiveFloat(&damage_blood);
    arc.ArchiveFloat(&damage_alpha);
    arc.ArchiveVector(&damage_blend);
    arc.ArchiveVector(&damage_from);
    arc.ArchiveVector(&damage_angles);
    arc.ArchiveFloat(&damage_count);
    arc.ArchiveFloat(&next_painsound_time);

    arc.ArchiveSafePointer(&camera);
    arc.ArchiveSafePointer(&actor_camera);
    arc.ArchiveSafePointer(&actor_to_watch);

    arc.ArchiveBoolean(&actor_camera_right);
    arc.ArchiveBoolean(&starting_actor_camera_right);

    arc.ArchiveInteger(&music_current_mood);
    arc.ArchiveInteger(&music_fallback_mood);

    arc.ArchiveFloat(&music_current_volume);
    arc.ArchiveFloat(&music_saved_volume);
    arc.ArchiveFloat(&music_volume_fade_time);

    arc.ArchiveInteger(&reverb_type);
    arc.ArchiveFloat(&reverb_level);

    arc.ArchiveBoolean(&gibbed);
    arc.ArchiveFloat(&pain);

    ArchiveEnum(pain_dir, painDirection_t);
    ArchiveEnum(pain_type, meansOfDeath_t);

    arc.ArchiveInteger(&pain_location);
    arc.ArchiveBool(&take_pain);
    arc.ArchiveInteger(&nextpaintime);

    arc.ArchiveFloat(&m_fHealRate);

    arc.ArchiveBool(&knockdown);
    arc.ArchiveBool(&canfall);
    arc.ArchiveBool(&falling);

    arc.ArchiveInteger(&feetfalling);
    arc.ArchiveVector(&falldir);

    arc.ArchiveBool(&mediumimpact);
    arc.ArchiveBool(&hardimpact);

    arc.ArchiveBoolean(&music_forced);

    arc.ArchiveRaw(&last_ucmd, sizeof(usercmd_t));
    arc.ArchiveRaw(&last_eyeinfo, sizeof(usereyes_t));

    arc.ArchiveFloat(&animheight);

    arc.ArchiveVector(&yaw_forward);
    arc.ArchiveVector(&yaw_left);

    arc.ArchiveSafePointer(&atobject);
    arc.ArchiveFloat(&atobject_dist);
    arc.ArchiveVector(&atobject_dir);

    arc.ArchiveSafePointer(&toucheduseanim);
    arc.ArchiveInteger(&useanim_numloops);
    arc.ArchiveSafePointer(&useitem_in_use);

    arc.ArchiveFloat(&move_left_vel);
    arc.ArchiveFloat(&move_right_vel);
    arc.ArchiveFloat(&move_backward_vel);
    arc.ArchiveFloat(&move_forward_vel);
    arc.ArchiveFloat(&move_up_vel);
    arc.ArchiveFloat(&move_down_vel);
    arc.ArchiveInteger(&moveresult);

    arc.ArchiveFloat(&damage_multiplier);

    arc.ArchiveString(&waitForState);
    arc.ArchiveInteger(&m_iNumObjectives);
    arc.ArchiveInteger(&m_iObjectivesCompleted);

    for (int i = 0; i < MAX_TRAILS; i++) {
        arc.ArchiveVector(&mvTrail[i]);
    }

    for (int i = 0; i < MAX_TRAILS; i++) {
        arc.ArchiveVector(&mvTrailEyes[i]);
    }

    arc.ArchiveInteger(&mCurTrailOrigin);
    arc.ArchiveInteger(&mLastTrailTime);

    arc.ArchiveInteger(&m_iNumHitsTaken);
    arc.ArchiveInteger(&m_iNumEnemiesKilled);
    arc.ArchiveInteger(&m_iNumObjectsDestroyed);
    arc.ArchiveInteger(&m_iNumShotsFired);
    arc.ArchiveInteger(&m_iNumHits);
    arc.ArchiveInteger(&m_iNumHeadShots);
    arc.ArchiveInteger(&m_iNumTorsoShots);
    arc.ArchiveInteger(&m_iNumLeftLegShots);
    arc.ArchiveInteger(&m_iNumRightLegShots);
    arc.ArchiveInteger(&m_iNumGroinShots);
    arc.ArchiveInteger(&m_iNumLeftArmShots);
    arc.ArchiveInteger(&m_iNumRightArmShots);

    // Added in 2.30
    //====
    ArchiveEnum(m_jailstate, jailstate_t);
    arc.ArchiveString(&m_sDmPrimary);
    arc.ArchiveBool(&m_bIsInJail);
    //====

    arc.ArchiveFloat(&m_fLastDeltaTime);

    // Added in 2.0
    //====
    arc.ArchiveFloat(&m_fLastSprintTime);
    arc.ArchiveBool(&m_bHasJumped);
    //====

    if (arc.Saving()) {
        if (currentState_Legs) {
            tempStr = currentState_Legs->getName();
        } else {
            tempStr = "NULL";
        }
        arc.ArchiveString(&tempStr);

        if (currentState_Torso) {
            tempStr = currentState_Torso->getName();
        } else {
            tempStr = "NULL";
        }
        arc.ArchiveString(&tempStr);
    } else {
        statemap_Legs = GetStatemap(
            str(g_statefile->string) + "_Legs.st", (Condition<Class> *)m_conditions, &legs_conditionals, false
        );
        statemap_Torso = GetStatemap(
            str(g_statefile->string) + "_Torso.st", (Condition<Class> *)m_conditions, &torso_conditionals, false
        );

        arc.ArchiveString(&tempStr);
        if (tempStr != "NULL") {
            currentState_Legs = statemap_Legs->FindState(tempStr);
        } else {
            currentState_Legs = NULL;
        }
        arc.ArchiveString(&tempStr);
        if (tempStr != "NULL") {
            currentState_Torso = statemap_Torso->FindState(tempStr);
        } else {
            currentState_Torso = NULL;
        }

        for (int i = 1; i <= legs_conditionals.NumObjects(); i++) {
            Conditional *c = legs_conditionals.ObjectAt(i);

            if (Q_stricmp(c->getName(), "PAIN") && !c->parmList.NumObjects()) {
                m_pLegsPainCond = c;
                break;
            }
        }

        for (int i = 1; i <= torso_conditionals.NumObjects(); i++) {
            Conditional *c = torso_conditionals.ObjectAt(i);

            if (Q_stricmp(c->getName(), "PAIN") && !c->parmList.NumObjects()) {
                m_pTorsoPainCond = c;
                break;
            }
        }
    }

    if (arc.Loading()) {
        UpdateWeapons();
        SetViewAngles(v_angle);
    }

    //
    // Added in OPM
    //
    arc.ArchiveBool(&m_bFrozen);

    for (int i = 0; i < MAX_SPEED_MULTIPLIERS; i++) {
        arc.ArchiveFloat(&speed_multiplier[i]);
    }

    if (arc.Loading()) {
        InitModelFps();
    }

    arc.ArchiveBool(&animDoneVM);
    arc.ArchiveFloat(&m_fVMAtime);
}

inline Camera *Player::CurrentCamera(void)
{
    return camera;
}

inline void Player::CameraCut(void)
{
    //
    // toggle the camera cut bit
    //
    client->ps.camera_flags = ((client->ps.camera_flags & CF_CAMERA_CUT_BIT) ^ CF_CAMERA_CUT_BIT)
                            | (client->ps.camera_flags & ~CF_CAMERA_CUT_BIT);
}

inline void Player::CameraCut(Camera *ent)
{
    if (ent == camera) {
        // if the camera we are currently looking through cut, than toggle the cut bits
        CameraCut();
    }
}

inline void Player::SetCamera(Camera *ent, float switchTime)
{
    camera                 = ent;
    client->ps.camera_time = switchTime;
    if (switchTime <= 0.0f) {
        CameraCut();
    }
}
