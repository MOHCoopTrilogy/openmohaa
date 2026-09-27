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
// playerbot.cpp: Multiplayer bot system.
//
// FIXME: Refactor code and use OOP-based state system

#include "g_local.h"
#include "actor.h"
#include "playerbot.h"
#include "consoleevent.h"
#include "debuglines.h"
#include "scriptexception.h"
#include "vehicleturret.h"
#include "weaputils.h"
#include "g_spawn.h" // [HZM bot breach] SpawnArgs (BotGetProjInfo)
#include "../qcommon/tiki.h" // [HZM bot room-clear step 2] the explosion tiki's init commands (BotExplR)
#include "navigation_recast_config_ext.h" // [HZM bot probe2] RECAST_AREA_ELEVATOR (lift telemetry)
#include "windows.h"
#include "g_bot.h"
#include "misc.h"    // [2026-09-25] UseAnim / UseObject / FuncLadder (BotUseWouldAct)
#include "trigger.h" // [2026-09-25] TriggerUse (BotUseWouldAct)

// We assume that we have limited access to the server-side
// and that most logic come from the playerstate_s structure

cvar_t *bot_manualmove;

CLASS_DECLARATION(Listener, BotController, NULL) {
    {NULL, NULL}
};

BotController::botfunc_t BotController::botfuncs[MAX_BOT_FUNCTIONS];

extern bool NavDanger_IsInside(const float *pos); // [HZM bot B3] navigation_recast_load_ext.cpp

// [HZM bot B-D] one ^~^~^ BOTEV line per tactic decision (bot_probe on) - lets a soak count what the brain actually did
static void BotEv(const Player *p, const char *what, const Vector& at)
{
    static cvar_t *s_probe = NULL;
    if (!s_probe) {
        s_probe = gi.Cvar_Get("bot_probe", "0", 0);
    }
    if (s_probe->integer && p) {
        gi.Printf(
            "^~^~^ BOTEV e=%d tm=%c ev=%s at=(%.0f %.0f %.0f)\n", p->entnum,
            p->GetTeam() == TEAM_ALLIES ? 'a' : (p->GetTeam() == TEAM_AXIS ? 'x' : '?'), what, at.x, at.y, at.z
        );
    }
}

// [HZM bot breach] TEAM MEMORY shared by every bot of a side (server-wide statics; level.inttime restarts at 0 on a map
// change, so an entry stamped in the FUTURE is from the last map and is ignored):
//  - blasts: a clearing grenade is in the air - team-mates in its sight take cover, the rest do not walk in until it went off
//  - spots:  a door / ladder already cleared recently - the next bot through does not throw a second grenade at it
//  - deaths: where the side lost people lately - a door / hatch next to one is worth a grenade
struct BotTeamMark {
    Vector pos;
    int    team;
    int    t;     // stamped at
    int    until; // blasts: goes off by
    int    id;
};
// [HZM bot probe2] state machine transitions (^~^~^ BOTSTATE, bot_probe on)
static void BotStateEv(const Player *p, int i, bool on)
{
    static cvar_t *s_probe = NULL;
    if (!s_probe) {
        s_probe = gi.Cvar_Get("bot_probe", "0", 0);
    }
    static const char *names[] = {"attack", "curious", "grenade", "idle", "weapon"};
    if (s_probe->integer && p && i >= 0 && i < 5) {
        gi.Printf("^~^~^ BOTSTATE e=%d %s=%s at=(%.0f %.0f %.0f)\n", p->entnum, on ? "on" : "off", names[i], p->origin.x, p->origin.y, p->origin.z);
    }
}

static bool BotMateNearBlast(const Player *self, const Vector& land); // [user 2026-09-24] defined by BreachSuspect

// [HZM bot room-clear step 2] bot_nadeSafe 1 (default) = the grenade-safety build; 0 = the step-1 code path exactly
static bool BotNadeSafe(void)
{
    static cvar_t *s = NULL;
    if (!s) {
        s = gi.Cvar_Get("bot_nadeSafe", "1", 0);
    }
    return s->integer != 0;
}

// Team-mates REACT to a team grenade (take cover from it, hold back from it) only when it can hurt them: FF is off on the
// shipped server (server.cfg g_teamdamage 0), and marks for every lob would hold and scatter mates mid-fight for nothing
// (vet C11). bot_nadeMateReact -1 (default) follows g_teamdamage, 0 never, 1 always. The THROWER always reacts to its own
// grenade, and the throw veto on a team-mate near the blast (BotMateNearBlast, user 2026-09-24) stays regardless.
static bool BotNadeMateReact(void)
{
    static cvar_t *s = NULL;
    if (!s) {
        s = gi.Cvar_Get("bot_nadeMateReact", "-1", 0);
    }
    if (s->integer < 0) {
        return g_teamdamage && g_teamdamage->integer != 0;
    }
    return s->integer != 0;
}

static BotTeamMark s_botBlasts[16];
static BotTeamMark s_botBreachSpots[16];
static BotTeamMark s_botDeaths[32];
static BotTeamMark s_botSightings[32]; // [HZM bug-2884] where the side has SEEN enemies lately (hunt targets)
static int         s_botBlastSeq, s_botSpotSeq, s_botDeathSeq, s_botSightSeq;
static int         s_botMarkClock;

// [HZM bot room-clear step 2] BLAST MARKS posted from the ACT (vet C3): every bot grenade that really leaves a hand - a lob,
// a clearing throw, a stray, a death-drop - at the moment its projectile spawns (BotNadeOnSpawn), with the projectile it
// belongs to; readers re-simulate the rest of its path from the live projectile (BotLiveNadeDet)
struct BotNadeMark {
    int    id, team, owner, projEnt, t, until;
    float  spawn, R;
    Vector det;
    int    op; // [room-clear step 4] a stack-and-clear op's grenade: the side holds back from it whatever g_teamdamage says
};
static BotNadeMark s_nadeMarks[16];
static int         s_nadeMarkSeq = 0;

// [room-clear step 4] STACK-AND-CLEAR OPS, server-wide (4 per side at most): one per clearing grenade, with up to two
// stackers. Phases: 1 STACK (the thrower solves / draws / cooks, the stackers go to their spots beside the door), 2 BLAST
// (the grenade is out: everyone holds), 3 ENTRY (it went off: staggered entry, crossing, each aiming at a corner), 4 over.
enum { BOP_STACK = 1, BOP_BLAST, BOP_ENTRY, BOP_DONE };
struct BotBreachOp {
    int    id, team, phase, t0, tPhase, thrower, detAt, entryT, throwerGo;
    Vector D, n, t, det, K[2];
    float  w, R;
    bool   ok[2];
    int    nm;
    int    mem[2], side[2], state[2], enterAt[2]; // state: 0 going to the spot, 1 stacked, 2 entering, 3 done
    Vector spot[2], E[2];
};
static BotBreachOp s_ops[8];
static int         s_opSeq = 0;
static BotBreachOp *BotOpFind(int id);

// [T2 corner checks] the openings the side is crossing right now (3s): a second bot through the same one takes the other
// corner first
struct BotCornerRec {
    Vector D;
    int    team, first, t, id;
};
static BotCornerRec s_cornerRec[16];
static int          s_cornerRecSeq = 0;
static int          s_openSeq      = 0;

static void BotMarksMapCheck(void)
{
    // level.inttime went backwards: a new map - forget the last one's marks
    if (level.inttime < s_botMarkClock) {
        for (int i = 0; i < 16; i++) {
            s_botBlasts[i].id      = 0;
            s_botBreachSpots[i].id = 0;
            s_nadeMarks[i].id      = 0;
            s_cornerRec[i].id      = 0;
        }
        for (int i = 0; i < 8; i++) {
            s_ops[i].id = 0;
        }
        for (int i = 0; i < 32; i++) {
            s_botDeaths[i].id    = 0;
            s_botSightings[i].id = 0;
        }
    }
    s_botMarkClock = level.inttime;
}

// every door entity on the map, rescanned every 10s (the breach look-ahead; the corner checks keep off closed doors)
static int BotDoorList(const int *& list)
{
    static int s_doors[64];
    static int s_nDoors   = 0;
    static int s_doorScan = -100000;
    if (level.inttime < s_doorScan || level.inttime - s_doorScan > 10000) {
        s_nDoors = 0;
        for (int i = game.maxclients; i < globals.num_entities && s_nDoors < 64; i++) {
            gentity_t *ge = &g_entities[i];
            if (ge->inuse && ge->entity && ge->entity->IsSubclassOfDoor()) {
                s_doors[s_nDoors++] = i;
            }
        }
        s_doorScan = level.inttime;
    }
    list = s_doors;
    return s_nDoors;
}

// a door that is not open within r (XY) of a bot standing at org: A4 is opening it and presses USE along the body's facing,
// so no eyes-off-the-route look may run there (T2 / T3)
static bool BotClosedDoorNear(const Vector& org, float r)
{
    const int *dl = NULL;
    const int  nd = BotDoorList(dl);
    for (int k = 0; k < nd; k++) {
        gentity_t *ge = &g_entities[dl[k]];
        if (!ge->inuse || !ge->entity || !ge->entity->IsSubclassOfDoor()) {
            continue;
        }
        Door        *door = static_cast<Door *>(ge->entity);
        const Vector c    = (door->absmin + door->absmax) * 0.5f;
        if ((c - org).lengthXYSquared() < Square(r) && fabs(c.z - (org.z + 40.0f)) < 96.0f && !door->isOpen()) {
            return true;
        }
    }
    return false;
}

// [2026-09-26] a tactic's cvar, per team - for WITHIN-MATCH A/B tests (one side has it, the other does not, then swap):
// 0 off, 1 both sides, 2 allies only, 3 axis only
static bool BotTeamGate(int v, const Player *p)
{
    if (v == 1) {
        return true;
    }
    if (!p || (v != 2 && v != 3)) {
        return false;
    }
    return p->GetTeam() == (v == 2 ? TEAM_ALLIES : TEAM_AXIS);
}

static bool BotMarkLive(const BotTeamMark& m, int team, int maxAgeMs)
{
    return m.id && m.team == team && m.t <= level.inttime && level.inttime - m.t < maxAgeMs;
}

static void BotMarkAdd(BotTeamMark *ring, int size, int& seq, const Vector& pos, int team, int until)
{
    BotTeamMark& m = ring[seq % size];
    m.pos          = pos;
    m.team         = team;
    m.t            = level.inttime;
    m.until        = until;
    m.id           = ++seq;
}


BotController::BotController()
{
    if (LoadingSavegame) {
        return;
    }

    m_botCmd.serverTime = 0;
    m_botCmd.msec       = 0;
    m_botCmd.buttons    = 0;
    m_botCmd.angles[0]  = ANGLE2SHORT(0);
    m_botCmd.angles[1]  = ANGLE2SHORT(0);
    m_botCmd.angles[2]  = ANGLE2SHORT(0);

    m_botCmd.forwardmove = 0;
    m_botCmd.rightmove   = 0;
    m_botCmd.upmove      = 0;

    m_iProbeLastTime = 0; // [HZM bot probe]

    m_botEyes.angles[0] = 0;
    m_botEyes.angles[1] = 0;
    m_botEyes.ofs[0]    = 0;
    m_botEyes.ofs[1]    = 0;
    m_botEyes.ofs[2]    = DEFAULT_VIEWHEIGHT;

    m_iCuriousTime        = 0;
    // [HZM bot B-D]
    m_iInvestigateUntil = 0;
    m_iAlertUntil       = 0;
    m_iGlanceNext       = 0;
    m_iGlanceUntil      = 0;
    m_fGlanceYaw        = 0;
    m_iVoiceNext        = 0;
    m_fSkillReact       = 1.0f;
    m_fSkillAim         = 1.0f;
    m_fSkillAggro       = 0.5f;
    m_fSkillTurn        = 1.0f;
    m_iLastEnemySeenAny = 0;
    m_iNadeScanTime     = 0;
    m_iGrenadeFleeUntil = 0;
    m_iNadeState        = 0;
    m_iNadeClobbered    = 0;
    m_vNadeAim          = vec_zero;
    m_iNadeTime         = 0;
    m_iNadeStart        = 0;
    m_iNadeNext         = 0;
    m_fNadeHold         = 0;
    m_iLaneState        = 0;
    m_iLaneSince        = 0;
    m_iRetreatUntil     = 0;
    m_iRetreatNext      = 0;
    m_bHolder           = false;
    m_bHolding          = false;
    m_iHoldCheck        = 0;
    m_bWantCrouch       = false;
    m_iCrouchStuckSince = 0;
    m_iNoCrouchUntil    = 0;
    m_iCrouchTapNext    = 0; // [HZM bug-2918]
    m_iHoldSince        = 0;
    m_iHoldCooldown     = 0;
    m_vHoldCover        = vec_zero;
    m_iHoldCoverTry     = 0;
    m_iHoldZone         = 0;
    m_iNadeMode         = 0; // [HZM bot breach]
    m_iBreachNext       = 0;
    m_iBreachKeyEnt     = -1;
    m_vBreachKey        = vec_zero;
    m_iBreachKeyTime    = 0;
    m_bBreachWant       = false;
    m_iBreachKind       = 0;
    m_vBreachGate       = vec_zero;
    m_vBreachAng        = vec_zero;
    m_vBreachLand       = vec_zero;
    m_fBreachFuse       = 0;
    m_iBreachGoAt       = 0;
    m_iBlastCoverId     = 0;
    m_iBlastWaitId      = 0;
    m_vHeardPos         = vec_zero;
    m_iHeardTime        = 0;
    memset(&m_solve, 0, sizeof(m_solve)); // [HZM bot room-clear step 2] (plain data: Vector is 3 floats)
    m_iNadeKind         = 0;
    m_vNadeTarget2      = vec_zero;
    m_vNadeD            = vec_zero;
    m_vNadeN            = vec_zero;
    m_vNadeT            = vec_zero;
    m_vNadeDet          = vec_zero;
    m_iNadeButtons      = 0;
    m_iNadeSolveT       = 0;
    m_iNadeReadyT       = 0;
    m_iNadeCookT        = 0;
    m_iNadeHoldUntil    = 0;
    m_iNadeSpawnId      = 0;
    m_iNadeSpawnT       = 0;
    m_iBreachRetry      = 0;
    m_iBlastCoverUntil  = 0;
    m_vBlastCoverSpot   = vec_zero;
    m_iNadeCrouchUntil  = 0;
    m_fPace             = 130.0f;
    m_iCfState          = 0; // [HZM bot cover]
    m_iHealState        = 0; // [user 2026-09-25] bandage
    m_iHealT            = 0;
    m_iHealNext         = 0;
    m_iHealFails        = 0;
    m_bHealOut          = false;
    m_iHealHoldUntil    = 0;
    m_iHealStepUntil    = 0;
    m_iHealStepDir      = 1;
    m_iHealDmgT         = 0;
    m_iHealSlowSince    = 0;
    m_iHealSafeT        = 0;
    m_iHealYawTry       = 0;
    m_fHealHp0          = 0;
    m_fHealLastHp       = 0;
    m_fHealYaw          = 0;
    m_bHealCrouch       = false;
    m_iCfType           = 0;
    m_iCfLean           = 0;
    m_vCfPos            = vec_zero;
    m_iCfUntil          = 0;
    m_iCfCycles         = 0;
    m_iCfMaxCycles      = 0;
    m_iCfSeenInPeek     = 0;
    m_iCfBlindPeeks     = 0;
    m_iCfExposedSince   = 0;
    m_iCfSearchNext     = 0;
    m_iCfTakeNext       = 0;
    m_iCfNoSession      = 0;
    m_iCfMoveUntil      = 0;
    m_iCfLastTick       = 0;
    m_bCfBlind          = false;
    m_iCfStrafe         = 0;
    m_iCfFwd            = 0;
    m_iCfSteerStall     = 0;
    memset(&m_open, 0, sizeof(m_open)); // [T2 corner checks] (plain data: Vector is 3 floats)
    m_iOpenScanNext     = 0;
    m_iCornerAimT       = -100000;
    m_vPieCorner        = vec_zero;
    m_iPieT             = 0;
    m_iPieAimed         = 0;
    m_iPieKey           = 0;
    m_iPieVisT          = 0;
    m_bPieBlind         = false;
    m_iCrossT           = 0;
    m_iCrossKind        = 0;
    m_iCrossAim         = 0;
    m_iCrossId          = 0;
    m_iCrossHurt        = -1;
    m_iCrossAcq         = -1;
    m_fCrossAcqAng      = -1.0f;
    m_iCrossKill        = 0;
    m_pBuddy            = NULL; // [T3 buddy pairs]
    m_bBuddyLead        = false;
    m_iBuddyT0          = 0;
    m_iBuddyNext        = 0;
    m_iBuddyStillT      = 0;
    m_iBuddySectT       = 0;
    m_bBuddyFlank       = false;
    m_iBuddySide        = 1;
    m_fB2BYaw           = 0;
    m_bB2B              = false;
    m_iB2BLogT          = 0;
    m_iBuddyKills       = 0;
    m_iOverwatchLogT    = 0;
    m_iOpId             = 0; // [room-clear steps 3-5]
    m_iOpSlot           = -1;
    m_iOpMoveT          = 0;
    m_bSawLast          = false;
    m_bBreachRollSus    = false;
    m_iDefendAt         = 0;
    m_iDefendUntil      = 0;
    m_vDefendFrom       = vec_zero;
    m_iDefendNade       = 0;
    m_bDefendPush       = false;
    m_iNoObjSince       = 0;
    m_iAtObjSince       = 0;
    m_iHuntUntil        = 0;
    m_iHuntNext         = 0;
    m_iSightMarkNext    = 0;
    m_bHuntDefend       = false;
    m_iAttackTime         = 0;
    m_iEnemyEyesTag       = -1;
    m_iContinuousFireTime = 0;
    m_iLastSeenTime       = 0;
    m_iLastUnseenTime     = 0;
    m_iLastBurstTime      = 0;
    m_iLastPainTime       = 0; // [HZM] cover-seek gate
    m_iEnemyLockTime      = 0; // [HZM Phase 5b] aim-convergence clock
    m_iReactUntil         = 0; // [HZM] reaction-time gate
    m_iCoverUntil         = 0; // [HZM] cover commitment
    m_iCoverRetryTime     = 0;

    m_iNextTauntTime = 0;

    m_StateFlags = 0;
    m_RunLabel.TrySetScript("global/bot_run.scr");
}

BotController::~BotController()
{
    if (controlledEnt) {
        controlledEnt->delegate_gotKill.Remove(delegateHandle_gotKill);
        controlledEnt->delegate_killed.Remove(delegateHandle_killed);
        controlledEnt->delegate_damage.Remove(delegateHandle_damage);
        controlledEnt->delegate_stufftext.Remove(delegateHandle_stufftext);
        controlledEnt->delegate_spawned.Remove(delegateHandle_spawned);
    }
}

BotMovement& BotController::GetMovement()
{
    return movement;
}

void BotController::Init(void)
{
    bot_manualmove = gi.Cvar_Get("bot_manualmove", "0", 0);

    for (int i = 0; i < MAX_BOT_FUNCTIONS; i++) {
        botfuncs[i].BeginState = &BotController::State_DefaultBegin;
        botfuncs[i].EndState   = &BotController::State_DefaultEnd;
    }

    InitState_Attack(&botfuncs[0]);
    InitState_Curious(&botfuncs[1]);
    InitState_Grenade(&botfuncs[2]);
    InitState_Idle(&botfuncs[3]);
    //InitState_Weapon(&botfuncs[4]);
}

void BotController::GetUsercmd(usercmd_t *ucmd)
{
    *ucmd = m_botCmd;
}

void BotController::GetEyeInfo(usereyes_t *eyeinfo)
{
    *eyeinfo = m_botEyes;
}

void BotController::UpdateBotStates(void)
{
    if (bot_manualmove->integer) {
        memset(&m_botCmd, 0, sizeof(usercmd_t));
        return;
    }

    if (!controlledEnt->client->pers.dm_primary[0]) {
        Event *event;

        //
        // Primary weapon
        //
        event = new Event(EV_Player_PrimaryDMWeapon);
        event->AddString("auto");

        controlledEnt->ProcessEvent(event);
    }

    if (controlledEnt->GetTeam() == TEAM_NONE || controlledEnt->GetTeam() == TEAM_SPECTATOR) {
        float time;

        // Add some delay to avoid telefragging
        time = controlledEnt->entnum / 20.0;

        if (controlledEnt->EventPending(EV_Player_AutoJoinDMTeam)) {
            return;
        }

        //
        // Team
        //
        controlledEnt->PostEvent(EV_Player_AutoJoinDMTeam, time);
        return;
    }

    if (controlledEnt->IsDead() || controlledEnt->IsSpectator()) {
        // The bot should respawn
        m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
        return;
    }

    m_botCmd.buttons |= BUTTON_RUN;
    m_botCmd.serverTime = level.svsTime;

    m_botEyes.ofs[0]    = 0;
    m_botEyes.ofs[1]    = 0;
    m_botEyes.ofs[2]    = controlledEnt->viewheight;
    m_botEyes.angles[0] = 0;
    m_botEyes.angles[1] = 0;
    {
        // [HZM bot cover] a LEANING bot must see and shoot from the leaned eye. The server takes a player's eye
        // (m_vViewPos, used by CanSee and as the muzzle) from the CLIENT's eye offset, and the client adds the lean
        // there (cg_view.c: the eye rotated about the view's forward axis, pivot 28.7u below it). A bot sent the plain
        // viewheight, so its lean was cosmetic - it leaned round the corner and still looked into the wall.
        const float fLean = controlledEnt->client ? controlledEnt->client->ps.fLeanAngle : 0.0f;
        if (fLean != 0.0f) {
            Vector vRight;
            AngleVectors(Vector(0, controlledEnt->GetViewAngles().y, 0), NULL, vRight, NULL);
            const float r       = DEG2RAD(fLean);
            m_botEyes.ofs[0]    = (signed char)(vRight.x * 28.7f * sin(r));
            m_botEyes.ofs[1]    = (signed char)(vRight.y * 28.7f * sin(r));
            m_botEyes.ofs[2]    = (signed char)(controlledEnt->viewheight - 28.7f * (1.0f - cos(r)));
        }
    }

    rotation.SetCombatTurn(false); // [HZM] State_Attack re-arms it on frames it aims at an enemy
    rotation.SetPrecise(false);    // [HZM bot breach] State_Grenade re-arms it while lining up a clearing throw
    m_bWantCrouch = false;          // [HZM bot D4] the tactics below re-request it every frame they want it
    m_iCfStrafe   = 0;              // [HZM bot cover] re-requested every frame by CoverFight
    m_iCfFwd      = 0;
    m_botCmd.buttons &= ~(BUTTON_LEAN_LEFT | BUTTON_LEAN_RIGHT | BUTTON_COOPADS);
    if (m_iCfState && level.inttime - m_iCfLastTick > 500) {
        m_iCfState = 0; // the attack state stopped running (enemy gone): the session is over
    }
    if (BotNadeSafe()) {
        // [HZM bot room-clear step 2] the bot's own running pace (sv_dmspeedmult, a limp): can it reach blast cover in
        // the fuse left? Measured, never assumed - at 0.6 a bot runs ~125-156u/s
        const float fSpd = controlledEnt->velocity.lengthXY();
        if (fSpd > 60.0f && controlledEnt->groundentity) {
            m_fPace = Q_min(Q_max(m_fPace * 0.98f + fSpd * 0.02f, 80.0f), 200.0f);
        }
        if (level.inttime < m_iNadeCrouchUntil) {
            m_bWantCrouch = true; // no blast cover reachable in time: get the centroid down behind whatever is there
        }
    }
    CheckGrenadeThreat();           // [HZM bot B4] a live grenade next to us beats everything
    CheckFriendlyBlast();           // [HZM bot breach] a team-mate's clearing grenade: get out of its sight / don't walk in
    HealThink();                    // [user 2026-09-25] out of harm's way and hurt: bandage (runs after the flee / blast checks)
    if (m_iCrossT && level.inttime - m_iCrossT >= 3000) {
        CrossEnd("time"); // [T2] the 3s after an opening / a corner are over
    }
    BuddyThink(); // [T3] pair up / hold the pair / back to back
    OpThink();    // [room-clear step 4] stack at the door / hold for the blast / go in
    DefendThink(); // [room-clear step 5] an enemy grenade came in: hold the angle it came from (or push out)
    CheckStates();

    // [HZM bug-2899] the movement layer's no-progress learner only runs while nothing about the fight explains the bot
    // milling about: no cover fight, no enemy seen for 5s, no shot for 4s, not fleeing a grenade
    movement.SetCalm(
        !m_iCfState && (!m_iLastSeenTime || level.inttime - m_iLastSeenTime > 5000)
        && (!m_iLastFireTime || level.inttime - m_iLastFireTime > 4000) && level.inttime >= m_iGrenadeFleeUntil
    );
    movement.MoveThink(m_botCmd);

    if (m_iCfStrafe || m_iCfFwd) {
        // [HZM bot cover] small positional moves on the cover spot (step out to peek round a corner, back in to hide,
        // drift back onto the spot) - after MoveThink, which zeroes the move for a bot with no route
        m_botCmd.forwardmove = (signed char)m_iCfFwd;
        m_botCmd.rightmove   = (signed char)m_iCfStrafe;
    }
    if (level.inttime < m_iHealStepUntil) {
        // [user 2026-09-25] an aborted bandage: the torso stays in the heal pose (no firing) until the bot moves 15u, so
        // side-step out of it
        m_botCmd.forwardmove = 0;
        m_botCmd.rightmove   = (signed char)(m_iHealStepDir * 127);
    }

    // [HZM bug-2871] on a ladder route the VIEW drives the climb (player_Torso.st: pitch up to mount/climb up, down to
    // mount at the top/climb down), so override whatever the states aimed at, at the normal (non-combat) turn rate,
    // and never hold a trigger (it blocks the mount).
    {
        Vector ldir;
        float  lpitch;
        if (movement.GetLadderSteer(ldir, lpitch)) {
            Vector la = ldir.toAngles();
            la.x      = lpitch;
            rotation.SetCombatTurn(false);
            rotation.SetTargetAngles(la);
            m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        }
    }

    BrainThink(); // [HZM bot B-D] look (alert / glance), danger walk, door hands-off, crouch

    if (BotNadeSafe() && m_iNadeState) {
        // [HZM bot room-clear step 2, vet B1] the grenade state is the LAST writer of the trigger and the view while a
        // throw is live: State_Idle ran after it and BrainThink's door-wait cleared the trigger, releasing a cooking
        // grenade at whatever the view was (the step-1 soak: 21 clobbers, 12% strays)
        m_botCmd.buttons = (m_botCmd.buttons & ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT)) | m_iNadeButtons;
        rotation.SetCombatTurn(false);
        rotation.SetTargetAngles(m_vNadeAim);
        if (m_iNadeState != 4) {
            rotation.SetPrecise(true);
        }
    }

    // [HZM bot room-clear step 1] who else writes the trigger / the aim while a grenade is out (vet B1: State_Idle runs
    // after State_Grenade, BrainThink's door wait clears the trigger). Log the ACT once per throw, with the state flags.
    if (m_iNadeState == 1 || m_iNadeState == 2) {
        char ev[48];
        // (not on the frame state 1 hands over to 2 - case 1 cleared the trigger and case 2 first presses it next frame)
        if (m_iNadeState == 2 && m_iNadeTime < level.inttime && !(m_iNadeClobbered & 1)
            && !(m_botCmd.buttons & BUTTON_ATTACKLEFT)) {
            m_iNadeClobbered |= 1;
            Com_sprintf(ev, sizeof(ev), "nadeclobber_m%d_st%d", m_iNadeMode, (int)m_StateFlags);
            BotEv(controlledEnt, ev, m_vNadeTarget);
        }
        const Vector ta = rotation.GetTargetAngles();
        if (!(m_iNadeClobbered & 2)
            && (fabs(AngleSubtract(ta.x, m_vNadeAim.x)) > 1.0f || fabs(AngleSubtract(ta.y, m_vNadeAim.y)) > 1.0f)) {
            m_iNadeClobbered |= 2;
            Com_sprintf(ev, sizeof(ev), "nadeaimclobber_m%d_s%d_st%d", m_iNadeMode, m_iNadeState, (int)m_StateFlags);
            BotEv(controlledEnt, ev, m_vNadeTarget);
        }
    }

    rotation.TurnThink(m_botCmd, m_botEyes);
    CheckUse();

    CheckValidWeapon();
}

// ======================================================================================================================
// [user 2026-09-25] BANDAGE WHEN OUT OF HARM'S WAY. "If they're injured and limping they already retreat, do we let them
// heal themselves at least once when they fall back and are out of harms way?" - they did not: every MP life carries a
// bandage (Medkits, mp_medkits.scr) but the heal is a held-[USE] channel and no bot code ever pressed USE for it.
//   1 STOP    hold, brake under 5u/s, face the threat axis, turn away from anything usable ahead (USE would mount an MG,
//             close a door or fire a trigger - DoUse runs on the first press)
//   2 PRESS   USE until the script's COOP_SELFHEAL torso state shows (the ACT, TRAPS T10); none in 600ms = no bandage,
//             Medkits off or refused -> stop trying for this life
//   3 CHANNEL hold still until the torso leaves COOP_SELFHEAL (the script cancels on 15u of movement or a hit)
//   4 STEP    an aborted channel: the heal pose cannot fire until the bot moves 15u, so side-step out of it
// Out of harm's way = no enemy seen for 5s, no damage for 4s, no close gunfire for 2s, and the last threat is over 1200u
// away or has no line to our (standing, else crouched) eye. bot_bandage 0 off / 1 on / 2 shadow (log ev=would only).
// ======================================================================================================================
// Would a USE press here DO something? Player::DoUse sends EV_Use to every entity in a 32u box at the end of a 64u look
// trace (getUseableEntities), and almost all of them ignore it - so count only the ones that act on a use: doors,
// turrets / emplacements, vehicles, use-triggers, use-anim / use-object props, ladders. (The raw count was never 0.)
static bool BotUseWouldAct(Player *p)
{
    int touch[32];
    const int n = p->getUseableEntities(touch, 32, true);
    for (int i = 0; i < n; i++) {
        gentity_t *ge = &g_entities[touch[i]];
        if (!ge->inuse || !ge->entity || ge->entity == p) {
            continue;
        }
        Entity *e = ge->entity;
        if (e->IsSubclassOfDoor() || e->IsSubclassOfTurretGun() || e->IsSubclassOfVehicle() || e->isSubclassOf(TriggerUse)
            || e->isSubclassOf(UseAnim) || e->isSubclassOf(UseObject) || e->isSubclassOf(FuncLadder)) {
            return true;
        }
    }
    return false;
}

bool BotController::HealSafe(Vector& threat, bool& crouch)
{
    crouch = false;
    threat = vec_zero;
    if (m_vLastEnemyPos != vec_zero) {
        threat = m_vLastEnemyPos;
    } else if (m_iHeardTime && level.inttime - m_iHeardTime < 20000) {
        threat = m_vHeardPos;
    }
    const Vector org = controlledEnt->origin;
    if (m_pEnemy && !m_pEnemy->IsDead() && IsValidEnemy(m_pEnemy)
        && G_SightTrace(m_pEnemy->EyePosition(), vec_zero, vec_zero, org + Vector(0, 0, controlledEnt->viewheight),
                        controlledEnt, m_pEnemy, MASK_SOLID, qfalse, "BotHealEnemy")) {
        return false; // the enemy we last fought still has a line to us
    }
    if (threat == vec_zero || (threat - org).lengthSquared() > Square(1200.0f)) {
        return true;
    }
    const Vector tEye = threat + Vector(0, 0, 48);
    if (!G_SightTrace(tEye, vec_zero, vec_zero, org + Vector(0, 0, controlledEnt->viewheight), controlledEnt, NULL,
                      MASK_SOLID, qfalse, "BotHealCover")) {
        return true; // a wall between the threat and our standing eye
    }
    if (!G_SightTrace(tEye, vec_zero, vec_zero, org + Vector(0, 0, 28), controlledEnt, NULL, MASK_SOLID, qfalse, "BotHealLow")) {
        crouch = true; // low cover: hidden only crouched
        return true;
    }
    return false;
}

void BotController::HealEnd(const char *ev, const char *why)
{
    if (m_iHealHoldUntil && movement.GetHoldUntil() == m_iHealHoldUntil) {
        movement.HoldFor(0); // still our hold: release it (someone else's hold is left alone)
    }
    m_iHealHoldUntil = 0;
    static cvar_t *s_probe = NULL;
    if (!s_probe) {
        s_probe = gi.Cvar_Get("bot_probe", "0", 0);
    }
    if (s_probe->integer) {
        gi.Printf(
            "^~^~^ BOTHEAL e=%d ev=%s why=%s st=%d hp0=%.0f hp1=%.0f dur=%d thr=%.0f crouch=%d\n", controlledEnt->entnum, ev, why,
            m_iHealState, m_fHealHp0, controlledEnt->health, level.inttime - m_iHealT,
            m_vHealThreat != vec_zero ? (m_vHealThreat - controlledEnt->origin).length() : -1.0f, m_bHealCrouch ? 1 : 0
        );
    }
    m_iHealState = 0;
}

void BotController::HealThink(void)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_bandage", "1", 0);
    }
    const int   now = level.inttime;
    const float hp  = controlledEnt->health;
    if (hp < m_fHealLastHp - 0.5f) {
        m_iHealDmgT = now;
    }
    m_fHealLastHp = hp;

    if (m_iHealState == 4) {
        if (now >= m_iHealStepUntil) {
            m_iHealState = 0;
        }
        return;
    }
    if (!s_on->integer || controlledEnt->IsDead()) {
        if (m_iHealState) {
            HealEnd("abort", "off");
        }
        return;
    }
    const int iChannelMs = 100 * level.intframetime + 750; // the script channel is 100 server frames

    if (m_iHealState) {
        // ---- abort on danger, in any active state
        const char *why = NULL;
        if (now < m_iGrenadeFleeUntil) {
            why = "nade";
        } else if (m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 300) {
            why = "enemy";
        } else if (m_iHealDmgT == now) {
            why = "pain";
        } else if (movement.IsOnLadder() || movement.IsOnElevatorLink() || movement.IsInLiftProtocol()) {
            why = "move";
        } else if (movement.IsHeld() && movement.GetHoldUntil() != m_iHealHoldUntil) {
            why = "blast"; // a breach / blast hold replaced ours this frame (CheckFriendlyBlast runs first)
        }
        if (why) {
            if (m_iHealState == 3 && (m_iHealT + iChannelMs - 750) - now <= 700) {
                // under 0.7s of channel left: finish it
            } else {
                const bool bStep = m_iHealState == 3;
                HealEnd("abort", why);
                m_iHealNext = now + 8000;
                if (bStep) {
                    // the heal pose cannot fire until we move 15u: side-step, toward the side with more room
                    Vector       vFwd, vRight; // vRight is the LEFT vector (AngleVectorsLeft)
                    const Vector eye = controlledEnt->origin + Vector(0, 0, 24);
                    controlledEnt->angles.AngleVectorsLeft(&vFwd, &vRight, NULL);
                    trace_t tR = G_Trace(eye, vec_zero, vec_zero, eye - vRight * 48.0f, controlledEnt, MASK_PLAYERSOLID, qfalse, "BotHealStep");
                    trace_t tL = G_Trace(eye, vec_zero, vec_zero, eye + vRight * 48.0f, controlledEnt, MASK_PLAYERSOLID, qfalse, "BotHealStep");
                    m_iHealStepDir   = (tR.fraction >= tL.fraction) ? 1 : -1;
                    m_iHealStepUntil = now + 250;
                    m_iHealState     = 4;
                }
                return;
            }
        }
    }

    switch (m_iHealState) {
    case 0: {
        if (m_bHealOut || m_iHealFails >= 3 || now < m_iHealNext) {
            return;
        }
        if (controlledEnt->max_health <= 0 || hp >= controlledEnt->max_health * 0.6f) {
            return;
        }
        if (!controlledEnt->groundentity || controlledEnt->groundentity->entity != world) {
            return;
        }
        if ((m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 5000) || (m_iHealDmgT && now - m_iHealDmgT < 4000)) {
            return;
        }
        if (m_iHeardTime && now - m_iHeardTime < 2000
            && (m_vHeardPos - controlledEnt->origin).lengthSquared() < Square(600.0f)) {
            return;
        }
        if (now < m_iRetreatUntil || now < m_iGrenadeFleeUntil || m_iNadeState || m_iCfState == 1 || m_iCfState == 3
            || m_iOpId) { // ([room-clear] not while stacking at a door)
            return;
        }
        if (movement.IsHeld() || movement.IsOnLadder() || movement.IsOnElevatorLink() || movement.IsInLiftProtocol()
            || movement.IsWaitingForDoor() || movement.IsDirectSteering()) {
            return;
        }
        Vector lf, lt;
        if (movement.GetApproachingLadder(lf, lt)) {
            return;
        }
        const char *torso = controlledEnt->GetTorsoStateName();
        if (Q_stricmp(torso, "STAND") && Q_stricmp(torso, "AIM")) {
            return; // mid reload / raise / throw: the forced heal pose would cut it off
        }
        if (now - m_iHealSafeT < 500) {
            return; // the traces below run at most twice a second
        }
        m_iHealSafeT = now;
        // a downed team-mate close by: our USE would count as a revive (mp_dbno) and the script refuses the heal anyway
        for (int i = 0; i < game.maxclients; i++) {
            gentity_t *ge = &g_entities[i];
            if (!ge->inuse || !ge->entity || ge->entity == controlledEnt || !ge->entity->IsSubclassOfPlayer()) {
                continue;
            }
            Player *o = static_cast<Player *>(ge->entity);
            if (o->GetTeam() == controlledEnt->GetTeam() && !Q_stricmpn(o->GetLegsStateName(), "DBNO", 4)
                && (o->origin - controlledEnt->origin).lengthSquared() < Square(100.0f)) {
                return;
            }
        }
        Vector threat;
        bool   crouch;
        if (!HealSafe(threat, crouch)) {
            return;
        }
        m_vHealThreat = threat;
        m_bHealCrouch = crouch;
        m_fHealYaw    = (threat != vec_zero) ? (threat - controlledEnt->origin).toYaw() : controlledEnt->GetViewAngles().y;
        m_iHealYawTry = 0;
        m_iHealT      = now;
        if (s_on->integer == 2) {
            m_iHealState = 1; // shadow: log what it WOULD do, never press
            HealEnd("would", "shadow");
            m_iHealNext = now + 15000;
            return;
        }
        m_iHealSlowSince = 0;
        m_iHealState     = 1;
    }
    // fall through: start stopping this frame
    case 1: {
        movement.HoldFor(200); // refreshed every frame: an orphaned hold dies in 200ms
        m_iHealHoldUntil = movement.GetHoldUntil();
        if (controlledEnt->velocity.lengthXYSquared() < Square(5.0f)) {
            if (!m_iHealSlowSince) {
                m_iHealSlowSince = now;
            }
        } else {
            m_iHealSlowSince = 0; // braking from a run slides ~25-30u: the channel cancels at 15u
        }
        if (now - m_iHealT > 3000) {
            HealEnd("fail", "stop");
            m_iHealFails++;
            m_iHealNext = now + 10000;
            return;
        }
        const float yawErr = fabs(AngleSubtract(controlledEnt->GetViewAngles().y, m_fHealYaw));
        if (!m_iHealSlowSince || now - m_iHealSlowSince < 150 || yawErr > 10.0f) {
            return;
        }
        if (BotUseWouldAct(controlledEnt)) {
            // something usable in front (an MG on the sill, a door, a trigger): turn away from it and try again
            static const float s_off[4] = {45.0f, -90.0f, 135.0f, -90.0f};
            if (m_iHealYawTry >= 4) {
                HealEnd("fail", "useable");
                m_iHealFails++;
                m_iHealNext = now + 10000;
                return;
            }
            m_fHealYaw = anglemod(m_fHealYaw + s_off[m_iHealYawTry++]);
            return;
        }
        m_iHealState = 2;
        m_iHealT     = now;
        return;
    }
    case 2: {
        movement.HoldFor(200);
        m_iHealHoldUntil = movement.GetHoldUntil();
        if (!Q_stricmp(controlledEnt->GetTorsoStateName(), "COOP_SELFHEAL")) {
            m_fHealHp0   = hp;
            m_iHealState = 3;
            m_iHealT     = now;
            return;
        }
        if (now - m_iHealT > 600) {
            // no channel: no bandage left, Medkits off, or the script refused - stop for this life
            m_bHealOut = true;
            HealEnd("fail", "nochannel");
            return;
        }
        return;
    }
    case 3: {
        movement.HoldFor(200);
        m_iHealHoldUntil = movement.GetHoldUntil();
        if (!Q_stricmp(controlledEnt->GetTorsoStateName(), "COOP_SELFHEAL") && now - m_iHealT < iChannelMs) {
            return;
        }
        const bool bOk = hp > m_fHealHp0 + 1.0f;
        HealEnd(bOk ? "done" : "fail", bOk ? "done" : "cancel");
        if (!bOk) {
            m_iHealFails++;
        }
        m_iHealNext = now + (bOk ? 3000 : 10000);
        return;
    }
    default:
        m_iHealState = 0;
        return;
    }
}

void BotController::CheckUse(void)
{
    Vector  dir;
    Vector  start;
    Vector  end;
    trace_t trace;
    if (m_iHealState) {
        // [user 2026-09-25] bandaging owns USE: pressed only in PRESS (until the channel starts - held any longer it would
        // plant / defuse / capture in the objective modes), released otherwise. Before the IsHeld clear below.
        if (m_iHealState == 2) {
            m_botCmd.buttons |= BUTTON_USE;
        } else {
            m_botCmd.buttons &= ~BUTTON_USE;
        }
        return;
    }

    if (controlledEnt->GetLadder()) {
        return;
    }
    if (movement.IsHeld()) {
        // [HZM bot breach] holding for a clearing grenade at a ladder foot: USE would mount the ladder into the blast
        m_botCmd.buttons &= ~BUTTON_USE;
        return;
    }

    controlledEnt->angles.AngleVectorsLeft(&dir);

    start = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight);
    end   = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight) + dir * 64;

    trace = G_Trace(
        start, vec_zero, vec_zero, end, controlledEnt, MASK_USABLE | MASK_LADDER, false, "BotController::CheckUse"
    );

    if (!trace.ent || trace.ent->entity == world) {
        m_botCmd.buttons &= ~BUTTON_USE;
        return;
    }

    if (trace.ent->entity->IsSubclassOfDoor()) {
        Door *door = static_cast<Door *>(trace.ent->entity);
        if (door->isOpen()) {
            // Don't use an open door
            m_botCmd.buttons &= ~BUTTON_USE;
            return;
        }
    } else if (!trace.ent->entity->isSubclassOf(FuncLadder)) {
        m_botCmd.buttons &= ~BUTTON_USE;
        return;
    } else if (!movement.IsOnLadder()) {
        // [HZM bug-2871] a ladder the ROUTE does not climb: don't grab it (bots brushing past one mounted it and
        // then had to jump off - jank). On a ladder route, USE backs up the forward+pitch auto-mount.
        m_botCmd.buttons &= ~BUTTON_USE;
        return;
    }

    //
    // Toggle the use button
    //
    m_botCmd.buttons ^= BUTTON_USE;

#if 0
    Vector  forward;
    Vector  start, end;

    AngleVectors(controlledEnt->GetViewAngles(), forward, NULL, NULL);

    start = (controlledEnt->m_vViewPos - forward * 12.0f);
    end   = (controlledEnt->m_vViewPos + forward * 128.0f);

    trace = G_Trace(start, vec_zero, vec_zero, end, controlledEnt, MASK_LADDER, qfalse, "checkladder");
    if (trace.ent->entity && trace.ent->entity->isSubclassOf(FuncLadder)) {
        return;
    }

    m_botCmd.buttons ^= BUTTON_USE;
#endif
}

bool BotController::CheckWindows(void)
{
    trace_t trace;
    Vector  start, end;
    Vector  dir;

    controlledEnt->angles.AngleVectorsLeft(&dir);
    start = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight);
    end   = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight) + dir * 64;

    trace = G_Trace(start, vec_zero, vec_zero, end, controlledEnt, MASK_PLAYERSOLID, false, "BotController::CheckUse");

    if (trace.fraction != 1 && trace.ent) {
        if (trace.ent->entity->isSubclassOf(WindowObject)) {
            return true;
        }
    }

    return false;
}

void BotController::CheckValidWeapon()
{
    if (m_iNadeState) {
        // [HZM bot B4/breach] mid-throw: the grenade state owns the weapon. Between putting the gun away and the
        // grenade coming up there is NO active weapon, and "holstered -> best gun" below re-drew the gun every time -
        // no bot grenade (lob at a hidden enemy, clearing throw) ever left the hand (test 3: 8 breaches, 0 throws)
        return;
    }
    Weapon *weapon = controlledEnt->GetActiveWeapon(WEAPON_MAIN);
    if (!weapon) {
        // If holstered, use the best weapon available
        UseWeaponWithAmmo();
    } else if (!weapon->HasAmmo(FIRE_PRIMARY) && !controlledEnt->GetNewActiveWeapon()) {
        // In case the current weapon has no ammo, use the best available weapon
        UseWeaponWithAmmo();
    }
}

void BotController::SendCommand(const char *text)
{
    char        *buffer;
    char        *data;
    size_t       len;
    ConsoleEvent ev;

    len = strlen(text) + 1;

    buffer = (char *)gi.Malloc(len);
    data   = buffer;
    Q_strncpyz(data, text, len);

    const char *com_token = COM_Parse(&data);

    // [HZM] the early returns leaked the copy: every stufftext "seta ..." to a bot ("seta" is not an event)
    if (!com_token) {
        gi.Free(buffer);
        return;
    }

    controlledEnt->m_lastcommand = com_token;

    if (!Event::GetEvent(com_token)) {
        gi.Free(buffer);
        return;
    }

    ev = ConsoleEvent(com_token);

    if (!(ev.GetEventFlags(ev.eventnum) & EV_CONSOLE)) {
        gi.Free(buffer);
        return;
    }

    ev.SetConsoleEdict(controlledEnt->edict);

    while (1) {
        com_token = COM_Parse(&data);

        if (!com_token || !*com_token) {
            break;
        }

        ev.AddString(com_token);
    }

    gi.Free(buffer);

    try {
        controlledEnt->ProcessEvent(ev);
    } catch (ScriptException& exc) {
        gi.DPrintf("*** Bot Command Exception *** %s\n", exc.string.c_str());
    }
}

/*
====================
AimAtAimNode

Make the bot face toward the current path
====================
*/
void BotController::AimAtAimNode(void)
{
    Vector goal;

    if (!movement.IsMoving()) {
        return;
    }

    //goal = movement.GetCurrentGoal();
    //if (goal != controlledEnt->origin) {
    //    rotation.AimAt(goal);
    //}

    if (controlledEnt->GetLadder()) {
        Vector vAngles = movement.GetCurrentPathDirection().toAngles();
        vAngles.x      = Q_clamp_float(vAngles.x, -80, 80);

        rotation.SetTargetAngles(vAngles);
        return;
    } else {
        Vector targetAngles;
        targetAngles   = movement.GetCurrentPathDirection().toAngles();
        targetAngles.x = 0;
        rotation.SetTargetAngles(targetAngles);
    }
}

/*
====================
CheckReload

Make the bot reload if necessary
====================
*/
void BotController::CheckReload(void)
{
    Weapon *weap;

    if (level.inttime < m_iLastFireTime + 2000) {
        // Don't reload while attacking
        return;
    }

    weap = controlledEnt->GetActiveWeapon(WEAPON_MAIN);

    if (weap && weap->CheckReload(FIRE_PRIMARY)) {
        SendCommand("reload");
    }
}

/*
====================
NoticeEvent

Warn the bot of an event
====================
*/
void BotController::NoticeEvent(Vector vPos, int iType, Entity *pEnt, float fDistanceSquared, float fRadiusSquared)
{
    Sentient *pSentOwner;
    float     fRangeFactor;
    Vector    delta1, delta2;

    // [HZM Phase 3a] reactive-hearing cvars (MP-bot-only; NoticeEvent binds only to bot-controlled Players).
    static cvar_t *s_botHearing     = NULL;
    static cvar_t *s_botHearReact   = NULL;
    static cvar_t *s_botHearFovFire = NULL;
    if (!s_botHearing) {
        s_botHearing     = gi.Cvar_Get("bot_hearing", "1", CVAR_ARCHIVE);
        s_botHearReact   = gi.Cvar_Get("bot_hearing_react", "1", CVAR_ARCHIVE);
        s_botHearFovFire = gi.Cvar_Get("bot_fov_fire", "45", CVAR_ARCHIVE);
    }
    const bool bGunfire =
        (iType == AI_EVENT_WEAPON_FIRE || iType == AI_EVENT_WEAPON_IMPACT || iType == AI_EVENT_EXPLOSION);

    if (m_iCuriousTime) {
        delta1 = vPos - controlledEnt->origin;
        delta2 = m_vNewCuriousPos - controlledEnt->origin;
        // [HZM Phase 3a] nearest/newest wins: keep the current curious point only if the NEW event is
        // FARTHER (this test was inverted, which fixated bots on a distant old sound and made them ignore a
        // closer new threat). Gunfire under bot_hearing always passes so a fresh shot is never dropped here.
        if (delta1.lengthSquared() > delta2.lengthSquared() && !(s_botHearing->integer && bGunfire)) {
            return;
        }
    }

    fRangeFactor = 1.0 - (fDistanceSquared / fRadiusSquared);

    // [HZM Phase 3a] you always notice gunfire aimed near you: bypass the probabilistic distance drop for
    // weapon fire / explosions (kept for footsteps and voices).
    if (fRangeFactor < random() && !(s_botHearing->integer && bGunfire)) {
        return;
    }

    if (!pEnt) {
        // [HZM] G_BroadcastAIEvent(NULL, ...) is legal (level.cpp badplace changes) - this dereferenced it
        pSentOwner = NULL;
    } else if (pEnt->IsSubclassOfSentient()) {
        pSentOwner = static_cast<Sentient *>(pEnt);
    } else if (pEnt->IsSubclassOfVehicleTurretGun()) {
        VehicleTurretGun *pVTG = static_cast<VehicleTurretGun *>(pEnt);
        pSentOwner             = pVTG->GetSentientOwner();
    } else if (pEnt->IsSubclassOfItem()) {
        Item *pItem = static_cast<Item *>(pEnt);
        pSentOwner  = pItem->GetOwner();
    } else if (pEnt->IsSubclassOfProjectile()) {
        Projectile *pProj = static_cast<Projectile *>(pEnt);
        pSentOwner        = pProj->GetOwner();
    } else {
        pSentOwner = NULL;
    }

    if (pSentOwner) {
        if (pSentOwner == controlledEnt) {
            // Ignore self
            return;
        }

        if ((pSentOwner->flags & FL_NOTARGET) || pSentOwner->getSolidType() == SOLID_NOT) {
            return;
        }

        // Ignore teammates
        if (pSentOwner->IsSubclassOfPlayer()) {
            Player *p = static_cast<Player *>(pSentOwner);

            if (g_gametype->integer >= GT_TEAM && p->GetTeam() == controlledEnt->GetTeam()) {
                return;
            }
        }
    }

    // [HZM bot B1] a HEARD enemy (gunfire, impacts, explosions, footsteps): look that way, and when it is close go and see -
    // State_Curious lets this beat the Push objective for a few seconds (before, the objective node always won, so bots
    // walked straight past a firefight they could hear). bot_investigate 0 = the old curiosity only.
    {
        static cvar_t *s_botInvestigate     = NULL;
        static cvar_t *s_botInvestigateDist = NULL;
        if (!s_botInvestigate) {
            s_botInvestigate     = gi.Cvar_Get("bot_investigate", "1", 0);
            s_botInvestigateDist = gi.Cvar_Get("bot_investigateDist", "1300", 0);
        }
        if (pSentOwner && IsValidEnemy(pSentOwner) && (bGunfire || iType == AI_EVENT_FOOTSTEP)) {
            m_vHeardPos  = pSentOwner->origin; // [HZM bot breach] "someone is in there" - makes a clearing throw likely
            m_iHeardTime = level.inttime;
        }
        if (s_botInvestigate->integer && pSentOwner && !m_pEnemy && IsValidEnemy(pSentOwner)
            && (bGunfire || iType == AI_EVENT_FOOTSTEP)) {
            m_vAlertPos   = vPos;
            m_iAlertUntil = level.inttime + (iType == AI_EVENT_FOOTSTEP ? 1200 : 1800);
            Vector vObjI;
            const bool bBehind = movement.GetObjective(vObjI)
                              && (vPos - vObjI).lengthXY() > (controlledEnt->origin - vObjI).lengthXY() + 400.0f;
            if (fDistanceSquared < Square(s_botInvestigateDist->value) && !bBehind) {
                // [HZM bug-2879] not a sound well BEHIND us relative to the objective: turning back for it was a
                // reversal source (probe2) - just look that way (the alert above)
                m_vInvestigatePos   = vPos;
                m_iInvestigateUntil = level.inttime + 9000;
            }
        }
    }

    switch (iType) {
    case AI_EVENT_MISC:
    case AI_EVENT_MISC_LOUD:
        break;
    case AI_EVENT_WEAPON_FIRE:
    case AI_EVENT_WEAPON_IMPACT:
    case AI_EVENT_EXPLOSION:
        // [HZM Phase 3a] REACT to gunfire instead of only wandering toward it. If we can see the shooter and
        // they are an enemy, engage now; otherwise snap our facing toward the shot source (peek) and sharpen
        // the next reaction. bot_hearing/bot_hearing_react gate it back to the stock curiosity-only behavior.
        if (s_botHearing->integer && s_botHearReact->integer && pSentOwner && IsValidEnemy(pSentOwner)) {
            float maxDist = VisionDistance();
            if (controlledEnt->CanSee(pSentOwner, s_botHearFovFire->value, maxDist, false)) {
                if (!m_pEnemy) {
                    m_iLastUnseenTime = level.inttime;
                }
                m_pEnemy             = pSentOwner;
                m_vLastEnemyPos      = pSentOwner->origin;
                m_iAttackTime        = level.inttime + 1000;
                m_iAttackStopAimTime = level.inttime + 2000;
            } else {
                m_vLastEnemyPos      = vPos;      // face + peek toward the sound, do not run onto it
                m_iAttackStopAimTime = level.inttime + 1500;
                m_iLastUnseenTime    = 0;         // awareness: shorten the next reaction gate
            }
        }
        m_iCuriousTime   = level.inttime + 20000;
        m_vNewCuriousPos = vPos;
        break;
    case AI_EVENT_AMERICAN_VOICE:
    case AI_EVENT_GERMAN_VOICE:
    case AI_EVENT_AMERICAN_URGENT:
    case AI_EVENT_GERMAN_URGENT:
    case AI_EVENT_FOOTSTEP:
    case AI_EVENT_GRENADE:
    default:
        m_iCuriousTime   = level.inttime + 20000;
        m_vNewCuriousPos = vPos;
        break;
    }
}

/*
====================
ClearEnemy

Clear the bot's enemy
====================
*/
void BotController::ClearEnemy(void)
{
    m_iAttackTime   = 0;
    m_pEnemy        = NULL;
    m_iEnemyEyesTag = -1;
    m_vOldEnemyPos  = vec_zero;
    m_vLastEnemyPos = vec_zero;
}

/*
====================
Bot states
--------------------
____________________
--------------------
____________________
--------------------
____________________
--------------------
____________________
====================
*/

void BotController::CheckStates(void)
{
    m_StateCount = 0;

    for (int i = 0; i < MAX_BOT_FUNCTIONS; i++) {
        botfunc_t *func = &botfuncs[i];

        if (func->CheckCondition) {
            if ((this->*func->CheckCondition)()) {
                if (!(m_StateFlags & (1 << i))) {
                    m_StateFlags |= 1 << i;
                    BotStateEv(controlledEnt, i, true);

                    if (func->BeginState) {
                        (this->*func->BeginState)();
                    }
                }

                if (func->ThinkState) {
                    m_StateCount++;
                    (this->*func->ThinkState)();
                }
            } else {
                if ((m_StateFlags & (1 << i))) {
                    m_StateFlags &= ~(1 << i);
                    BotStateEv(controlledEnt, i, false);

                    if (func->EndState) {
                        (this->*func->EndState)();
                    }
                }
            }
        } else {
            if (func->ThinkState) {
                m_StateCount++;
                (this->*func->ThinkState)();
            }
        }
    }

    assert(m_StateCount);
    if (!m_StateCount) {
        gi.DPrintf("*** WARNING *** %s was stuck with no states !!!", controlledEnt->client->pers.netname);
        State_Reset();
    }
}

/*
====================
Default state


====================
*/
void BotController::State_DefaultBegin(void)
{
    movement.SetWhy("statebegin"); // [HZM bot probe2]
    movement.ClearMove();
}

void BotController::State_DefaultEnd(void) {}

void BotController::State_Reset(void)
{
    m_iCuriousTime    = 0;
    m_iAttackTime     = 0;
    m_vLastCuriousPos = vec_zero;
    m_vOldEnemyPos    = vec_zero;
    m_vLastEnemyPos   = vec_zero;
    m_vLastDeathPos   = vec_zero;
    m_pEnemy          = NULL;
    m_iEnemyEyesTag   = -1;
    m_iLastPainTime   = 0;
    m_pLastAimEnemy   = NULL; // [HZM Phase 5b] force a fresh aim-convergence ramp on the next enemy lock
    m_iEnemyLockTime  = 0;
    m_iReactUntil     = 0;
    m_iCoverUntil     = 0;
    m_iCoverRetryTime = 0;
}

/*
====================
Idle state

Make the bot move to random directions
====================
*/
void BotController::InitState_Idle(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Idle;
    func->ThinkState     = &BotController::State_Idle;
}

bool BotController::CheckCondition_Idle(void)
{
    if (m_iCuriousTime) {
        return false;
    }

    if (m_iAttackTime) {
        return false;
    }

    return true;
}

void BotController::State_Idle(void)
{
    if (BotNadeSafe() && m_iNadeState) {
        // [HZM bot room-clear step 2, vet B1] Idle runs AFTER the grenade state: its trigger clear and path aim released
        // cooking grenades (pitch 0 along the route). A live throw owns the trigger, the view and the feet.
        return;
    }
    if (CheckWindows()) {
        m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
        m_iLastFireTime = level.inttime;
    } else {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        CheckReload();
    }

    AimAtPath(); // [T2] corner checks in front of the route aim

    // [HZM bot C4] the team's HOLDERS set up at its front line; [bot C1] the rest take their lane out of spawn first
    if (IdleHoldFront() || IdleLane()) {
        return;
    }

    // [HZM bug-2884] out HUNTING the fight (see below): let that route run
    if (level.inttime < m_iHuntUntil && movement.IsMoving() && !Q_stricmp(movement.GetGoalWhy(), "hunt")) {
        return;
    }
    // [HZM bug-2890] a no-knowledge hunt is a DEFEND: arrived at our own end, hold it for the rest of the window instead
    // of walking straight back to the enemy end (Desert Road round 10: hunt ping-pong between the ends, 105 reversals and
    // 28 loops in 6 min)
    if (level.inttime < m_iHuntUntil && m_bHuntDefend && !movement.IsMoving()) {
        m_iGlanceNext = Q_min(m_iGlanceNext, level.inttime + 800);
        return;
    }
    movement.SetWhy("objective"); // [HZM bot probe2]
    const bool bHasObj = movement.MoveToBestAttractivePoint();
    if (bHasObj) {
        m_iNoObjSince = 0;
    }
    // [HZM bug-2884] AT the objective (Push: the enemy END) with nobody to fight: go and find the fight. Once bots stopped
    // churning there (bug-2883) both teams on Desert Road reached each other's end by different roads and then stood
    // in them (probe2: 65% of bot time standing, 4 deaths in 6 min). After 6s there with no enemy seen for 10s: head
    // for the side's freshest SIGHTING of an enemy (what team-mates actually saw), else where the side last lost
    // someone, else the contested front line - never a position the side has no knowledge of.
    if (bHasObj && movement.AtObjective()) {
        if (!m_iAtObjSince) {
            m_iAtObjSince = level.inttime;
        }
        static cvar_t *s_botHunt = NULL;
        if (!s_botHunt) {
            s_botHunt = gi.Cvar_Get("bot_hunt", "1", 0);
        }
        if (s_botHunt->integer && level.inttime - m_iAtObjSince > 6000 && level.inttime - m_iLastEnemySeenAny > 10000
            && level.inttime >= m_iHuntNext) {
            m_iHuntNext = level.inttime + 20000;
            Vector h;
            int    kind = 0;
            if (FindHuntTarget(h, kind)) {
                movement.SetWhy("hunt");
                movement.MoveNear(h, 300.0f);
                if (movement.IsMoving()) {
                    // knowledge (a sighting / a death): go and look, 25s. No knowledge (kind 3/4: where the enemy is
                    // heading / the front): a DEFEND - get there and hold it ~30s, and not again for 90s
                    m_bHuntDefend = kind >= 3;
                    m_iHuntUntil  = level.inttime + (m_bHuntDefend ? 60000 : 25000);
                    m_iHuntNext   = level.inttime + (m_bHuntDefend ? 90000 : 20000);
                    m_iAtObjSince = 0;
                    BotEv(controlledEnt, m_bHuntDefend ? "defend" : "hunt", h);
                    return;
                }
            }
        }
    } else {
        m_iAtObjSince = 0;
    }
    if (!bHasObj && !movement.IsMoving()) {
        // [HZM bug-2879] a map WITH objectives (Push): a bot between objectives (it just finished its stay at one and
        // the node is on its short cooldown, or it cannot path to one from here) HOLDS and looks round for 8s rather
        // than the stock random 0.5-2.5k-unit walk in an arbitrary direction (probe2: all 'wander' samples were bots
        // with no objective - 6% of The Rail Yard's bot time, much of it walking away from the fight). Past 8s it
        // repositions with a SHORT wander (it may be on an island the objective cannot be reached from).
        if (attractiveNodes.NumObjects() && m_vLastDeathPos == vec_zero) {
            if (!m_iNoObjSince) {
                m_iNoObjSince = level.inttime;
            }
            if (level.inttime - m_iNoObjSince < 8000) {
                m_iGlanceNext = Q_min(m_iGlanceNext, level.inttime + 800);
                return;
            }
            m_iNoObjSince = level.inttime;
            Vector rd(G_CRandom(1.0f), G_CRandom(1.0f), 0);
            if (rd.lengthSquared() < 0.01f) {
                rd = Vector(1, 0, 0);
            }
            VectorNormalize2D(rd);
            movement.SetWhy("wandershort");
            movement.AvoidPath(controlledEnt->origin - rd * 16.0f, 300.0f + G_Random(400.0f), rd * 512.0f);
            return;
        }
        if (m_vLastDeathPos != vec_zero) {
            movement.SetWhy("lastdeath"); // [HZM bot probe2]
            movement.MoveTo(m_vLastDeathPos);

            if (movement.MoveDone()) {
                m_vLastDeathPos = vec_zero;
            }
        } else {
            Vector randomDir(G_CRandom(16), G_CRandom(16), G_CRandom(16));
            Vector preferredDir;
            float  radius = 512 + G_Random(2048);

            preferredDir += Vector(controlledEnt->orientation[0]) * (rand() % 5 ? 1024 : -1024);
            preferredDir += Vector(controlledEnt->orientation[2]) * (rand() % 5 ? 1024 : -1024);
            movement.SetWhy("wander"); // [HZM bot probe2]
            movement.AvoidPath(controlledEnt->origin + randomDir, radius, preferredDir);
        }
    }
}

/*
====================
Curious state

Forward to the last event position
====================
*/
void BotController::InitState_Curious(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Curious;
    func->ThinkState     = &BotController::State_Curious;
}

bool BotController::CheckCondition_Curious(void)
{
    if (m_iAttackTime) {
        m_iCuriousTime = 0;
        return false;
    }

    if (level.inttime > m_iCuriousTime) {
        if (m_iCuriousTime) {
            movement.SetWhy("curiousclr"); // [HZM bot probe2]
            movement.ClearMove();
            m_iCuriousTime = 0;
        }

        return false;
    }

    return true;
}

void BotController::State_Curious(void)
{
    if (CheckWindows()) {
        m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
        m_iLastFireTime = level.inttime;
    } else {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    }

    AimAtPath(); // [T2] corner checks in front of the route aim

    // [HZM bot B1/B2] a heard / just-lost ENEMY close by beats the Push objective for a few seconds: go and look
    if (level.inttime < m_iInvestigateUntil) {
        if ((m_vInvestigatePos - controlledEnt->origin).lengthXYSquared() < Square(160)) {
            m_iInvestigateUntil = 0; // got there: a look round (BrainThink glances) and carry on
            m_iGlanceNext       = 0;
        } else {
            if (!movement.IsMoving() || (m_vInvestigateIssued - m_vInvestigatePos).lengthSquared() > Square(96)) {
                movement.SetWhy("investigate"); // [HZM bot probe2]
                movement.MoveNear(m_vInvestigatePos, 96.0f);
                m_vInvestigateIssued = m_vInvestigatePos;
                if (!movement.IsMoving() || !movement.PathReaches(m_vInvestigatePos, 256.0f)) {
                    movement.SetWhy("investigateclr"); // [HZM bot probe2]
                    movement.ClearMove();
                    m_iInvestigateUntil = 0; // cannot get there: not worth it
                }
            }
            if (m_iInvestigateUntil) {
                m_vAlertPos   = m_vInvestigatePos; // walk in with the gun on the spot
                m_iAlertUntil = level.inttime + 300;
                return;
            }
        }
    }

    // [HZM bug-2959] a HOLDER goes to (and holds) its line in the curious state too: flipping between the line (idle) and
    // the enemy-end objective (curious / attack) every time it heard something reversed it each time - most of the
    // "holdfront" loops were these state flips, not the front moving (bot_holdFrontSmooth)
    if (gi.Cvar_Get("bot_holdFrontSmooth", "0", 0)->integer >= 2 && IdleHoldFront()) {
        return;
    }
    movement.SetWhy("curiousobj"); // [HZM bot probe2]
    if (!movement.MoveToBestAttractivePoint(3) && (!movement.IsMoving() || m_vLastCuriousPos != m_vNewCuriousPos)) {
        movement.SetWhy("curious"); // [HZM bot probe2]
        movement.MoveTo(m_vNewCuriousPos);
        m_vLastCuriousPos = m_vNewCuriousPos;
        // [HZM bug-2849] a noise the navmesh cannot reach (the far side of a closed gate/fence) is not worth a trip:
        // the partial path ends AT the bars and the bot parks there. Drop the curiosity and carry on.
        if (!movement.PathReaches(m_vNewCuriousPos, 192.0f)) {
            movement.SetWhy("curiousclr"); // [HZM bot probe2]
            movement.ClearMove();
            m_iCuriousTime = 0;
            return;
        }
    }

    if (movement.MoveDone()) {
        m_iCuriousTime = 0;
    }
}

/*
====================
Attack state

Attack the enemy
====================
*/
void BotController::InitState_Attack(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Attack;
    func->EndState       = &BotController::State_EndAttack;
    func->ThinkState     = &BotController::State_Attack;
}

static Vector bot_origin;

static int sentients_compare(const void *elem1, const void *elem2)
{
    Entity *e1, *e2;
    float   delta[3];
    float   d1, d2;

    e1 = *(Entity **)elem1;
    e2 = *(Entity **)elem2;

    VectorSubtract(bot_origin, e1->origin, delta);
    d1 = VectorLengthSquared(delta);

    VectorSubtract(bot_origin, e2->origin, delta);
    d2 = VectorLengthSquared(delta);

    if (d2 <= d1) {
        return d1 > d2;
    } else {
        return -1;
    }
}

bool BotController::IsValidEnemy(Sentient *sent) const
{
    if (sent == controlledEnt) {
        return false;
    }

    if (sent->hidden() || (sent->flags & FL_NOTARGET)) {
        // Ignore hidden / non-target enemies
        return false;
    }

    if (sent->IsDead()) {
        // Ignore dead enemies
        return false;
    }

    if (sent->getSolidType() == SOLID_NOT) {
        // Ignore non-solid, like spectators
        return false;
    }

    if (sent->IsSubclassOfPlayer()) {
        Player *player = static_cast<Player *>(sent);

        if (g_gametype->integer >= GT_TEAM && player->GetTeam() == controlledEnt->GetTeam()) {
            return false;
        }
    } else {
        if (sent->m_Team == controlledEnt->m_Team) {
            return false;
        }
    }

    return true;
}

bool BotController::FindCoverPosition(const Vector& threatPos, Vector& outCover)
{
    // [HZM Phase 1] Sample a small ring of nearby spots and return the NEAREST that is both (a) reachable
    // from us in a straight line (bot->candidate not walled off) and (b) COVERED from the threat
    // (candidate->threat IS blocked by geometry). Cheap (a handful of point traces) and best-effort: returns
    // false if none qualify, so the caller just keeps its normal behaviour. MP playerbots only.
    Vector vBotEye = controlledEnt->origin;
    vBotEye.z += controlledEnt->viewheight;

    Vector vThreatEye = threatPos;
    vThreatEye.z += 48.0f; // approximate standing eye height above the threat's origin

    Vector vToThreat = threatPos - controlledEnt->origin;
    vToThreat.z = 0;
    if (vToThreat.length() < 1.0f) {
        return false;
    }
    VectorNormalizeFast(vToThreat);
    Vector vRight(vToThreat[1], -vToThreat[0], 0.0f); // ground-plane right-hand perpendicular

    // offset weights: x = along 'right', y = along 'toThreat' (negative y = away from the threat)
    static const float offs[7][2] = {
        {-1.0f, 0.0f },
        { 1.0f, 0.0f },
        {-0.8f, -0.6f},
        { 0.8f, -0.6f},
        { 0.0f, -1.0f},
        {-0.5f, -0.3f},
        { 0.5f, -0.3f}
    };

    bool   bFound     = false;
    float  bestDistSq = 0.0f;
    Vector vBest;

    for (int i = 0; i < 7; i++) {
        const float dist    = 200.0f;
        Vector      cand    = controlledEnt->origin + vRight * (offs[i][0] * dist) + vToThreat * (offs[i][1] * dist);
        Vector      candEye = cand;
        candEye.z += controlledEnt->viewheight;

        // (a) reachable-ish: the straight line from us to the candidate must be clear
        trace_t tReach = G_Trace(vBotEye, vec_zero, vec_zero, candEye, controlledEnt, MASK_SOLID, false, "BotCoverReach");
        if (tReach.fraction < 0.98f) {
            continue;
        }

        // (b) covered: the line from the candidate to the threat must be BLOCKED by geometry
        trace_t tCover = G_Trace(candEye, vec_zero, vec_zero, vThreatEye, controlledEnt, MASK_SOLID, false, "BotCoverLos");
        if (tCover.fraction >= 0.98f) {
            continue; // still exposed there
        }

        float d = (cand - controlledEnt->origin).lengthSquared();
        if (!bFound || d < bestDistSq) {
            bFound     = true;
            bestDistSq = d;
            vBest      = cand;
        }
    }

    if (bFound) {
        outCover = vBest;
    }
    return bFound;
}

// [user 2026-09-25] "some allies and axis also walk right past each other": the vis7 soak probe (vwhy=) put 42 of the 48
// seen-but-not-targeted samples at 2050-2900u - past the 2048u AI vision cap, while a human sees ~2500u through Rail
// Yard's fog. A bot holding a scoped or bolt-action rifle may notice out to the fog line (0.828 x farplane, the stock
// visibility factor, capped at 4000u); everyone else keeps the stock cap. Used for acquiring, holding and reacting,
// so a rifleman does not drop a target it just acquired. No fog (farplane 0) is unlimited already (CanSee: 0 = none).
// bot_visionRifle 0 = the stock cap for all.
float BotController::VisionDistance(void) const
{
    static cvar_t *s_rifle = NULL;
    if (!s_rifle) {
        s_rifle = gi.Cvar_Get("bot_visionRifle", "1", 0);
    }
    const float fStock = Q_min(world->m_fAIVisionDistance, world->farplane_distance * 0.828);
    if (!s_rifle->integer || world->farplane_distance <= 0.0f) {
        return fStock;
    }
    Weapon *pW = controlledEnt->GetActiveWeapon(WEAPON_MAIN);
    if (!pW || !(pW->GetZoom() > 0 || (pW->GetWeaponClass() & WEAPON_CLASS_RIFLE))) {
        return fStock;
    }
    return Q_max(fStock, Q_min(4000.0f, world->farplane_distance * 0.828f));
}

bool BotController::CheckCondition_Attack(void)
{
    Container<Sentient *> sents       = SentientList;
    float                 maxDistance = 0;

    bot_origin = controlledEnt->origin;
    sents.Sort(sentients_compare);

    for (int i = 1; i <= sents.NumObjects(); i++) {
        Sentient *sent = sents.ObjectAt(i);

        if (!IsValidEnemy(sent)) {
            continue;
        }

        maxDistance = VisionDistance();

        // [HZM Phase 1a] graded acquisition cone (MP-bot-only): wide up close, narrowing with range, so bots
        // notice enemies in their PERIPHERY, not only dead-ahead (the "bots must look right at me" bug). Only
        // CanSee's ARGUMENTS change; Sentient::CanSee (shared with coop) is untouched. bot_perception 0 = 80.
        static cvar_t *s_botPerception = NULL;
        static cvar_t *s_botFovAcquire = NULL;
        static cvar_t *s_botFovAcqFar  = NULL;
        if (!s_botPerception) {
            s_botPerception = gi.Cvar_Get("bot_perception", "1", CVAR_ARCHIVE);
            s_botFovAcquire = gi.Cvar_Get("bot_fov_acquire", "150", CVAR_ARCHIVE);
            s_botFovAcqFar  = gi.Cvar_Get("bot_fov_acquire_far", "90", CVAR_ARCHIVE);
        }
        float fFovAcq = 80.0f;
        if (s_botPerception->integer && maxDistance > 1.0f) {
            float fDistSq = (sent->origin - controlledEnt->origin).lengthSquared();
            float tRange  = Q_min(1.0f, (float)sqrt(fDistSq) / maxDistance);
            fFovAcq       = s_botFovAcquire->value + tRange * (s_botFovAcqFar->value - s_botFovAcquire->value);
        }

        if (controlledEnt->CanSee(sent, fFovAcq, maxDistance, false)) {
            if (m_pEnemy != sent) {
                m_iEnemyEyesTag = -1;
            }
            if (!m_pEnemy && level.inttime - m_iLastEnemySeenAny > 10000) {
                VoiceCallout("*43", 9000); // [HZM bot D3] "Enemy spotted."
            }
            m_iLastEnemySeenAny = level.inttime;

            if (!m_pEnemy) {
                m_iLastUnseenTime = level.inttime;
                // [T2 corner checks] a fresh sighting: how far off the view it was (a checked corner puts it near the
                // centre), whether it hit us first, and whether it came right after an opening / a corner (A/B metric)
                const float fAng = fabs(AngleSubtract(controlledEnt->GetViewAngles().y, (sent->origin - controlledEnt->origin).toYaw()));
                const int   iCx  = m_iCrossT ? level.inttime - m_iCrossT : -1;
                if (m_iCrossT && m_iCrossAcq < 0) {
                    m_iCrossAcq    = iCx;
                    m_fCrossAcqAng = fAng;
                }
                {
                    // [T3] a pair shares what it sees: the partner, if it is not fighting one of its own, turns to it
                    BotController *bc = BuddyCtl();
                    if (bc && !(bc->m_iLastEnemySeenAny && level.inttime - bc->m_iLastEnemySeenAny < 1000)) {
                        bc->m_vAlertPos   = sent->origin + Vector(0, 0, 40);
                        bc->m_iAlertUntil = level.inttime + 1500;
                        bc->m_vHeardPos   = sent->origin;
                        bc->m_iHeardTime  = level.inttime;
                        bc->m_iBuddySectT = level.inttime + 1500; // (and its flank slice gives way to it)
                        bc->m_bBuddyFlank = false;
                    }
                }
                static cvar_t *s_probeAcq = NULL;
                if (!s_probeAcq) {
                    s_probeAcq = gi.Cvar_Get("bot_probe", "0", 0);
                }
                if (s_probeAcq->integer) {
                    gi.Printf(
                        "^~^~^ BOTACQ e=%d en=%d d=%.0f ang=%.0f hit=%d cx=%d ck=%d ca=%d cm=%d\n", controlledEnt->entnum,
                        sent->entnum, (sent->origin - controlledEnt->origin).length(), fAng,
                        (m_iLastPainTime && level.inttime - m_iLastPainTime < 1500) ? 1 : 0, iCx, m_iCrossT ? m_iCrossKind : 0,
                        m_iCrossT ? m_iCrossAim : 0, gi.Cvar_Get("bot_cornerCheck", "1", 0)->integer
                    );
                }
            }

            m_pEnemy        = sent;
            m_vLastEnemyPos = m_pEnemy->origin;
        }

        if (m_pEnemy) {
            m_iAttackTime = level.inttime + 1000;
            return true;
        }
    }

    if (level.inttime > m_iAttackTime) {
        if (m_iAttackTime) {
            // [HZM bot B2] lost them (they are not dead): remember where, and go and check that spot, gun up
            static cvar_t *s_botInvestigate = NULL;
            if (!s_botInvestigate) {
                s_botInvestigate = gi.Cvar_Get("bot_investigate", "1", 0);
            }
            if (s_botInvestigate->integer && m_pEnemy && !m_pEnemy->IsDead() && m_vLastEnemyPos != vec_zero
                && (m_vLastEnemyPos - controlledEnt->origin).lengthSquared() < Square(1600)) {
                m_vInvestigatePos   = m_vLastEnemyPos;
                m_iInvestigateUntil = level.inttime + 10000;
                BotEv(controlledEnt, "search", m_vLastEnemyPos);
                m_iCuriousTime      = level.inttime + 10000;
                m_vNewCuriousPos    = m_vLastEnemyPos;
                m_vAlertPos         = m_vLastEnemyPos;
                m_iAlertUntil       = level.inttime + 2500;
            }
            movement.SetWhy("attackclr"); // [HZM bot probe2]
            movement.ClearMove();
            m_iAttackTime = 0;
        }

        return false;
    }

    return true;
}

void BotController::State_EndAttack(void)
{
    m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    controlledEnt->ZoomOff();
}

void BotController::State_Attack(void)
{
    bool    bMelee              = false;
    bool    bCanSee             = false;
    bool    bCanAttack          = false;
    float   fMinDistance        = 128;
    float   fMinDistanceSquared = fMinDistance * fMinDistance;
    float   fEnemyDistanceSquared;
    Weapon *pWeap   = controlledEnt->GetActiveWeapon(WEAPON_MAIN);
    bool    bNoMove = false;
    bool    bFiring = false;

    if (BotNadeSafe() && m_iNadeState) {
        // [HZM bot room-clear step 2, vet B1] ABOVE the invalid-enemy return: a lob's target dying mid-cook zeroed
        // m_iAttackTime there, Idle ran the same frame and let go of the grenade along the route
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    if (!m_pEnemy || !IsValidEnemy(m_pEnemy)) {
        // Ignore dead enemies
        m_iAttackTime = 0;
        m_bSawLast    = false; // [room-clear step 3] (no VANISH for a dead one)
        if (m_iCfState) {
            EndCoverSession("noenemy");
        }
        return;
    }

    // [HZM bot B4] mid-throw: the grenade state owns the view and the trigger
    if (m_iNadeState) {
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    // [HZM bug-2871] mid-climb (or walking onto a ladder route): no aiming, no firing, no combat moves. Aiming at the
    // enemy repitches the view - looking down reverses the climb - and a held trigger blocks the mount (USE_LADDER is
    // !ATTACK_PRIMARY). Keep the enemy remembered; the fight resumes the moment the bot steps off.
    if (movement.IsOnLadder()) {
        m_iAttackTime = level.inttime + 1000;
        return;
    }
    float fDistanceSquared = (m_pEnemy->origin - controlledEnt->origin).lengthSquared();

    m_vOldEnemyPos = m_vLastEnemyPos;

    // [HZM Phase 1b] widen the punishing ~10deg firing cone so a bot roughly facing you opens fire instead of
    // needing to be aimed dead-on (also covers the "crouched facing me, never fired" case). bot_perception 0
    // restores the stock 20. Only CanSee's arguments change.
    static cvar_t *s_botFovFire  = NULL;
    static cvar_t *s_botPercFire = NULL;
    if (!s_botFovFire) {
        s_botFovFire  = gi.Cvar_Get("bot_fov_fire", "45", CVAR_ARCHIVE);
        s_botPercFire = gi.Cvar_Get("bot_perception", "1", CVAR_ARCHIVE);
    }
    float fFovFire = s_botPercFire->integer ? s_botFovFire->value : 20.0f;
    bCanSee =
        controlledEnt->CanSee(m_pEnemy, fFovFire, VisionDistance(), false);
    if (m_bSawLast && !bCanSee && m_vLastEnemyPos != vec_zero && level.inttime >= m_iSightMarkNext
        && gi.Cvar_Get("bot_breachIntel", "1", 0)->integer) {
        // [room-clear step 3] VANISH: he just went out of sight there - tell the side (a door next to it is worth a grenade)
        m_iSightMarkNext = level.inttime + 2000;
        BotMarkAdd(s_botSightings, 32, s_botSightSeq, m_vLastEnemyPos, controlledEnt->GetTeam(), 0);
    }
    m_bSawLast = bCanSee;

    // [HZM 2026-09-23] REACTION TIME on every fresh lock (user: "they snap on to enemies a bit too quickly, they need
    // some kind of reaction time when aiming"). The stock gate below only armed when a bot had NO enemy, so switching
    // to a second enemy got none, and even when armed the bot started turning on the SAME frame it noticed you and
    // was on target by the time the ~200ms fire gate opened. Now each new enemy costs bot_reactMs + up to
    // bot_reactRandMs + up to 400ms by range BEFORE the bot starts turning onto it or may fire; an already-alert bot
    // (just shot at, heard the shot, mid-fight) reacts 40% quicker but never instantly. Not CVAR_ARCHIVE.
    static cvar_t *s_botCombat     = NULL;
    static cvar_t *s_botReact      = NULL;
    static cvar_t *s_botReactRand  = NULL;
    if (!s_botCombat) {
        s_botCombat    = gi.Cvar_Get("bot_combat_realism", "1", CVAR_ARCHIVE);
        s_botReact     = gi.Cvar_Get("bot_reactMs", "300", 0);
        s_botReactRand = gi.Cvar_Get("bot_reactRandMs", "350", 0);
    }
    if (m_pEnemy != m_pLastAimEnemy) {
        m_pLastAimEnemy  = m_pEnemy;
        m_iEnemyLockTime = level.inttime;
        float fReact     = s_botReact->value + G_Random(s_botReactRand->value)
                     + 400.0f * Q_min(1.0f, fDistanceSquared / Square(2048));
        if (level.inttime < m_iAttackStopAimTime || level.inttime < m_iLastPainTime + 1500) {
            fReact *= 0.6f;
        }
        fReact *= m_fSkillReact; // [HZM bot D1] some soldiers are quicker on the draw
        m_iReactUntil = level.inttime + (int)Q_max(0.0f, fReact);
    }
    const bool bReacting = s_botCombat->integer && level.inttime < m_iReactUntil;

    if (bCanSee) {
        if (!pWeap) {
            return;
        }

        bCanAttack = true;
        if (bReacting) {
            bCanAttack = false;
            m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT); // don't keep a trigger held from the last fight
        }
        if (m_iLastUnseenTime) {
            const float reactionTime = Q_min(1000 * Q_min(1, fDistanceSquared / Square(2048)), 1000);
            if (level.inttime <= m_iLastUnseenTime + 200 + G_Random(reactionTime)) {
                bCanAttack = false;
            } else {
                m_iLastUnseenTime = 0;
            }
        }

        if (bCanAttack) {
            const int fireDelay                    = pWeap->FireDelay(FIRE_PRIMARY) * 1000;
            float     fPrimaryBulletRange          = pWeap->GetBulletRange(FIRE_PRIMARY) / 1.25f;
            float     fPrimaryBulletRangeSquared   = fPrimaryBulletRange * fPrimaryBulletRange;
            float     fSecondaryBulletRange        = pWeap->GetBulletRange(FIRE_SECONDARY);
            float     fSecondaryBulletRangeSquared = fSecondaryBulletRange * fSecondaryBulletRange;
            float     fSpreadFactor                = pWeap->GetSpreadFactor(FIRE_PRIMARY);

            const int maxContinousFireTime = fireDelay + 500 + G_Random(1500);
            const int maxBurstTime         = fireDelay + 100 + G_Random(500);

            //
            // check the fire movement speed if the weapon has a max fire movement
            //
            if (pWeap->GetMaxFireMovement() < 1 && pWeap->HasAmmoInClip(FIRE_PRIMARY)) {
                float length;

                length = controlledEnt->velocity.length();
                if ((length / sv_runspeed->value) > (pWeap->GetMaxFireMovementMult())) {
                    bNoMove = true;
                    movement.SetWhy("firestop"); // [HZM bot probe2]
                    movement.ClearMove();
                }
            }

            fMinDistance = fPrimaryBulletRange;

            if (fMinDistance > 256) {
                fMinDistance = 256;
            }

            fMinDistanceSquared = fMinDistance * fMinDistance;

            if (controlledEnt->client->ps.stats[STAT_AMMO] <= 0
                && controlledEnt->client->ps.stats[STAT_CLIPAMMO] <= 0) {
                m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
                controlledEnt->ZoomOff();
            } else if (fDistanceSquared > fPrimaryBulletRangeSquared) {
                m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
                controlledEnt->ZoomOff();
            } else {
                //
                // Attacking
                //

                if (pWeap->IsSemiAuto()) {
                    if (controlledEnt->client->ps.iViewModelAnim != VM_ANIM_IDLE
                        && (controlledEnt->client->ps.iViewModelAnim < VM_ANIM_IDLE_0
                            || controlledEnt->client->ps.iViewModelAnim > VM_ANIM_IDLE_2)) {
                        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
                        controlledEnt->ZoomOff();
                    } else if (fSpreadFactor < 0.25) {
                        bFiring = true;
                        m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
                        if (pWeap->GetZoom()) {
                            if (!controlledEnt->IsZoomed()) {
                                m_botCmd.buttons |= BUTTON_ATTACKRIGHT;
                            } else {
                                m_botCmd.buttons &= ~BUTTON_ATTACKRIGHT;
                            }
                        }
                    } else {
                        bNoMove = true;
                        movement.SetWhy("firestop"); // [HZM bot probe2]
                        movement.ClearMove();
                    }
                } else {
                    bFiring = true;
                    m_botCmd.buttons |= BUTTON_ATTACKLEFT;
                }
            }

            //
            // Burst
            //

            if (m_iLastBurstTime) {
                if (level.inttime > m_iLastBurstTime + maxBurstTime) {
                    m_iLastBurstTime      = 0;
                    m_iContinuousFireTime = 0;
                } else {
                    m_botCmd.buttons &= ~BUTTON_ATTACKLEFT;
                }
            } else {
                if (bFiring) {
                    m_iContinuousFireTime += level.intframetime;
                } else {
                    m_iContinuousFireTime = 0;
                }

                if (!m_iLastBurstTime && m_iContinuousFireTime > maxContinousFireTime) {
                    m_iLastBurstTime      = level.inttime;
                    m_iContinuousFireTime = 0;
                }
            }

            m_iLastFireTime = level.inttime;

            if (pWeap->GetFireType(FIRE_SECONDARY) == FT_MELEE) {
                if (controlledEnt->client->ps.stats[STAT_AMMO] <= 0
                    && controlledEnt->client->ps.stats[STAT_CLIPAMMO] <= 0) {
                    bMelee = true;
                } else if (fDistanceSquared <= fSecondaryBulletRangeSquared) {
                    bMelee = true;
                }
            }

            if (bMelee) {
                m_botCmd.buttons &= ~BUTTON_ATTACKLEFT;

                if (fDistanceSquared <= fSecondaryBulletRangeSquared) {
                    m_botCmd.buttons ^= BUTTON_ATTACKRIGHT;
                } else {
                    m_botCmd.buttons &= ~BUTTON_ATTACKRIGHT;
                }
            }

            m_iAttackTime        = level.inttime + 1000;
            m_iAttackStopAimTime = level.inttime + 3000;
            m_iLastSeenTime      = level.inttime;
            m_vLastEnemyPos      = m_pEnemy->origin;
            if (level.inttime >= m_iSightMarkNext) {
                m_iSightMarkNext = level.inttime + 2000; // [HZM bug-2884] tell the side where the fight is
                BotMarkAdd(s_botSightings, 32, s_botSightSeq, m_pEnemy->origin, controlledEnt->GetTeam(), 0);
            }
        }
    } else {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        fMinDistanceSquared = 0;

        if (level.inttime > m_iLastSeenTime + 2000) {
            m_iLastUnseenTime = level.inttime;
        }
    }

    if (bReacting) {
        // still registering the enemy: keep looking where we were going (see the reaction block above)
        AimAtAimNode();
    } else if (bCanSee || level.inttime < m_iAttackStopAimTime) {
        Vector        vRandomOffset;
        Vector        vTarget;
        orientation_t eyes_or;

        if (m_iEnemyEyesTag == -1) {
            // Cache the tag
            m_iEnemyEyesTag = gi.Tag_NumForName(m_pEnemy->edict->tiki, "eyes bone");
        }

        if (m_iEnemyEyesTag != -1) {
            // Use the enemy's eyes bone
            m_pEnemy->GetTag(m_iEnemyEyesTag, &eyes_or);

            //vRandomOffset = Vector(G_CRandom(8), G_CRandom(8), -G_Random(32));
            vTarget = eyes_or.origin;
        } else {
            //vRandomOffset = Vector(G_CRandom(8), G_CRandom(8), 16 + G_Random(m_pEnemy->viewheight - 16));
            vTarget = m_pEnemy->origin;
        }

        // [HZM Phase 5b] aim CONVERGENCE: on first locking a new enemy the horizontal aim is loose (early
        // misses read as suppression) and tightens to a residual-jitter floor - this removes the "instant
        // lock-on" aimbot tell. bot_combat_realism 0 = the stock flat spread.
        // [bug-2791 -> 2026-09-23] The residual spread was bot_aimFloor 0.6 of the enemy half-width: the aim point
        // then NEVER left the body, so a settled bot hit nearly every shot ("accuracy is still a bit too good ...
        // they should miss more often" - user). Now, all tunable and NOT archived (bot_aimFloor "0.6" froze in the
        // user's omconfig.cfg, so re-tuning its default never reached them - it is no longer read):
        //   bot_aimSpread   settled spread, x enemy half-width (1.0 = the aim point roams the full body width)
        //   bot_aimAngErr   extra ANGULAR error in degrees, so world-space misses grow with range like a person's
        //   bot_aimConvMs   how long the post-reaction aim takes to tighten from 2.2x to the settled spread
        //   bot_aimHoldMs   how long each aim point is held before re-rolling (the view settles ONTO a miss
        //                   instead of the slower combat turn averaging a fast 100ms jitter back onto centre)
        static cvar_t *s_botAimSpread = NULL;
        static cvar_t *s_botAimAngErr = NULL;
        static cvar_t *s_botAimConvMs = NULL;
        static cvar_t *s_botAimHoldMs = NULL;
        if (!s_botAimSpread) {
            s_botAimSpread = gi.Cvar_Get("bot_aimSpread", "1.0", 0);
            s_botAimAngErr = gi.Cvar_Get("bot_aimAngErr", "1.1", 0);
            s_botAimConvMs = gi.Cvar_Get("bot_aimConvMs", "1800", 0);
            s_botAimHoldMs = gi.Cvar_Get("bot_aimHoldMs", "200", 0);
        }
        const float fFloor  = Q_clamp_float(s_botAimSpread->value, 0.05f, 2.2f);
        const float fConvMs = Q_max(100.0f, s_botAimConvMs->value);
        const int   iHoldMs = (int)Q_clamp_float(s_botAimHoldMs->value, 50.0f, 1000.0f);
        float       fAimConv = 1.0f;
        float       fAngErr  = 0.0f;
        if (s_botCombat->integer) {
            // the ramp starts when the reaction ends, i.e. when the bot actually begins to swing onto the target
            fAimConv = 2.2f - (float)(level.inttime - Q_max(m_iEnemyLockTime, m_iReactUntil)) / fConvMs * (2.2f - fFloor);
            fAimConv = Q_clamp_float(fAimConv, fFloor, 2.2f) * m_fSkillAim; // [HZM bot D1] steadier / shakier hands
            fAngErr  = sqrt(fDistanceSquared) * tan(DEG2RAD(Q_clamp_float(s_botAimAngErr->value, 0.0f, 6.0f)));
            rotation.SetCombatTurn(true);
        }

        if (level.inttime >= m_iLastAimTime + (s_botCombat->integer ? iHoldMs : 100)) {
            if (m_iEnemyEyesTag != -1) {
                m_vAimOffset[0] = G_CRandom((m_pEnemy->maxs.x - m_pEnemy->mins.x) * 0.5) * fAimConv + G_CRandom(fAngErr);
                m_vAimOffset[1] = G_CRandom((m_pEnemy->maxs.y - m_pEnemy->mins.y) * 0.5) * fAimConv + G_CRandom(fAngErr);
                m_vAimOffset[2] = -G_Random(m_pEnemy->maxs.z * 0.5) + G_CRandom(fAngErr * 0.6f);
            } else {
                m_vAimOffset[0] = G_CRandom((m_pEnemy->maxs.x - m_pEnemy->mins.x) * 0.5) * fAimConv + G_CRandom(fAngErr);
                m_vAimOffset[1] = G_CRandom((m_pEnemy->maxs.y - m_pEnemy->mins.y) * 0.5) * fAimConv + G_CRandom(fAngErr);
                m_vAimOffset[2] = 16 + G_Random(m_pEnemy->viewheight - 16) + G_CRandom(fAngErr * 0.6f);
            }
            m_iLastAimTime = level.inttime;
        }

        rotation.AimAt(vTarget + m_vAimOffset);
    } else {
        AimAtPath(); // [T2] corner checks in front of the route aim
    }

    // [user 2026-09-25] bandaging: aim / fire above still ran; no cover, retreat, bound or chase starts (HealThink aborts
    // on a sighting next frame and side-steps out of the heal pose)
    if (m_iHealState >= 1 && m_iHealState <= 3) {
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    // [HZM bot room-clear step 2, vet C10] getting out of a blast (grenade cover / crouch) owns the feet: every combat move
    // below starts with CommitMove(0) (hurt / reload cover, take cover, retreat, bound, the chase) and replaced the flee -
    // TryBound even aimed at m_vLastEnemyPos, the lob's own target. Aim / fire above already ran.
    if (MovementLocked()) {
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    // [HZM bug-2866] Already ON an elevator link (boarding / in the cab): keep it. The user's test showed allies reach
    // m4l2's lift top landing only under fire from axis riding up, and combat's MoveTo(enemy)/cover moves replaced
    // the path and threw the ride away - so fight from where we are (aim/fire above already ran) and let the lift
    // carry us; the link's own 45s give-up still applies.
    if (movement.IsOnElevatorLink()) {
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    // [HZM bot cover] in a cover session (hide / peek / lean) or bounding to the next spot: it owns the stance and the
    // feet; the aim / fire above already ran from wherever the eye is this frame
    if (CoverFight(bCanSee, pWeap)) {
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    // [HZM Phase 1] COVER + FALL BACK, layered on the stock advance logic below. The bot keeps aiming/firing
    // (handled above) while it repositions. Escalates by health: healthy -> fight (fall through); hurt +
    // under fire + exposed -> peek from cover; critical -> break contact. It ONLY ever moves to a cover spot
    // that FindCoverPosition validated as reachable (a clear straight-line trace from the bot), so it must
    // not send a bot into a wall; the raw "run directly away" fallback was removed because that point can be
    // off the navmesh. Gated on bot_botcover (default on) so it can be toggled off live for A/B testing.
    static cvar_t *bot_botcover = NULL;
    if (!bot_botcover) {
        bot_botcover = gi.Cvar_Get("bot_botcover", "1", CVAR_ARCHIVE);
    }
    if (bot_botcover->integer) {
        float fHealthFrac = 1.0f;
        if (controlledEnt->max_health > 0) {
            fHealthFrac = (float)controlledEnt->health / (float)controlledEnt->max_health;
        }
        const bool bRecentlyShot = (m_iLastPainTime != 0 && level.inttime < m_iLastPainTime + 2500);

        // [HZM 2026-09-23] COMMIT to the chosen spot. FindCoverPosition samples a ring 200u around the bot's
        // CURRENT origin, and the critical (<=35%) branch re-ran it EVERY frame with no commitment - so the spot
        // moved with the bot (a carrot it could never reach), flipped side as the ring moved, and MoveTo ran a full
        // FindPath + NewMove per frame. The bot jittered in place; the 1s block check then read <64u progress as
        // BLOCKED and backed it off. That is the "bots get stuck on the blood they drop" (user): the same bots are
        // wounded, so they were bleeding where they thrashed. Soak (8 runs, 125k moving samples): hp<30 moved at
        // 37u/s and read blocked 68% vs 112u/s / 49% for healthy bots. Now: pick one spot, go, hold it briefly.
        if (m_iCoverUntil) {
            if (level.inttime < m_iCoverUntil) {
                const bool bArrived = (controlledEnt->origin - m_vCoverPos).lengthXYSquared() <= Square(40);
                if (bArrived) {
                    CheckReload(); // [HZM bot C3] in cover: top the magazine up
                    // [HZM bot cover] and fight from it rather than just crouching there for 2.5s
                    if (StartCoverSession(m_vLastEnemyPos, "hurt")) {
                        m_iCoverUntil = 0;
                        m_iAttackTime = level.inttime + 1000;
                        return;
                    }
                }
                if (bArrived || movement.IsMoving()) {
                    m_iAttackTime = level.inttime + 1000;
                    return; // travelling to, or holding, the committed spot (still aiming/firing - handled above)
                }
                // path failed, was cleared by block recovery, or MoveTo was suppressed by a stuck-escape commit:
                // drop the commitment and back off the search so it can't re-fire every frame
                m_iCoverRetryTime = level.inttime + 500;
            }
            m_iCoverUntil = 0;
        }

        // Move to cover only when a VALIDATED cover spot exists. Critical health tries hard while under threat;
        // a hurt+exposed bot peeks. No cover -> keep the stock fight logic, and don't re-trace for 0.5s.
        const bool bWantCover = (fHealthFrac <= 0.35f && (bCanSee || bRecentlyShot))
                             || (fHealthFrac < 0.6f && bRecentlyShot && bCanSee && !movement.IsMoving());
        if (bWantCover && level.inttime >= m_iCoverRetryTime) {
            Vector vCover;
            if (FindCoverPosition(m_vLastEnemyPos, vCover)) {
                m_vCoverPos   = vCover;
                m_iCoverUntil = level.inttime + 2500; // 200u at a limp is ~1.5-2s, then a short hold in cover
                movement.SetWhy("hurtcover"); // [HZM bot probe2]
                movement.MoveTo(vCover);
                m_iAttackTime = level.inttime + 1000;
                return;
            }
            m_iCoverRetryTime = level.inttime + 500;
        }
    }

    // [HZM bot cover] a live fight: get into cover to fight it (or fight from here, if this is cover already)
    if (TryTakeCover(bCanSee)) {
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    {
        static cvar_t *s_botRetreat     = NULL;
        static cvar_t *s_botRoles       = NULL;
        static cvar_t *s_botCoverReload = NULL;
        if (!s_botRetreat) {
            s_botRetreat     = gi.Cvar_Get("bot_retreat", "1", 0);
            s_botRoles       = gi.Cvar_Get("bot_roles", "1", 0);
            s_botCoverReload = gi.Cvar_Get("bot_coverReload", "1", 0);
        }
        // [HZM bot C3] badly hurt and still being shot at: FALL BACK (the brave keep fighting). The aim/fire above keeps
        // running, so the bot backs off shooting rather than turning its back.
        if (s_botRetreat->integer) {
            if (level.inttime < m_iRetreatUntil) {
                m_iAttackTime = level.inttime + 1000;
                return;
            }
            const float fHp = controlledEnt->max_health > 0 ? controlledEnt->health / controlledEnt->max_health : 1.0f;
            if (fHp < 0.35f && bCanSee && level.inttime >= m_iRetreatNext && G_Random(1.0f) > m_fSkillAggro * 0.7f) {
                Vector away = controlledEnt->origin - m_vLastEnemyPos;
                away.z      = 0;
                if (away.lengthSquared() > 1.0f) {
                    VectorNormalize2D(away);
                    movement.CommitMove(0);
                    movement.SetWhy("retreat"); // [HZM bot probe2]
                    movement.AvoidPath(m_vLastEnemyPos, 700.0f, away * 512.0f);
                    movement.CommitMove(2500);
                    m_iRetreatUntil = level.inttime + 3000;
                    m_iRetreatNext  = level.inttime + 12000;
                    VoiceCallout((rand() & 1) ? "*35" : "*22", 8000); // "Taking fire! Need some help!" / "Fall back!"
                    BotEv(controlledEnt, "retreat", controlledEnt->origin);
                    m_iAttackTime = level.inttime + 1000;
                    return;
                }
            }
        }
        // [HZM bot C2] weapon ROLES: a scoped rifle or an MG holds and fires from range, crouched, instead of running at
        // the target; cautious riflemen hold at long range too; holders keep their line. SMGs/pistols press in (stock).
        if (s_botRoles->integer && bCanSee && pWeap) {
            const int   wc      = pWeap->GetWeaponClass();
            const bool  bScoped = pWeap->GetZoom() > 0;
            const float fDist   = sqrt(fDistanceSquared);
            bool        bHold   = false;
            // [user 2026-09-25] "snipers will stay back but they should really only do that if they have a target that is
            // visible at a long distance" - 450u is room-to-room range. The block is already gated on bCanSee.
            if (bScoped && fDist > 1000.0f) {
                bHold = true;
            } else if ((wc & WEAPON_CLASS_MG) && fDist > 550.0f) {
                bHold = true;
            } else if ((wc & WEAPON_CLASS_RIFLE) && fDist > 900.0f && m_fSkillAggro < 0.35f) {
                bHold = true;
            } else if (m_bHolding && fDist > 300.0f) {
                bHold = true;
            }
            if (bHold) {
                // [HZM bot cover] holding here: if there is cover right at this spot, fight from it (hide / peek)
                if (StartCoverSession(m_vLastEnemyPos, "hold")) {
                    m_iAttackTime = level.inttime + 1000;
                    return;
                }
                movement.SetWhy("rolehold"); // [HZM bot probe2]
                movement.ClearMove();
                m_bWantCrouch = bScoped || (wc & WEAPON_CLASS_MG) || m_bHolding || m_fSkillAggro < 0.3f;
                m_iAttackTime = level.inttime + 1000;
                return;
            }
        }
        // [HZM bot C3] magazine nearly dry mid-fight: duck into cover to reload ("Cover me!")
        if (s_botCoverReload->integer && bCanSee && pWeap && !m_iCoverUntil && level.inttime >= m_iCoverRetryTime) {
            const int iClip = pWeap->GetClipSize(FIRE_PRIMARY);
            if (iClip > 1 && pWeap->ClipAmmo(FIRE_PRIMARY) <= iClip / 5) {
                Vector vCover;
                if (FindCoverPosition(m_vLastEnemyPos, vCover)) {
                    m_vCoverPos   = vCover;
                    m_iCoverUntil = level.inttime + 2500;
                    movement.SetWhy("reloadcover"); // [HZM bot probe2]
                    movement.MoveTo(vCover);
                    VoiceCallout("*31", 9000); // "Cover me!"
                    m_iAttackTime = level.inttime + 1000;
                    return;
                }
                m_iCoverRetryTime = level.inttime + 500;
            }
        }
    }

    if (bNoMove) {
        return;
    }

    // [HZM bot cover] advancing on a known enemy: bound from cover to cover instead of walking straight at them
    if (TryBound(bCanSee)) {
        m_iAttackTime = level.inttime + 1000;
        return;
    }

    fEnemyDistanceSquared = (controlledEnt->origin - m_vLastEnemyPos).lengthSquared();

    movement.SetWhy("attackobj"); // [HZM bot probe2]
    // [HZM bug-2959] a holder's "objective" in the attack state is its line (see State_Curious); a chase still wins below
    const bool bHoldLine = gi.Cvar_Get("bot_holdFrontSmooth", "0", 0)->integer >= 2 && m_vOldEnemyPos == m_vLastEnemyPos
                        && fEnemyDistanceSquared >= fMinDistanceSquared && IdleHoldFront();
    if (bHoldLine) {
        if (movement.IsMoving() || m_bHolding) {
            m_iAttackTime = level.inttime + 1000;
        }
        return;
    }
    if ((!movement.MoveToBestAttractivePoint(5) && !movement.IsMoving())
        || (m_vOldEnemyPos != m_vLastEnemyPos && !movement.MoveDone()) || fEnemyDistanceSquared < fMinDistanceSquared) {
        if (!bMelee || !bCanSee) {
            if (fEnemyDistanceSquared < fMinDistanceSquared) {
                Vector vDir = controlledEnt->origin - m_vLastEnemyPos;
                VectorNormalizeFast(vDir);

                movement.SetWhy("backoff"); // [HZM bot probe2]
                movement.AvoidPath(m_vLastEnemyPos, fMinDistance, Vector(controlledEnt->orientation[1]) * 512);
            } else {
                movement.SetWhy("chase"); // [HZM bot probe2]
                movement.MoveTo(m_vLastEnemyPos);
                // [HZM bug-2849] enemy on ground we cannot reach (seen through a closed gate's bars, up on an
                // unreachable ledge): don't march into the partial path's dead end - hold here and shoot if we can see
                // them; if we can't, the MoveDone check below drops the enemy.
                if (!movement.PathReaches(m_vLastEnemyPos, 256.0f)) {
                    movement.SetWhy("chaseunreach"); // [HZM bot probe2]
                    movement.ClearMove();
                }
            }

            if (!bCanSee && movement.MoveDone()) {
                // Lost track of the enemy
                ClearEnemy();
                return;
            }
        } else {
            movement.SetWhy("melee"); // [HZM bot probe2]
            movement.MoveTo(m_vLastEnemyPos);
            if (!movement.PathReaches(m_vLastEnemyPos, 256.0f)) {
                movement.SetWhy("chaseunreach"); // [HZM bot probe2]
                movement.ClearMove(); // [HZM bug-2849] same, melee range
            }
        }
    }

    if (movement.IsMoving()) {
        m_iAttackTime = level.inttime + 1000;
    }
}

/*
====================
Grenade state

Avoid any grenades
====================
*/
void BotController::InitState_Grenade(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Grenade;
    func->ThinkState     = &BotController::State_Grenade;
}

bool BotController::CheckCondition_Grenade(void)
{
    if (m_iNadeState) {
        return true;
    }
    if (m_iHealState) {
        return false; // [user 2026-09-25] never switch to a grenade mid-bandage
    }
    return CheckNadeLob() || CheckBreach();
}

bool BotController::CheckNadeLob(void)
{
    if (BotNadeSafe()) {
        return CheckNadeLobSafe(); // [HZM bot room-clear step 2]
    }
    // [HZM bot B4] throw a frag at an enemy who just ducked out of sight behind cover, at a lobbable range
    static cvar_t *s_botNadeThrow = NULL;
    if (!s_botNadeThrow) {
        s_botNadeThrow = gi.Cvar_Get("bot_nadeThrow", "1", 0);
    }
    if (!s_botNadeThrow->integer || level.inttime < m_iNadeNext) {
        return false;
    }
    m_iNadeNext = level.inttime + 1000; // weigh it up once a second
    if (!m_pEnemy || !IsValidEnemy(m_pEnemy) || movement.IsOnLadder() || movement.IsOnElevatorLink()) {
        return false;
    }
    const int iUnseen = level.inttime - m_iLastSeenTime;
    if (!m_iLastSeenTime || iUnseen < 700 || iUnseen > 4500) {
        return false;
    }
    const float fDist = (m_vLastEnemyPos - controlledEnt->origin).length();
    // [HZM bot room-clear step 1] bot_nadeDrill 1 (dev, soak only): every lob chance taken and a short cooldown, so a soak
    // yields hundreds of throws to check BotNadeSim / BOTNADEDET against. 0 (default) = the stock odds.
    static cvar_t *s_botNadeDrill = NULL;
    if (!s_botNadeDrill) {
        s_botNadeDrill = gi.Cvar_Get("bot_nadeDrill", "0", 0);
    }
    if (fDist < 380.0f || fDist > 1300.0f
        || (!s_botNadeDrill->integer && G_Random(1.0f) > 0.25f + 0.35f * m_fSkillAggro)) {
        return false;
    }
    Weapon *w = NULL;
    if (!FindFragGrenade(w)) {
        return false;
    }
    if (BotMateNearBlast(controlledEnt, m_vLastEnemyPos)) {
        BotEv(controlledEnt, "nademate", m_vLastEnemyPos); // [user 2026-09-24] never lob where a team-mate is
        return false;
    }
    m_pNadeWeapon = w;
    m_vNadeTarget = m_vLastEnemyPos;
    m_iNadeMode   = 0;
    m_iNadeState  = 1;
    m_iNadeClobbered = 0;
    m_iNadeStart  = level.inttime;
    m_iNadeTime   = level.inttime;
    controlledEnt->useWeapon(w, WEAPON_MAIN);
    BotEv(controlledEnt, "nadethrow", m_vNadeTarget);
    return true;
}

void BotController::State_BeginGrenade(void) {}

void BotController::State_EndGrenade(void) {}

void BotController::State_Grenade(void)
{
    if (BotNadeSafe()) {
        State_GrenadeSafe(); // [HZM bot room-clear step 2]
        return;
    }
    Weapon *w = m_pNadeWeapon;
    if (m_iNadeMode == 1 && m_iNadeStart && w && !controlledEnt->IsDead() && level.inttime - m_iNadeStart > 6000) {
        // [HZM bot probe2] the 6s bail: say which phase it stuck in (1 switch/aim, 2 cook, 3 release)
        char ev[40];
        Com_sprintf(ev, sizeof(ev), "breachabort_timeout%d", m_iNadeState);
        BotEv(controlledEnt, ev, m_vNadeTarget);
    }
    if (!w || controlledEnt->IsDead() || level.inttime - m_iNadeStart > 6000) {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        m_iNadeState = 0;
        m_iNadeNext  = level.inttime
                    + (gi.Cvar_Get("bot_nadeDrill", "0", 0)->integer ? 3000 : 15000 + (int)G_Random(12000.0f));
        if (m_iNadeMode == 1 && movement.GetHoldUntil() > level.inttime + 400) {
            movement.HoldFor(0); // an aborted clearing throw: walk on
        }
        m_iNadeMode = 0;
        UseWeaponWithAmmo(); // back to the gun (FindWeaponWithAmmo skips throwables)
        return;
    }
    // a lob at the spot they vanished: pitch up with range
    const Vector dir  = m_vNadeTarget - controlledEnt->origin;
    const float  fD   = dir.length();
    Vector       vAng = dir.toAngles();
    vAng.x            = -Q_clamp_float(8.0f + fD / 90.0f, 8.0f, 30.0f);
    if (m_iNadeMode == 1) {
        vAng = m_vBreachAng; // [HZM bot breach] the solved arc through the doorway / hatch
        rotation.SetPrecise(true);
    }
    rotation.SetCombatTurn(false);
    rotation.SetTargetAngles(vAng);
    m_vNadeAim = vAng; // [HZM bot room-clear step 1] the clobber probe compares against this

    if (m_iNadeMode == 1 && m_iNadeState == 1 && m_pEnemy && level.inttime - m_iLastSeenTime < 300) {
        m_iNadeStart = 0; // an enemy showed up while we were getting the grenade out: fight instead
        BotEv(controlledEnt, "breachabort_enemy", m_vNadeTarget);
        return;
    }
    if (m_iNadeMode == 1 && m_iNadeState <= 2 && BotMateNearBlast(controlledEnt, m_vBreachLand)) {
        m_iNadeStart = 0; // a team-mate walked in meanwhile
        BotEv(controlledEnt, "breachabort_mate", m_vBreachLand);
        return;
    }

    switch (m_iNadeState) {
    case 1: // switching to the grenade
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        if (m_iNadeMode == 1 && controlledEnt->GetActiveWeapon(WEAPON_MAIN) == w && w->ReadyToFire(FIRE_PRIMARY, qfalse)) {
            // [HZM bot breach] now standing still with the grenade out: solve the arc again from exactly here (the first
            // solve was made walking), and do not start cooking until the view is on it - a clearing throw that clips
            // the door frame comes straight back at the team
            if (!m_fNadeHold) {
                Vector ang, land;
                float  hold, fuse;
                if (!SolveThrow(w, m_vNadeTarget, m_iBreachKind, m_vBreachGate, ang, hold, land, fuse)) {
                    m_iNadeStart = 0;
                    BotEv(controlledEnt, "breachabort_resolve", m_vNadeTarget);
                    break;
                }
                m_vBreachAng  = ang;
                m_vBreachLand = land;
                m_fBreachFuse = fuse;
                m_fNadeHold   = hold;
                m_iNadeTime   = level.inttime; // ready + solved since
            }
            const Vector va  = controlledEnt->GetViewAngles();
            const float  err = Q_max(fabs(AngleSubtract(va.x, m_vBreachAng.x)), fabs(AngleSubtract(va.y, m_vBreachAng.y)));
            static cvar_t *s_dbg = NULL;
            if (!s_dbg) {
                s_dbg = gi.Cvar_Get("bot_breachDebug", "0", 0);
            }
            if (s_dbg->integer && ((level.inttime - m_iNadeTime) % 500) < level.intframetime) {
                gi.Printf(
                    "^~^~^ BREACHAIM e=%d view=(%.1f %.1f) want=(%.1f %.1f) err=%.1f t=%d\n", controlledEnt->entnum, va.x,
                    va.y, m_vBreachAng.x, m_vBreachAng.y, err, level.inttime - m_iNadeTime
                );
            }
            // on the arc (3 deg), or near it (8 deg) after 1.5s of trying - the 6s overall timeout above bounds this
            if (err > 3.0f && !(level.inttime - m_iNadeTime > 1500 && err < 8.0f)) {
                break;
            }
            m_iNadeState = 2;
            m_iNadeTime  = level.inttime;
            break;
        }
        if (controlledEnt->GetActiveWeapon(WEAPON_MAIN) == w && w->ReadyToFire(FIRE_PRIMARY, qfalse)) {
            float fMax = w->GetMaxChargeTime(FIRE_PRIMARY);
            float fMin = w->GetMinChargeTime(FIRE_PRIMARY);
            if (fMax <= 0.0f) {
                fMax = 1.5f;
            }
            const float fFrac = Q_clamp_float((fD - 250.0f) / 1050.0f, 0.15f, 1.0f); // farther = harder throw
            m_fNadeHold       = Q_max(fMin + 0.05f, fFrac * fMax);
            m_iNadeState      = 2;
            m_iNadeTime       = level.inttime;
        } else if (level.inttime - m_iNadeTime > 2500) {
            m_iNadeStart = 0; // switch never happened: bail out next frame
            BotEv(controlledEnt, m_iNadeMode == 1 ? "breachabort_switch" : "nadeabort_switch", m_vNadeTarget);
        }
        break;
    case 2: // cooking: hold the trigger for the charge that carries it about fD
        m_botCmd.buttons |= BUTTON_ATTACKLEFT;
        if (level.inttime - m_iNadeTime >= (int)(m_fNadeHold * 1000.0f)) {
            m_iNadeState = 3;
            m_iNadeTime  = level.inttime;
            if (m_iNadeMode == 0) {
                BotEv(controlledEnt, "nadeloose", m_vNadeTarget); // [HZM bot B4] the lob actually left the hand
            }
            if (m_iNadeMode == 1) {
                // [HZM bot breach] it is away: tell the side (team-mates in its sight take cover, the rest hold back until
                // it has gone off), shout it, and step out of the doorway ourselves (CheckFriendlyBlast, next frame)
                const int iGoesOff = level.inttime + 350 + (int)(m_fBreachFuse * 1000.0f) + 400; // throw anim + fuse + margin
                BotMarkAdd(
                    s_botBlasts, 16, s_botBlastSeq, m_vBreachLand, controlledEnt->GetTeam(),
                    Q_min(iGoesOff, level.inttime + 7000)
                );
                movement.HoldFor(300);
                m_iBreachGoAt = Q_min(iGoesOff, level.inttime + 7000);
                VoiceCallout("*45", 1500); // "Grenade! Take cover!"
                BotEv(controlledEnt, "breachthrow", m_vBreachLand);
            }
        }
        break;
    case 3: // released: let the throw animation play, then back to the gun
    default:
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        if (level.inttime - m_iNadeTime > 1100) {
            m_iNadeStart = 0;
        }
        break;
    }
}

/*
====================
Weapon state

Change weapon when necessary
====================
*/
void BotController::InitState_Weapon(botfunc_t *func)
{
    func->CheckCondition = &BotController::CheckCondition_Weapon;
    func->BeginState     = &BotController::State_BeginWeapon;
}

bool BotController::CheckCondition_Weapon(void)
{
    return controlledEnt->GetActiveWeapon(WEAPON_MAIN)
        != controlledEnt->BestWeapon(NULL, false, WEAPON_CLASS_THROWABLE);
}

void BotController::State_BeginWeapon(void)
{
    Weapon *weap = controlledEnt->BestWeapon(NULL, false, WEAPON_CLASS_THROWABLE);

    if (weap == NULL) {
        SendCommand("safeholster 1");
        return;
    }

    SendCommand(va("use \"%s\"", weap->model.c_str()));
}

Weapon *BotController::FindWeaponWithAmmo()
{
    Weapon               *next;
    int                   n;
    int                   j;
    int                   bestrank;
    Weapon               *bestweapon;
    const Container<int>& inventory = controlledEnt->getInventory();

    n = inventory.NumObjects();

    // Search until we find the best weapon with ammo
    bestweapon = NULL;
    bestrank   = -999999;

    for (j = 1; j <= n; j++) {
        next = (Weapon *)G_GetEntity(inventory.ObjectAt(j));

        assert(next);
        if (!next || !next->IsSubclassOfWeapon() || next->IsSubclassOfInventoryItem()) {
            continue;
        }

        if (next->GetWeaponClass() & WEAPON_CLASS_THROWABLE) {
            continue;
        }

        if (next->GetRank() < bestrank) {
            continue;
        }

        if (!next->HasAmmo(FIRE_PRIMARY)) {
            continue;
        }

        bestweapon = (Weapon *)next;
        bestrank   = bestweapon->GetRank();
    }

    return bestweapon;
}

Weapon *BotController::FindMeleeWeapon()
{
    Weapon               *next;
    int                   n;
    int                   j;
    int                   bestrank;
    Weapon               *bestweapon;
    const Container<int>& inventory = controlledEnt->getInventory();

    n = inventory.NumObjects();

    // Search until we find the best weapon with ammo
    bestweapon = NULL;
    bestrank   = -999999;

    for (j = 1; j <= n; j++) {
        next = (Weapon *)G_GetEntity(inventory.ObjectAt(j));

        assert(next);
        if (!next || !next->IsSubclassOfWeapon() || next->IsSubclassOfInventoryItem()) {
            continue;
        }

        if (next->GetRank() < bestrank) {
            continue;
        }

        if (next->GetFireType(FIRE_SECONDARY) != FT_MELEE) {
            continue;
        }

        bestweapon = (Weapon *)next;
        bestrank   = bestweapon->GetRank();
    }

    return bestweapon;
}

void BotController::UseWeaponWithAmmo()
{
    Weapon *bestWeapon = FindWeaponWithAmmo();
    if (!bestWeapon) {
        //
        // If there is no weapon with ammo, fallback to a weapon that can melee
        //
        bestWeapon = FindMeleeWeapon();
    }

    if (!bestWeapon || bestWeapon == controlledEnt->GetActiveWeapon(WEAPON_MAIN)) {
        return;
    }

    controlledEnt->useWeapon(bestWeapon, WEAPON_MAIN);
}

void BotController::Spawned(void)
{
    ClearEnemy();
    m_iCuriousTime   = 0;
    m_botCmd.buttons = 0;
    // [HZM bot B-D] per-life tactics start fresh
    m_iLaneState        = 0;
    m_bHolding          = false;
    m_vHoldPos          = vec_zero;
    m_iHoldCheck        = 0;
    m_iNadeState        = 0;
    m_iNadeMode         = 0;
    m_iNadeClobbered    = 0;
    m_iRetreatUntil     = 0;
    m_iInvestigateUntil = 0;
    m_iAlertUntil       = 0;
    m_vHoldCover        = vec_zero;
    m_iHoldCoverTry     = 0;
    m_iHoldZone         = 0;
    m_bBreachWant       = false;
    m_iBreachKeyTime    = 0;
    m_iBreachGoAt       = 0;
    m_iCfState          = 0;
    m_iHealState        = 0; // [user 2026-09-25] a new life: a new bandage, a fresh heal state (dies with the bot)
    m_iHealNext         = 0;
    m_iHealFails        = 0;
    m_bHealOut          = false;
    m_iHealStepUntil    = 0;
    m_iHealDmgT         = 0;
    m_iHealSlowSince    = 0;
    m_fHealLastHp       = controlledEnt ? controlledEnt->health : 0.0f;
    m_iCfSearchNext     = 0;
    m_iCfNoSession      = 0;
    m_iAtObjSince       = 0;
    m_iHuntUntil        = 0;
    // [HZM bot room-clear step 2] every grenade state machine dies with its bot (TRAPS T10: the lift protocol did not)
    m_solve.state       = 0;
    m_iNadeButtons      = 0;
    m_iNadeHoldUntil    = 0;
    m_iNadeSpawnId      = 0;
    m_iBreachRetry      = 0;
    m_iBlastCoverUntil  = 0;
    m_iNadeCrouchUntil  = 0;
    if (BotNadeSafe()) {
        m_iBlastCoverId     = 0;
        m_iBlastWaitId      = 0;
        m_iGrenadeFleeUntil = 0;
        m_fNadeHold         = 0;
    }
    // [T2 corner checks] the opening being crossed / the corner being pied belong to the last life
    m_open.state = 0;
    m_iPieT      = 0;
    if (m_iCrossT) {
        CrossEnd("spawn");
    }
    if (m_pBuddy) {
        BuddySplit("spawn"); // [T3] a pair dies with its bot
    }
    if (m_iOpId) {
        OpLeave("spawn"); // [room-clear] and so does a stack
    }
    m_iDefendUntil = 0;
    m_bSawLast     = false;
    m_bB2B       = false;
    m_iBuddyNext = level.inttime + 5000;
    movement.HoldFor(0);
    // [HZM bug-2912] a bot that died queuing for / riding the lift respawned still IN the protocol (LiftThink does not
    // run while dead) and was steered in a straight line at the lift from across the map (lift1 soak: 6 min into a wall)
    movement.LiftAbort("spawn");

    // [HZM bug-2867] exact spawn position for the heat/soak tools (bot_probe on). The probe's al 0->1 edge is
    // only a guess - it also fires on a team switch, and at a 500ms interval it can land after the bot has moved.
    static cvar_t *s_botProbe = NULL;
    if (!s_botProbe) {
        s_botProbe = gi.Cvar_Get("bot_probe", "0", 0);
    }
    if (s_botProbe->integer && controlledEnt) {
        teamtype_t team = controlledEnt->GetTeam();
        gi.Printf(
            "^~^~^ BOTSPAWN e=%d tm=%c px=%.0f py=%.0f pz=%.0f\n",
            controlledEnt->entnum,
            (team == TEAM_ALLIES) ? 'a' : ((team == TEAM_AXIS) ? 'x' : '?'),
            controlledEnt->origin.x,
            controlledEnt->origin.y,
            controlledEnt->origin.z
        );
    }
}

void BotController::Think()
{
    usercmd_t  ucmd;
    usereyes_t eyeinfo;

    UpdateBotStates();
    GetUsercmd(&ucmd);
    GetEyeInfo(&eyeinfo);

    G_ClientThink(controlledEnt->edict, &ucmd, &eyeinfo);

    ProbeThink(ucmd); // [HZM bot probe] behavioural telemetry AFTER the move is applied this frame
}

// [HZM bot probe] Per-bot behavioural telemetry for the Push bot study. Silent unless bot_probe >= 1; the
// value doubles as the per-bot log interval in ms (>=2), default 250. One compact, machine-parseable line per
// bot per interval, logged AFTER G_ClientThink so origin/velocity reflect this frame's applied move:
//   spd   = horizontal speed (jump z excluded) - near 0 with fm!=0 is "running in place"
//   fm/rm/um = this frame's movement INTENT (forward/right/up); um>0 or jmp=1 is a jump (stuck tell)
//   blk   = cumulative block count (walking into geometry); jmp = movement wants to jump; pth = following a path
//   gd    = distance to the current move goal; en/ed = enemy entnum/distance; seen/pain = ms since last saw
//           an enemy / last took damage (reaction latency). Team a=allies x=axis for the allies-vs-axis study.
void BotController::ProbeThink(const usercmd_t& ucmd)
{
    static cvar_t *s_botProbe = NULL;
    if (!s_botProbe) {
        s_botProbe = gi.Cvar_Get("bot_probe", "0", 0);
    }
    if (!s_botProbe->integer || !controlledEnt) {
        return;
    }

    int interval = s_botProbe->integer > 1 ? s_botProbe->integer : 250;
    if (level.inttime - m_iProbeLastTime < interval) {
        return;
    }
    m_iProbeLastTime = level.inttime;

    teamtype_t team = controlledEnt->GetTeam();
    char       tm    = (team == TEAM_ALLIES) ? 'a' : ((team == TEAM_AXIS) ? 'x' : '?');
    int        alive = controlledEnt->IsDead() ? 0 : 1;
    Vector     org   = controlledEnt->origin;
    Vector     vel   = controlledEnt->velocity;
    Vector     velH(vel.x, vel.y, 0);
    float      spd   = velH.length();

    Vector goal     = movement.GetCurrentGoal();
    float  goalDist = (goal - org).length();

    int   enemyEnt  = -1;
    float enemyDist = -1.0f;
    if (m_pEnemy) {
        enemyEnt  = m_pEnemy->entnum;
        enemyDist = (m_pEnemy->origin - org).length();
    }

    int seenAgo = m_iLastSeenTime ? (level.inttime - m_iLastSeenTime) : -1;
    int painAgo = m_iLastPainTime ? (level.inttime - m_iLastPainTime) : -1;
    int fire    = (ucmd.buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT)) ? 1 : 0;
    // [HZM bot cover] cov bits: 1 engine low-cover pose, 2 engine wall-cover pose, 4 engine cover peek, 8 leaning,
    // 16 bot cover session hidden, 32 session peeking, 64 bounding to a cover spot (appended last: the heat / soak
    // parsers read the fields before it and ignore unknown keys)
    int cov = 0;
    cov |= controlledEnt->IsCoopCoverLow() ? 1 : 0;
    cov |= controlledEnt->IsCoopCoverWall() ? 2 : 0;
    cov |= controlledEnt->IsCoopCoverPeek() ? 4 : 0;
    cov |= (controlledEnt->client && fabs(controlledEnt->client->ps.fLeanAngle) > 5.0f) ? 8 : 0;
    cov |= m_iCfState == 2 ? 16 : (m_iCfState == 3 ? 32 : (m_iCfState == 1 ? 64 : 0));
    // [HZM bot probe2] od = distance to the objective the bot is attracted to (-1 none): rising while not fighting = going
    // the wrong way; st = state flags (1 attack, 2 curious, 4 grenade, 8 idle); why = the tag of the route being walked
    float  objDist = -1.0f;
    Vector vObj;
    if (movement.GetObjective(vObj)) {
        objDist = (vObj - org).lengthXY();
    }

    {
        // [HZM bot probe2] LIFT telemetry (user 2026-09-24: bots "glitched around and in it, and stuck under it even,
        // which stalls the elevator"): any bot on / approaching an elevator link, standing on a mover, or inside the
        // cab's footprint - where it is relative to the cab (the Push lift's cab is $elevator_cab; other maps: none)
        static int     s_cabT   = -1;
        static int     s_cabNum = -1;
        if (level.inttime < s_cabT || s_cabT < 0 || level.inttime - s_cabT > 10000) {
            Entity *c = G_FindTarget(NULL, "elevator_cab");
            s_cabNum  = c ? c->entnum : -1;
            s_cabT    = level.inttime;
        }
        Entity *cab = (s_cabNum >= 0 && g_entities[s_cabNum].inuse) ? g_entities[s_cabNum].entity : NULL;
        Entity *gnd = controlledEnt->groundentity ? controlledEnt->groundentity->entity : NULL;
        const bool onLink = movement.IsOnElevatorLink();
        const bool appr   = movement.GetApproachArea() == RECAST_AREA_ELEVATOR;
        bool       shaft = false, under = false;
        if (cab) {
            shaft = org.x > cab->absmin.x - 24 && org.x < cab->absmax.x + 24 && org.y > cab->absmin.y - 24
                 && org.y < cab->absmax.y + 24;
            under = shaft && org.z + controlledEnt->maxs.z < cab->absmin.z + 8;
        }
        const bool onMover = gnd && gnd != world;
        if (alive && (onLink || appr || shaft || onMover)) {
            gi.Printf(
                "^~^~^ BOTELEV e=%d tm=%c link=%d appr=%d shaft=%d under=%d onmover=%d z=%.0f cab=%.0f..%.0f gnd=%d:%s spd=%.0f fm=%d rm=%d why=%s\n",
                controlledEnt->entnum, tm, onLink ? 1 : 0, appr ? 1 : 0, shaft ? 1 : 0, under ? 1 : 0, onMover ? 1 : 0, org.z,
                cab ? cab->absmin.z : 0.0f, cab ? cab->absmax.z : 0.0f, gnd ? gnd->entnum : -1,
                gnd ? gnd->TargetName().c_str() : "", spd, (int)ucmd.forwardmove, (int)ucmd.rightmove, movement.GetGoalWhy()
            );
        }
    }

    // [user 2026-09-25] nearest enemy in line of sight (any bearing, 3000u) - the probe's en/ed only show a TARGET
    int   visEnt  = -1;
    float visDist = -1;
    if (alive) {
        for (int i = 0; i < game.maxclients; i++) {
            gentity_t *ge = &g_entities[i];
            if (!ge->inuse || !ge->entity || ge->entity == controlledEnt || !ge->entity->IsSubclassOfPlayer()) {
                continue;
            }
            Player *o = static_cast<Player *>(ge->entity);
            if (o->IsDead() || o->IsSpectator() || o->GetTeam() == controlledEnt->GetTeam()) {
                continue;
            }
            const float d = (o->origin - controlledEnt->origin).length();
            if (d > 3000.0f || (visEnt >= 0 && d >= visDist)) {
                continue;
            }
            if (controlledEnt->CanSee(o, 360, 3000, false)) {
                visEnt  = o->entnum;
                visDist = d;
            }
        }
    }
    const Vector va        = controlledEnt->GetViewAngles();
    const float  viewPitch = (va.x > 180.0f) ? va.x - 360.0f : va.x;

    // [user 2026-09-25] "walk right past each other": when an enemy is in line of sight but is NOT our target, which
    // clause of CheckCondition_Attack rejected it - 0 all pass (timing), 1 IsValidEnemy, 2 range, 3 cone, 4 areas.
    // oy = the BODY yaw FovCheck tests (orientation[0]); yaw = the view yaw.
    int   visWhy = -1;
    float bodyYaw = vectoyaw(Vector(controlledEnt->orientation[0]));
    if (visEnt >= 0 && !m_pEnemy && g_entities[visEnt].entity) {
        Sentient   *o    = static_cast<Sentient *>(g_entities[visEnt].entity);
        const float maxD = VisionDistance();
        vec2_t      dl;
        VectorSub2D(o->centroid, controlledEnt->centroid, dl);
        const float tR  = maxD > 1.0f ? Q_min(1.0f, visDist / maxD) : 0.0f;
        const float fov = gi.Cvar_Get("bot_fov_acquire", "150", CVAR_ARCHIVE)->value
                        + tR * (gi.Cvar_Get("bot_fov_acquire_far", "90", CVAR_ARCHIVE)->value
                                - gi.Cvar_Get("bot_fov_acquire", "150", CVAR_ARCHIVE)->value);
        if (!IsValidEnemy(o)) {
            visWhy = 1;
        } else if (maxD > 0 && Square(maxD) < VectorLength2DSquared(dl)) {
            visWhy = 2;
        } else if (!controlledEnt->FovCheck(dl, cos(DEG2RAD(fov / 2.f)))) {
            visWhy = 3;
        } else if (!controlledEnt->AreasConnected(o)) {
            visWhy = 4;
        } else {
            visWhy = 0;
        }
    }

    gi.Printf(
        "^~^~^ BOTPROBE e=%d tm=%c al=%d hp=%d px=%.0f py=%.0f pz=%.0f spd=%.0f fm=%d rm=%d um=%d fire=%d "
        "blk=%d jmp=%d pth=%d gd=%.0f en=%d ed=%.0f seen=%d pain=%d cov=%d od=%.0f st=%d why=%s duck=%d pitch=%.0f "
        "vis=%d vd=%.0f yaw=%.0f oy=%.0f vwhy=%d lk=%d lc=%d\n",
        controlledEnt->entnum, tm, alive, (int)controlledEnt->health,
        org.x, org.y, org.z, spd,
        (int)ucmd.forwardmove, (int)ucmd.rightmove, (int)ucmd.upmove, fire,
        movement.GetNumBlocks(), movement.IsJumping() ? 1 : 0, movement.IsPathing() ? 1 : 0, goalDist,
        enemyEnt, enemyDist, seenAgo, painAgo, cov, objDist, (int)m_StateFlags, movement.GetGoalWhy(),
        // [user 2026-09-25] measure before fixing: crouched stance (pmove's own flag, not the upmove input - the
        // "walking around crouched" report showed um<0 in only 0.2% of samples), view pitch (idle aim-high), and the
        // nearest enemy in LINE OF SIGHT at any bearing (walking past each other: seen but not engaged, or unseen)
        (controlledEnt->client && (controlledEnt->client->ps.pm_flags & PMF_DUCKED)) ? 1 : 0, viewPitch, visEnt, visDist, va.y, bodyYaw, visWhy,
        // [bug-2986 finding] lk = degrees between the view and the direction of travel (-1 standing), lc = look compensation live
        velH.lengthSquared() > Square(40.0f) ? (int)fabs(AngleSubtract(va.y, velH.toYaw())) : -1, movement.IsLookComp() ? 1 : 0
    );
}

void BotController::Pain(const Event& ev)
{
    Entity   *attacker;
    Sentient *sent;

    // [HZM] React to being shot, even from outside our current view. Acquire the attacker as our enemy and
    // remember where the shot came from, so the bot turns to fight back / moves to the last known position
    // instead of carrying on obliviously. The reaction-time gate in State_Attack still applies via
    // m_iLastUnseenTime, so this reads as "someone hit me - where?" rather than an instant aimbot snap.
    if (!controlledEnt || controlledEnt->IsDead()) {
        return;
    }

    attacker = ev.GetEntity(1);

    // [user 2026-09-25] "when they throw grenades to clear rooms they will sometimes be too close to the door and get hurt
    // or even move in and get hurt" - BOTPAIN below only prints on an enemy switch, so self / team-mate damage was never
    // logged. Measure it first: who, how much, with what, how far from the bot.
    {
        static cvar_t *s_probeFF = NULL;
        if (!s_probeFF) {
            s_probeFF = gi.Cvar_Get("bot_probe", "0", 0);
        }
        if (s_probeFF->integer && attacker && controlledEnt
            && (attacker == controlledEnt
                || (attacker->IsSubclassOfPlayer() && static_cast<Player *>(attacker)->GetTeam() == controlledEnt->GetTeam()))) {
            Entity *inflictor = ev.NumArgs() >= 3 ? ev.GetEntity(3) : NULL;
            gi.Printf(
                "^~^~^ BOTFFHARM e=%d by=%d self=%d dmg=%.0f mod=%d inflictor=%s idist=%.0f at=(%.0f %.0f %.0f) hp=%d nade=%d\n",
                controlledEnt->entnum, attacker->entnum, attacker == controlledEnt ? 1 : 0,
                ev.NumArgs() >= 2 ? ev.GetFloat(2) : -1.0f, ev.NumArgs() >= 9 ? ev.GetInteger(9) : -1,
                inflictor ? inflictor->getClassname() : "-",
                inflictor ? (inflictor->origin - controlledEnt->origin).length() : -1.0f, controlledEnt->origin.x,
                controlledEnt->origin.y, controlledEnt->origin.z, (int)controlledEnt->health, g_iBotNadeDetId
            );
        }
    }

    if (!attacker || !attacker->IsSubclassOfSentient()) {
        return;
    }

    sent = static_cast<Sentient *>(attacker);
    if (!IsValidEnemy(sent)) {
        return;
    }

    // Remember we were just shot (drives cover-seeking in State_Attack), even during an ongoing fight.
    m_iLastPainTime = level.inttime;
    if (m_iCrossT && m_iCrossHurt < 0) {
        m_iCrossHurt = level.inttime - m_iCrossT; // [T2] hit by an enemy within 3s of crossing an opening / a corner
    }

    // [HZM Phase 3b] Switch to whoever just shot us UNLESS our current enemy is both SEEN and at least as
    // close - so a flank shot while we're fighting someone else actually turns us around (the "shot from
    // behind, no reaction" case), while stray third-party splash mid-fight doesn't yank us off a live target.
    // bot_flankreact 0 = stock sticky enemy.
    static cvar_t *s_botFlank = NULL;
    if (!s_botFlank) {
        s_botFlank = gi.Cvar_Get("bot_flankreact", "1", CVAR_ARCHIVE);
    }
    if (m_pEnemy && IsValidEnemy(m_pEnemy)) {
        if (!s_botFlank->integer) {
            return;
        }
        bool  bCurUnseen = (m_iLastUnseenTime != 0);
        float dNew       = (sent->origin - controlledEnt->origin).lengthSquared();
        float dCur       = (m_pEnemy->origin - controlledEnt->origin).lengthSquared();
        if (!bCurUnseen && dCur <= dNew) {
            return; // keep the current enemy only if we can see it and it is at least as close
        }
    }

    m_pEnemy             = sent;
    m_vLastEnemyPos      = sent->origin;
    m_iLastUnseenTime    = level.inttime;
    m_iAttackTime        = level.inttime + 1000;
    m_iAttackStopAimTime = level.inttime + 2000; // keep aiming toward the shooter so we turn to face them

    // [HZM bot probe] reaction-to-being-shot event: who hit us, from how far, and that we turned to fight back.
    {
        static cvar_t *s_botProbeP = NULL;
        if (!s_botProbeP) {
            s_botProbeP = gi.Cvar_Get("bot_probe", "0", 0);
        }
        if (s_botProbeP->integer) {
            gi.Printf(
                "^~^~^ BOTPAIN e=%d by=%d bydist=%.0f\n",
                controlledEnt->entnum, attacker->entnum, (sent->origin - controlledEnt->origin).length()
            );
        }
    }
}

void BotController::Killed(const Event& ev)
{
    Entity *attacker;

    // send the respawn buttons
    if (!(m_botCmd.buttons & BUTTON_ATTACKLEFT)) {
        m_botCmd.buttons |= BUTTON_ATTACKLEFT;
    } else {
        m_botCmd.buttons &= ~BUTTON_ATTACKLEFT;
    }

    m_botEyes.ofs[0]    = 0;
    m_botEyes.ofs[1]    = 0;
    m_botEyes.ofs[2]    = 0;
    m_botEyes.angles[0] = 0;
    m_botEyes.angles[1] = 0;

    attacker = ev.GetEntity(1);

    if (controlledEnt) {
        // [HZM bot breach] the side remembers where it is losing people
        BotMarkAdd(s_botDeaths, 32, s_botDeathSeq, controlledEnt->origin, controlledEnt->GetTeam(), 0);
    }
    if (m_iCrossT) {
        CrossEnd("died"); // [T2] died within 3s of crossing an opening / rounding a corner
    }
    if (m_pBuddy) {
        BuddySplit("died"); // [T3]
    }
    if (m_iOpId) {
        OpLeave("died"); // [room-clear]
    }

    if (attacker && rand() % 5 == 0) {
        // 1/5 chance to go back to the attacker position
        m_vLastDeathPos = attacker->origin;
    } else {
        m_vLastDeathPos = vec_zero;
    }

    // Choose a new random primary weapon
    Event event(EV_Player_PrimaryDMWeapon);
    event.AddString("auto");

    controlledEnt->ProcessEvent(event);

    //
    // This is useful to change nationality in Spearhead and Breakthrough
    // this allows the AI to use more weapons
    //
    Info_SetValueForKey(controlledEnt->client->pers.userinfo, "dm_playermodel", G_GetRandomAlliedPlayerModel());
    Info_SetValueForKey(controlledEnt->client->pers.userinfo, "dm_playergermanmodel", G_GetRandomGermanPlayerModel());

    G_ClientUserinfoChanged(controlledEnt->edict, controlledEnt->client->pers.userinfo);
}

/*
====================
[HZM bot B-D] awareness, tactics and personality helpers
====================
*/
void BotController::RollPersonality(void)
{
    // [HZM bot D1] stable traits seeded from the bot's NAME (bots are rebuilt every map, the name survives), so the same
    // soldier keeps his temperament all session. bot_personality 0 = everyone average.
    static cvar_t *s_botPersonality = NULL;
    if (!s_botPersonality) {
        s_botPersonality = gi.Cvar_Get("bot_personality", "1", 0);
    }
    unsigned int h  = 2166136261u;
    const char  *nm = (controlledEnt && controlledEnt->client) ? controlledEnt->client->pers.netname : "";
    for (const char *p = nm; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    if (!*nm && controlledEnt) {
        h ^= (unsigned int)controlledEnt->entnum * 2654435761u;
    }
    auto rnd = [&h]() {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        return (float)(h & 0xFFFF) / 65535.0f;
    };
    if (!s_botPersonality->integer) {
        m_fSkillReact = m_fSkillAim = m_fSkillTurn = 1.0f;
        m_fSkillAggro = 0.5f;
    } else {
        m_fSkillReact = 0.75f + 0.6f * rnd(); // 0.75x .. 1.35x reaction time
        m_fSkillAim   = 0.8f + 0.5f * rnd();  // 0.8x .. 1.3x aim spread
        m_fSkillAggro = rnd();                // 0 cautious .. 1 aggressive
        m_fSkillTurn  = 0.85f + 0.3f * rnd(); // combat turn speed
    }
    m_bHolder = m_fSkillAggro < 0.2f; // [bot C4] the most cautious ~fifth hold the line (a third made fights too rare)
    rotation.SetTurnMul(m_fSkillTurn);
}

void BotController::VoiceCallout(const char *code, int iCooldownMs)
{
    // [HZM bot D3] team voice callouts through the stock instant-message path (dmmessage *NN - player.cpp
    // pInstantMsgEng: *43 Enemy spotted, *45 Grenade! Take cover!, *46 Area clear, *31 Cover me!, *35 Taking fire! Need
    // some help!, *22 Fall back!, *25 Hold this position!). One line per team per 7s, per bot per iCooldownMs, so the
    // radio stays readable; the engine adds its own g_instamsg_minDelay.
    static cvar_t *s_botVoice = NULL;
    static int     s_teamNext[2][64];
    static int     s_lastTime = 0;
    if (!s_botVoice) {
        s_botVoice = gi.Cvar_Get("bot_voice", "1", 0);
    }
    if (!s_botVoice->integer || !controlledEnt || controlledEnt->IsDead() || controlledEnt->IsSpectator()) {
        return;
    }
    if (level.inttime < s_lastTime) { // a new map: the clock restarted
        memset(s_teamNext, 0, sizeof(s_teamNext));
    }
    s_lastTime = level.inttime;
    if (level.inttime < m_iVoiceNext || !code || code[0] != '*' || !code[1] || !code[2]) {
        return;
    }
    const int team = (controlledEnt->GetTeam() == TEAM_ALLIES) ? 0 : 1;
    const int idx  = ((code[1] - '0') * 10 + (code[2] - '0')) & 63;
    if (level.inttime < s_teamNext[team][idx]) {
        return;
    }
    s_teamNext[team][idx] = level.inttime + 7000;
    m_iVoiceNext          = level.inttime + iCooldownMs;

    Event event("dmmessage");
    event.AddInteger(0);
    event.AddString(code);
    controlledEnt->ProcessEvent(event);
}

bool BotController::FindFragGrenade(Weapon *&out)
{
    const Container<int>& inventory = controlledEnt->getInventory();
    for (int j = 1; j <= inventory.NumObjects(); j++) {
        Weapon *w = (Weapon *)G_GetEntity(inventory.ObjectAt(j));
        if (!w || !w->IsSubclassOfWeapon() || !(w->GetWeaponClass() & WEAPON_CLASS_GRENADE)) {
            continue;
        }
        const char *nm = w->getName().c_str();
        if (Q_stristr(nm, "smoke") || Q_stristr(nm, "nebel") || Q_stristr(nm, "rdg") || Q_stristr(nm, "breda")) {
            continue; // smoke is not a frag
        }
        // [2026-09-25] user: "an axis bot is using a mine detector". The Minensuchgerat / mine detector (and every finish
        // variant of it) and the landmines are weapontype GRENADE too (firetype landmine / defuse, ammotype "landmine"):
        // a real frag is thrown as a projectile. Type check first, the name as a belt.
        if (w->GetFireType(FIRE_PRIMARY) != FT_PROJECTILE || w->GetFireType(FIRE_PRIMARY) == FT_LANDMINE
            || w->GetFireType(FIRE_PRIMARY) == FT_DEFUSE || !w->GetAmmoType(FIRE_PRIMARY).icmp("landmine")) {
            continue;
        }
        const char *mdl = w->model.c_str();
        if (Q_stristr(nm, "mine") || Q_stristr(nm, "minensuch") || Q_stristr(nm, "detector") || Q_stristr(mdl, "mine")
            || Q_stristr(mdl, "detector")) {
            continue;
        }
        if (!w->HasAmmo(FIRE_PRIMARY)) {
            continue;
        }
        out = w;
        return true;
    }
    return false;
}

void BotController::CheckGrenadeThreat(void)
{
    if (BotNadeSafe()) {
        CheckGrenadeThreatSafe(); // [HZM bot room-clear step 2]
        return;
    }
    // [HZM bot B4] a live frag within ~380u with a clear line to us: get away from it ("Grenade! Take cover!")
    static cvar_t *s_botNadeFlee = NULL;
    if (!s_botNadeFlee) {
        s_botNadeFlee = gi.Cvar_Get("bot_nadeFlee", "1", 0);
    }
    if (!s_botNadeFlee->integer || level.inttime < m_iNadeScanTime || controlledEnt->IsDead()) {
        return;
    }
    m_iNadeScanTime = level.inttime + 150;
    if (level.inttime < m_iGrenadeFleeUntil || movement.IsOnLadder() || movement.IsOnElevatorLink()) {
        return;
    }
    for (Entity *e = findradius(NULL, controlledEnt->origin, 380.0f); e; e = findradius(e, controlledEnt->origin, 380.0f)) {
        if (!e->IsSubclassOfProjectile() || e->isSubclassOf(Explosion)) {
            continue;
        }
        const char *mdl = e->model.c_str();
        if (!(Q_stristr(mdl, "grenade") || Q_stristr(mdl, "granate") || Q_stristr(mdl, "masher")
              || Q_stristr(mdl, "mills") || Q_stristr(mdl, "bomba"))
            || Q_stristr(mdl, "smoke") || Q_stristr(mdl, "nebel")) {
            continue;
        }
        Projectile *pr = static_cast<Projectile *>(e);
        if (pr->GetOwner() == controlledEnt && level.time - e->edict->spawntime < 1.0f) {
            continue; // our own throw leaving the hand
        }
        if (!G_SightTrace(
                e->origin + Vector(0, 0, 8), vec_zero, vec_zero, controlledEnt->origin + Vector(0, 0, 24), e,
                controlledEnt, MASK_SOLID, qfalse, "BotNadeLOS"
            )) {
            continue; // a wall between us takes the blast
        }
        Vector away = controlledEnt->origin - e->origin;
        away.z      = 0;
        if (away.lengthSquared() < 1.0f) {
            away = Vector(controlledEnt->orientation[1]);
        }
        VectorNormalize2D(away);
        movement.CommitMove(0);
        movement.SetWhy("nadeflee"); // [HZM bot probe2]
        movement.AvoidPath(e->origin, 450.0f, away * 512.0f);
        movement.CommitMove(1600);
        m_iGrenadeFleeUntil = level.inttime + 1600;
        VoiceCallout("*45", 6000);
        BotEv(controlledEnt, "nadeflee", e->origin);
        return;
    }
}

// HZM-MP-BEGIN(mp_bot_push_front)
// [HZM bot C4] the ONLY place the engine reads the Push script state (mp_push.scr publishes it as level vars). Outside
// Push (coop, DM, TDM) every read below finds no variable and the hold-the-front brain stays off.
static int BotPushLevelInt(const char *which)
{
    if (!level.vars) {
        return 0;
    }
    ScriptVariable *v = level.vars->GetVariable(!Q_stricmp(which, "front") ? "coop_mpPushFront" : "coop_mpPushZN");
    return (v && v->GetType() != VARIABLE_NONE) ? v->intValue() : 0;
}

static bool BotLevelZoneBox(int zone, Vector& mn, Vector& mx)
{
    // the Push zone boxes live only in script: level.coop_mpPushZAmin/ZAmax[1..N] (mp_push_maps / mp_push_bsp seeds)
    if (!level.vars) {
        return false;
    }
    ScriptVariable *n = level.vars->GetVariable("coop_mpPushZN");
    if (!n || n->GetType() == VARIABLE_NONE || zone < 1 || zone > n->intValue()) {
        return false;
    }
    ScriptVariable *amin = level.vars->GetVariable("coop_mpPushZAmin");
    ScriptVariable *amax = level.vars->GetVariable("coop_mpPushZAmax");
    if (!amin || !amax || amin->GetType() != VARIABLE_ARRAY || amax->GetType() != VARIABLE_ARRAY) {
        return false;
    }
    ScriptVariable key;
    key.setIntValue(zone);
    ScriptVariable& vmin = (*amin)[key];
    ScriptVariable& vmax = (*amax)[key];
    if (vmin.GetType() != VARIABLE_VECTOR || vmax.GetType() != VARIABLE_VECTOR) {
        return false;
    }
    mn = vmin.vectorValue();
    mx = vmax.vectorValue();
    return true;
}

static bool BotLevelZoneCentre(int zone, Vector& out)
{
    Vector mn, mx;
    if (!BotLevelZoneBox(zone, mn, mx)) {
        return false;
    }
    out = (mn + mx) * 0.5f;
    return true;
}

// [HZM bug-2959] holdfront v3. A Push zone is a BAND across the map (e1l1: 6700u wide, 2600u deep), and a holder was sent
// to its CENTRE - 3000u sideways for a bot already standing in it - and judged "past the line" by comparing its own
// XY distance to the enemy end with the centre's. On a wide band that test flips along the route: holders walked to the
// centre, tripped "past" half-way (holdpast), attacked for 20s, came back and tripped it again at the same spot (e1l1
// hf2 soak: e=21 at (685 51) / (700 44) / (682 54), each a BOTLOOP holdfront). Now the line is the band itself: hold at
// its nearest point (inset 96u), taken once per line, and "past" = beyond the band's far edge along the attack axis.
// bot_holdFrontLine 0 = the centre / XY test.
static bool BotHoldFrontLineOn(void)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_holdFrontLine", "0", 0);
    }
    return s_on->integer != 0;
}

// [bug-2959 v3.1] bot_holdFrontLine 2: re-anchor a holder that is not holding (see IdleHoldFront)
static bool BotHoldFrontReanchor(void)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_holdFrontLine", "0", 0);
    }
    return s_on->integer >= 2;
}

// the nearest point of zone `zone`'s box to p (XY inset 96u; a band thinner than twice that: its middle), z clamped too
static bool BotZoneNearest(int zone, const Vector& p, Vector& out)
{
    Vector mn, mx;
    if (!BotLevelZoneBox(zone, mn, mx)) {
        return false;
    }
    out = p;
    for (int k = 0; k < 2; k++) {
        const float lo = mn[k] + 96.0f, hi = mx[k] - 96.0f;
        out[k]         = lo > hi ? (mn[k] + mx[k]) * 0.5f : Q_clamp_float(p[k], lo, hi);
    }
    out[2] = Q_clamp_float(p[2], mn[2], mx[2]);
    return true;
}

// how far p is past zone `zone`'s far edge along the attack axis of a side (own end zone -> enemy end zone), in units
// (<= 0: not past). false = the zones are not readable.
static bool BotZonePastBy(int zone, bool bAllies, const Vector& p, float& out)
{
    const int n = BotPushLevelInt("zn");
    Vector    own, enemy, mn, mx;
    if (n < 2 || !BotLevelZoneCentre(bAllies ? 1 : n, own) || !BotLevelZoneCentre(bAllies ? n : 1, enemy)
        || !BotLevelZoneBox(zone, mn, mx)) {
        return false;
    }
    Vector d = enemy - own;
    d.z      = 0;
    if (d.lengthSquared() < 1.0f) {
        return false;
    }
    VectorNormalize2D(d);
    float fFar = -1e9f;
    for (int c = 0; c < 4; c++) {
        const float x = (c & 1) ? mx.x : mn.x, y = (c & 2) ? mx.y : mn.y;
        fFar          = Q_max(fFar, x * d.x + y * d.y);
    }
    out = p.x * d.x + p.y * d.y - fFar;
    return true;
}

// [HZM bug-2959] the Push front as the holders read it. mp_push.scr moves coop_mpPushFront on every tick with who is in
// the band - while both sides contest it, it flips each second (e1l1: F 4/5/6/5/4...) - and IdleHoldFront sampled it every
// 3s, so a holder's point jumped a whole zone (1000-2100u) back and forth and the holder paced between the two (BOTLOOP
// holdfront: 10-15 episodes per 15-min e1l1 soak). Holders now follow a smoothed front (8s average, rounded) that moves
// only once the new zone has held for 6s; a jump of 3+ zones (a round reset) moves it at once. Server-wide: both sides
// read the same front. bot_holdFrontSmooth 0 = the raw per-tick front.
static int BotHoldFrontZone(void)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_holdFrontSmooth", "0", 0);
    }
    const int raw = BotPushLevelInt("front");
    if (!s_on->integer || raw < 1) {
        return raw;
    }
    static float s_ema   = 0.0f;
    static int   s_t     = -1;
    static int   s_zone  = 0;
    static int   s_pend  = 0;
    static int   s_pendT = 0;
    const int    now     = level.inttime;
    if (s_t < 0 || now < s_t || s_zone < 1 || abs(raw - s_zone) >= 3) {
        s_ema   = (float)raw; // first read, a new map, or a round reset
        s_zone  = raw;
        s_pend  = raw;
        s_pendT = now;
        s_t     = now;
        return s_zone;
    }
    if (now - s_t >= 250) {
        const float dt = (now - s_t) / 1000.0f;
        s_t            = now;
        s_ema += (raw - s_ema) * Q_min(1.0f, dt / 8.0f);
        const int cand = (int)floorf(s_ema + 0.5f);
        if (cand == s_zone || cand != s_pend) {
            s_pend  = cand;
            s_pendT = now;
        } else if (now - s_pendT >= 6000) {
            s_zone = cand;
        }
    }
    return s_zone;
}
// HZM-MP-END(mp_bot_push_front)

// [item 5, 2026-09-26] team roles per TEAM. The lane side (entnum % 3) and the holder flag (a name hash: aggro < 0.2) were
// decided per bot, while the teams are made by join order - so one side could get 5 of its 6 bots on side lanes and the
// other 3 (t3l1), and the three holder names on this roster always split 2:1 (e1l1 / m4l2 allies, e3l2 axis). Keyed to a
// bot's rank in its own team instead: lanes by entnum rank (-1, 0, +1 repeating), holders = the team's most cautious
// round(n / 5) (at least 1). bot_teamRoles 1 (default) / 0 = the per-bot keys.
static bool BotTeamRolesOn(void)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_teamRoles", "1", 0);
    }
    return s_on->integer != 0;
}

bool BotController::IdleHoldFront(void)
{
    // [HZM bot C4] HOLDERS (the cautious third of each team) set up on the team's current FRONT zone - allies on zone F
    // (the last one they hold), axis on zone F+1 - facing the enemy end, crouched ("Hold this position!"). The rest
    // keep attacking the enemy end. The teams then meet at the line instead of running past each other. Push only.
    static cvar_t *s_botHold = NULL;
    if (!s_botHold) {
        s_botHold = gi.Cvar_Get("bot_holdFront", "1", 0);
    }
    bool bHolder = m_bHolder;
    if (BotTeamRolesOn() && controlledEnt) {
        // [item 5] the team's most cautious round(n / 5) hold its line (see BotTeamRolesOn)
        int                               n = 0, lower = 0;
        const Container<BotController *>& cl = botManager.getControllerManager().getControllers();
        for (int i = 1; i <= cl.NumObjects(); i++) {
            BotController *o = cl.ObjectAt(i);
            if (!o || !o->controlledEnt || o->controlledEnt->GetTeam() != controlledEnt->GetTeam()) {
                continue;
            }
            n++;
            if (o != this
                && (o->m_fSkillAggro < m_fSkillAggro
                    || (o->m_fSkillAggro == m_fSkillAggro && o->controlledEnt->entnum < controlledEnt->entnum))) {
                lower++;
            }
        }
        bHolder = lower < Q_max(1, (int)floorf(n / 5.0f + 0.5f));
    }
    if (!s_botHold->integer || !bHolder || !level.vars || level.inttime < m_iHoldCooldown) {
        if (m_bHolding && !bHolder) {
            m_bHolding = false; // [item 5] no longer one of the team's holders (the team changed)
            m_vHoldPos = vec_zero;
        }
        return false;
    }
    // a quiet line for 25s: go and fight for 45s, then come back to it (holders must not camp an empty front)
    if (m_bHolding && level.inttime - Q_max(m_iHoldSince, m_iLastEnemySeenAny) > 25000) {
        m_bHolding      = false;
        m_vHoldPos      = vec_zero;
        m_iHoldCooldown = level.inttime + 45000;
        BotEv(controlledEnt, "holdrotate", controlledEnt->origin);
        return false;
    }
    const bool bAllies = controlledEnt->GetTeam() == TEAM_ALLIES;
    const bool bLine = BotHoldFrontLineOn();
    if (level.inttime >= m_iHoldCheck) {
        m_iHoldCheck      = level.inttime + 3000;
        const int iF = BotHoldFrontZone(); // [HZM bug-2959] smoothed (bot_holdFrontSmooth)
        Vector    c;
        if (iF < 1 || !BotLevelZoneCentre(bAllies ? iF : iF + 1, c)) {
            m_vHoldPos  = vec_zero;
            m_bHolding  = false;
            m_iHoldZone = 0;
            return false;
        }
        if (bLine) {
            // [bug-2959 v3] a new line (or none yet): its nearest point; the same line keeps the point it has
            const int zi = bAllies ? iF : iF + 1;
            // [v3.1] ...unless it is not holding right now (away fighting, or on its way back): then the post is the
            // band's nearest point to where it stands NOW - after a fight inside the band it holds where it is, and
            // one that drifted out walks the shortest way back, instead of recrossing the band to the old point
            const bool bReanchor = BotHoldFrontReanchor() && !m_bHolding;
            if (zi == m_iHoldZone && m_vHoldPos != vec_zero && !bReanchor) {
                c = m_vHoldPos;
            } else if (!BotZoneNearest(zi, controlledEnt->origin, c)) {
                m_vHoldPos  = vec_zero;
                m_bHolding  = false;
                m_iHoldZone = 0;
                return false;
            }
            m_iHoldZone = zi;
        }
        const float fMove = (bLine && BotHoldFrontReanchor() && !m_bHolding) ? 96.0f : 300.0f; // [v3.1]
        if ((c - m_vHoldPos).lengthSquared() > Square(fMove)) {
            m_vHoldPos      = c;
            m_vHoldIssued   = vec_zero;
            m_bHolding      = false;
            m_vHoldCover    = vec_zero; // a new line: look for new cover on it
            m_iHoldCoverTry = 0;
        }
    }
    if (m_vHoldPos == vec_zero) {
        return false;
    }
    if (!m_bHolding) {
        // [HZM bug-2879] already PAST the line (nearer the enemy end than the hold point): going back to hold it is a
        // reversal (probe2: holdfront was The Rail Yard's #1 loop source and #4 reversal source) - keep attacking
        Vector    eEnd;
        const int iZN = BotPushLevelInt("zn");
        float     past = 0.0f;
        if (bLine ? (m_iHoldZone > 0 && BotZonePastBy(m_iHoldZone, bAllies, controlledEnt->origin, past) && past > 200.0f)
                  : (iZN > 0 && BotLevelZoneCentre(bAllies ? iZN : 1, eEnd)
                     && (controlledEnt->origin - eEnd).lengthXY() + 200.0f < (m_vHoldPos - eEnd).lengthXY())) {
            m_vHoldPos      = vec_zero;
            m_iHoldZone     = 0;
            m_iHoldCooldown = level.inttime + 20000;
            BotEv(controlledEnt, "holdpast", controlledEnt->origin);
            return false;
        }
    }
    if ((m_vHoldPos - controlledEnt->origin).lengthXYSquared() > Square(420)) {
        if (!movement.IsMoving() || m_vHoldIssued != m_vHoldPos) {
            movement.SetWhy("holdfront"); // [HZM bot probe2]
            movement.MoveNear(m_vHoldPos, 380.0f);
            m_vHoldIssued = m_vHoldPos;
            if (!movement.IsMoving()) {
                m_vHoldPos      = vec_zero; // no reachable ground in that band: attack like everyone else this time
                m_iHoldCooldown = level.inttime + 15000; // [HZM bug-2879] and don't re-try it every 3s
                return false;
            }
        }
        m_bHolding = false;
        return true;
    }
    if (!m_bHolding) {
        m_bHolding   = true;
        m_iHoldSince = level.inttime;
        movement.SetWhy("holdarrive"); // [HZM bot probe2]
        movement.ClearMove();
        VoiceCallout("*25", 20000); // "Hold this position!"
        BotEv(controlledEnt, "hold", controlledEnt->origin);
    }
    Vector          e;
    const int       iN = BotPushLevelInt("zn");
    if (iN > 0 && BotLevelZoneCentre(bAllies ? iN : 1, e)) { // face the ENEMY end
        Vector d = e - controlledEnt->origin;
        d.z      = 0;
        if (d.lengthSquared() > 1.0f) {
            VectorNormalize2D(d);
            m_vAlertPos   = controlledEnt->origin + d * 512.0f + Vector(0, 0, controlledEnt->viewheight);
            m_iAlertUntil = level.inttime + 300;
            // [HZM bot C4 cover] set up BEHIND something: a spot near the line with a waist-high blocker toward the enemy
            // end and a clear view over it - crouched it hides the bot, standing to fight it shoots over (State_Attack
            // drops the crouch). Once per line; no such spot = hold in the open as before.
            if (m_iHoldCoverTry == 0) {
                m_iHoldCoverTry = 1;
                Vector cov;
                if (FindLowCover(m_vHoldPos, d, 400.0f, cov)) {
                    m_vHoldCover = cov;
                    movement.SetWhy("holdcover"); // [HZM bot probe2]
                    movement.MoveTo(cov);
                    BotEv(controlledEnt, "holdcover", cov);
                }
            }
        }
    }
    m_bWantCrouch = true;
    if (m_vHoldCover != vec_zero && (controlledEnt->origin - m_vHoldCover).lengthXYSquared() < Square(48)) {
        // [HZM bot C4 cover] crouched behind low cover a soldier sees nothing over it (and so spots nobody - enemies are
        // noticed by sight): come up for a 2s look every 6s, staggered per bot so a line never pops up in unison
        if ((level.inttime + controlledEnt->entnum * 997) % 6000 < 2000) {
            m_bWantCrouch = false;
        }
    }
    return true;
}

bool BotController::FindLowCover(const Vector& centre, const Vector& threatDir, float maxFromCentre, Vector& out)
{
    // [HZM bot C4 cover] sample the bot's spot and two rings round it (90u, 170u, 8 bearings) for LOW cover facing
    // threatDir: solid at crouched-head height within 64u in front, open at standing-eye height for 160u over it,
    // floor under it, and a straight walk from here (no wall between). Nearest wins.
    Vector td(threatDir.x, threatDir.y, 0);
    if (td.lengthSquared() < 0.01f) {
        return false;
    }
    VectorNormalize2D(td);
    const Vector org   = controlledEnt->origin;
    bool         found = false;
    float        best  = 0.0f;
    for (int ring = 0; ring < 3; ring++) {
        const float r     = ring == 0 ? 0.0f : (ring == 1 ? 90.0f : 170.0f);
        const int   steps = ring == 0 ? 1 : 8;
        for (int k = 0; k < steps; k++) {
            const float a    = DEG2RAD(45.0f * k);
            Vector      cand = org + Vector(cos(a) * r, sin(a) * r, 0);
            if ((cand - centre).lengthXYSquared() > Square(maxFromCentre)) {
                continue;
            }
            trace_t fl = G_Trace(
                cand + Vector(0, 0, 128), vec_zero, vec_zero, cand - Vector(0, 0, 160), controlledEnt, MASK_PLAYERSOLID,
                qfalse, "BotLowCoverFloor"
            );
            if (fl.startsolid || fl.allsolid || fl.fraction >= 1.0f || fl.plane.normal[2] < 0.7f) {
                continue;
            }
            const Vector g = fl.endpos;
            if (r > 0.0f) {
                trace_t wk = G_Trace(
                    org + Vector(0, 0, 40), vec_zero, vec_zero, g + Vector(0, 0, 40), controlledEnt, MASK_PLAYERSOLID,
                    qfalse, "BotLowCoverWalk"
                );
                if (wk.fraction < 0.98f) {
                    continue;
                }
            }
            // the engine's own low-cover test (Player::TickCoopCover: chest 36u hits within 48u, head 72u clear), so its
            // cover pose - and its blind fire - can engage for the bot here; plus a clear view over it to fight from
            trace_t lo = G_Trace(
                g + Vector(0, 0, 36), vec_zero, vec_zero, g + Vector(0, 0, 36) + td * 44.0f, controlledEnt, MASK_SOLID,
                qfalse, "BotLowCoverLo"
            );
            if (lo.fraction >= 1.0f || lo.startsolid || fabs(lo.plane.normal[2]) >= 0.7f) {
                continue; // nothing waist-high right in front
            }
            trace_t hd = G_Trace(
                g + Vector(0, 0, 72), vec_zero, vec_zero, g + Vector(0, 0, 72) + td * 48.0f, controlledEnt, MASK_SOLID,
                qfalse, "BotLowCoverHead"
            );
            trace_t hi = G_Trace(
                g + Vector(0, 0, 80), vec_zero, vec_zero, g + Vector(0, 0, 80) + td * 160.0f, controlledEnt, MASK_SOLID,
                qfalse, "BotLowCoverHi"
            );
            if (hd.fraction < 1.0f || hd.startsolid || hi.fraction < 1.0f || hi.startsolid) {
                continue; // a wall, not low cover: could not fight from here
            }
            const float d2 = (g - org).lengthSquared();
            if (!found || d2 < best) {
                found = true;
                best  = d2;
                out   = g;
            }
        }
        if (found) {
            break; // the nearest ring that has any
        }
    }
    return found;
}

bool BotController::IdleLane(void)
{
    // [HZM bot C1] SPREAD across routes: once per life, a bot whose lane is left or right (by entnum) first walks to a
    // point ~40% of the way to the objective and ~700u off the direct line, if the navmesh reaches it and the detour
    // costs < 35% extra - so a team comes in on two or three fronts instead of single-filing one corridor.
    static cvar_t *s_botLanes = NULL;
    if (!s_botLanes) {
        s_botLanes = gi.Cvar_Get("bot_lanes", "1", 0);
    }
    if (!s_botLanes->integer || m_iLaneState == 2) {
        return false;
    }
    if (m_iLaneState == 0) {
        m_iLaneState = 2; // decided now, whatever happens below
        if (!movement.IsMoving()) {
            movement.SetWhy("laneobj"); // [HZM bot probe2]
            movement.MoveToBestAttractivePoint();
            if (!movement.IsMoving()) {
                return false;
            }
        }
        const Vector S    = controlledEnt->origin;
        const Vector G    = movement.GetPathDestination();
        Vector       sg   = G - S;
        sg.z              = 0;
        const float  L    = sg.length();
        int          lane = (controlledEnt->entnum % 3) - 1;
        if (BotTeamRolesOn()) {
            // [item 5] by the bot's entnum rank in its own team: every side gets left / centre / right in turn
            int                               rank = 0;
            const Container<BotController *>& cl   = botManager.getControllerManager().getControllers();
            for (int i = 1; i <= cl.NumObjects(); i++) {
                BotController *o = cl.ObjectAt(i);
                if (o && o != this && o->controlledEnt && o->controlledEnt->GetTeam() == controlledEnt->GetTeam()
                    && o->controlledEnt->entnum < controlledEnt->entnum) {
                    rank++;
                }
            }
            lane = (rank % 3) - 1;
        }
        if (L < 2200.0f || lane == 0) {
            return false;
        }
        VectorNormalize2D(sg);
        const Vector perp(-sg.y, sg.x, 0);
        m_vLanePoint = S + sg * (L * 0.4f) + perp * (lane * 700.0f);
        movement.SetWhy("lane"); // [HZM bot probe2]
        movement.MoveNear(m_vLanePoint, 350.0f);
        if (!movement.IsMoving() || !movement.PathReaches(m_vLanePoint, 450.0f)) {
            movement.SetWhy("laneclr"); // [HZM bot probe2]
            movement.ClearMove();
            return false;
        }
        const Vector D = movement.GetPathDestination();
        if ((D - S).length() + (G - D).length() > 1.35f * L) {
            movement.SetWhy("laneclr"); // [HZM bot probe2]
            movement.ClearMove();
            return false;
        }
        m_iLaneState = 1;
        m_iLaneSince = level.inttime;
        BotEv(controlledEnt, lane < 0 ? "laneL" : "laneR", m_vLanePoint);
        return true;
    }
    if ((m_vLanePoint - controlledEnt->origin).lengthXYSquared() < Square(300) || level.inttime - m_iLaneSince > 30000
        || !movement.IsMoving()) {
        m_iLaneState = 2;
        movement.SetWhy("laneclr"); // [HZM bot probe2]
        movement.ClearMove();
        return false;
    }
    return true;
}

// [HZM bot breach] the grenade's flight model, read once per projectile model from a throw-away spawn of it (the tiki
// init commands set speed / minspeed / life / gravity / size) - exactly what ProjectileAttack will use for a real throw
struct BotProjInfo {
    str    model;
    bool   ok;
    float  speed, minspeed, life, dmlife, minlife, gravity;
    int    flags;
    Vector addvel, mins, maxs;
    str    explModel; // [HZM bot room-clear step 2] its explosionmodel and that tiki's radius (BotExplR)
};

// [HZM bot room-clear step 2] the reach of a grenade's blast, read from its EXPLOSION tiki's server init commands
// ("radius 400" for the M2 / Stielhandgranate's M2FGrenadeExplosion, 500 the F1, 350 the Mills) - the number
// RadiusDamage uses - cached per model. R = radius + 8 (findradius admits |centroid-org|^2 <= r^2 + radius2, ~403u for
// a standing player) + 24 margin. Unknown / no radius: 450.
static float BotExplR(const str& explModel)
{
    static str   s_name[8];
    static float s_R[8];
    static int   s_n = 0;
    if (!explModel.length()) {
        return 450.0f;
    }
    for (int i = 0; i < s_n; i++) {
        if (!Q_stricmp(s_name[i].c_str(), explModel.c_str())) {
            return s_R[i];
        }
    }
    float        radius = 0.0f;
    dtiki_t     *tiki   = gi.modeltiki(CanonicalTikiName(explModel.c_str()));
    if (tiki && tiki->a) {
        for (int i = 0; i < tiki->a->num_server_initcmds; i++) {
            const dtikicmd_t& c = tiki->a->server_initcmds[i];
            if (c.num_args >= 2 && !Q_stricmp(c.args[0], "radius")) {
                radius = (float)atof(c.args[1]);
            }
        }
    }
    const float R = radius > 1.0f ? radius + 8.0f + 24.0f : 450.0f;
    if (s_n < 8) {
        s_name[s_n] = explModel;
        s_R[s_n++]  = R;
    }
    static cvar_t *s_p = NULL;
    if (!s_p) {
        s_p = gi.Cvar_Get("bot_probe", "0", 0);
    }
    if (s_p->integer) {
        gi.Printf("^~^~^ BOTNADERADIUS model=%s radius=%.0f R=%.0f\n", explModel.c_str(), radius, R);
    }
    return R;
}

static bool BotGetProjInfo(Weapon *w, BotProjInfo& out)
{
    static BotProjInfo s_cache[8];
    static int         s_n = 0;
    const str&         m   = w->GetProjectileModel(FIRE_PRIMARY);
    if (!m.length()) {
        return false;
    }
    for (int i = 0; i < s_n; i++) {
        if (!Q_stricmp(s_cache[i].model.c_str(), m.c_str())) {
            out = s_cache[i];
            return out.ok;
        }
    }
    BotProjInfo pi;
    pi.model = m;
    pi.ok    = false;
    pi.speed = pi.minspeed = pi.life = pi.dmlife = pi.minlife = pi.gravity = 0.0f;
    pi.flags = 0;
    pi.addvel = pi.mins = pi.maxs = vec_zero;

    SpawnArgs args;
    args.setArg("model", m);
    Entity *obj = static_cast<Entity *>(args.Spawn());
    if (obj) {
        if (obj->isSubclassOf(Projectile)) {
            Projectile *p = static_cast<Projectile *>(obj);
            p->ProcessInitCommands();
            pi.speed    = p->speed;
            pi.minspeed = p->minspeed;
            pi.life     = p->life;
            pi.dmlife   = p->dmlife;
            pi.minlife  = p->minlife;
            pi.gravity  = p->gravity;
            pi.flags    = p->projFlags;
            pi.addvel   = p->addvelocity;
            pi.mins     = p->mins;
            pi.maxs     = p->maxs;
            pi.ok       = p->speed > 0.0f;
            pi.explModel = p->explosionmodel;
        }
        obj->hideModel();
        obj->setSolidType(SOLID_NOT);
        obj->PostEvent(EV_Remove, 0);
    }
    if (s_n < 8) {
        s_cache[s_n++] = pi;
    }
    out = pi;
    return pi.ok;
}

// ======================================================================================================================
// [HZM bot room-clear step 1] GRENADE TELEMETRY - log the ACT, not the decision (TRAPS T10). One BOTNADE line when a bot's
// grenade projectile really spawns (ProjectileAttack, or the overcook in Weapon::OnOverCooked), one BOTNADEDET when it
// goes off, and g_iBotNadeDetId so BOTFFHARM names the throw behind a self-hit. BotNadeSim predicts the detonation with
// the engine's own toss rules so step 2 (bot_nadeSafe) can be built on a model checked against real throws; here it is
// only logged. All of it is a no-op unless bot_probe is set. No behaviour change.
// ======================================================================================================================
int g_iBotNadeDetId = 0;

struct BotNadeRec {
    int    id;
    int    owner;
    int    mode;
    Vector pred;
    Vector tgt;
    Vector sdet; // [room-clear step 2] the throw solver's predicted detonation (vec_zero = none)
};
static BotNadeRec  s_nadeRec[32];
static int         s_nadeSeq       = 0;
static const char *s_nadeModeName[] = {"lob", "clear", "stray", "deathdrop", "overcook"};

// ---- [HZM bot room-clear step 2] SIM TRACE BUDGET (vet C6). Every toss-simulation step and blast-reach trace the bots run
// is counted per server frame, and the throw solver only starts another arc while the frame has used < ~1200. BOTNADESIM
// (bot_probe) prints the cumulative per-frame histogram every 60s so a soak can read the p99.
static int s_nsFrame = -1, s_nsUsed = 0, s_nsLogT = 0, s_nsMax = 0;
static int s_nsHist[26]; // frames that used traces, by 50-trace bucket (the last = >= 1250)

static void BotNadeBudgetFrame(void)
{
    if (level.framenum == s_nsFrame) {
        return;
    }
    if (s_nsUsed > 0) {
        s_nsHist[Q_min(s_nsUsed / 50, 25)]++;
        s_nsMax = Q_max(s_nsMax, s_nsUsed);
    }
    s_nsFrame = level.framenum;
    s_nsUsed  = 0;
    if (level.inttime < s_nsLogT) {
        s_nsLogT = 0; // a new map
    }
    if (level.inttime - s_nsLogT >= 60000) {
        s_nsLogT = level.inttime;
        static cvar_t *s_p = NULL;
        if (!s_p) {
            s_p = gi.Cvar_Get("bot_probe", "0", 0);
        }
        if (s_p->integer) {
            char   buf[300];
            size_t o = 0;
            buf[0]   = 0;
            for (int i = 0; i < 26 && o + 12 < sizeof(buf); i++) {
                o += Com_sprintf(buf + o, sizeof(buf) - o, "%s%d", i ? "," : "", s_nsHist[i]);
            }
            gi.Printf("^~^~^ BOTNADESIM max=%d hist50=%s\n", s_nsMax, buf);
        }
    }
}

static int BotNadeBudgetLeft(void)
{
    BotNadeBudgetFrame();
    return 1200 - s_nsUsed;
}

static void BotNadeBudgetUse(int n)
{
    BotNadeBudgetFrame();
    s_nsUsed += n;
}

// one simulated toss: where it goes off, and (for the solver) its first impact and whether it went through an opening's
// plane and came back out of it
struct BotNadeTrack {
    Vector  det;
    int     bounces, steps;
    bool    rest;
    bool    hit;
    Vector  hitPos, hitN;
    Entity *hitEnt;
    float   hitT;
    bool    crossed, recrossed;
};

// G_Physics_Toss for a MOVETYPE_BOUNCE projectile, step for step: gravity BEFORE the move, one clip per frame (the rest
// of that frame's move is lost), overbounce 1.4 with STOP_EPSILON zeroing, xy damped by 1.75*dt on every contact,
// and on a floor (normal.z > 0.7) it stops under 40u/s - or next frame when it did not bounce up (groundentity kept,
// velocity zeroed). dt = level.frametime (sv_fps 40 -> 0.025). det = where it is after `life` seconds.
static void BotNadeSimEx(
    Vector        p,
    Vector        vel,
    const Vector& mins,
    const Vector& maxs,
    float         grav,
    float         life,
    Entity       *ignore,
    const Vector *planeD,
    const Vector *planeN,
    BotNadeTrack& out,
    int           mask = MASK_PROJECTILE
)
{
    const float dt = level.frametime > 0.0f ? level.frametime : 0.025f;
    out.bounces    = 0;
    out.steps      = 0;
    out.rest       = false;
    out.hit        = false;
    out.crossed    = false;
    out.recrossed  = false;
    out.hitEnt     = NULL;
    out.hitT       = 0.0f;
    out.hitPos     = vec_zero;
    out.hitN       = vec_zero;
    for (float t = 0.0f; t < life; t += dt) {
        vel.z -= grav * dt;
        const Vector np = p + vel * dt;
        // CYLINDRICAL, as G_PushEntity traces a moving entity (a capsule against the brushes): with a box, a grenade
        // rolling over an edge, a kerb or terrain parted from the engine's (round-1 soak: rolling errors of 120-400u)
        trace_t tr = G_Trace(p, mins, maxs, np, ignore, mask, qtrue, "BotNadeSimToss");
        out.steps++;
        if (tr.allsolid) {
            break;
        }
        p = tr.endpos;
        if (tr.fraction == 0.0f) {
            // G_Physics_Toss: blocked at once - slide by the horizontal move x frametime x 15 (the clip below still
            // uses the first trace's plane)
            const Vector slide(vel.x * dt * dt * 15.0f, vel.y * dt * dt * 15.0f, 0.0f);
            trace_t      ts = G_Trace(p, mins, maxs, p + slide, ignore, mask, qtrue, "BotNadeSimSlide");
            out.steps++;
            if (!ts.allsolid) {
                p = ts.endpos;
            }
        }
        if (planeD) {
            const float s = DotProduct(p - *planeD, *planeN);
            if (s > 8.0f) {
                out.crossed = true;
            } else if (out.crossed && s < -8.0f) {
                out.recrossed = true;
            }
        }
        if (tr.fraction < 1.0f) {
            const Vector n = tr.plane.normal;
            if (!out.hit) {
                out.hit    = true;
                out.hitPos = p;
                out.hitN   = n;
                out.hitEnt = tr.ent ? tr.ent->entity : NULL;
                out.hitT   = t + dt * tr.fraction;
            }
            const float back = DotProduct(vel, n) * 1.4f;
            vel -= n * back;
            for (int i = 0; i < 3; i++) {
                if (vel[i] > -0.1f && vel[i] < 0.1f) {
                    vel[i] = 0.0f;
                }
            }
            vel.x -= vel.x * dt * 1.75f;
            vel.y -= vel.y * dt * 1.75f;
            out.bounces++;
            if (n[2] > 0.7f && (vel.length() < 40.0f || vel.z <= 0.0f)) {
                out.rest = true;
                break;
            }
        }
    }
    out.det = p;
    BotNadeBudgetUse(out.steps);
}

static Vector BotNadeSim(
    Vector p, Vector vel, const Vector& mins, const Vector& maxs, float grav, float life, Entity *ignore, int& bounces,
    bool& rest
)
{
    BotNadeTrack tk;
    BotNadeSimEx(p, vel, mins, maxs, grav, life, ignore, NULL, NULL, tk);
    bounces = tk.bounces;
    rest    = tk.rest;
    return tk.det;
}

static bool BotNadeProbeOn(void)
{
    static cvar_t *s_p = NULL;
    if (!s_p) {
        s_p = gi.Cvar_Get("bot_probe", "0", 0);
    }
    return s_p->integer != 0;
}

// ---- [HZM bot room-clear step 2] THE ENGINE'S BLAST TEST (vet C4). RadiusDamage hurts a body when findradius admits its
// CENTROID (+47 standing, +28 crouched; R carries findradius' slack) and a MASK_EXPLOSION sight line runs from the blast to
// that centroid - fences, grates, bodies and BBOX props do NOT stop a blast (MASK_SOLID, which every bot LOS test used,
// does). Tested from the predicted point and 4 points 24u round it, so a door-frame edge plus the prediction error cannot
// flip the verdict.
static bool BotBlastReaches(const Vector& det, float R, const Vector& pos, bool crouched, Entity *victim, Entity *pass2)
{
    const Vector c = pos + Vector(0, 0, crouched ? 28.0f : 47.0f);
    if ((c - det).lengthSquared() > Square(R)) {
        return false;
    }
    static const float offs[5][2] = {
        {0,   0  },
        {24,  0  },
        {-24, 0  },
        {0,   24 },
        {0,   -24}
    };
    for (int i = 0; i < 5; i++) {
        BotNadeBudgetUse(1);
        if (G_SightTrace(
                det + Vector(offs[i][0], offs[i][1], 2.0f), vec_zero, vec_zero, c, victim, pass2, MASK_EXPLOSION, qfalse,
                "BotBlastReach"
            )) {
            return true;
        }
    }
    return false;
}

// a living team-mate the blast at det would reach (the user's veto, 2026-09-24 - kept with FF off)
static bool BotMateNearBlast2(const Player *self, const Vector& det, float R)
{
    for (int i = 0; i < game.maxclients; i++) {
        gentity_t *ge = &g_entities[i];
        if (!ge->inuse || !ge->entity || ge->entity == self || !ge->entity->IsSubclassOfPlayer()) {
            continue;
        }
        Player *o = static_cast<Player *>(ge->entity);
        if (o->IsDead() || o->IsSpectator() || o->GetTeam() != self->GetTeam()) {
            continue;
        }
        const bool bDuck = o->client && (o->client->ps.pm_flags & PMF_DUCKED);
        if (BotBlastReaches(det, R, o->origin, bDuck, o, (Entity *)self)) {
            return true;
        }
    }
    return false;
}

// a thrown frag (not smoke, not the Explosion it turns into)
static bool BotIsFragProjectile(Entity *e)
{
    if (!e || !e->IsSubclassOfProjectile() || e->isSubclassOf(Explosion)) {
        return false;
    }
    const char *mdl = e->model.c_str();
    if (!(Q_stristr(mdl, "grenade") || Q_stristr(mdl, "granate") || Q_stristr(mdl, "masher") || Q_stristr(mdl, "mills")
          || Q_stristr(mdl, "bomba"))) {
        return false;
    }
    return !(Q_stristr(mdl, "smoke") || Q_stristr(mdl, "nebel"));
}

// Where a LIVE grenade goes off: the rest of its path simulated from its current state and the fuse it has left
// (Projectile::m_fExplodeAt), cached 200ms per projectile - per projectile, not per bot. R = its explosion's reach,
// until = when (inttime).
struct BotLiveNade {
    int    ent, t, until;
    float  spawn, R;
    Vector det;
};
static BotLiveNade s_liveNade[32];

static void BotLiveNadeDet(Projectile *pr, Vector& det, float& R, int& until)
{
    BotLiveNade& c = s_liveNade[pr->entnum & 31];
    if (c.ent == pr->entnum && c.spawn == pr->edict->spawntime && c.t <= level.inttime && level.inttime - c.t < 200) {
        det   = c.det;
        R     = c.R;
        until = c.until;
        return;
    }
    const float left = (pr->m_fExplodeAt > 0.0f) ? Q_max(0.0f, pr->m_fExplodeAt - level.time)
                                                 : Q_max(0.3f, 3.0f - (level.time - pr->edict->spawntime));
    Vector d = pr->origin;
    if (left > 0.0f && (!pr->groundentity || pr->velocity.lengthSquared() > 1.0f)) {
        const float  g = sv_gravity->value * (pr->gravity > 0.0f ? pr->gravity : 1.0f);
        BotNadeTrack tk;
        BotNadeSimEx(pr->origin, pr->velocity, pr->mins, pr->maxs, g, left, pr, NULL, NULL, tk);
        d = tk.det;
    }
    c.ent   = pr->entnum;
    c.spawn = pr->edict->spawntime;
    c.t     = level.inttime;
    c.det   = d;
    c.R     = BotExplR(pr->explosionmodel);
    c.until = level.inttime + (int)(left * 1000.0f);
    det     = c.det;
    R       = c.R;
    until   = c.until;
}

// a blast mark's detonation NOW, from its live projectile (a door or a body can knock it off the predicted path). The
// projectile gone = it went off: nothing left to fear.
static bool BotNadeMarkLive(const BotNadeMark& m, Vector& det, float& R, int& until, Projectile **proj)
{
    if (!m.id || m.t > level.inttime || level.inttime >= m.until || m.projEnt < 0 || m.projEnt >= globals.max_entities) {
        return false;
    }
    gentity_t *ge = &g_entities[m.projEnt];
    if (!ge->inuse || !ge->entity || !ge->entity->IsSubclassOfProjectile() || ge->spawntime != m.spawn) {
        return false;
    }
    Projectile *pr = static_cast<Projectile *>(ge->entity);
    BotLiveNadeDet(pr, det, R, until);
    if (proj) {
        *proj = pr;
    }
    return true;
}

void BotNadeOnSpawn(Projectile *proj, Entity *owner, Weapon *weap, float fraction, float life, const char *how)
{
    if (!proj || !owner || !owner->IsSubclassOfPlayer() || (!BotNadeProbeOn() && !BotNadeSafe())) {
        return;
    }
    BotController *bc = botManager.getControllerManager().findController(owner);
    if (bc) {
        bc->OnNadeSpawn(proj, weap, fraction, life, how);
    }
}

void BotController::OnNadeSpawn(Projectile *proj, Weapon *weap, float fraction, float life, const char *how)
{
    if (!weap || !(weap->GetWeaponClass() & WEAPON_CLASS_GRENADE)) {
        return; // a bazooka / rifle grenade is not a thrown grenade
    }
    int mode;
    if (!Q_stricmp(how, "overcook")) {
        mode = 4;
    } else if (controlledEnt->IsDead()) {
        mode = 3; // RELEASE_KILLED_FRAG
    } else if (m_iNadeState == 3 && level.inttime - m_iNadeTime <= (BotNadeSafe() ? 2000 : 1200)) {
        mode = m_iNadeMode == 1 ? 1 : 0; // ([step 2] its state 3 waits up to 2s for the throw animation)
    } else {
        mode = 2; // released outside the state 2 -> 3 hand-off (an abort let go of a cooking grenade)
    }
    const int id      = ++s_nadeSeq;
    proj->m_iBotNadeId = id;

    int    nb   = 0;
    bool   rest = false;
    Vector pred = proj->origin;
    if (life > 0.0f) {
        const float g = sv_gravity->value * (proj->gravity > 0.0f ? proj->gravity : 1.0f);
        // pass the PROJECTILE: the engine skips the passent, its owner and the owner's other missiles - passing the
        // thrower instead let the first trace start inside the grenade's own box and "bounce" off itself
        pred          = BotNadeSim(proj->origin, proj->velocity, proj->mins, proj->maxs, g, life, proj, nb, rest);
    }
    if (BotNadeSafe()) {
        // [HZM bot room-clear step 2] the blast mark, from the ACT (vet C3): lobs, clears, strays and death-drops alike
        if (life > 0.0f) {
            BotMarksMapCheck();
            BotNadeMark& m = s_nadeMarks[s_nadeMarkSeq % 16];
            m.id           = ++s_nadeMarkSeq;
            m.team         = controlledEnt->GetTeam();
            m.owner        = controlledEnt->entnum;
            m.projEnt      = proj->entnum;
            m.spawn        = proj->edict->spawntime;
            m.t            = level.inttime;
            m.until        = level.inttime + (int)(life * 1000.0f) + 300;
            m.det          = pred;
            m.R            = BotExplR(proj->explosionmodel);
            m.op           = (mode == 1 && m_iOpId > 0 && m_iOpSlot < 0) ? 1 : 0; // [room-clear step 4]
        }
        if (mode <= 1) {
            m_iNadeSpawnId = id; // State_GrenadeSafe: it has left the hand
            m_iNadeSpawnT  = level.inttime;
            if (mode == 1 && m_iOpId > 0 && m_iOpSlot < 0) {
                OpThrown(life, pred); // [room-clear step 4] the stack holds for it now
            }
        }
    }
    if (!BotNadeProbeOn()) {
        return;
    }
    BotNadeRec& r = s_nadeRec[id & 31];
    r.id          = id;
    r.owner       = controlledEnt->entnum;
    r.mode        = mode;
    r.pred        = pred;
    r.tgt         = (mode <= 1) ? m_vNadeTarget : vec_zero;
    r.sdet        = (mode <= 1 && BotNadeSafe()) ? m_vNadeDet : vec_zero;
    gi.Printf(
        "^~^~^ BOTNADE e=%d id=%d mode=%s st=%d f=%.2f fuse=%.2f hold=%.2f at=(%.0f %.0f %.0f) vel=(%.0f %.0f %.0f) "
        "pred=(%.0f %.0f %.0f) predB=%d predRest=%d tgt=(%.0f %.0f %.0f) crouch=%d sdet=(%.0f %.0f %.0f)\n",
        controlledEnt->entnum, id, s_nadeModeName[mode], m_iNadeState, fraction, life, m_fNadeHold, proj->origin.x,
        proj->origin.y, proj->origin.z, proj->velocity.x, proj->velocity.y, proj->velocity.z, pred.x, pred.y, pred.z, nb,
        rest ? 1 : 0, r.tgt.x, r.tgt.y, r.tgt.z,
        (controlledEnt->client && (controlledEnt->client->ps.pm_flags & PMF_DUCKED)) ? 1 : 0, r.sdet.x, r.sdet.y,
        r.sdet.z
    );
}

void BotNadeOnExplode(Projectile *proj)
{
    if (!BotNadeProbeOn()) {
        return;
    }
    const int   id = proj->m_iBotNadeId;
    BotNadeRec *r  = &s_nadeRec[id & 31];
    if (r->id != id) {
        return;
    }
    const Vector at    = proj->origin;
    Entity      *own   = G_GetEntity(r->owner);
    float        oDist = -1.0f;
    int          oBlk  = -1;
    int          oExp  = 0;
    if (own && own->IsSubclassOfPlayer() && !own->IsDead()) {
        // the engine's own damage test (RadiusDamage): a MASK_EXPLOSION sight trace blast -> centroid; findradius admits
        // |centroid - org|^2 <= r^2 + radius2 (~403u standing for the 400u frag)
        oDist = (own->centroid - at).length();
        oBlk  = G_SightTrace(at, vec_zero, vec_zero, own->centroid, proj, own, MASK_EXPLOSION, qfalse, "BotNadeDetOwn") ? 0 : 1;
        oExp  = (oDist < 403.0f && !oBlk) ? 1 : 0;
    }
    const bool bRest = proj->groundentity || proj->velocity.length() < 40.0f;
    const bool bFuse = level.time >= proj->m_fExplodeAt - level.frametime * 0.5f;
    gi.Printf(
        "^~^~^ BOTNADEDET e=%d id=%d mode=%s why=%s at=(%.0f %.0f %.0f) pred=(%.0f %.0f %.0f) err=%.0f rest=%d spd=%.0f "
        "own=%.0f ownblk=%d exp=%d tgt=%.0f serr=%.0f\n",
        r->owner, id, s_nadeModeName[r->mode], bFuse ? "fuse" : "early", at.x, at.y, at.z, r->pred.x, r->pred.y, r->pred.z,
        (at - r->pred).length(), bRest ? 1 : 0, proj->velocity.length(), oDist, oBlk, oExp,
        r->tgt != vec_zero ? (at - r->tgt).length() : -1.0f, r->sdet != vec_zero ? (at - r->sdet).length() : -1.0f
    );
}

bool BotController::SolveThrow(
    Weapon *w, const Vector& target, int kind, const Vector& gate, Vector& ang, float& hold, Vector& land, float& fuse
)
{
    // [HZM bot breach] find a charge + pitch whose arc, simulated against the real world (traces, sv_gravity, the tiki's
    // speed curve and fuse), gets the grenade PAST the gate (the doorway / the hatch) and lands it near the target:
    // for each charge the two ballistic pitches (flat and lobbed) and +-3 deg either side. A throw that would bounce off
    // the frame, hit a team-mate, blow up in the air or land in our own face is rejected. Cheap enough for the rare
    // moment it runs: <= 36 short arcs.
    BotProjInfo pi;
    if (!BotGetProjInfo(w, pi)) {
        return false;
    }
    const float  g     = sv_gravity->value * (pi.gravity > 0.0f ? pi.gravity : 1.0f);
    const float  fMax  = w->GetMaxChargeTime(FIRE_PRIMARY);
    const float  fMin  = w->GetMinChargeTime(FIRE_PRIMARY);
    const Vector start = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight);
    const Vector aim   = target + Vector(0, 0, 8);
    const Vector d     = aim - start;
    const float  dx    = sqrt(d.x * d.x + d.y * d.y);
    const float  dy    = d.z;
    const float  yaw   = (dx > 1.0f) ? RAD2DEG(atan2(d.y, d.x)) : controlledEnt->angles.y;
    Vector       tdir(d.x, d.y, 0);
    if (tdir.lengthSquared() > 1.0f) {
        VectorNormalize2D(tdir);
    }
    const float gateAlong = DotProduct2D(gate - start, tdir);
    const bool  bCharge   = (pi.flags & P_CHARGE_SPEED) && fMax > 0.0f;

    float fracs[6];
    int   nf = 0;
    if (!bCharge) {
        fracs[nf++] = 1.0f;
    } else {
        const float f0      = Q_clamp_float((fMin + 0.05f) / fMax, 0.05f, 1.0f);
        const float cand[5] = {0.0f, 0.35f, 0.55f, 0.75f, 1.0f};
        fracs[nf++]         = f0;
        for (int i = 1; i < 5; i++) {
            if (cand[i] > f0 + 0.05f) {
                fracs[nf++] = cand[i];
            }
        }
    }

    bool  found   = false;
    float bestErr = 0.0f;
    // reject tally for bot_breachDebug: reach, fuse, pitch, air, mate, ceiling, gate, off-target, own face
    int   rj[9]   = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    float nearErr = 1e9f;
    for (int fi = 0; fi < nf; fi++) {
        const float f    = fracs[fi];
        const float v    = bCharge ? pi.minspeed + (pi.speed - pi.minspeed) * f : pi.speed;
        float       life = (g_gametype->integer != GT_SINGLE_PLAYER && pi.dmlife) ? pi.dmlife : pi.life;
        if (pi.flags & P_CHARGE_LIFE) {
            life *= (1.0f - f);
            if (life < pi.minlife) {
                life = pi.minlife;
            }
        }
        if (v < 50.0f || life < 1.8f) {
            rj[1]++;
            continue; // too short a fuse to throw and get out of the doorway before it goes off
        }
        const float v2   = v * v;
        const float disc = v2 * v2 - g * (g * dx * dx + 2.0f * dy * v2);
        if (disc < 0.0f) {
            rj[0]++;
            continue; // out of reach at this charge
        }
        const float sq     = sqrt(disc);
        const float th[2]  = {(float)RAD2DEG(atan2(v2 - sq, g * dx)), (float)RAD2DEG(atan2(v2 + sq, g * dx))};
        const float adj[3] = {0.0f, 3.0f, -3.0f};
        for (int ti = 0; ti < 2; ti++) {
            for (int ai = 0; ai < 3; ai++) {
                const float elev = th[ti] + adj[ai];
                if (elev > 86.0f || elev < -86.0f) {
                    rj[2]++;
                    continue; // past the view pitch limit (pmove clamps at ~88; up a ladder shaft needs 83-86)
                }
                Vector fwd;
                AngleVectors(Vector(-elev, yaw, 0), fwd, NULL, NULL);
                Vector       p   = start;
                Vector       vel = fwd * v + pi.addvel;
                const float  dt  = 0.05f;
                const float  tmax = Q_min(life, 3.0f);
                bool         hit  = false;
                float        tImp = 0.0f;
                trace_t      tr;
                for (float t = 0.0f; t < tmax; t += dt) {
                    vel.z -= g * dt;
                    const Vector np = p + vel * dt;
                    tr = G_Trace(p, pi.mins, pi.maxs, np, controlledEnt, MASK_PROJECTILE, qfalse, "BotNadeSim");
                    if (tr.startsolid || tr.allsolid) {
                        break;
                    }
                    if (tr.fraction < 1.0f) {
                        hit  = true;
                        tImp = t + dt * tr.fraction;
                        p    = tr.endpos;
                        break;
                    }
                    p = np;
                }
                if (!hit || tImp > life - 0.3f) {
                    rj[3]++;
                    continue; // never came down in time: it would go off in the air
                }
                if (tr.ent && tr.ent->entity && tr.ent->entity->IsSubclassOfPlayer()
                    && static_cast<Player *>(tr.ent->entity)->GetTeam() == controlledEnt->GetTeam()) {
                    rj[4]++;
                    continue; // straight into a team-mate
                }
                if (tr.plane.normal[2] < -0.3f) {
                    rj[5]++;
                    continue; // a ceiling / the top of the frame: it drops back at us
                }
                if ((kind == 1 && DotProduct2D(p - start, tdir) < gateAlong + 24.0f) // not through the doorway
                    || (kind == 2 && p.z < gate.z - 24.0f)                       // not up through the hatch
                    || (kind == 3 && p.z > gate.z + 24.0f)) {                    // not down the hatch
                    rj[6]++;
                    continue;
                }
                const float err = (p - target).lengthXY() + fabs(p.z - target.z) * 0.5f;
                nearErr         = Q_min(nearErr, err);
                if (err > (kind == 1 ? 128.0f : 200.0f)) { // a hatch: anywhere on the landing past it will do
                    rj[7]++;
                    continue;
                }
                if ((p - start).lengthSquared() < Square(180.0f)
                    && G_SightTrace(
                        p + Vector(0, 0, 16), vec_zero, vec_zero, start, controlledEnt, (Entity *)NULL, MASK_SOLID, qfalse,
                        "BotBreachSelf"
                    )) {
                    rj[8]++;
                    continue; // lands in our own face
                }
                if (!found || err < bestErr) {
                    found   = true;
                    bestErr = err;
                    ang     = Vector(-elev, yaw, 0);
                    hold    = bCharge ? Q_max(fMin + 0.05f, f * fMax) : 0.1f;
                    land    = p;
                    fuse    = life;
                }
            }
        }
        if (found && bestErr < 40.0f) {
            break; // good enough - no need to try harder throws
        }
    }
    static cvar_t *s_dbg = NULL;
    if (!s_dbg) {
        s_dbg = gi.Cvar_Get("bot_breachDebug", "0", 0);
    }
    if (s_dbg->integer) {
        gi.Printf(
            "^~^~^ BREACHSOLVE e=%d kind=%d ok=%d err=%.0f near=%.0f dx=%.0f dy=%.0f g=%.0f v=%.0f-%.0f life=%.1f/%.1f fl=%d "
            "chg=%.2f-%.2f rj=reach%d fuse%d pitch%d air%d mate%d ceil%d gate%d off%d self%d\n",
            controlledEnt->entnum, kind, found ? 1 : 0, found ? bestErr : -1.0f, nearErr, dx, dy, g, pi.minspeed, pi.speed,
            pi.life, pi.dmlife, pi.flags, fMin, fMax, rj[0], rj[1], rj[2], rj[3], rj[4], rj[5], rj[6], rj[7], rj[8]
        );
    }
    return found;
}

// [user 2026-09-24] "one cleared a room with a grenade seeing clearly an allied was on the other side": the throw check
// only rejected an arc whose FIRST IMPACT was a team-mate. Any living team-mate within 420u of the landing with a line
// to it (the blast reaches them) - or in the room itself - vetoes the throw.
static bool BotMateNearBlast(const Player *self, const Vector& land)
{
    for (int i = 0; i < game.maxclients; i++) {
        gentity_t *ge = &g_entities[i];
        if (!ge->inuse || !ge->entity || ge->entity == self || !ge->entity->IsSubclassOfPlayer()) {
            continue;
        }
        Player *o = static_cast<Player *>(ge->entity);
        if (o->IsDead() || o->IsSpectator() || o->GetTeam() != self->GetTeam()) {
            continue;
        }
        if ((o->origin - land).lengthSquared() > Square(420)) {
            continue;
        }
        if (G_SightTrace(
                land + Vector(0, 0, 16), vec_zero, vec_zero, o->origin + Vector(0, 0, 40), (Entity *)self, (Entity *)o,
                MASK_SOLID, qfalse, "BotBreachMate"
            )) {
            return true;
        }
    }
    return false;
}

// ======================================================================================================================
// [HZM bot room-clear step 2] GRENADE SAFETY - bot_nadeSafe 1 (0 = the step-1 code path). User 2026-09-25: "when they throw
// grenades to clear rooms they will sometimes be too close to the door and get hurt or even move in and get hurt".
// Measured on the ACT (step 1, bug-2925): 23% of throws went off with the thrower in reach, ~65% of lobs air-burst (BT
// grenades COOK: the charge-scaled lob hold shortened the fuse), 12% were strays (an abort let go of a cooking grenade),
// 21 trigger / aim clobbers.
//  - ONE throw model for every grenade (lob and clear): a pitch SWEEP x a few charges, each arc simulated to DETONATION
//    with the engine's toss rules incl. the tiki's +250 addvelocity (the analytic seed ignored it and aimed 1.5-2x long,
//    vet B2), under a server-wide trace budget, spread over the frames the bot stands still before it draws (SOLVE)
//  - accepted only when it goes off in the room / by the target, reaches no team-mate, and the blast (the engine's own
//    test, BotBlastReaches) cannot reach the thrower where it will stand: where it is, or a cover spot it can get to in
//    the fuse (FindBlastCover)
//  - the thrower stands still from the solve to the release; once cooking, the throw ALWAYS completes on the solved angles
//  - after it: every grenade posts a blast mark from its projectile; nobody the blast can hurt walks into it; anyone it can
//    reach takes BLAST cover, never a blind AvoidPath (the old flee ran 800u past a grenade into a dead end)
// ======================================================================================================================

bool BotController::MovementLocked(void) const
{
    // (vet C10) a live throw, or getting out of a blast, owns the feet: combat moves that CommitMove(0) must not replace it
    if (!BotNadeSafe()) {
        return false;
    }
    // ([room-clear step 4] a stacker's feet too: going to its spot, holding it, going in)
    return m_iNadeState != 0 || level.inttime < m_iGrenadeFleeUntil || level.inttime < m_iBlastCoverUntil
        || (m_iOpId != 0 && m_iOpSlot >= 0);
}

bool BotController::FindBlastCover(
    const Vector& det, float R, const Vector& from, float timeLeft, Vector& out, float *travel, Vector *pFar
)
{
    // the nearest spot the blast at det cannot reach (BotBlastReaches), a straight run from `from` (player box sweep and a
    // floor under it), whose run never passes within 128u of the grenade, reachable in timeLeft at this bot's measured
    // pace (85%). 8 bearings, starting straight away from the grenade, x 96 / 192 / 288u. pFar (optional): when no spot is
    // out of its reach, the valid run that ends FARTHEST from it - RadiusDamage falls off with distance (200 at the blast,
    // ~100 at 300u), so running away still halves the hit where standing still takes all of it.
    const float pace = Q_min(Q_max(m_fPace, 80.0f), 200.0f) * 0.85f;
    Vector      away = from - det;
    away.z           = 0;
    const float base = (away.lengthSquared() > 1.0f) ? RAD2DEG(atan2(away.y, away.x)) : controlledEnt->angles.y;
    const float dFrom = away.length();
    static const float s_ang[8] = {0.0f, 45.0f, -45.0f, 90.0f, -90.0f, 135.0f, -135.0f, 180.0f};
    Vector              runMaxs = controlledEnt->maxs;
    runMaxs.z                   = Q_max(runMaxs.z - STEPSIZE, 20.0f);
    bool  found   = false;
    float best    = 0.0f;
    float farBest = dFrom + 48.0f; // a fallback must gain real distance
    bool  bFar    = false;
    for (int ai = 0; ai < 8; ai++) {
        const float  yaw = DEG2RAD(base + s_ang[ai]);
        const Vector dir(cos(yaw), sin(yaw), 0.0f);
        for (int ri = 1; ri <= 3; ri++) {
            const float r  = 96.0f * ri;
            const float tt = r / pace;
            const bool  bInTime = tt <= timeLeft && !(found && tt >= best);
            if (!bInTime && !pFar) {
                break; // the farther rings on this bearing are later still
            }
            const Vector cand = from + dir * r;
            trace_t      fl   = G_Trace(
                cand + Vector(0, 0, 64), vec_zero, vec_zero, cand - Vector(0, 0, 128), controlledEnt, MASK_PLAYERSOLID,
                qfalse, "BotBlastCovFloor"
            );
            BotNadeBudgetUse(1);
            if (fl.startsolid || fl.allsolid || fl.fraction >= 1.0f || fl.plane.normal[2] < 0.7f) {
                continue;
            }
            const Vector g = fl.endpos;
            if (fabs(g.z - from.z) > 64.0f) {
                continue;
            }
            trace_t run = G_Trace(
                from + Vector(0, 0, STEPSIZE), controlledEnt->mins, runMaxs, g + Vector(0, 0, STEPSIZE), controlledEnt,
                MASK_PLAYERSOLID, qfalse, "BotBlastCovRun"
            );
            BotNadeBudgetUse(1);
            if (run.startsolid || run.allsolid || run.fraction < 0.99f) {
                break; // blocked this way: the farther rings on this bearing are behind the same wall
            }
            // never across the grenade: the closest the run comes to it (from inside 128u, only straight away from it)
            Vector      seg = g - from;
            seg.z           = 0;
            const float sl2 = seg.lengthSquared();
            float       t   = 0.0f;
            if (sl2 > 1.0f) {
                t = Q_clamp_float(((det.x - from.x) * seg.x + (det.y - from.y) * seg.y) / sl2, 0.0f, 1.0f);
            }
            const Vector q  = from + seg * t;
            const float  dq = Vector(det.x - q.x, det.y - q.y, 0).length();
            if (dq < 128.0f && !(dFrom < 128.0f && t <= 0.001f)) {
                continue;
            }
            const float dg = (g - det).length();
            if (pFar && dg > farBest) {
                farBest = dg;
                *pFar   = g;
                bFar    = true;
            }
            if (!bInTime || BotBlastReaches(det, R, g, false, controlledEnt, NULL)) {
                continue;
            }
            found = true;
            best  = tt;
            out   = g;
            if (!pFar) {
                break;
            }
        }
    }
    if (found && travel) {
        *travel = best;
    }
    if (pFar && !bFar) {
        *pFar = vec_zero;
    }
    return found;
}

// the charges the solver tries: the shortest the torso allows (it leaves CHARGE only once MIN_CHARGE_TIME is met: fMin +
// 0.05 margin), then 0.22 / 0.35 of the max. The cook is timed from the frame the torso ENTERS CHARGE_ATTACK_GRENADE
// (State_GrenadeSafe case 2), so charge = f * fMax. The fuse is dmlife * (1 - f): 2.6 / 2.3 / 1.95s.
static int BotSolveCharges(Weapon *w, const BotProjInfo& pi, float *fr, int kind)
{
    const float fMax = w->GetMaxChargeTime(FIRE_PRIMARY);
    const float fMin = w->GetMinChargeTime(FIRE_PRIMARY);
    if (!(pi.flags & P_CHARGE_SPEED) || fMax <= 0.0f) {
        fr[0] = 1.0f;
        return 1;
    }
    const float c[3] = {(fMin + 0.10f) / fMax, 0.22f, 0.35f};
    int         n    = 0;
    float       last = -1.0f;
    const int   nc   = (kind == 0) ? 3 : 2; // a clearing throw is short: the two longest fuses (2.6 / 2.3s)
    for (int i = 0; i < nc; i++) {
        if (c[i] < last + 0.04f) {
            continue;
        }
        fr[n++] = Q_clamp_float(c[i], 0.0f, 1.0f);
        last    = c[i];
    }
    return n;
}

static int BotSolvePitches(int kind, float *p)
{
    int n = 0;
    if (kind == 2) {
        for (int e = 30; e <= 85; e += 5) {
            p[n++] = (float)e; // up through a hatch
        }
    } else if (kind == 3) {
        for (int e = -85; e <= -20; e += 5) {
            p[n++] = (float)e; // down a hatch
        }
    } else {
        const int top = (kind == 1) ? 45 : 70; // (a door: indoors the high lobs only find the ceiling)
        for (int e = -40; e <= top; e += 5) {
            p[n++] = (float)e; // door / lob: flat and downward throws too (at door range the right one is -10..-32)
        }
    }
    return n;
}

void BotController::SolveStart(void)
{
    BotSolveJob& J = m_solve;
    memset(&J, 0, sizeof(J));
    J.state     = 1;
    J.kind      = m_iNadeKind;
    J.target[0] = m_vNadeTarget;
    J.target[1] = m_vNadeTarget2;
    J.gate      = m_vBreachGate;
    J.D         = m_vNadeD;
    J.n         = m_vNadeN;
    J.eye       = controlledEnt->m_vViewPos;
    J.T         = controlledEnt->origin;
    J.startT    = level.inttime;
    J.R         = 450.0f;
    // the AIM LINES (yaws) swept: the target; for a door also straight through its middle, and at the route's doorway
    // point +-15 / +-30 deg - from a spot beside the frame both aim points can cross the wall (m4l2 round 2: 22 of 39
    // door solves found no arc, nearly all "first impact within 96u")
    float cand[7];
    int   nc = 0;
    auto  yawTo = [&](const Vector& p) {
        const Vector d = p - J.eye;
        return (d.x * d.x + d.y * d.y > 1.0f) ? (float)RAD2DEG(atan2(d.y, d.x)) : controlledEnt->angles.y;
    };
    cand[nc++] = yawTo(J.target[0]);
    if (J.kind == 1) {
        cand[nc++]     = yawTo(J.target[1]);
        const float gy = yawTo(J.gate);
        const float off[5] = {0.0f, 15.0f, -15.0f, 30.0f, -30.0f};
        for (int i = 0; i < 5; i++) {
            cand[nc++] = anglemod(gy + off[i]);
        }
    }
    J.nTargets = 0;
    for (int i = 0; i < nc; i++) {
        bool dup = false;
        for (int k = 0; k < J.nTargets; k++) {
            dup = dup || fabs(AngleSubtract(cand[i], J.yaw[k])) < 4.0f;
        }
        if (!dup) {
            J.yaw[J.nTargets++] = cand[i];
        }
    }
}

void BotController::SolveArc(float f, float elev, float yaw, int ti)
{
    BotSolveJob& J = m_solve;
    Weapon      *w = m_pNadeWeapon;
    BotProjInfo  pi;
    J.arcs++;
    if (!w || !BotGetProjInfo(w, pi)) {
        J.rj[11]++;
        return;
    }
    const float g       = sv_gravity->value * (pi.gravity > 0.0f ? pi.gravity : 1.0f);
    const bool  bCharge = (pi.flags & P_CHARGE_SPEED) && w->GetMaxChargeTime(FIRE_PRIMARY) > 0.0f;
    const float v       = bCharge ? pi.minspeed + (pi.speed - pi.minspeed) * f : pi.speed;
    float       life    = (g_gametype->integer != GT_SINGLE_PLAYER && pi.dmlife) ? pi.dmlife : pi.life;
    if (pi.flags & P_CHARGE_LIFE) {
        life *= (1.0f - f);
        if (life < pi.minlife) {
            life = pi.minlife;
        }
    }
    if (v < 50.0f || life < 1.8f) {
        J.rj[0]++;
        return; // too short a fuse to throw and get clear
    }
    Vector fwd;
    AngleVectors(Vector(-elev, yaw, 0), fwd, NULL, NULL);
    const Vector  vel = fwd * v + pi.addvel;
    const Vector& tg  = J.target[0];
    (void)ti;
    BotNadeTrack  tk;
    BotNadeSimEx(
        J.eye, vel, pi.mins, pi.maxs, g, life, controlledEnt, J.kind == 1 ? &J.D : NULL, J.kind == 1 ? &J.n : NULL, tk,
        MASK_PROJECTILE & ~CONTENTS_BODY
    );
    J.traces += tk.steps;
    if (tk.hit) {
        if (tk.hitEnt && tk.hitEnt->IsSubclassOfPlayer()
            && static_cast<Player *>(tk.hitEnt)->GetTeam() == controlledEnt->GetTeam()) {
            J.rj[1]++;
            return; // straight into a team-mate
        }
        if (tk.hitN[2] < -0.3f) {
            J.rj[2]++;
            return; // a ceiling / the top of the frame: it drops back at us
        }
        if ((tk.hitPos - J.eye).lengthXYSquared() < Square(96.0f)) {
            J.rj[3]++;
            return; // the wall / the frame right beside us
        }
        if (J.kind == 1) {
            const Vector tdir(cos(DEG2RAD(yaw)), sin(DEG2RAD(yaw)), 0.0f);
            if (DotProduct2D(tk.hitPos - J.eye, tdir) < DotProduct2D(J.gate - J.eye, tdir) + 24.0f) {
                J.rj[4]++;
                return; // first impact before the doorway: the frame / the wall beside it
            }
        } else if ((J.kind == 2 && tk.hitPos.z < J.gate.z - 24.0f) || (J.kind == 3 && tk.hitPos.z > J.gate.z + 24.0f)) {
            J.rj[4]++;
            return; // not up through / down the hatch
        }
    }
    const Vector det = tk.det;
    if (J.kind == 0) {
        if ((det - tg).lengthXY() > 160.0f || fabs(det.z - tg.z) > 64.0f) {
            J.rj[5]++;
            return; // does not go off by the target, on its floor (an air-burst, or rolled away)
        }
    } else if (J.kind == 1) {
        if (DotProduct(det - J.D, J.n) < 64.0f || tk.recrossed || fabs(det.z - tg.z) > 96.0f) {
            J.rj[5]++;
            return; // must go off INSIDE, on the room's floor (m4l2: a lob that burst 300u over the roof), and never
                    // roll back out through the doorway
        }
    } else if ((J.kind == 2 && det.z < J.gate.z - 24.0f) || (J.kind == 3 && det.z > J.gate.z + 24.0f)) {
        J.rj[5]++;
        return;
    }
    const float err = (det - tg).lengthXY() + fabs(det.z - tg.z) * 0.5f;
    if (BotMateNearBlast2(controlledEnt, det, J.R)) {
        J.rj[6]++;
        return; // a team-mate in its reach (user 2026-09-24)
    }
    if (!BotBlastReaches(det, J.R, J.T, false, controlledEnt, NULL)) {
        if (!J.okT || err < J.errT) {
            J.okT   = true;
            J.errT  = err;
            J.fT    = f;
            J.lifeT = life;
            J.angT  = Vector(-elev, yaw, 0);
            J.detT  = det;
            J.landT = tk.hit ? tk.hitPos : det;
        }
    } else {
        J.rj[7]++; // it would reach us where we stand: only with a cover spot to get to in time (SolveFinish)
        if (!J.okC || err < J.errC) {
            J.okC   = true;
            J.errC  = err;
            J.fC    = f;
            J.lifeC = life;
            J.angC  = Vector(-elev, yaw, 0);
            J.detC  = det;
            J.landC = tk.hit ? tk.hitPos : det;
        }
    }
}

void BotController::SolveFinish(void)
{
    BotSolveJob& J = m_solve;
    Weapon      *w = m_pNadeWeapon;
    J.state        = 3;
    if (J.okT) {
        J.ang       = J.angT;
        J.det       = J.detT;
        J.land      = J.landT;
        J.f         = J.fT;
        J.fuse      = J.lifeT;
        J.S         = J.T;
        J.needCover = false;
        J.state     = 2;
    } else if (J.okC) {
        // it reaches us where we stand: only with blast cover we can get to in the fuse. The grenade leaves the hand
        // ~0.35s after the release and the bot starts moving once it is out, so: fuse - 0.25 margin - 0.2 reaction.
        Vector S;
        float  tt = 0.0f;
        if (FindBlastCover(J.detC, J.R, J.T, J.lifeC - 0.45f, S, &tt) && movement.CanMoveTo(S)) {
            J.ang       = J.angC;
            J.det       = J.detC;
            J.land      = J.landC;
            J.f         = J.fC;
            J.fuse      = J.lifeC;
            J.S         = S;
            J.needCover = true;
            J.state     = 2;
        } else {
            J.rj[8]++;
        }
    }
    J.hold = 0.1f;
    if (J.state == 2 && w) {
        const float fMax = w->GetMaxChargeTime(FIRE_PRIMARY);
        BotProjInfo pi;
        if (fMax > 0.0f && BotGetProjInfo(w, pi) && (pi.flags & P_CHARGE_SPEED)) {
            // the cook, timed from the torso's CHARGE entry (never under the minimum it will release at)
            // the trigger comes up at this charge; the engine reads it ~0.10s later (ReleaseFire runs from the throw
            // animation - round 3: released at 0.65 -> f 0.15), and the torso never lets go before fMin
            J.hold = Q_max(w->GetMinChargeTime(FIRE_PRIMARY), J.f * fMax - 0.10f);
        }
    }
    if (BotNadeProbeOn()) {
        gi.Printf(
            "^~^~^ BOTSOLVE e=%d kind=%d ok=%d cover=%d arcs=%d traces=%d frames=%d ms=%d err=%.0f f=%.2f hold=%.2f "
            "pitch=%.1f det=(%.0f %.0f %.0f) T=(%.0f %.0f %.0f) S=(%.0f %.0f %.0f) R=%.0f tgt=(%.0f %.0f %.0f) "
            "rj=fuse%d mate%d ceil%d face%d gate%d off%d mate2%d exposed%d nocover%d gate=(%.0f %.0f %.0f) n=(%.2f %.2f) "
            "lines=%d yaw=%.1f\n",
            controlledEnt->entnum, J.kind, J.state == 2 ? 1 : 0, J.needCover ? 1 : 0, J.arcs, J.traces, J.frames,
            level.inttime - J.startT, J.state == 2 ? (J.needCover ? J.errC : J.errT) : -1.0f, J.f, J.hold, -J.ang.x,
            J.det.x, J.det.y, J.det.z, J.T.x, J.T.y, J.T.z, J.S.x, J.S.y, J.S.z, J.R, J.target[0].x, J.target[0].y,
            J.target[0].z, J.rj[0], J.rj[1], J.rj[2], J.rj[3], J.rj[4], J.rj[5], J.rj[6], J.rj[7], J.rj[8], J.gate.x,
            J.gate.y, J.gate.z, J.n.x, J.n.y, J.nTargets, J.ang.y
        );
    }
}

void BotController::SolveThink(void)
{
    // (vet C6) up to 12 arcs a frame for this bot, and only while the server frame's trace budget has room for another
    BotSolveJob& J = m_solve;
    if (J.state != 1) {
        return;
    }
    Weapon     *w = m_pNadeWeapon;
    BotProjInfo pi;
    if (!w || !BotGetProjInfo(w, pi)) {
        J.rj[11]++;
        J.state = 3;
        return;
    }
    J.R = BotExplR(pi.explModel);
    J.frames++;
    float     fr[3], pt[24];
    const int nf   = BotSolveCharges(w, pi, fr, J.kind);
    const int np   = BotSolvePitches(J.kind, pt);
    int       arcs = 0;
    // (an arc is <= ~110 toss steps + the reach traces: stop while a whole one still fits under the 1200 cap)
    while (J.state == 1 && arcs < 12 && BotNadeBudgetLeft() >= 200) {
        if (J.fi >= nf) {
            SolveFinish();
            break;
        }
        SolveArc(fr[J.fi], pt[J.pi], J.yaw[J.ti], J.ti);
        arcs++;
        if (++J.pi >= np) {
            J.pi = 0;
            if (++J.ti >= J.nTargets) {
                J.ti = 0;
                J.fi++;
                // a whole charge swept and a safe arc already: no need to throw harder (a lob wants it by the target; a
                // clearing throw only has to go off inside, which every accepted arc does - m4l2: 138-arc sweeps, 0.4s)
                if (J.okT && (J.kind != 0 || J.errT <= 64.0f)) {
                    SolveFinish();
                    break;
                }
            }
        }
    }
    if (J.state == 1 && J.frames > 60) {
        SolveFinish(); // 1.5s of a starved budget: decide on what was swept
    }
}

void BotController::NadeSafeEnd(const char *why)
{
    (void)why;
    const int  now    = level.inttime;
    const bool bDrawn = m_iNadeState != 4
                     || (m_pNadeWeapon && controlledEnt->GetActiveWeapon(WEAPON_MAIN) == m_pNadeWeapon);
    m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    m_iNadeButtons = 0;
    m_iNadeState   = 0;
    m_solve.state  = 0;
    m_iNadeNext    = now + (gi.Cvar_Get("bot_nadeDrill", "0", 0)->integer ? 3000 : 15000 + (int)G_Random(12000.0f));
    if (m_iNadeHoldUntil && movement.GetHoldUntil() == m_iNadeHoldUntil) {
        movement.HoldFor(0); // still our hold (a blast hold set since is left alone)
    }
    m_iNadeHoldUntil = 0;
    m_iNadeMode      = 0;
    m_fNadeHold      = 0;
    if (m_iOpId > 0 && m_iOpSlot < 0) {
        BotBreachOp *op = BotOpFind(m_iOpId);
        if (op && op->phase == BOP_STACK) {
            OpLeave(why ? why : "end"); // [room-clear step 4] the throw is off (or nothing flew): so is the op
        }
    }
    if (bDrawn && !controlledEnt->IsDead()) {
        UseWeaponWithAmmo(); // back to the gun (FindWeaponWithAmmo skips throwables)
    }
}

bool BotController::CheckNadeLobSafe(void)
{
    // [HZM bot B4] a frag at an enemy who just ducked out of sight, now through the solver (kind 0): stop, solve, draw,
    // aim, cook for the solved charge, throw. No arc that goes off by him and misses us = no lob (half of the old lobs
    // could not reach before they went off)
    static cvar_t *s_botNadeThrow = NULL;
    static cvar_t *s_botNadeDrill = NULL;
    if (!s_botNadeThrow) {
        s_botNadeThrow = gi.Cvar_Get("bot_nadeThrow", "1", 0);
        s_botNadeDrill = gi.Cvar_Get("bot_nadeDrill", "0", 0);
    }
    const int now = level.inttime;
    if (!s_botNadeThrow->integer || now < m_iNadeNext) {
        return false;
    }
    m_iNadeNext = now + 1000; // weigh it up once a second
    if (!m_pEnemy || !IsValidEnemy(m_pEnemy) || movement.IsOnLadder() || movement.IsOnElevatorLink()
        || movement.IsInLiftProtocol() || movement.IsHeld() || MovementLocked()) {
        return false;
    }
    const int iUnseen = now - m_iLastSeenTime;
    if (!m_iLastSeenTime || iUnseen < 700 || iUnseen > 4500) {
        return false;
    }
    if (m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 700) {
        // m_iLastSeenTime only moves on frames the bot could SHOOT (fire cone, reaction over): an enemy still in the
        // wide acquisition cone is not hidden - no stopping to lob at someone we can see (the SOLVE would abort anyway)
        return false;
    }
    const float fDist = (m_vLastEnemyPos - controlledEnt->origin).length();
    if (fDist < 380.0f || fDist > 1300.0f
        || (!s_botNadeDrill->integer && G_Random(1.0f) > 0.25f + 0.35f * m_fSkillAggro)) {
        return false;
    }
    Weapon     *w = NULL;
    BotProjInfo pi;
    if (!FindFragGrenade(w) || !BotGetProjInfo(w, pi)) {
        return false;
    }
    Vector  tgt = m_vLastEnemyPos;
    trace_t ft  = G_Trace(
        tgt + Vector(0, 0, 32), vec_zero, vec_zero, tgt - Vector(0, 0, 160), controlledEnt, MASK_SOLID, qfalse,
        "BotLobFloor"
    );
    if (!ft.startsolid && ft.fraction < 1.0f) {
        tgt = ft.endpos;
    }
    if (BotMateNearBlast2(controlledEnt, tgt, BotExplR(pi.explModel))) {
        BotEv(controlledEnt, "nademate", tgt); // [user 2026-09-24] never lob where a team-mate is
        return false;
    }
    m_pNadeWeapon    = w;
    m_vNadeTarget    = tgt;
    m_vNadeTarget2   = tgt;
    m_iNadeKind      = 0;
    m_iNadeMode      = 0;
    m_iNadeState     = 4;
    m_iNadeClobbered = 0;
    m_iNadeStart     = now;
    m_iNadeTime      = now;
    m_iNadeSolveT    = now;
    m_vNadeDet       = vec_zero;
    m_solve.state    = 0;
    movement.HoldFor(600);
    m_iNadeHoldUntil = movement.GetHoldUntil();
    BotEv(controlledEnt, "nadesolve", tgt);
    return true;
}

bool BotController::BreachStartSafe(
    Weapon *w, int kind, const Vector *tl, int nt, const Vector& gate, const Vector& D, const Vector& n
)
{
    // the door / hatch roll said "clear it": stop where we are, solve from here (SOLVE), then draw. The side's "cleared"
    // memory and the callout come with the draw, once there is an arc.
    m_bBreachWant    = false;
    m_pNadeWeapon    = w;
    m_vNadeTarget    = tl[0];
    m_vNadeTarget2   = nt > 1 ? tl[1] : tl[0];
    m_iNadeKind      = kind;
    m_iBreachKind    = kind;
    m_vBreachGate    = gate;
    m_vNadeD         = D;
    m_vNadeN         = n;
    m_vNadeDet       = vec_zero;
    m_iNadeMode      = 1;
    m_iNadeState     = 4;
    m_iNadeClobbered = 0;
    m_iNadeStart     = level.inttime;
    m_iNadeTime      = level.inttime;
    m_iNadeSolveT    = level.inttime;
    m_solve.state    = 0;
    m_iBreachNext    = level.inttime + 12000;
    movement.HoldFor(600);
    m_iNadeHoldUntil = movement.GetHoldUntil();
    BotEv(controlledEnt, "breachsolve", tl[0]);
    return true;
}

void BotController::State_GrenadeSafe(void)
{
    Weapon    *w       = m_pNadeWeapon;
    const int  now     = level.inttime;
    const bool bBreach = (m_iNadeMode == 1);
    m_bWantCrouch      = false; // (vet C8) solved from, and thrown from, the standing eye
    if (!w || controlledEnt->IsDead()) {
        NadeSafeEnd("gone");
        return;
    }
    // ---- aborts only BEFORE the cook. Once cooking, the throw always completes on the solved angles: letting go of a
    // cooking grenade IS a throw, at whatever the view is (the step-1 strays: breachabort_mate at state <= 2)
    if (m_iNadeState == 4 || m_iNadeState == 1) {
        const char *why = NULL;
        if (now - m_iNadeStart > 4000) {
            why = "timeout";
        } else if (m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 300) {
            why = "enemy"; // someone showed up: fight instead
        } else if (now < m_iGrenadeFleeUntil) {
            why = "flee";
        } else if (movement.IsOnLadder() || movement.IsOnElevatorLink() || movement.IsInLiftProtocol()) {
            why = "move";
        } else if (m_iNadeState == 1 && BotMateNearBlast2(controlledEnt, m_vNadeDet, m_solve.R)) {
            why = "mate"; // a team-mate walked into its reach while we drew
        }
        if (why) {
            char ev[48];
            Com_sprintf(ev, sizeof(ev), "%sabort_%s%d", bBreach ? "breach" : "nade", why, m_iNadeState);
            BotEv(controlledEnt, ev, m_vNadeTarget);
            NadeSafeEnd(why);
            return;
        }
    }
    // the thrower stands still from the solve to the release (the arc was solved from exactly here)
    if (movement.GetHoldUntil() < now + 300) {
        movement.HoldFor(600);
        m_iNadeHoldUntil = movement.GetHoldUntil();
    }
    Vector aim = m_vBreachAng;
    switch (m_iNadeState) {
    case 4: // SOLVE - standing still, the gun still up
    {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        aim = Vector(0, (m_vNadeTarget - controlledEnt->origin).toYaw(), 0);
        if (m_solve.state == 0) {
            const bool bDuck = controlledEnt->client && (controlledEnt->client->ps.pm_flags & PMF_DUCKED);
            const bool bStill =
                controlledEnt->groundentity && controlledEnt->velocity.lengthXYSquared() < Square(20.0f);
            if (bDuck || !bStill) {
                if (now - m_iNadeSolveT > 1500) {
                    BotEv(controlledEnt, bBreach ? "breachabort_stance4" : "nadeabort_stance4", m_vNadeTarget);
                    NadeSafeEnd("stance");
                    return;
                }
                break; // still braking / standing up
            }
            SolveStart();
        }
        SolveThink();
        if (m_solve.state == 1) {
            break;
        }
        if (m_solve.state != 2) {
            BotEv(controlledEnt, bBreach ? "breachnoarc" : "nadenoarc", m_vNadeTarget);
            const bool bRetry =
                bBreach && m_iBreachRetry < 2 && (m_vBreachGate - controlledEnt->origin).lengthXY() > 96.0f;
            NadeSafeEnd("noarc");
            if (bRetry) {
                // no clean arc from here: walk on and try again from closer (the step-1 breach did too, to 56u)
                m_iBreachRetry++;
                m_bBreachWant = true;
                m_iBreachNext = now + 800;
            }
            return;
        }
        m_vBreachAng  = m_solve.ang;
        m_vBreachLand = m_solve.land;
        m_fBreachFuse = m_solve.fuse;
        m_fNadeHold   = m_solve.hold;
        m_vNadeDet    = m_solve.det;
        m_vNadeT      = controlledEnt->origin;
        aim           = m_vBreachAng;
        if (controlledEnt->GetActiveWeapon(WEAPON_MAIN) != w) {
            // the DRAW - the decision the players see
            if (bBreach) {
                BotMarkAdd(s_botBreachSpots, 16, s_botSpotSeq, m_vBreachKey, controlledEnt->GetTeam(), 0);
                if (G_Random(1.0f) < 0.4f) {
                    VoiceCallout("*36", 600); // "Get ready to move in on my signal."
                }
                BotEv(
                    controlledEnt, m_iNadeKind == 1 ? "breachdoor" : (m_iNadeKind == 2 ? "breachup" : "breachdown"),
                    m_vNadeDet
                );
                OpForm(); // [room-clear step 4] the team-mates near the door stack for it
            } else {
                BotEv(controlledEnt, "nadethrow", m_vNadeDet);
            }
            controlledEnt->useWeapon(w, WEAPON_MAIN);
        }
        m_iNadeState  = 1;
        m_iNadeTime   = now;
        m_iNadeReadyT = 0;
        break;
    }
    case 1: // DRAW + AIM - precise, on the solved arc before the cook starts
    {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        if ((controlledEnt->origin - m_vNadeT).lengthXYSquared() > Square(24.0f)) {
            // shoved off the spot it was solved from: solve again from here (the grenade stays out)
            BotEv(controlledEnt, "nadeshoved", controlledEnt->origin);
            m_iNadeState  = 4;
            m_solve.state = 0;
            m_iNadeSolveT = now;
            aim           = Vector(0, (m_vNadeTarget - controlledEnt->origin).toYaw(), 0);
            break;
        }
        if (controlledEnt->GetActiveWeapon(WEAPON_MAIN) == w && w->ReadyToFire(FIRE_PRIMARY, qfalse)) {
            if (!m_iNadeReadyT) {
                m_iNadeReadyT = now;
            }
            const Vector va  = controlledEnt->GetViewAngles();
            const float  err = Q_max(fabs(AngleSubtract(va.x, aim.x)), fabs(AngleSubtract(va.y, aim.y)));
            // on the arc (3 deg), or near it (8 deg) after 1.5s of trying - the 4s cap above bounds this
            if (err > 3.0f && !(now - m_iNadeReadyT > 1500 && err < 8.0f)) {
                break;
            }
            m_iNadeState = 2;
            m_iNadeTime  = now;
            m_iNadeCookT = 0;
        } else if (now - m_iNadeTime > 2500) {
            BotEv(controlledEnt, bBreach ? "breachabort_switch1" : "nadeabort_switch1", m_vNadeTarget);
            NadeSafeEnd("switch");
            return;
        }
        break;
    }
    case 2: // COOK for the solved charge - always completes (forced by 4.5s; the overcook is 5.5s)
    {
        m_botCmd.buttons |= BUTTON_ATTACKLEFT;
        // the cook is the ENGINE's charge clock (Sentient::charge_start_time, set when the charge really starts), not
        // the press: a press while the grenade is still coming up is only seen once it is up, and a trigger let go
        // before that throws NOTHING (the first soak: a nadeloose with no grenade behind it - TRAPS T10). The engine
        // reads the cook as level.time - charge_start when it sees the trigger come up, which is NEXT frame.
        const float fCst = controlledEnt->GetChargeStartTime();
        if (!m_iNadeCookT) {
            if (fCst > 0.0f) {
                m_iNadeCookT = now;
            } else if (now - m_iNadeTime > 1500) {
                // it never started cooking: nothing is live in the hand, so letting go is safe
                BotEv(controlledEnt, bBreach ? "breachabort_nocharge2" : "nadeabort_nocharge2", m_vNadeTarget);
                NadeSafeEnd("nocharge");
                return;
            }
        }
        const bool bCooked = fCst > 0.0f && level.time + level.frametime - fCst >= m_fNadeHold - 0.005f;
        if (bCooked || now - m_iNadeTime >= 4500) {
            if (now - m_iNadeTime >= 4500) {
                BotEv(controlledEnt, "nadeforce", m_vNadeTarget);
            }
            m_iNadeState   = 3; // (the trigger comes up next frame)
            m_iNadeTime    = now;
            m_iNadeSpawnId = 0;
            if (bBreach) {
                VoiceCallout("*45", 1500); // "Grenade! Take cover!"
                m_iBreachGoAt = now + Q_min(350 + (int)(m_fBreachFuse * 1000.0f) + 400, 7000);
                BotEv(controlledEnt, "breachthrow", m_vNadeDet);
            } else {
                BotEv(controlledEnt, "nadeloose", m_vNadeTarget);
            }
        }
        break;
    }
    case 3: // RELEASED - keep the aim while the throw animation lets it go
    default:
    {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        // out: its blast mark is posted (BotNadeOnSpawn); CheckGrenadeThreatSafe takes the thrower to cover if it is in
        // reach, CheckFriendlyBlastSafe holds its route out of the blast. Never switch back while the throw animation
        // still runs (it would cut the throw off).
        const bool bThrowAnim = !Q_stricmpn(controlledEnt->GetTorsoStateName(), "RELEASE_ATTACK_GRENADE", 22)
                             || !Q_stricmpn(controlledEnt->GetTorsoStateName(), "CHARGE_ATTACK_GRENADE", 21);
        if ((m_iNadeSpawnId && now - m_iNadeSpawnT >= 100) || now - m_iNadeTime > 2000
            || (!bThrowAnim && now - m_iNadeTime > 1200)) {
            if (!m_iNadeSpawnId) {
                BotEv(controlledEnt, bBreach ? "breachphantom" : "nadephantom", m_vNadeTarget); // released, nothing flew
            }
            NadeSafeEnd("done");
            return;
        }
        break;
    }
    }
    m_vNadeAim     = aim;
    m_iNadeButtons = m_botCmd.buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
    rotation.SetCombatTurn(false);
    rotation.SetTargetAngles(aim);
    if (m_iNadeState != 4) {
        rotation.SetPrecise(true);
    }
}

void BotController::CheckGrenadeThreatSafe(void)
{
    // a live frag whose blast can REACH us (the engine's test, at where it will go off, from its live state and the fuse
    // it has left): go to the nearest spot it cannot reach that we can get to in time and that is not past the grenade
    // (FindBlastCover); none - stop and crouch. Never a blind AvoidPath. A team-mate's grenade cannot hurt us with FF off.
    static cvar_t *s_botNadeFlee = NULL;
    if (!s_botNadeFlee) {
        s_botNadeFlee = gi.Cvar_Get("bot_nadeFlee", "1", 0);
    }
    const int now = level.inttime;
    if (!s_botNadeFlee->integer || now < m_iNadeScanTime || controlledEnt->IsDead()) {
        return;
    }
    m_iNadeScanTime = now + 150;
    if (movement.IsOnLadder() || movement.IsOnElevatorLink() || movement.IsInLiftProtocol()) {
        return;
    }
    const Vector org   = controlledEnt->origin;
    const bool   bDuck = controlledEnt->client && (controlledEnt->client->ps.pm_flags & PMF_DUCKED);
    const int    team  = controlledEnt->GetTeam();
    for (Entity *e = findradius(NULL, org, 640.0f); e; e = findradius(e, org, 640.0f)) {
        if (!BotIsFragProjectile(e)) {
            continue;
        }
        Projectile *pr  = static_cast<Projectile *>(e);
        Entity     *own = pr->GetOwner();
        if (own && own != controlledEnt && own->IsSubclassOfPlayer() && static_cast<Player *>(own)->GetTeam() == team
            && !BotNadeMateReact()) {
            continue;
        }
        Vector det;
        float  R;
        int    until;
        BotLiveNadeDet(pr, det, R, until);
        if (now >= until) {
            continue;
        }
        if (!BotBlastReaches(det, R, org, bDuck, controlledEnt, pr)) {
            // not in its reach - but not walking into it either (a bot grenade's mark does this in CheckFriendlyBlastSafe)
            if (!pr->m_iBotNadeId && movement.PathEntersBlast(det, R + 32.0f, 400.0f, MASK_EXPLOSION, 47.0f)) {
                if (movement.GetHoldUntil() < until + 250) {
                    movement.HoldFor(until + 250 - now);
                }
                m_vAlertPos   = det + Vector(0, 0, 16);
                m_iAlertUntil = now + 300;
                return;
            }
            continue;
        }
        // [room-clear step 5] an ENEMY grenade can reach us: once it has gone off, hold the angle it came from (or push out)
        if (own && own != controlledEnt && own->IsSubclassOfPlayer() && static_cast<Player *>(own)->GetTeam() != team
            && m_iDefendNade != pr->entnum + 1 && BotTeamGate(gi.Cvar_Get("bot_breachDefend", "0", 0)->integer, controlledEnt)) {
            m_iDefendNade = pr->entnum + 1;
            Vector v      = pr->velocity;
            v.z           = 0;
            if (v.lengthSquared() > Square(60.0f)) {
                VectorNormalize2D(v);
                m_vDefendFrom = pr->origin - v * 200.0f; // back along its flight: the way it came in
            } else {
                m_vDefendFrom = pr->origin;
            }
            m_bDefendPush  = m_fSkillAggro > 0.65f;
            m_iDefendAt    = until + 300;
            m_iDefendUntil = m_iDefendAt + 3000 + (int)((1.0f - m_fSkillAggro) * 2000.0f);
            if (BotNadeProbeOn()) {
                gi.Printf(
                    "^~^~^ BOTDEFEND e=%d ev=nade mode=%s from=(%.0f %.0f %.0f) det=(%.0f %.0f %.0f)\n", controlledEnt->entnum,
                    m_bDefendPush ? "push" : "hold", m_vDefendFrom.x, m_vDefendFrom.y, m_vDefendFrom.z, det.x, det.y, det.z
                );
            }
        }
        // on the way to (or on) a spot this blast cannot reach: keep going
        if (now < m_iBlastCoverUntil && m_vBlastCoverSpot != org
            && !BotBlastReaches(det, R, m_vBlastCoverSpot, false, controlledEnt, pr)) {
            return;
        }
        if (m_iNadeState == 2 || m_iNadeState == 3) {
            return; // cooking / just thrown: the throw completes first (a cook is <= ~1.2s)
        }
        if (m_iNadeState) {
            BotEv(controlledEnt, m_iNadeMode == 1 ? "breachabort_threat" : "nadeabort_threat", det);
            NadeSafeEnd("threat");
        }
        // already running (no cover within reach) from this very grenade, still moving: keep running
        if (now < m_iBlastCoverUntil && m_iBlastCoverId == -(pr->entnum + 1) && movement.IsMoving()) {
            return;
        }
        const bool  bFirst = (abs(m_iBlastCoverId) != pr->entnum + 1);
        const float tLeft  = (until - now) / 1000.0f - 0.25f;
        m_iBlastCoverId    = pr->entnum + 1;
        Vector S, F;
        float  tt = 0.0f;
        const bool bCover = tLeft > 0.1f && FindBlastCover(det, R, org, tLeft, S, &tt, &F);
        if (bCover || F != vec_zero) {
            if (movement.GetHoldUntil() > now) {
                movement.HoldFor(0);
            }
            movement.CommitMove(0);
            movement.SetWhy(bCover ? "nadecover" : "naderun"); // [HZM bot probe2]
            movement.MoveTo(bCover ? S : F);
            if (movement.IsMoving()) {
                movement.CommitMove(until - now + 300);
                m_iBlastCoverUntil  = until + 300;
                m_vBlastCoverSpot   = bCover ? S : F;
                m_iGrenadeFleeUntil = until + 300;
                m_iNadeCrouchUntil  = 0;
                if (!bCover) {
                    m_iBlastCoverId = -(pr->entnum + 1); // a run for distance, not cover: see the keep-running check
                }
                VoiceCallout("*45", 6000);
                BotEv(
                    controlledEnt,
                    bCover ? (own == controlledEnt ? "nadecoverown" : "nadecover")
                           : (own == controlledEnt ? "naderunown" : "naderun"),
                    det
                );
                return;
            }
        }
        // boxed in - nowhere to run that is not past the grenade: stop here and get the centroid down behind whatever there is
        movement.CommitMove(0);
        movement.SetWhy("nadecrouch"); // [HZM bot probe2]
        movement.ClearMove();
        if (movement.GetHoldUntil() < until + 300) {
            movement.HoldFor(until + 300 - now);
        }
        m_iNadeCrouchUntil  = until + 300;
        m_iGrenadeFleeUntil = until + 300;
        m_iBlastCoverUntil  = until + 300;
        m_vBlastCoverSpot   = org;
        m_bWantCrouch       = true;
        if (bFirst) {
            VoiceCallout("*45", 6000);
            BotEv(controlledEnt, own == controlledEnt ? "nadecrouchown" : "nadecrouch", det);
        }
        return;
    }
}

void BotController::CheckFriendlyBlastSafe(void)
{
    // a bot grenade's blast mark: never WALK INTO it - a route that enters its reach (the engine's line, from the
    // centroid) holds, crouched and facing it, until it has gone off. Subjects: the thrower (always - "or even move in and
    // get hurt"), the enemy side (it hurts them), team-mates only when it can hurt them (bot_nadeMateReact, FF). Never
    // while the lift protocol is live (vet B3: a bot held in a gate stops the lift for everyone). Taking COVER from one is
    // CheckGrenadeThreatSafe's (it runs first).
    BotMarksMapCheck();
    static cvar_t *s_botBreachWait = NULL;
    if (!s_botBreachWait) {
        s_botBreachWait = gi.Cvar_Get("bot_breachWait", "1", 0);
    }
    const int now = level.inttime;
    if (m_iBreachGoAt && now >= m_iBreachGoAt) {
        m_iBreachGoAt = 0;
        VoiceCallout("*21", 12000); // "Squad, move in!"
        BotEv(controlledEnt, "breachgo", controlledEnt->origin);
        if (!MovementLocked()) {
            movement.CommitMove(0);
        }
    }
    if (!s_botBreachWait->integer || controlledEnt->IsDead() || movement.IsOnLadder()) {
        return;
    }
    if (now < m_iBlastCoverUntil || now < m_iGrenadeFleeUntil) {
        return; // getting out of one: the route to cover starts inside it
    }
    const int team = controlledEnt->GetTeam();
    for (int i = 0; i < 16; i++) {
        const BotNadeMark& m    = s_nadeMarks[i];
        const bool         bOwn = (m.owner == controlledEnt->entnum);
        if (!m.id || (!bOwn && m.team == team && !BotNadeMateReact() && !m.op)) { // ([room-clear] an op's grenade: always)
            continue;
        }
        Vector det;
        float  R;
        int    until;
        if (!BotNadeMarkLive(m, det, R, until, NULL)) {
            continue;
        }
        if (!movement.PathEntersBlast(det, R + 32.0f, 480.0f, MASK_EXPLOSION, 47.0f)) {
            continue;
        }
        if (movement.IsInLiftProtocol()) {
            if (m_iBlastWaitId != -m.id) {
                m_iBlastWaitId = -m.id;
                BotEv(controlledEnt, "blastliftskip", det);
            }
            continue;
        }
        if (movement.GetHoldUntil() < until + 250) {
            movement.HoldFor(until + 250 - now);
        }
        m_bWantCrouch = true;
        m_vAlertPos   = det + Vector(0, 0, 16);
        m_iAlertUntil = now + 300;
        if (m_iBlastWaitId != m.id) {
            m_iBlastWaitId = m.id;
            BotEv(controlledEnt, bOwn ? "blastwaitown" : "breachwait", det);
        }
        return;
    }
}

bool BotController::FindHuntTarget(Vector& out, int& kind)
{
    // [HZM bug-2884] the side's freshest knowledge of the enemy, >600u away: a sighting (30s), else a death (45s), else the
    // Push front line
    const int    team = controlledEnt->GetTeam();
    const Vector org  = controlledEnt->origin;
    int          best = -1, bestT = -1;
    for (int i = 0; i < 32; i++) {
        const BotTeamMark& m = s_botSightings[i];
        if (BotMarkLive(m, team, 30000) && m.t > bestT && (m.pos - org).lengthXYSquared() > Square(600)) {
            best  = i;
            bestT = m.t;
        }
    }
    if (best >= 0) {
        out  = s_botSightings[best].pos;
        kind = 1;
        return true;
    }
    for (int i = 0; i < 32; i++) {
        const BotTeamMark& m = s_botDeaths[i];
        if (BotMarkLive(m, team, 45000) && m.t > bestT && (m.pos - org).lengthXYSquared() > Square(600)) {
            best  = i;
            bestT = m.t;
        }
    }
    if (best >= 0) {
        out  = s_botDeaths[best].pos;
        kind = 2;
        return true;
    }
    // no knowledge at all (the teams took different roads - Desert Road: 90% of seconds the nearest enemies were >3000u
    // apart, each side camping the other's end): go where the ENEMY is heading - their objective node, i.e. our end
    for (int i = attractiveNodes.NumObjects(); i > 0; i--) {
        AttractiveNode *n = attractiveNodes.ObjectAt(i);
        if (n && !n->CheckTeam(controlledEnt) && (n->origin - org).lengthXYSquared() > Square(600)) {
            out  = n->origin;
            kind = 3;
            return true;
        }
    }
    const int iF = BotPushLevelInt("front");
    Vector    c;
    if (iF > 0 && BotLevelZoneCentre(controlledEnt->GetTeam() == TEAM_ALLIES ? iF : iF + 1, c) && (c - org).lengthXYSquared() > Square(600)) {
        out  = c;
        kind = 4;
        return true;
    }
    return false;
}

bool BotController::BreachSuspect(const Vector& p) const
{
    // [HZM bot breach] is someone likely in there? an enemy heard or seen near it lately, or our side died near it
    if (m_iHeardTime && level.inttime - m_iHeardTime < 20000 && (m_vHeardPos - p).lengthSquared() < Square(650)) {
        return true;
    }
    if (m_iLastSeenTime && level.inttime - m_iLastSeenTime < 20000 && (m_vLastEnemyPos - p).lengthSquared() < Square(650)) {
        return true;
    }
    const int team = controlledEnt->GetTeam();
    for (int i = 0; i < 32; i++) {
        if (BotMarkLive(s_botDeaths[i], team, 30000) && (s_botDeaths[i].pos - p).lengthSquared() < Square(600)) {
            return true;
        }
    }
    if (gi.Cvar_Get("bot_breachIntel", "1", 0)->integer) {
        // [room-clear step 3] what the SIDE saw there lately (sightings, and the spots enemies ducked out of sight)
        for (int i = 0; i < 32; i++) {
            if (BotMarkLive(s_botSightings[i], team, 20000) && (s_botSightings[i].pos - p).lengthSquared() < Square(650)) {
                return true;
            }
        }
    }
    return false;
}

bool BotController::CheckBreach(void)
{
    // [HZM bot breach] CLEAR IT FIRST (user 2026-09-24): walking up to an open door, or to the foot / top of a ladder
    // hatch, a bot may stop, grenade the room / the landing beyond it, call it, and step out of the doorway; team-mates
    // in its sight take cover and the rest hold back until it has gone off (CheckFriendlyBlast), then go in. One roll per
    // door / ladder: very likely when the side heard / saw an enemy or lost someone near it (BreachSuspect), otherwise
    // bot_breachChance scaled by personality (indoors / hatches only). A door or ladder cleared by the side in the last
    // 30s is not grenaded again. Needs a frag and a clean simulated arc (SolveThrow).
    static cvar_t *s_botBreach       = NULL;
    static cvar_t *s_botBreachChance = NULL;
    if (!s_botBreach) {
        s_botBreach       = gi.Cvar_Get("bot_breach", "1", 0);
        s_botBreachChance = gi.Cvar_Get("bot_breachChance", "0", 0);
    }
    if (!s_botBreach->integer || level.inttime < m_iBreachNext) {
        return false;
    }
    m_iBreachNext = level.inttime + 300;
    // [room-clear step 3, vet F2] bot_breachIntel: CheckCondition_Attack keeps m_iAttackTime alive while ANY enemy lives, so
    // "no attack state" kept every pursuit (the chase, the attack route, bounds) from ever clearing the door the enemy went
    // through - the bot was in the room before the gate opened. With intel the gate is "no enemy SEEN for 1.5s".
    const bool bIntel = gi.Cvar_Get("bot_breachIntel", "1", 0)->integer != 0;
    const bool bFight = bIntel ? ((m_iLastEnemySeenAny && level.inttime - m_iLastEnemySeenAny < 1500)
                                  || (m_pEnemy && m_iLastSeenTime && level.inttime - m_iLastSeenTime < 1500))
                               : (m_iAttackTime || (m_pEnemy && level.inttime - m_iLastSeenTime < 2500));
    if (!movement.IsMoving() || movement.IsOnLadder() || movement.IsOnElevatorLink() || movement.IsHeld() || bFight
        || level.inttime < m_iGrenadeFleeUntil) {
        return false;
    }
    if (BotNadeSafe() && (movement.IsInLiftProtocol() || MovementLocked())) {
        return false; // [HZM bot room-clear step 2, vet B3] never stop to clear a room from the lift queue / a blast
    }
    const Vector org  = controlledEnt->origin;
    const int    team = controlledEnt->GetTeam();
    int          kind = 0, keyEnt = -1;
    Vector       key, gate, target;
    Vector       nrm(0, 0, 0); // [HZM bot room-clear step 2] the route's way through the doorway (kind 1)

    Vector lf, lt;
    if (movement.GetApproachingLadder(lf, lt) && fabs(lt.z - lf.z) >= 96.0f) {
        Vector ld = lt - lf;
        ld.z      = 0;
        if (ld.lengthSquared() < 1.0f) {
            return false;
        }
        VectorNormalize2D(ld);
        kind   = (lt.z > lf.z) ? 2 : 3;
        key    = lf;
        gate   = lt;
        target = lt + ld * (kind == 2 ? 96.0f : 64.0f); // on over the top edge / on out from the foot
    } else {
        // an OPEN door the route goes through in the next ~260u
        const int *s_doors  = NULL;
        const int  s_nDoors = BotDoorList(s_doors);
        const bool bGapOn   = gi.Cvar_Get("bot_breachGap", "1", 0)->integer != 0; // [room-clear step 3]
        if (!s_nDoors && !bGapOn) {
            return false;
        }
        Vector pts[5];
        pts[0]       = org;
        const int np = 1 + movement.GetPathCorners(pts + 1, 4);
        if (np < 2) {
            return false;
        }
        for (int k = 0; k < s_nDoors && !kind; k++) {
            gentity_t *ge = &g_entities[s_doors[k]];
            if (!ge->inuse || !ge->entity || !ge->entity->IsSubclassOfDoor()) {
                continue;
            }
            Door        *door = static_cast<Door *>(ge->entity);
            const Vector c    = (door->absmin + door->absmax) * 0.5f;
            if (fabs(c.z - (org.z + 40.0f)) > 80.0f || (c - org).lengthXYSquared() > Square(220)) {
                continue;
            }
            static cvar_t *s_dbgD = NULL;
            if (!s_dbgD) {
                s_dbgD = gi.Cvar_Get("bot_breachDebug", "0", 0);
            }
            if (s_dbgD->integer) {
                gi.Printf(
                    "^~^~^ BREACHDOOR e=%d door=%d open=%d closed=%d at=(%.0f %.0f %.0f) d=%.0f\n", controlledEnt->entnum,
                    s_doors[k], door->isOpen() ? 1 : 0, door->isCompletelyClosed() ? 1 : 0, c.x, c.y, c.z, (c - org).lengthXY()
                );
            }
            if (!door->isOpen()) {
                continue; // shut / swinging: BotMovement's A4 wait handles it, and we look again once it is open
            }
            // where does the route pass the door, and how far along it is that?
            float  bestD = 1e9f, bestS = 0.0f, walked = 0.0f;
            int    bestSeg = -1;
            Vector bestQ;
            for (int i = 0; i + 1 < np && walked < 300.0f; i++) {
                const Vector seg = pts[i + 1] - pts[i];
                const float  len = seg.length();
                if (len < 1.0f) {
                    continue;
                }
                const Vector sd = seg * (1.0f / len);
                const float  t  = Q_clamp_float(DotProduct(c - pts[i], sd), 0.0f, len);
                const Vector q  = pts[i] + sd * t;
                const float  dd = (q - c).lengthXY();
                if (dd < bestD) {
                    bestD   = dd;
                    bestS   = walked + t;
                    bestSeg = i;
                    bestQ   = q;
                }
                walked += len;
            }
            if (bestSeg < 0 || bestD > 96.0f || bestS > 260.0f) { // 96: an open leaf sits beside the doorway, not in it
                continue;
            }
            if (bestS < 8.0f && DotProduct2D(c - org, pts[1] - org) < 0.0f) {
                continue; // it is behind us - already through it
            }
            // the target: 260u on along the route past the doorway (straight on past the last corner)
            float  left = 260.0f;
            Vector tp   = bestQ;
            int    si   = bestSeg;
            for (;;) {
                const Vector seg = pts[si + 1] - tp;
                const float  l   = seg.length();
                if (l >= left) {
                    if (l > 0.0f) {
                        tp += seg * (left / l);
                    }
                    break;
                }
                if (si + 2 >= np) { // the corners run out: carry on straight
                    const Vector sd = pts[si + 1] - pts[si];
                    const float  sl = sd.length();
                    tp              = pts[si + 1] + (sl > 1.0f ? sd * ((left - l) / sl) : vec_zero);
                    break;
                }
                left -= l;
                tp = pts[si + 1];
                si++;
            }
            kind   = 1;
            keyEnt = s_doors[k];
            key    = c;
            gate   = bestQ;
            target = tp;
            nrm    = pts[bestSeg + 1] - pts[bestSeg];
            nrm.z  = 0;
            if (nrm.lengthSquared() > 1.0f) {
                VectorNormalize2D(nrm);
            }
        }
        if (!kind && bGapOn) {
            // [room-clear step 3] bot_breachGap: a doorway with no door entity (an arch, an open frame - the T2 detector),
            // only while the side has intel within 900u (no traces otherwise)
            bool bNear = (m_iHeardTime && level.inttime - m_iHeardTime < 20000 && (m_vHeardPos - org).lengthSquared() < Square(900))
                      || (m_iLastSeenTime && level.inttime - m_iLastSeenTime < 20000 && m_vLastEnemyPos != vec_zero
                          && (m_vLastEnemyPos - org).lengthSquared() < Square(900));
            for (int i = 0; i < 32 && !bNear; i++) {
                bNear = (BotMarkLive(s_botSightings[i], team, 20000) && (s_botSightings[i].pos - org).lengthSquared() < Square(900))
                     || (BotMarkLive(s_botDeaths[i], team, 30000) && (s_botDeaths[i].pos - org).lengthSquared() < Square(900));
            }
            if (bNear) {
                BotOpening go;
                memset(&go, 0, sizeof(go));
                bool bGot = false;
                if (m_open.state && DotProduct2D(org - m_open.D, m_open.n) < -24.0f) {
                    go   = m_open;
                    bGot = true;
                } else {
                    bGot = ScanOpening(go) && DotProduct2D(org - go.D, go.n) < -24.0f;
                }
                if (bGot && go.kind == 2 && (go.D - org).lengthXYSquared() < Square(260.0f) && fabs(go.D.z - org.z) < 80.0f) {
                    kind   = 1;
                    keyEnt = -2;
                    key    = go.D;
                    gate   = go.D;
                    target = go.D + go.n * 260.0f;
                    nrm    = go.n;
                }
            }
        }
    }
    if (!kind) {
        return false;
    }
    // put the aim point on the floor out there
    trace_t gt = G_Trace(
        target + Vector(0, 0, 32), vec_zero, vec_zero, target - Vector(0, 0, 160), controlledEnt, MASK_SOLID, qfalse,
        "BotBreachFloor"
    );
    if (!gt.startsolid && gt.fraction < 1.0f) {
        target = gt.endpos;
    } else if (kind == 1) {
        return false;
    }

    // one roll per door / ladder, remembered for 25s
    const bool bSameKey = (keyEnt >= 0) ? (keyEnt == m_iBreachKeyEnt)
                                        : (m_iBreachKeyEnt < 0 && (key - m_vBreachKey).lengthSquared() < Square(96));
    // [room-clear step 3, F4] a "no" rolled with no evidence is rolled again as soon as there is some (the 25s memory kept
    // 15% of known-occupied rooms un-cleared)
    const bool bReroll = bIntel && bSameKey && m_iBreachKeyTime && !m_bBreachWant && !m_bBreachRollSus && BreachSuspect(target);
    if (!bSameKey || !m_iBreachKeyTime || level.inttime - m_iBreachKeyTime > 25000 || bReroll) {
        m_iBreachKeyEnt  = keyEnt;
        m_vBreachKey     = key;
        m_iBreachKeyTime = level.inttime;
        m_iBreachRetry   = 0; // [HZM bot room-clear step 2] a fresh roll: fresh no-arc retries
        bool bCleared    = false;
        for (int i = 0; i < 16; i++) {
            if (BotMarkLive(s_botBreachSpots[i], team, 30000) && (s_botBreachSpots[i].pos - key).lengthSquared() < Square(200)) {
                bool bSeenSince = false;
                for (int j = 0; j < 32 && bIntel && !bSeenSince; j++) {
                    // [room-clear step 3, F5] an enemy seen past it since it was cleared: it is not clear any more
                    bSeenSince = BotMarkLive(s_botSightings[j], team, 20000) && s_botSightings[j].t > s_botBreachSpots[i].t
                              && (s_botSightings[j].pos - target).lengthSquared() < Square(650);
                }
                if (!bSeenSince) {
                    bCleared = true;
                }
            }
        }
        bool bIndoor = kind != 1;
        if (!bIndoor) {
            trace_t ct = G_Trace(
                target + Vector(0, 0, 40), vec_zero, vec_zero, target + Vector(0, 0, 400), controlledEnt, MASK_SOLID,
                qfalse, "BotBreachRoof"
            );
            bIndoor = ct.fraction < 1.0f; // a roof over it: a room, not the street
        }
        // [user 2026-09-24] ONLY on evidence: an enemy seen / heard near that doorway or hatch lately, or our side lost
        // someone there. The chance roll on quiet doors had bots drawing a grenade at every door ("pull out grenades
        // and then just switch back"). bot_breachChance (default 0) keeps the old no-evidence roll for tests.
        const bool  bSus = BreachSuspect(target);
        const float p    = bSus ? 0.85f : (bIndoor ? s_botBreachChance->value * (0.6f + 0.8f * m_fSkillAggro) : 0.0f);
        m_bBreachWant    = !bCleared && G_Random(1.0f) < p;
        m_bBreachRollSus = bSus;
    }
    if (!m_bBreachWant) {
        return false;
    }
    Weapon *w = NULL;
    if (!FindFragGrenade(w)) {
        m_bBreachWant = false;
        return false;
    }
    // aim points, best first: the one above, then (a door) straight on through the middle of the doorway from where we
    // stand at 200 / 140 / 100u deep - a route target round a corner put the throw line across the door frame (test 2:
    // 0 of 6 door solves, rejects mostly frame + ceiling) - or (a hatch) nearer / further onto the landing
    Vector tl[5];
    int    nt = 0;
    tl[nt++]  = target;
    {
        const Vector eye = org + Vector(0, 0, controlledEnt->viewheight);
        Vector       ray = (kind == 1) ? gate - eye : lt - lf;
        ray.z            = 0;
        const float rl   = ray.length();
        if (rl > 8.0f) {
            ray *= 1.0f / rl;
            const float depths[3] = {200.0f, 140.0f, 100.0f};
            const float offs[3]   = {0.0f, 40.0f, 150.0f};
            for (int i = 0; i < 3; i++) {
                Vector tp = (kind == 1) ? Vector(eye.x, eye.y, gate.z) + ray * (rl + depths[i]) : lt + ray * offs[i];
                trace_t ft = G_Trace(
                    tp + Vector(0, 0, 32), vec_zero, vec_zero, tp - Vector(0, 0, 160), controlledEnt, MASK_SOLID, qfalse,
                    "BotBreachFloor2"
                );
                if (!ft.startsolid && ft.fraction < 1.0f) {
                    tl[nt++] = ft.endpos;
                }
            }
        }
    }
    if (BotNadeSafe()) {
        // [HZM bot room-clear step 2] stop here and solve the throw standing still (SOLVE), over the next few frames:
        // the route target, and straight through the middle of the doorway (the first two aim lines above)
        if (kind == 1 && nrm.lengthSquared() < 0.5f) {
            nrm   = gate - org;
            nrm.z = 0;
            if (nrm.lengthSquared() < 1.0f) {
                return false;
            }
            VectorNormalize2D(nrm);
        }
        if (kind == 1
            && !G_SightTrace(
                org + Vector(0, 0, controlledEnt->viewheight), vec_zero, vec_zero, gate + Vector(0, 0, 48), controlledEnt,
                (Entity *)NULL, MASK_SOLID & ~CONTENTS_BODY, qfalse, "BotBreachGateSeen"
            )) {
            // the doorway is not in sight from here (the route reaches it round a corner): no throw can get through it,
            // so do not stop yet - walk on and look again in 300ms, the roll kept (m4l2 round 5: 11 of 14 door solves
            // found no arc, every one with the first impact on the wall beside the thrower)
            return false;
        }
        return BreachStartSafe(w, kind, tl, Q_min(nt, 2), gate, gate, nrm);
    }
    Vector ang, land;
    float  hold, fuse;
    bool   bSolved = false;
    for (int i = 0; i < nt && !bSolved; i++) {
        if (SolveThrow(w, tl[i], kind, gate, ang, hold, land, fuse)) {
            target  = tl[i];
            bSolved = true;
        }
    }
    if (!bSolved) {
        // no clean arc from here: keep walking up and try again from closer, until at the doorway / the ladder foot
        const float dNear = (kind == 1) ? (gate - org).lengthXY() : (key - org).lengthXY();
        if (dNear > 56.0f) {
            return false;
        }
        m_bBreachWant = false;
        BotEv(controlledEnt, "breachnoarc", target);
        return false;
    }
    m_bBreachWant = false;
    if (BotMateNearBlast(controlledEnt, land)) {
        BotEv(controlledEnt, "breachmate", land); // a team-mate is in there / by the landing: no grenade
        return false;
    }
    BotMarkAdd(s_botBreachSpots, 16, s_botSpotSeq, key, team, 0);
    m_pNadeWeapon = w;
    m_vNadeTarget = target;
    m_iBreachKind = kind;
    m_vBreachGate = gate;
    m_vBreachAng  = ang;
    m_vBreachLand = land;
    m_fBreachFuse = fuse;
    m_fNadeHold   = 0; // State_Grenade re-solves once standing still with the grenade out
    m_iNadeMode   = 1;
    m_iNadeState  = 1;
    m_iNadeClobbered = 0;
    m_iNadeStart  = level.inttime;
    m_iNadeTime   = level.inttime;
    m_iBreachNext = level.inttime + 12000;
    movement.HoldFor(5000);
    controlledEnt->useWeapon(w, WEAPON_MAIN);
    if (G_Random(1.0f) < 0.4f) {
        // "Get ready to move in on my signal." - a short per-bot cooldown: VoiceCallout's per-bot gate would otherwise
        // swallow the "Grenade!" and "Move in!" of this same breach (the per-team 7s gate still stops repeats)
        VoiceCallout("*36", 600);
    }
    BotEv(controlledEnt, kind == 1 ? "breachdoor" : (kind == 2 ? "breachup" : "breachdown"), target);
    return true;
}

void BotController::CheckFriendlyBlast(void)
{
    // [HZM bot breach] a team-mate's clearing grenade (s_botBlasts): in its sight within ~380u -> take cover from it (the
    // thrower too, stepping out of the doorway); route about to walk into it -> hold back, crouched, facing it, until it
    // has gone off. The thrower calls "move in" once it has.
    if (BotNadeSafe()) {
        CheckFriendlyBlastSafe(); // [HZM bot room-clear step 2]
        return;
    }
    BotMarksMapCheck();
    static cvar_t *s_botBreachWait = NULL;
    if (!s_botBreachWait) {
        s_botBreachWait = gi.Cvar_Get("bot_breachWait", "1", 0);
    }
    if (m_iBreachGoAt && level.inttime >= m_iBreachGoAt) {
        m_iBreachGoAt = 0;
        VoiceCallout("*21", 12000); // "Squad, move in!"
        BotEv(controlledEnt, "breachgo", controlledEnt->origin);
        movement.CommitMove(0);
    }
    if (!s_botBreachWait->integer || controlledEnt->IsDead() || movement.IsOnLadder()) {
        return;
    }
    const int    team = controlledEnt->GetTeam();
    const Vector eye  = controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight);
    for (int i = 0; i < 16; i++) {
        const BotTeamMark& b = s_botBlasts[i];
        if (!BotMarkLive(b, team, 8000) || level.inttime >= b.until) {
            continue;
        }
        if ((b.pos - controlledEnt->origin).lengthSquared() < Square(380)
            && G_SightTrace(
                b.pos + Vector(0, 0, 16), vec_zero, vec_zero, eye, controlledEnt, (Entity *)NULL, MASK_SOLID, qfalse,
                "BotBlastLOS"
            )) {
            if (m_iBlastCoverId != b.id) {
                m_iBlastCoverId = b.id;
                Vector cover;
                movement.CommitMove(0);
                if (FindCoverPosition(b.pos, cover)) {
                    movement.SetWhy("blastcover"); // [HZM bot probe2]
                    movement.MoveTo(cover);
                } else {
                    Vector away = controlledEnt->origin - b.pos;
                    away.z      = 0;
                    if (away.lengthSquared() < 1.0f) {
                        away = Vector(controlledEnt->orientation[1]);
                    }
                    VectorNormalize2D(away);
                    movement.SetWhy("blastflee"); // [HZM bot probe2]
                    movement.AvoidPath(b.pos, 420.0f, away * 512.0f);
                }
                movement.CommitMove(b.until - level.inttime);
                BotEv(controlledEnt, "breachcover", b.pos);
            }
            return;
        }
        if (movement.PathEntersBlast(b.pos, 360.0f, 420.0f)) {
            if (movement.GetHoldUntil() < b.until + 250) {
                movement.HoldFor(b.until + 250 - level.inttime);
            }
            m_bWantCrouch = true;
            m_vAlertPos   = b.pos + Vector(0, 0, 16);
            m_iAlertUntil = level.inttime + 300;
            if (m_iBlastWaitId != b.id) {
                m_iBlastWaitId = b.id;
                BotEv(controlledEnt, "breachwait", b.pos);
            }
            return;
        }
    }
}

static int s_botCovWhy[6]; // bot_breachDebug tally: low chest-miss / head-blocked / still-seen, corner open / wall-far / no-lean

int BotController::DetectCoverAt(const Vector& pos, const Vector& threat, int& lean) const
{
    // [HZM bot cover] what cover does the floor point `pos` give against an enemy standing at `threat`?
    //  1 LOW    - the engine's own low-cover test (Player::TickCoopCover): chest height (36u) hits a face within 48u
    //             toward the threat and head height (72u) is clear over it - and crouched we really are out of sight
    //  2 CORNER - standing, a wall right beside us blocks the threat; stepped 32u out to one side and leaned (~18u
    //             more) the eye sees it. lean = +1 that side is our right (facing the threat), -1 our left
    lean      = 0;
    Vector td = threat - pos;
    td.z      = 0;
    if (td.lengthSquared() < Square(64)) {
        return 0;
    }
    VectorNormalize2D(td);
    const Vector thEye = threat + Vector(0, 0, 64);

    const Vector chest = pos + Vector(0, 0, 36);
    trace_t      c     = G_Trace(chest, vec_zero, vec_zero, chest + td * 48.0f, controlledEnt, MASK_SOLID, qfalse, "BotCovLowChest");
    if (!c.startsolid && c.fraction < 1.0f && fabs(c.plane.normal[2]) < 0.7f) {
        const Vector head = pos + Vector(0, 0, 72);
        trace_t      h    = G_Trace(head, vec_zero, vec_zero, head + td * 48.0f, controlledEnt, MASK_SOLID, qfalse, "BotCovLowHead");
        if (h.startsolid || h.fraction < 1.0f) {
            s_botCovWhy[1]++;
        } else if (G_SightTrace(
                       pos + Vector(0, 0, 44), vec_zero, vec_zero, thEye, controlledEnt, (Entity *)NULL, MASK_SOLID, qfalse,
                       "BotCovLowHid"
                   )) {
            s_botCovWhy[2]++;
        } else {
            return 1;
        }
    } else {
        s_botCovWhy[0]++;
    }

    const Vector eye = pos + Vector(0, 0, 80);
    trace_t      s   = G_Trace(eye, vec_zero, vec_zero, thEye, controlledEnt, MASK_SOLID, qfalse, "BotCovCorner");
    if (s.startsolid || s.fraction >= 1.0f) {
        s_botCovWhy[3]++;
    } else if ((Vector(s.endpos) - eye).lengthXY() >= 80.0f) {
        s_botCovWhy[4]++;
    }
    if (!s.startsolid && s.fraction < 1.0f && (Vector(s.endpos) - eye).lengthXY() < 80.0f) {
        const Vector right(td.y, -td.x, 0);
        for (int k = 0; k < 2; k++) {
            const int    side = k ? -1 : 1;
            const Vector step = pos + Vector(0, 0, 40);
            trace_t st = G_Trace(step, vec_zero, vec_zero, step + right * (44.0f * side), controlledEnt, MASK_PLAYERSOLID, qfalse, "BotCovStep");
            if (st.fraction < 1.0f) {
                continue; // no room to step out that way
            }
            if (G_SightTrace(
                    eye + right * (50.0f * side) - Vector(0, 0, 6), vec_zero, vec_zero, thEye, controlledEnt, (Entity *)NULL,
                    MASK_SOLID, qfalse, "BotCovLean"
                )) {
                lean = side;
                return 2;
            }
        }
        s_botCovWhy[5]++;
    }
    return 0;
}

void BotController::CoverSteerTo(const Vector& spot, float tol)
{
    // [HZM bot cover] a small positional step toward `spot`, in the view's frame (UpdateBotStates applies it after
    // MoveThink): stepping out to a corner's peek spot, back in to the hide spot, or back onto it after a nudge
    Vector d = spot - controlledEnt->origin;
    d.z      = 0;
    if (d.lengthSquared() <= Square(tol)) {
        m_iCfSteerStall = 0;
        return;
    }
    // can't get any closer (the exact spot is jammed against geometry): stop pushing after 0.4s - pushing a wall reads
    // as a stuck bot (test 5: m5l1a stuck time 2.7% -> 4.8%). Re-armed at every hide / peek switch.
    if (controlledEnt->velocity.lengthXYSquared() < Square(25.0f)) {
        if (!m_iCfSteerStall) {
            m_iCfSteerStall = level.inttime;
        } else if (level.inttime - m_iCfSteerStall > 400) {
            return;
        }
    } else {
        m_iCfSteerStall = 0;
    }
    Vector f, r;
    AngleVectors(Vector(0, controlledEnt->GetViewAngles().y, 0), f, r, NULL);
    const float df = d.x * f.x + d.y * f.y;
    const float dr = d.x * r.x + d.y * r.y;
    const float m  = Q_max(fabs(df), fabs(dr));
    if (m < 1.0f) {
        return;
    }
    m_iCfFwd    = (int)(df / m * 100.0f);
    m_iCfStrafe = (int)(dr / m * 100.0f);
}

bool BotController::StartCoverSession(const Vector& threat, const char *why, const Vector *spot)
{
    // [HZM bot cover] arrived somewhere with cover against the enemy (a bound, a hurt / reload dash, a holder's line):
    // start the hide / peek cycle there. How many peeks before moving on depends on the soldier (bold: 1-2, careful 3-4)
    static cvar_t *s_botCoverFight = NULL;
    if (!s_botCoverFight) {
        s_botCoverFight = gi.Cvar_Get("bot_coverFight", "1", 0);
    }
    if (!s_botCoverFight->integer || m_iCfState >= 2 || movement.IsOnLadder() || movement.IsOnElevatorLink()
        || (Q_stricmp(why, "bound") && level.inttime < m_iCfNoSession)) {
        return false;
    }
    if (Q_stricmp(why, "bound") && MovementLocked()) {
        return false; // [HZM bot room-clear step 2, vet C10] its ClearMove would drop a blast-cover run
    }
    // the intended spot (a bound's target, within reach) or where we stand
    const Vector pos  = (spot && (*spot - controlledEnt->origin).lengthXYSquared() <= Square(80)) ? *spot : controlledEnt->origin;
    int          lean = 0;
    const int    type = DetectCoverAt(pos, threat, lean);
    if (!type) {
        if (Q_stricmp(why, "bound")) {
            m_iCfNoSession = level.inttime + 1000; // no cover here: don't re-trace it every frame (C2 hold calls us per frame)
        }
        return false;
    }
    m_iCfState        = 2;
    m_iCfType         = type;
    m_iCfLean         = lean;
    m_vCfPos          = pos;
    m_iCfCycles       = 0;
    m_iCfMaxCycles    = 1 + (int)G_Random(2.0f) + (int)((1.0f - m_fSkillAggro) * 2.0f);
    m_iCfBlindPeeks   = 0;
    m_iCfExposedSince = 0;
    m_iCfSteerStall   = 0;
    m_bCfBlind        = false;
    m_iCfUntil        = level.inttime + 400 + (int)G_Random(500.0f); // a short first tuck, then the first peek
    m_iCfLastTick     = level.inttime;
    movement.CommitMove(0);
    movement.SetWhy("covsession"); // [HZM bot probe2]
    movement.ClearMove();
    char ev[48];
    Com_sprintf(ev, sizeof(ev), "%s_%s", type == 1 ? "covlow" : (lean > 0 ? "covcornerR" : "covcornerL"), why);
    BotEv(controlledEnt, ev, pos);
    return true;
}

void BotController::EndCoverSession(const char *why)
{
    if (!m_iCfState) {
        return;
    }
    char ev[48];
    Com_sprintf(ev, sizeof(ev), "covout_%s", why);
    BotEv(controlledEnt, ev, controlledEnt->origin);
    if (m_iCfState == 1 && !MovementLocked()) { // [step 2] not a blast-cover run's commit
        movement.CommitMove(0);
    }
    m_iCfState     = 0;
    m_iCfNoSession = level.inttime + 2500; // not straight back into the same spot
}

bool BotController::FindFightCover(const Vector& threat, const Vector& toward, bool bAdvance, Vector& out)
{
    // [HZM bot cover] the next cover spot: ADVANCE = 170-330u ahead toward `toward` (within 60 deg of it), gaining >=100u
    // on it; otherwise (relocating, flanked) 110-200u anywhere round us. Each must be floor with room to stand, a
    // straight run from here, >=300u from the enemy, and LOW or CORNER cover against it (DetectCoverAt). Low cover
    // scores higher (it can be fought from without stepping out). ~15 candidates x ~6 traces, at most every 1.5s.
    const Vector org = controlledEnt->origin;
    Vector       fwd = toward - org;
    fwd.z            = 0;
    const float dTo  = fwd.length();
    if (dTo < 1.0f) {
        fwd = Vector(controlledEnt->orientation[0]);
    }
    const float base      = RAD2DEG(atan2(fwd.y, fwd.x));
    const float advA[5]   = {0.0f, 30.0f, -30.0f, 60.0f, -60.0f};
    const float advR[3]   = {170.0f, 250.0f, 330.0f};
    const float relA[8]   = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 225.0f, 270.0f, 315.0f};
    const float relR[3]   = {60.0f, 130.0f, 200.0f};
    const int   nA        = bAdvance ? 5 : 8;
    const int   nR        = 3;
    Vector      fitMaxs   = controlledEnt->maxs;
    fitMaxs.z             = 54.0f - STEPSIZE;
    bool        found     = false;
    float       best      = 0.0f;
    int         rj[8]     = {0, 0, 0, 0, 0, 0, 0, 0}; // bot_breachDebug: floor, height, fit, run, near, progress, nocover, snapped
    int         nLow = 0, nCorner = 0;
    for (int ai = 0; ai < nA; ai++) {
        for (int ri = 0; ri < nR; ri++) {
            const float  yaw  = DEG2RAD(base + (bAdvance ? advA[ai] : relA[ai]));
            const float  r    = bAdvance ? advR[ri] : relR[ri];
            const Vector cand = org + Vector(cos(yaw) * r, sin(yaw) * r, 0);
            // from well above: on a hillside (Omaha's bluffs) a probe starting 64u up began INSIDE the rising ground and
            // threw the spot away (test 4: 1188 of ~2100 samples lost to the floor probe)
            trace_t      fl   = G_Trace(
                cand + Vector(0, 0, 128), vec_zero, vec_zero, cand - Vector(0, 0, 160), controlledEnt, MASK_PLAYERSOLID,
                qfalse, "BotCovFloor"
            );
            if (fl.startsolid || fl.allsolid || fl.fraction >= 1.0f || fl.plane.normal[2] < 0.7f) {
                rj[0]++;
                continue;
            }
            Vector g = fl.endpos;
            if (fabs(g.z - org.z) > 128.0f) {
                rj[1]++;
                continue;
            }
            {
                // SNAP up to whatever stands between this spot and the enemy (within 96u): cover only works pressed up
                // against it, and a fixed ring almost never lands there (test 2: 0.1% of fight time in cover)
                Vector sd = threat - g;
                sd.z      = 0;
                if (sd.lengthSquared() > 1.0f) {
                    VectorNormalize2D(sd);
                    const Vector c0 = g + Vector(0, 0, 36);
                    trace_t      sn = G_Trace(c0, vec_zero, vec_zero, c0 + sd * 96.0f, controlledEnt, MASK_SOLID, qfalse, "BotCovSnap");
                    if (!sn.startsolid && sn.fraction < 1.0f && fabs(sn.plane.normal[2]) < 0.7f) {
                        const float h = sn.fraction * 96.0f;
                        if (h > 30.0f) {
                            const Vector s2 = g + sd * (h - 26.0f);
                            trace_t      f2 = G_Trace(
                                s2 + Vector(0, 0, 48), vec_zero, vec_zero, s2 - Vector(0, 0, 128), controlledEnt,
                                MASK_PLAYERSOLID, qfalse, "BotCovSnapFloor"
                            );
                            if (!f2.startsolid && f2.fraction < 1.0f && f2.plane.normal[2] >= 0.7f) {
                                g = f2.endpos;
                                rj[7]++;
                            }
                        }
                    }
                }
            }
            // room to stand, checked a step up (the box at the floor clipped any slope: 445 more lost in test 4)
            trace_t fit = G_Trace(
                g + Vector(0, 0, STEPSIZE), controlledEnt->mins, fitMaxs, g + Vector(0, 0, STEPSIZE), controlledEnt,
                MASK_PLAYERSOLID, qfalse, "BotCovFit"
            );
            if (fit.startsolid || fit.allsolid) {
                rj[2]++;
                continue;
            }
            trace_t run = G_Trace(
                org + Vector(0, 0, 40), vec_zero, vec_zero, g + Vector(0, 0, 40), controlledEnt, MASK_PLAYERSOLID, qfalse,
                "BotCovRun"
            );
            if (run.fraction < 0.98f) {
                rj[3]++;
                continue;
            }
            if ((threat - g).lengthXY() < 300.0f) {
                rj[4]++;
                continue; // not into their lap
            }
            float progress = 0.0f;
            if (bAdvance) {
                progress = dTo - (toward - g).lengthXY();
                if (progress < 100.0f) {
                    rj[5]++;
                    continue;
                }
            }
            int       lean = 0;
            const int type = DetectCoverAt(g, threat, lean);
            if (!type) {
                rj[6]++;
                continue;
            }
            if (BotNadeSafe()) {
                // [HZM bot room-clear step 2, vet C10] not into a live grenade's reach (any side's)
                bool bInBlast = false;
                for (int mi = 0; mi < 16 && !bInBlast; mi++) {
                    Vector md;
                    float  mR;
                    int    mu;
                    bInBlast = BotNadeMarkLive(s_nadeMarks[mi], md, mR, mu, NULL)
                            && BotBlastReaches(md, mR, g, false, controlledEnt, NULL);
                }
                if (bInBlast) {
                    rj[6]++;
                    continue;
                }
            }
            (type == 1 ? nLow : nCorner)++;
            const float score = (bAdvance ? progress * 0.5f : 0.0f) - r * 0.3f + (type == 1 ? 60.0f : 0.0f) + G_Random(30.0f);
            if (!found || score > best) {
                found = true;
                best  = score;
                out   = g;
            }
        }
    }
    static cvar_t *s_dbg = NULL;
    if (!s_dbg) {
        s_dbg = gi.Cvar_Get("bot_breachDebug", "0", 0);
    }
    if (s_dbg->integer) {
        gi.Printf(
            "^~^~^ COVERSEARCH e=%d adv=%d found=%d low=%d corner=%d thr=%.0f snapped=%d rj=floor%d height%d fit%d run%d near%d prog%d nocover%d\n",
            controlledEnt->entnum, bAdvance ? 1 : 0, found ? 1 : 0, nLow, nCorner, (threat - org).lengthXY(), rj[7], rj[0], rj[1],
            rj[2], rj[3], rj[4], rj[5], rj[6]
        );
        gi.Printf(
            "^~^~^ COVERWHY e=%d lowChestMiss=%d lowHeadBlocked=%d lowStillSeen=%d cornerOpen=%d cornerWallFar=%d cornerNoLean=%d\n",
            controlledEnt->entnum, s_botCovWhy[0], s_botCovWhy[1], s_botCovWhy[2], s_botCovWhy[3], s_botCovWhy[4], s_botCovWhy[5]
        );
    }
    for (int i = 0; i < 6; i++) {
        s_botCovWhy[i] = 0;
    }
    return found;
}

bool BotController::TryTakeCover(bool bCanSee)
{
    // [HZM bot cover] TAKE COVER ON CONTACT: in a live fight (enemy seen in the last 3s, 350-2600u off) a soldier who is
    // not already fighting from cover looks for some within ~200u - pressed up against something between him and the
    // enemy (FindFightCover snaps the samples to it) - and fights from there (StartCoverSession on arrival). If where he
    // stands is already cover, he fights from it straight away. The boldest now and then just stand and shoot. The
    // badly hurt are left to the Phase 1 cover / C3 retreat that run before this.
    static cvar_t *s_botCoverTake = NULL;
    if (!s_botCoverTake) {
        s_botCoverTake = gi.Cvar_Get("bot_coverTake", "1", 0);
    }
    if (!s_botCoverTake->integer || m_iCfState || level.inttime < m_iCfTakeNext || !m_iLastSeenTime || movement.IsOnLadder()
        || movement.IsOnElevatorLink() || level.inttime - m_iLastSeenTime > 3000 || MovementLocked()) {
        return false;
    }
    m_iCfTakeNext = level.inttime + 2500;
    if (controlledEnt->max_health > 0 && controlledEnt->health < controlledEnt->max_health * 0.35f) {
        return false;
    }
    const Vector org   = controlledEnt->origin;
    const float  fDist = (m_vLastEnemyPos - org).lengthXY();
    if (fDist < 350.0f || fDist > 2600.0f) {
        return false;
    }
    if (StartCoverSession(m_vLastEnemyPos, "take")) {
        return true;
    }
    if (G_Random(1.0f) < 0.25f * m_fSkillAggro) {
        return false;
    }
    Vector spot;
    if (!FindFightCover(m_vLastEnemyPos, org, false, spot)) {
        return false;
    }
    movement.CommitMove(0);
    movement.SetWhy("takecover"); // [HZM bot probe2]
    movement.MoveTo(spot);
    if (!movement.IsMoving() || !movement.PathReaches(spot, 48.0f)) {
        movement.SetWhy("takecoverfail"); // [HZM bot probe2]
        movement.ClearMove();
        return false;
    }
    movement.CommitMove(4000);
    m_iCfState     = 1;
    m_vCfPos       = spot;
    m_iCfMoveUntil = level.inttime + 4000;
    m_iCfLastTick  = level.inttime;
    BotEv(controlledEnt, "takecover", spot);
    return true;
}

bool BotController::TryBound(bool bCanSee)
{
    // [HZM bot cover] BOUNDING: advancing on a known enemy, dash to the next cover spot toward them ("Cover me!"), fight
    // from it for a few peeks (the session), then the next bound. The bold, when they can see the enemy, often just keep
    // shooting and pressing instead (stock advance). Healthy bots only - the hurt ones have the Phase 1 / C3 cover.
    static cvar_t *s_botCoverBound = NULL;
    if (!s_botCoverBound) {
        s_botCoverBound = gi.Cvar_Get("bot_coverBound", "1", 0);
    }
    if (s_botCoverBound->integer && !m_iCfState && !movement.IsOnLadder() && !movement.IsOnElevatorLink() && m_iLastSeenTime
        && !MovementLocked())
    {
        // [T3] BOUNDING OVERWATCH: the buddy is dashing to its next cover right now - stay and cover it (the fight / cover
        // session here goes on), bound once it has arrived
        BotController *bc = BuddyCtl();
        if (bc && bc->m_iCfState == 1) {
            if (level.inttime - m_iOverwatchLogT > 3000) {
                m_iOverwatchLogT = level.inttime;
                if (G_Random(1.0f) < 0.5f) {
                    VoiceCallout("*32", 1500); // "I'll cover you!" (once an episode at most)
                }
                if (BotNadeProbeOn()) {
                    gi.Printf(
                        "^~^~^ BOTBUDDY e=%d mate=%d role=%s ev=overwatch at=(%.0f %.0f %.0f)\n", controlledEnt->entnum,
                        bc->controlledEnt->entnum, m_bBuddyLead ? "lead" : "follow", controlledEnt->origin.x,
                        controlledEnt->origin.y, controlledEnt->origin.z
                    );
                }
            }
            // from cover here if there is some, else stand and shoot while we can see them; blind - carry on as before
            if (StartCoverSession(m_vLastEnemyPos, "overwatch")) {
                return true;
            }
            if (bCanSee) {
                movement.SetWhy("overwatch"); // [HZM bot probe2]
                movement.ClearMove();
                return true;
            }
            return false;
        }
    }
    if (!s_botCoverBound->integer || m_iCfState || level.inttime < m_iCfSearchNext || m_bHolding || movement.IsOnLadder()
        || movement.IsOnElevatorLink() || !m_iLastSeenTime || MovementLocked()) {
        return false;
    }
    m_iCfSearchNext = level.inttime + 1500;
    if (controlledEnt->max_health > 0 && controlledEnt->health < controlledEnt->max_health * 0.45f) {
        return false;
    }
    const float fDist = (m_vLastEnemyPos - controlledEnt->origin).lengthXY();
    if (fDist < 420.0f || fDist > 2400.0f) {
        return false;
    }
    if (bCanSee && G_Random(1.0f) < 0.35f + 0.4f * m_fSkillAggro) {
        return false;
    }
    Vector spot;
    if (!FindFightCover(m_vLastEnemyPos, m_vLastEnemyPos, true, spot)) {
        m_iCfSearchNext = level.inttime + 2500;
        return false;
    }
    movement.CommitMove(0);
    movement.SetWhy("bound"); // [HZM bot probe2]
    movement.MoveTo(spot);
    if (!movement.IsMoving() || !movement.PathReaches(spot, 48.0f)) {
        movement.SetWhy("boundfail"); // [HZM bot probe2]
        movement.ClearMove();
        m_iCfSearchNext = level.inttime + 2500;
        return false;
    }
    movement.CommitMove(5000);
    m_iCfState      = 1;
    m_vCfPos        = spot;
    m_iCfMoveUntil  = level.inttime + 5000;
    m_iCfLastTick   = level.inttime;
    m_iCfSearchNext = level.inttime + 3000;
    if (G_Random(1.0f) < 0.3f) {
        VoiceCallout("*31", 1500); // "Cover me!"
    }
    BotEv(controlledEnt, "bound", spot);
    return true;
}

bool BotController::CoverFight(bool bCanSee, Weapon *pWeap)
{
    // [HZM bot cover] one frame of fighting from cover (user 2026-09-24: "use it to peek and lean ... move from cover
    // to cover"). HIDDEN: tucked in (low: crouched behind it; corner: back behind the edge), no trigger unless they can
    // see us anyway, reload here, and now and then BLIND-FIRE an SMG / MG over the top (the engine's own cover blind
    // fire, when its cover pose has engaged). PEEK: low - stand up over it (+ the engine's cover peek, BUTTON_COOPADS,
    // when its pose engaged); corner - step out 32u and LEAN (UpdateBotStates moves the eye with the lean). The aim /
    // fire code above shoots whenever the eye sees the enemy. Hit hard while peeking: back down early. Flanked (seen
    // while hidden): fight it out, then relocate. After the soldier's quota of peeks, or two peeks that saw nobody: out
    // - the next bound (TryBound) or the stock advance.
    static cvar_t *s_botCoverFight = NULL;
    if (!s_botCoverFight) {
        s_botCoverFight = gi.Cvar_Get("bot_coverFight", "1", 0);
    }
    if (!m_iCfState) {
        return false;
    }
    if (!s_botCoverFight->integer) {
        m_iCfState = 0;
        return false;
    }
    m_iCfLastTick    = level.inttime;
    const Vector org = controlledEnt->origin;

    if (m_iCfState == 1) {
        // bounding: running to the spot. A spot snapped up against its cover usually lies just OFF the navmesh (walls
        // are inset by the agent radius), so the route ends 20-40u short (test 5: 8 of 11 bounds 'failed' there) -
        // arrived = close, or the route has ended near it; CoverSteerTo then walks the last bit up to the cover
        const float fLeft2 = (org - m_vCfPos).lengthXYSquared();
        if (fLeft2 <= Square(40) || (!movement.IsMoving() && fLeft2 <= Square(80))) {
            const Vector spot = m_vCfPos;
            movement.CommitMove(0);
            movement.SetWhy("covarrive"); // [HZM bot probe2]
            movement.ClearMove();
            m_iCfState = 0;
            return StartCoverSession(m_vLastEnemyPos, "bound", &spot);
        }
        if (!movement.IsMoving() || level.inttime > m_iCfMoveUntil) {
            EndCoverSession("boundfail");
            return false;
        }
        return true;
    }

    if ((org - m_vCfPos).lengthXYSquared() > Square(110)) {
        EndCoverSession("moved"); // a grenade flee / a shove took us off the spot
        return false;
    }
    if (m_iLastSeenTime && level.inttime - m_iLastSeenTime > 7000) {
        EndCoverSession("lost");
        return false;
    }
    if (movement.IsMoving()) {
        movement.SetWhy("covhold"); // [HZM bot probe2]
        movement.ClearMove(); // (a committed move - the grenade flee - is not cleared, and ends the session above)
    }
    if (!bCanSee && level.inttime >= m_iAttackStopAimTime) {
        // face where they were: the hide / peek frame, and where the gun comes up
        const Vector a = (m_vLastEnemyPos + Vector(0, 0, 48) - (org + Vector(0, 0, controlledEnt->viewheight))).toAngles();
        rotation.SetTargetAngles(a);
    }
    const bool bLow = (m_iCfType == 1);
    Vector     td   = m_vLastEnemyPos - m_vCfPos;
    td.z            = 0;
    if (td.lengthSquared() > 1.0f) {
        VectorNormalize2D(td);
    }
    const Vector right(td.y, -td.x, 0);
    const Vector peekSpot = bLow ? m_vCfPos : m_vCfPos + right * (32.0f * m_iCfLean);

    if (m_iCfState == 2) {
        // HIDDEN
        if (bLow) {
            m_bWantCrouch = true;
        }
        CoverSteerTo(m_vCfPos, 10.0f);
        if (!bCanSee) {
            m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
            if (m_bCfBlind && (controlledEnt->IsCoopCoverLow() || controlledEnt->IsCoopCoverWall()) && pWeap
                && pWeap->HasAmmoInClip(FIRE_PRIMARY)) {
                m_botCmd.buttons |= BUTTON_ATTACKLEFT;
            }
        }
        if (pWeap) {
            const int iClip = pWeap->GetClipSize(FIRE_PRIMARY);
            if (iClip > 1 && pWeap->ClipAmmo(FIRE_PRIMARY) < iClip * 0.6f) {
                CheckReload(); // top up behind cover, never while peeking
            }
        }
        if (bCanSee) {
            if (!m_iCfExposedSince) {
                m_iCfExposedSince = level.inttime;
            } else if (level.inttime - m_iCfExposedSince > 700 && m_iCfMaxCycles > m_iCfCycles + 1) {
                BotEv(controlledEnt, "covflank", org); // this cover does not cover us from them: fight, then move
                m_iCfMaxCycles = m_iCfCycles + 1;
                m_iCfUntil     = level.inttime;
            }
        } else {
            m_iCfExposedSince = 0;
        }
        if (level.inttime >= m_iCfUntil) {
            m_iCfState      = 3;
            m_iCfSeenInPeek = 0;
            m_iCfSteerStall = 0;
            m_iCfUntil      = level.inttime + (int)((1000.0f + G_Random(1200.0f)) * (0.8f + 0.4f * m_fSkillAggro));
            BotEv(controlledEnt, bLow ? "peek" : (m_iCfLean > 0 ? "leanR" : "leanL"), org);
        }
        return true;
    }

    // PEEKING
    if (bLow) {
        CoverSteerTo(m_vCfPos, 10.0f);
        if (controlledEnt->IsCoopCoverLow() || controlledEnt->IsCoopCoverWall()) {
            m_botCmd.buttons |= BUTTON_COOPADS; // the engine's cover peek pose (stands and aims over the top)
        }
    } else {
        CoverSteerTo(peekSpot, 8.0f);
        if ((org - peekSpot).lengthXYSquared() <= Square(14)) {
            m_botCmd.buttons |= (m_iCfLean > 0) ? BUTTON_LEAN_RIGHT : BUTTON_LEAN_LEFT;
        }
    }
    if (bCanSee) {
        m_iCfSeenInPeek = 1;
    }
    const bool bHitHard = m_iLastPainTime && level.inttime - m_iLastPainTime < 300 && controlledEnt->max_health > 0
                       && controlledEnt->health < controlledEnt->max_health * 0.6f;
    if (level.inttime >= m_iCfUntil || bHitHard) {
        m_iCfCycles++;
        m_iCfBlindPeeks = m_iCfSeenInPeek ? 0 : m_iCfBlindPeeks + 1;
        if (m_iCfExposedSince && m_iCfCycles >= m_iCfMaxCycles) {
            // flanked: move to other cover round here rather than stay seen
            Vector spot;
            EndCoverSession("flanked");
            if (FindFightCover(m_vLastEnemyPos, org, false, spot)) {
                movement.CommitMove(0);
                movement.SetWhy("relocate"); // [HZM bot probe2]
                movement.MoveTo(spot);
                if (movement.IsMoving()) {
                    movement.CommitMove(3000);
                    m_iCfState     = 1;
                    m_vCfPos       = spot;
                    m_iCfMoveUntil = level.inttime + 3000;
                    m_iCfLastTick  = level.inttime;
                    BotEv(controlledEnt, "relocate", spot);
                    return true;
                }
            }
            return false;
        }
        if (m_iCfCycles >= m_iCfMaxCycles || m_iCfBlindPeeks >= 2) {
            EndCoverSession(m_iCfBlindPeeks >= 2 ? "nobody" : "done");
            m_iCfSearchNext = level.inttime; // the next bound may go right away
            return false;
        }
        m_iCfState        = 2;
        m_iCfExposedSince = 0;
        m_iCfSteerStall   = 0;
        m_bCfBlind        = pWeap && !pWeap->IsSemiAuto() && (pWeap->GetWeaponClass() & (WEAPON_CLASS_SMG | WEAPON_CLASS_MG))
                  && (m_vLastEnemyPos - org).lengthSquared() < Square(900) && G_Random(1.0f) < 0.3f + 0.3f * m_fSkillAggro;
        m_iCfUntil = level.inttime + (int)((700.0f + G_Random(1100.0f)) * (1.2f - 0.5f * m_fSkillAggro));
        if (m_bCfBlind) {
            BotEv(controlledEnt, "blindfire", org);
        }
    }
    return true;
}

// ======================================================================================================================
// [user 2026-09-25, team tactics T2] CORNER CHECKS: "All bots should check corners when clearing rooms too in general while
// pathing." Eyes only - the vet ruled out any pace change (a dropped BUTTON_RUN is the SPRINT key under coop_sprint, and a
// real walk sits on the 60u/s "blocked" line: the door stall).
//  T2b DOORWAYS: the route ahead passes through a narrow opening in a wall (a door, an archway, a gap). The two HARD
//      corners just inside it, either side, are where a defender waits. Approaching, look through the opening at the FAR
//      corner (the pie slice that opens up first); crossing, snap to the NEAR one beside the frame; then the far one
//      again. A second bot of the side through the same opening within 3s takes the other corner first.
//  T2a ROUTE CORNERS: within 110u of a route corner that turns >= 35 deg, look along the next leg before arriving.
// The yaw is capped 60 deg off the travel line and the move input is rescaled while the eyes are off it
// (BotMovement::SetLookComp): a bot's unit-length input at 45 deg off its view otherwise runs at ~65% (PM_CmdScale takes
// the largest axis). Never on ladders / lifts / at a closed door (A4's USE needs the body facing it) / held / in a cover
// session / a throw / a bandage / an escape, never while the bot-bot sidestep (view-relative) is working.
// bot_cornerCheck 0 off / 1 on / 2 shadow (detect and log BOTCORNER / BOTCORNEROUT, eyes untouched - the A/B arm).
// ======================================================================================================================
bool BotController::ScanOpening(BotOpening& o)
{
    // Walk the route ahead (the bot + up to 4 corners) in 16u steps. At each sample, trace square to the route both ways
    // at waist height: a DOORWAY is where both sides are closed within 64u by two parallel faces (the jambs - their normals
    // give the wall's own axis, so a diagonal crossing still reads true), and the space opens up again past it (else it is
    // just a corridor). One trace per sample in the open (the second only when the first hit), ~13 per scan. Successive
    // scans sample different offsets as the bot walks on, so a thin wall between two samples is caught on the next scan.
    Vector       pts[5];
    const Vector org = controlledEnt->origin;
    pts[0]           = org;
    const int np     = 1 + movement.GetPathCorners(pts + 1, 4);
    if (np < 2) {
        return false;
    }
    static int s_rj[8]; // bot_cornerDebug tally: 0 samples / 1 one side open / 2 other side open / 3 not a wall /
                        // 4 not parallel jambs / 5 width / 6 no room beyond (corridor) / 7 found
    {
        static int     s_rjT = 0;
        static cvar_t *s_dbg = NULL;
        if (!s_dbg) {
            s_dbg = gi.Cvar_Get("bot_cornerDebug", "0", 0);
        }
        if (level.inttime < s_rjT) {
            s_rjT = level.inttime;
        }
        if (s_dbg->integer && level.inttime - s_rjT >= 30000) {
            s_rjT = level.inttime;
            gi.Printf(
                "^~^~^ BOTOPENDBG samples=%d open1=%d open2=%d notwall=%d notparallel=%d width=%d corridor=%d found=%d\n",
                s_rj[0], s_rj[1], s_rj[2], s_rj[3], s_rj[4], s_rj[5], s_rj[6], s_rj[7]
            );
        }
    }
    const int   mask  = MASK_SOLID & ~CONTENTS_BODY;
    const float kLook = 208.0f;
    const float kSide = 64.0f;
    // phase: where along the first leg the first sample falls - varies as the bot walks, so thin walls get sampled
    int   seg      = 0;
    float segStart = 0.0f; // route distance at pts[seg]
    for (float s = 16.0f; s <= kLook; s += 16.0f) {
        // locate s on the polyline
        Vector a, b, dd;
        float  len = 0.0f;
        for (;;) {
            if (seg + 1 >= np) {
                return false;
            }
            a  = pts[seg];
            b  = pts[seg + 1];
            dd = b - a;
            dd.z = 0;
            len  = dd.length();
            if (s <= segStart + len || seg + 2 >= np) {
                break;
            }
            segStart += len;
            seg++;
        }
        if (len < 1.0f || s > segStart + len + 8.0f) {
            return false; // the route ends here
        }
        if (fabs(b.z - a.z) > 0.3f * len + 8.0f) {
            continue; // stairs / a ramp: no doorway at our level
        }
        const Vector d = dd * (1.0f / len);
        const float  f = Q_clamp_float((s - segStart) / len, 0.0f, 1.0f);
        const Vector P = a + (b - a) * f + Vector(0, 0, 40);
        const Vector left(-d.y, d.x, 0);
        s_rj[0]++;
        trace_t tl = G_Trace(P, vec_zero, vec_zero, P + left * kSide, controlledEnt, mask, qfalse, "BotOpenL");
        if (tl.startsolid || tl.allsolid || tl.fraction >= 1.0f) {
            s_rj[1]++;
            continue;
        }
        trace_t tr = G_Trace(P, vec_zero, vec_zero, P - left * kSide, controlledEnt, mask, qfalse, "BotOpenR");
        if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f) {
            s_rj[2]++;
            continue;
        }
        if (fabs(tl.plane.normal[2]) > 0.3f || fabs(tr.plane.normal[2]) > 0.3f) {
            s_rj[3]++;
            continue;
        }
        // two jambs face each other: their normals are the wall's axis
        const Vector nl(tl.plane.normal[0], tl.plane.normal[1], 0), nr(tr.plane.normal[0], tr.plane.normal[1], 0);
        if (DotProduct2D(nl, nr) > -0.9f) {
            s_rj[4]++;
            continue;
        }
        Vector wt = nr - nl; // ~ from the right jamb toward the left one's side: along the wall
        VectorNormalize2D(wt);
        Vector n(-wt.y, wt.x, 0);
        if (DotProduct2D(n, d) < 0.0f) {
            n = n * -1.0f;
        }
        if (DotProduct2D(n, d) < 0.34f) {
            s_rj[4]++;
            continue; // crossing the gap at more than 70 deg off square: not a doorway we walk through
        }
        const Vector t(n.y, -n.x, 0);
        const Vector pl(tl.endpos), pr(tr.endpos);
        const float  w = fabs(DotProduct2D(pl - pr, t));
        if (w < 28.0f || w > 124.0f) {
            s_rj[5]++;
            continue;
        }
        // the room: 48u on through it, the space opens up on at least one side
        const Vector Q  = (pl + pr) * 0.5f + n * 48.0f;
        trace_t      qa = G_Trace(Q, vec_zero, vec_zero, Q + t * 128.0f, controlledEnt, mask, qfalse, "BotOpenRoom");
        trace_t      qb = G_Trace(Q, vec_zero, vec_zero, Q - t * 128.0f, controlledEnt, mask, qfalse, "BotOpenRoom");
        const bool   bWide =
            !qa.startsolid && !qb.startsolid && (qa.fraction * 128.0f > w * 0.5f + 40.0f || qb.fraction * 128.0f > w * 0.5f + 40.0f);
        if (!bWide) {
            s_rj[6]++;
            continue;
        }
        s_rj[7]++;
        o.w = w;
        o.D = (pl + pr) * 0.5f - Vector(0, 0, 40);
        o.n = n;
        o.t = t;
        // a door entity in it?
        o.kind        = 2;
        const int *dl = NULL;
        const int  nd = BotDoorList(dl);
        for (int k = 0; k < nd; k++) {
            gentity_t *ge = &g_entities[dl[k]];
            if (!ge->inuse || !ge->entity) {
                continue;
            }
            const Vector c = (ge->entity->absmin + ge->entity->absmax) * 0.5f;
            if ((c - o.D).lengthXYSquared() < Square(o.w * 0.5f + 48.0f) && fabs(c.z - (o.D.z + 48.0f)) < 96.0f) {
                o.kind = 1;
                break;
            }
        }
        CornerCorners(o);
        return true;
    }
    return false;
}

void BotController::CornerCorners(BotOpening& o) const
{
    // from just inside the opening (28u, else 56u when the frame is thick), trace along the wall both ways at chest height:
    // the corner is where the side wall is, pulled 24u back toward the door. A side that runs on past 380u is an open hall
    const int mask = MASK_SOLID & ~CONTENTS_BODY;
    for (int k = 0; k < 2; k++) {
        const float sgn = k ? -1.0f : 1.0f;
        o.ok[k]         = false;
        o.c[k]          = -1.0f;
        o.K[k]          = o.D + o.n * 48.0f + o.t * (sgn * 120.0f) + Vector(0, 0, 48);
        for (int j = 0; j < 2; j++) {
            const Vector P  = o.D + o.n * (j ? 56.0f : 28.0f) + Vector(0, 0, 48);
            trace_t      tr = G_Trace(P, vec_zero, vec_zero, P + o.t * (sgn * 400.0f), controlledEnt, mask, qfalse, "BotOpenCorner");
            if (tr.startsolid || tr.allsolid) {
                continue;
            }
            const float c = tr.fraction * 400.0f;
            if (c < o.w * 0.5f + 12.0f) {
                continue; // still inside a thick frame: look from deeper
            }
            o.c[k] = c;
            if (c < 380.0f) {
                o.K[k]  = P + o.t * (sgn * Q_max(c - 24.0f, o.w * 0.5f));
                o.ok[k] = true;
            }
            break;
        }
    }
}

void BotController::CrossBegin(int kind, int aimed, const Vector& at)
{
    if (m_iCrossT) {
        CrossEnd("next");
    }
    m_iCrossT      = level.inttime;
    m_iCrossKind   = kind;
    m_iCrossAim    = aimed;
    m_iCrossId     = (kind == 3) ? ++s_openSeq : m_open.id;
    m_iCrossHurt   = -1;
    m_iCrossAcq    = -1;
    m_fCrossAcqAng = -1.0f;
    m_iCrossKill   = 0;
    if (BotNadeProbeOn() && kind != 3) {
        gi.Printf(
            "^~^~^ BOTCORNER e=%d ev=cross id=%d kind=%d aim=%d at=(%.0f %.0f %.0f) mode=%d\n", controlledEnt->entnum, m_iCrossId,
            kind, aimed, at.x, at.y, at.z, gi.Cvar_Get("bot_cornerCheck", "1", 0)->integer
        );
    }
}

void BotController::CrossEnd(const char *why)
{
    if (!m_iCrossT) {
        return;
    }
    if (BotNadeProbeOn() && controlledEnt) {
        const teamtype_t team = controlledEnt->GetTeam();
        gi.Printf(
            "^~^~^ BOTCORNEROUT e=%d tm=%c id=%d kind=%d aim=%d mode=%d why=%s died=%d hurt=%d acq=%d ang=%.0f kill=%d\n",
            controlledEnt->entnum, team == TEAM_ALLIES ? 'a' : (team == TEAM_AXIS ? 'x' : '?'), m_iCrossId, m_iCrossKind,
            m_iCrossAim, gi.Cvar_Get("bot_cornerCheck", "1", 0)->integer, why, !Q_stricmp(why, "died") ? 1 : 0, m_iCrossHurt,
            m_iCrossAcq, m_fCrossAcqAng, m_iCrossKill
        );
    }
    m_iCrossT = 0;
}

bool BotController::CornerThink(void)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_cornerCheck", "1", 0);
    }
    int mode = s_on->integer;
    if (mode == 3 || mode == 4) {
        // (within-match A/B: one side checks corners, the other runs the detector in shadow)
        mode = (controlledEnt->GetTeam() == (mode == 3 ? TEAM_ALLIES : TEAM_AXIS)) ? 1 : 2;
    }
    if (!mode) {
        m_open.state = 0;
        m_iPieT      = 0;
        return false;
    }
    const int    now = level.inttime;
    const Vector org = controlledEnt->origin;
    // whoever else owns the view or the feet this frame (the tracked opening is kept: a short stop does not forget it)
    const unsigned char appr = movement.GetApproachArea();
    if (!movement.IsMoving() || controlledEnt->GetLadder() || movement.IsOnLadder() || movement.IsOnElevatorLink()
        || movement.IsInLiftProtocol() || movement.IsHeld() || movement.IsWaitingForDoor() || movement.IsStruggling()
        || appr == RECAST_AREA_ELEVATOR || appr == RECAST_AREA_LADDER || m_iNadeState || m_iHealState || m_iCfState
        || MovementLocked() || now < m_iGrenadeFleeUntil || now - movement.GetSepT() < 400) {
        return false;
    }
    BotMarksMapCheck();

    // ---- T2b: the opening being crossed - passed it (150u beyond), strayed off its line, or stale
    if (m_open.state) {
        const Vector r   = org - m_open.D;
        const float  s   = DotProduct2D(r, m_open.n);
        const float  lat = DotProduct2D(r, m_open.t);
        if (!m_open.crossed && s >= 0.0f && fabs(lat) < m_open.w) {
            m_open.crossed = true;
            CrossBegin(m_open.kind, m_open.aimed ? 1 : 0, m_open.D);
        }
        if (s > 150.0f || s < -320.0f || fabs(lat) > 260.0f || fabs(r.z) > 96.0f || now - m_open.t0 > 6000) {
            m_open.state = 0;
        }
    }
    if (!m_open.state && now >= m_iOpenScanNext) {
        m_iOpenScanNext = now + 350 + (controlledEnt->entnum % 7) * 10; // (staggered: ~12 traces a scan)
        BotOpening o;
        memset(&o, 0, sizeof(o));
        if (ScanOpening(o) && DotProduct2D(org - o.D, o.n) < -24.0f) {
            o.state = 1;
            o.t0    = now;
            o.id    = ++s_openSeq;
            // the FAR corner first: the side we are NOT on (the pie slice through the opening opens toward it)
            const float lat = DotProduct2D(org - o.D, o.t);
            int         far = (lat >= 12.0f) ? 1 : ((lat <= -12.0f) ? 0 : (o.c[0] >= o.c[1] ? 0 : 1));
            if (!o.ok[far] && o.ok[1 - far]) {
                far = 1 - far;
            }
            // a team-mate is through this opening right now: take the other corner first
            const int team = controlledEnt->GetTeam();
            int       mate = -1;
            for (int i = 0; i < 16; i++) {
                const BotCornerRec& r = s_cornerRec[i];
                if (r.id && r.team == team && r.t <= now && now - r.t < 3000 && (r.D - o.D).lengthSquared() < Square(64)) {
                    mate = r.first;
                }
            }
            if (mate >= 0 && o.ok[1 - mate]) {
                far = 1 - mate;
            }
            o.first           = far;
            BotCornerRec& rec = s_cornerRec[s_cornerRecSeq % 16];
            rec.D             = o.D;
            rec.team          = team;
            rec.first         = o.first;
            rec.t             = now;
            rec.id            = ++s_cornerRecSeq;
            m_open            = o;
            if (BotNadeProbeOn()) {
                gi.Printf(
                    "^~^~^ BOTCORNER e=%d ev=found id=%d kind=%d D=(%.0f %.0f %.0f) n=(%.2f %.2f) w=%.0f c=%.0f/%.0f ok=%d%d "
                    "first=%d mate=%d s=%.0f mode=%d\n",
                    controlledEnt->entnum, o.id, o.kind, o.D.x, o.D.y, o.D.z, o.n.x, o.n.y, o.w, o.c[0], o.c[1], o.ok[0] ? 1 : 0,
                    o.ok[1] ? 1 : 0, o.first, mate >= 0 ? 1 : 0, DotProduct2D(org - o.D, o.n), mode
                );
            }
        }
    }

    Vector aim;
    bool   bAim = false;
    int   *pAimed = NULL;
    if (m_open.state) {
        // v2 (A/B round 1: looking at the corners the whole way in turned the gun off the room - nearby enemies were first
        // seen a median 47 deg off the view on m4l2, against 6 without): only a corner the eye can SEE now (from outside,
        // the hard corners are behind the wall), and briefly - approaching, the far one through the opening; crossing,
        // the near one for 0.7s, then the far one for 0.5s; the route aim the rest of the time
        const float s = DotProduct2D(org - m_open.D, m_open.n);
        int         k = -1;
        if (s > -200.0f && s < -40.0f) {
            k = m_open.first;
        } else if (s >= -40.0f && s < 80.0f) {
            if (!m_open.nearT) {
                m_open.nearT = now;
            }
            if (now - m_open.nearT < 700) {
                k = 1 - m_open.first;
            } else if (now - m_open.nearT < 1200) {
                k = m_open.first;
            }
        }
        if (k >= 0 && m_open.ok[k]) {
            if (now - m_open.visT[k] >= 200) {
                m_open.visT[k] = now;
                m_open.vis[k]  = G_SightTrace(
                    org + Vector(0, 0, controlledEnt->viewheight), vec_zero, vec_zero, m_open.K[k], controlledEnt, (Entity *)NULL,
                    MASK_SOLID & ~CONTENTS_BODY, qfalse, "BotCornerSeen"
                );
            }
            if (m_open.vis[k]) {
                aim    = m_open.K[k];
                bAim   = true;
                pAimed = &m_open.aimed;
            }
        }
    } else {
        // ---- T2a: a route corner that turns >= 35 deg, within 110u: look along the next leg before getting there
        Vector    cc[3];
        const int nc = movement.GetPathCorners(cc, 3);
        if (m_iPieT && (nc < 1 || (cc[0] - m_vPieCorner).lengthSquared() > Square(24))) {
            // the route moved on past the pied corner: its 3s window starts (if we really are there)
            if ((org - m_vPieCorner).lengthXYSquared() < Square(80)) {
                CrossBegin(3, m_iPieAimed ? 1 : 0, m_vPieCorner);
            }
            m_iPieT = 0;
        }
        if (nc >= 2) {
            Vector d0  = cc[0] - org;
            d0.z       = 0;
            Vector leg = cc[1] - cc[0];
            leg.z      = 0;
            const float l0 = d0.length(), l1 = leg.length();
            if (l0 > 24.0f && l0 < 90.0f && l1 > 48.0f && fabs(cc[1].z - cc[0].z) < 64.0f) {
                d0 *= 1.0f / l0;
                leg *= 1.0f / l1;
                if (DotProduct2D(d0, leg) < 0.819f) { // turns more than 35 deg
                    if (!m_iPieT || (cc[0] - m_vPieCorner).lengthSquared() > Square(24)) {
                        m_vPieCorner = cc[0];
                        m_iPieT      = now;
                        m_iPieAimed  = 0;
                        m_iPieVisT   = 0;
                    }
                    // v2: a BLIND corner only - the next leg hidden from here by what the route turns round - and pie its
                    // EDGE (40u round it), not the open road (round 1: every route corner, 100% of them, cost the fights)
                    if (now - m_iPieVisT >= 250) {
                        m_iPieVisT  = now;
                        m_bPieBlind = !G_SightTrace(
                            org + Vector(0, 0, controlledEnt->viewheight), vec_zero, vec_zero,
                            cc[0] + leg * Q_min(l1, 96.0f) + Vector(0, 0, 48), controlledEnt, (Entity *)NULL,
                            MASK_SOLID & ~CONTENTS_BODY, qfalse, "BotPieBlind"
                        );
                    }
                    if (m_bPieBlind) {
                        aim    = cc[0] + leg * 40.0f + Vector(0, 0, 48);
                        bAim   = true;
                        pAimed = &m_iPieAimed;
                    }
                }
            }
        }
    }
    if (!bAim) {
        return false;
    }
    // not with a closed door just ahead: A4 presses USE along the BODY's facing
    if (BotClosedDoorNear(org, 120.0f)) {
        return false;
    }
    // the eyes: capped 50 deg off the travel line, pitched toward level
    Vector      a      = (aim - (org + Vector(0, 0, controlledEnt->viewheight))).toAngles();
    const float travel = movement.GetCurrentPathDirection().toYaw();
    a.y                = anglemod(travel + Q_clamp_float(AngleSubtract(a.y, travel), -50.0f, 50.0f));
    const float px     = (a.x > 180.0f) ? a.x - 360.0f : a.x;
    a.x                = px * 0.35f;
    a.z                = 0;
    if (mode != 1) {
        return false; // shadow: detected and logged, the eyes are left alone
    }
    rotation.SetTargetAngles(a);
    movement.SetLookComp(400);
    m_iCornerAimT = now;
    if (pAimed) {
        (*pAimed)++;
    }
    return true;
}

void BotController::AimAtPath(void)
{
    if (OpAim() || CornerThink() || BuddySectorAim()) {
        return;
    }
    AimAtAimNode();
}

// ======================================================================================================================
// [user 2026-09-25, team tactics T3] BUDDY PAIRS: "if they have friendlies nearby and have heard or seen an enemy they
// should work together to be tactical, perhaps occasionally moving in formations (back to back)". Per the vet there is NO
// formation movement (a follow point sits in the avoidance lane, re-paths every 32u and makes the follower backpedal): both
// keep their own routes, and the pair only
//   1 splits the aim sectors on the move - the leader looks ahead (and checks corners), the partner takes a flank toward
//     the last intel in slices (70 deg, never the rear: a backpedal runs at 0.72),
//   2 stands BACK TO BACK once both have been holding for 1.5s (at the front, an objective, a wait) - BrainThink,
//   3 alternates the bounds in a fight: while one dashes to its next cover the other stays and covers it (TryBound).
// Pairing: intel (heard / seen in the last 6s, not seeing one now), the nearest unpaired team bot within 350u heading the
// same way (45 deg), and a coin flip ("occasionally"); up to 30s, split beyond 600u, on death / spawn / team change, and
// 15-25s before the next. The lower entnum leads (the ladder and lift queues use the same order). On a lift or ladder the
// pair is suspended, never split. bot_buddy 1 on (default) / 0 off / 2 allies only / 3 axis only (team-split A/B).
// ======================================================================================================================
BotController *BotController::BuddyCtl(void) const
{
    if (!m_pBuddy || !controlledEnt) {
        return NULL;
    }
    Player *p = m_pBuddy;
    if (p->IsDead() || p->IsSpectator() || p->GetTeam() != controlledEnt->GetTeam()) {
        return NULL;
    }
    BotController *bc = botManager.getControllerManager().findController(p);
    if (!bc || (Player *)bc->m_pBuddy != (Player *)controlledEnt) {
        return NULL; // not mutual any more
    }
    return bc;
}

void BotController::BuddySplit(const char *why)
{
    Player        *mate = m_pBuddy;
    BotController *bc   = mate ? botManager.getControllerManager().findController(mate) : NULL;
    if (BotNadeProbeOn() && controlledEnt) {
        gi.Printf(
            "^~^~^ BOTBUDDY e=%d mate=%d role=%s ev=split why=%s dur=%d kills=%d\n", controlledEnt->entnum,
            mate ? mate->entnum : -1, m_bBuddyLead ? "lead" : "follow", why, level.inttime - m_iBuddyT0, m_iBuddyKills
        );
    }
    m_pBuddy       = NULL;
    m_bB2B         = false;
    m_iBuddyStillT = 0;
    m_iBuddyNext   = level.inttime + 15000 + (int)G_Random(10000.0f); // "occasionally"
    if (bc && (Player *)bc->m_pBuddy == (Player *)controlledEnt) {
        bc->BuddySplit("mate"); // (our link is already cleared: no recursion back)
    }
}

bool BotController::BuddyTraveling(Vector& dir)
{
    if (!movement.IsMoving() || controlledEnt->velocity.lengthXYSquared() < Square(60.0f)) {
        return false;
    }
    dir   = movement.GetCurrentPathDirection();
    dir.z = 0;
    if (dir.lengthSquared() < 0.01f) {
        return false;
    }
    VectorNormalize2D(dir);
    return true;
}

void BotController::BuddyThink(void)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_buddy", "1", 0);
    }
    const int  now   = level.inttime;
    const bool bWas  = m_bB2B;
    m_bB2B           = false;
    if (!BotTeamGate(s_on->integer, controlledEnt) || controlledEnt->IsDead()) {
        if (m_pBuddy) {
            BuddySplit("off");
        }
        return;
    }
    BotController *bc = NULL;
    if (m_pBuddy) {
        bc              = BuddyCtl();
        const char *why = NULL;
        if (!bc) {
            why = "gone";
        } else if ((bc->controlledEnt->origin - controlledEnt->origin).lengthSquared() > Square(600.0f)) {
            why = "far";
        } else if (now - m_iBuddyT0 > 30000) {
            why = "time";
        }
        if (why) {
            BuddySplit(why);
            return;
        }
    }
    const unsigned char appr   = movement.GetApproachArea();
    const bool          bQueue = controlledEnt->GetLadder() || movement.IsOnLadder() || movement.IsOnElevatorLink()
                         || movement.IsInLiftProtocol() || appr == RECAST_AREA_ELEVATOR || appr == RECAST_AREA_LADDER;
    if (!m_pBuddy) {
        if (now < m_iBuddyNext || bQueue || m_iNadeState || m_iHealState || m_iCfState) {
            return;
        }
        m_iBuddyNext      = now + 1000 + (controlledEnt->entnum % 5) * 50;
        const bool bIntel = (m_iHeardTime && now - m_iHeardTime < 6000) || (m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 6000);
        if (!bIntel || (m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 1000)) {
            return;
        }
        Vector                            myDir;
        const bool                        bMyMove = BuddyTraveling(myDir);
        BotController                    *best    = NULL;
        float                             bestD   = Square(350.0f);
        const Container<BotController *>& cl      = botManager.getControllerManager().getControllers();
        for (int i = 1; i <= cl.NumObjects(); i++) {
            BotController *o = cl.ObjectAt(i);
            if (o == this || !o->controlledEnt) {
                continue;
            }
            Player *op = o->controlledEnt;
            if (op->IsDead() || op->IsSpectator() || op->GetTeam() != controlledEnt->GetTeam() || o->m_pBuddy || o->m_iNadeState
                || o->m_iHealState || op->GetLadder() || o->movement.IsOnLadder() || o->movement.IsInLiftProtocol()
                || o->movement.IsOnElevatorLink()) {
                continue;
            }
            const float d2 = (op->origin - controlledEnt->origin).lengthSquared();
            if (d2 >= bestD || fabs(op->origin.z - controlledEnt->origin.z) > 96.0f) {
                continue;
            }
            Vector oDir;
            if (bMyMove && o->BuddyTraveling(oDir) && DotProduct2D(myDir, oDir) < 0.707f) {
                continue; // heading somewhere else
            }
            best  = o;
            bestD = d2;
        }
        if (!best) {
            return;
        }
        if (G_Random(1.0f) > 0.5f) {
            m_iBuddyNext = now + 8000; // not this time
            return;
        }
        m_pBuddy                = best->controlledEnt;
        best->m_pBuddy          = controlledEnt;
        m_bBuddyLead            = controlledEnt->entnum < best->controlledEnt->entnum;
        best->m_bBuddyLead      = !m_bBuddyLead;
        m_iBuddyT0 = best->m_iBuddyT0 = now;
        m_iBuddyKills = best->m_iBuddyKills = 0;
        m_iBuddyStillT = best->m_iBuddyStillT = 0;
        m_iBuddySectT = best->m_iBuddySectT = 0;
        m_bBuddyFlank = best->m_bBuddyFlank = false;
        BotController *lead    = m_bBuddyLead ? this : best;
        if (G_Random(1.0f) < 0.5f) {
            lead->VoiceCallout("*33", 1500); // "Follow me!"
        }
        if (BotNadeProbeOn()) {
            gi.Printf(
                "^~^~^ BOTBUDDY e=%d mate=%d role=lead ev=pair d=%.0f at=(%.0f %.0f %.0f)\n", lead->controlledEnt->entnum,
                (lead == this ? best : this)->controlledEnt->entnum, sqrt(bestD), controlledEnt->origin.x,
                controlledEnt->origin.y, controlledEnt->origin.z
            );
        }
        return;
    }
    // ---- paired: BACK TO BACK once both have been standing for 1.5s close together - not in a queue, not fighting
    Player            *mp     = bc->controlledEnt;
    const unsigned char mappr = bc->movement.GetApproachArea();
    const bool          bMateQ = mp->GetLadder() || bc->movement.IsOnLadder() || bc->movement.IsOnElevatorLink()
                         || bc->movement.IsInLiftProtocol() || mappr == RECAST_AREA_ELEVATOR || mappr == RECAST_AREA_LADDER;
    const bool bStill = controlledEnt->velocity.lengthXYSquared() < Square(20.0f) && mp->velocity.lengthXYSquared() < Square(20.0f);
    if (!bStill || bQueue || bMateQ || (mp->origin - controlledEnt->origin).lengthSquared() > Square(250.0f)) {
        m_iBuddyStillT = 0;
        return;
    }
    if (!m_iBuddyStillT) {
        m_iBuddyStillT = now;
        return;
    }
    const bool bQuiet = !(m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 1500) && now >= m_iAttackStopAimTime && !m_iNadeState
                     && !m_iHealState && !m_iCfState;
    if (now - m_iBuddyStillT < 1500 || !bQuiet) {
        return;
    }
    if (m_bBuddyLead) {
        // the leader faces the threat axis: what it is alerted to, else the last intel, else where it looked when it began
        Vector thr;
        bool   bThr = true;
        if (now < m_iAlertUntil) {
            thr = m_vAlertPos;
        } else if (m_vLastEnemyPos != vec_zero) {
            thr = m_vLastEnemyPos;
        } else if (m_iHeardTime && now - m_iHeardTime < 20000) {
            thr = m_vHeardPos;
        } else {
            bThr = false;
        }
        if (bThr && (thr - controlledEnt->origin).lengthXYSquared() > Square(32.0f)) {
            m_fB2BYaw = (thr - controlledEnt->origin).toYaw();
        } else if (!bWas) {
            m_fB2BYaw = controlledEnt->GetViewAngles().y;
        }
    } else {
        if (!bc->m_bB2B) {
            return; // the leader decides the axis
        }
        m_fB2BYaw = anglemod(bc->m_fB2BYaw + 180.0f);
    }
    m_bB2B = true;
    if (!bWas && BotNadeProbeOn() && now - m_iB2BLogT > 5000) {
        m_iB2BLogT = now;
        gi.Printf(
            "^~^~^ BOTBUDDY e=%d mate=%d role=%s ev=b2b yaw=%.0f at=(%.0f %.0f %.0f)\n", controlledEnt->entnum, mp->entnum,
            m_bBuddyLead ? "lead" : "follow", m_fB2BYaw, controlledEnt->origin.x, controlledEnt->origin.y, controlledEnt->origin.z
        );
    }
}

bool BotController::BuddySectorAim(void)
{
    // the partner, on the move: a flank slice (1.5-2.5s at 70 deg toward the last intel), then a look ahead (1.2-2s)
    if (!m_pBuddy || m_bBuddyLead) {
        return false;
    }
    BotController *bc = BuddyCtl();
    if (!bc) {
        return false;
    }
    const int    now = level.inttime;
    const Vector org = controlledEnt->origin;
    Vector       dir;
    if (!BuddyTraveling(dir) || (bc->controlledEnt->origin - org).lengthSquared() > Square(400.0f)) {
        return false; // standing, or catching up
    }
    const unsigned char appr = movement.GetApproachArea();
    if (controlledEnt->GetLadder() || movement.IsOnLadder() || movement.IsOnElevatorLink() || movement.IsInLiftProtocol()
        || movement.IsHeld() || movement.IsWaitingForDoor() || movement.IsStruggling() || appr == RECAST_AREA_ELEVATOR
        || appr == RECAST_AREA_LADDER || m_iNadeState || m_iHealState || m_iCfState || MovementLocked()
        || now < m_iGrenadeFleeUntil || now - movement.GetSepT() < 400
        || (m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 1500)) {
        return false;
    }
    if (now >= m_iBuddySectT) {
        m_bBuddyFlank = !m_bBuddyFlank;
        m_iBuddySectT = now + (m_bBuddyFlank ? 1500 + (int)G_Random(1000.0f) : 1200 + (int)G_Random(800.0f));
        if (m_bBuddyFlank) {
            Vector intel;
            bool   bI = true;
            if (m_iHeardTime && now - m_iHeardTime < 10000) {
                intel = m_vHeardPos;
            } else if (m_vLastEnemyPos != vec_zero) {
                intel = m_vLastEnemyPos;
            } else if (bc->m_iHeardTime && now - bc->m_iHeardTime < 10000) {
                intel = bc->m_vHeardPos;
            } else {
                bI = false;
            }
            if (bI) {
                const Vector to = intel - org;
                m_iBuddySide    = (dir.x * to.y - dir.y * to.x > 0.0f) ? -1 : 1; // left of the travel line = -1
            } else {
                m_iBuddySide = -m_iBuddySide;
            }
        }
    }
    if (!m_bBuddyFlank || BotClosedDoorNear(org, 120.0f)) {
        return false;
    }
    rotation.SetTargetAngles(Vector(0, anglemod(dir.toYaw() - m_iBuddySide * 70.0f), 0));
    movement.SetLookComp(400);
    m_iCornerAimT = now;
    return true;
}

// ======================================================================================================================
// [user 2026-09-25, room-clear steps 3-5] STACK AND CLEAR: "they should be aware of room clearing when they are near the
// door that a grenade is going into and work together after the nade goes off to secure the room together."
// Step 4, the team op (the vet: the pair IS the smallest op; S4-1..S4-4): when a bot draws a clearing grenade at a doorway,
// up to two team-mates within 700u whose route goes through it (or who are within 450u) STACK beside it - on the
// thrower's side, each on the jamb of its own side (never across the doorway mouth: that is the throw line), 56u back
// from the frame, on a spot the blast cannot reach (the engine's line, crouched) - eyes on the far edge of the opening. When
// the grenade has gone off (+300ms) they go in one after the other, 600ms apart, each crossing to the far side and
// clearing the corner there (the T2 corners); the thrower goes last. Caps: a stacker that cannot reach its spot in 5s
// holds where it is (out of the blast) or drops out; every op is over 14s after it formed (the 2026-09-24 door stall). Its
// grenade makes the side's routes hold back from the blast whatever g_teamdamage says. bot_roomClear 0 off / 1 on.
// Step 3 (bot_breachIntel): the door roll also reads what the SIDE saw (sightings, and the spot an enemy ducked out of
// sight - VANISH), a "no" rolled without evidence is re-rolled once evidence arrives, a door the side cleared is fair game
// again once an enemy is seen past it, and a bot that lost sight of its enemy may clear the door he went through (the
// attack gate is "no enemy SEEN for 1.5s", not "no attack state" - vet F2). bot_breachGap: doorways without a door entity
// too (arches, open frames: the T2 detector).
// Step 5 (bot_breachDefend): a bot an ENEMY grenade came at holds the angle it came from for 3-5s after the blast; the
// bold ones push out toward it instead.
// ======================================================================================================================
static bool BotRoomClearOn(const Player *p)
{
    static cvar_t *s = NULL;
    if (!s) {
        s = gi.Cvar_Get("bot_roomClear", "1", 0);
    }
    return BotTeamGate(s->integer, p);
}

static BotBreachOp *BotOpFind(int id)
{
    if (!id) {
        return NULL;
    }
    for (int i = 0; i < 8; i++) {
        if (s_ops[i].id == id) {
            return &s_ops[i];
        }
    }
    return NULL;
}

static void BotOpLog(const BotBreachOp& op, int e, const char *role, const char *ev, const char *why, int slot)
{
    if (!BotNadeProbeOn()) {
        return;
    }
    gi.Printf(
        "^~^~^ BOTOP op=%d e=%d role=%s ev=%s why=%s slot=%d phase=%d dur=%d D=(%.0f %.0f %.0f) w=%.0f nm=%d\n", op.id, e, role, ev,
        why, slot, op.phase, level.inttime - op.t0, op.D.x, op.D.y, op.D.z, op.w, op.nm
    );
}

// can a stacker stand here: floor under it (snapped), the body fits, on the near side, and the blast cannot reach it crouched
static bool BotStackSpotOk(Vector& spot, const BotBreachOp& op, Entity *self)
{
    trace_t fl = G_Trace(
        spot + Vector(0, 0, 48), vec_zero, vec_zero, spot - Vector(0, 0, 96), self, MASK_PLAYERSOLID, qfalse, "BotStackFloor"
    );
    if (fl.startsolid || fl.allsolid || fl.fraction >= 1.0f || fl.plane.normal[2] < 0.7f) {
        return false;
    }
    const Vector g = fl.endpos;
    if (fabs(g.z - op.D.z) > 48.0f || DotProduct2D(g - op.D, op.n) > -24.0f) {
        return false;
    }
    const Vector gb = g + Vector(0, 0, 1);
    trace_t      bx = G_Trace(gb, self->mins, self->maxs, gb, self, MASK_PLAYERSOLID, qtrue, "BotStackBox");
    if (bx.startsolid || bx.allsolid) {
        return false;
    }
    if (BotBlastReaches(op.det, op.R, g, true, self, NULL)) {
        return false;
    }
    spot = g;
    return true;
}

// the entry point for a stacker from side s: across the doorway into the far half of the room, floor under it and a
// straight walk from the doorway (else straight in; else none - it just walks its own route in)
static Vector BotEntryPoint(const BotBreachOp& op, int s, Entity *self)
{
    const float lat[2] = {Q_min(48.0f, op.w * 0.4f), 0.0f};
    for (int i = 0; i < 2; i++) {
        const Vector c  = op.D + op.n * 104.0f - op.t * (s * lat[i]);
        trace_t      fl = G_Trace(
            c + Vector(0, 0, 48), vec_zero, vec_zero, c - Vector(0, 0, 96), self, MASK_PLAYERSOLID, qfalse, "BotEntryFloor"
        );
        if (fl.startsolid || fl.allsolid || fl.fraction >= 1.0f || fl.plane.normal[2] < 0.7f || fabs(fl.endpos[2] - op.D.z) > 32.0f) {
            continue;
        }
        const Vector a  = op.D + Vector(0, 0, 18);
        const Vector b  = Vector(fl.endpos) + Vector(0, 0, 18);
        trace_t      wk = G_Trace(
            a, Vector(-15, -15, 0), Vector(15, 15, 54), b, self, MASK_PLAYERSOLID & ~CONTENTS_BODY, qtrue, "BotEntryWalk"
        ); // (a waist-high box: a doorway lintel at 96u must not read as a wall)
        if (wk.startsolid || wk.fraction < 1.0f) {
            continue;
        }
        return fl.endpos;
    }
    return vec_zero;
}

bool BotController::OpForm(void)
{
    if (!BotRoomClearOn(controlledEnt) || m_iOpId || m_iNadeKind != 1 || m_iNadeMode != 1) {
        return false;
    }
    BotMarksMapCheck();
    const int    now  = level.inttime;
    const int    team = controlledEnt->GetTeam();
    int          live = 0;
    BotBreachOp *free = NULL;
    for (int i = 0; i < 8; i++) {
        BotBreachOp& o = s_ops[i];
        if (o.id && (o.phase >= BOP_DONE || o.t0 > now || now - o.t0 > 15000)) {
            o.id = 0; // over / stale
        }
        if (!o.id) {
            if (!free) {
                free = &o;
            }
            continue;
        }
        if (o.team == team) {
            live++;
            if ((o.D - m_vNadeD).lengthXYSquared() < Square(160.0f)) {
                return false; // this door already has one
            }
        }
    }
    if (!free || live >= 4) {
        return false;
    }
    BotBreachOp& op = *free;
    memset(&op, 0, sizeof(op));
    // the doorway: the T2 detector along the route when it finds this door, else the breach gate and the route's way in
    BotOpening o;
    memset(&o, 0, sizeof(o));
    if (ScanOpening(o) && (o.D - m_vNadeD).lengthXYSquared() < Square(128.0f)) {
        op.D = o.D;
        op.n = o.n;
        op.t = o.t;
        op.w = o.w;
    } else {
        op.D = m_vNadeD;
        op.n = m_vNadeN;
        op.n.z = 0;
        if (op.n.lengthSquared() < 0.25f) {
            return false;
        }
        VectorNormalize2D(op.n);
        op.t   = Vector(op.n.y, -op.n.x, 0);
        op.w   = 64.0f;
        o.D    = op.D;
        o.n    = op.n;
        o.t    = op.t;
        o.w    = op.w;
        CornerCorners(o);
    }
    // a door entity in the opening? (its leaf swings through the space right behind the frame)
    bool bDoor = false;
    {
        const int *dl = NULL;
        const int  nd = BotDoorList(dl);
        for (int k = 0; k < nd && !bDoor; k++) {
            gentity_t *ge = &g_entities[dl[k]];
            if (ge->inuse && ge->entity) {
                const Vector c = (ge->entity->absmin + ge->entity->absmax) * 0.5f;
                bDoor          = (c - op.D).lengthXYSquared() < Square(op.w * 0.5f + 64.0f) && fabs(c.z - (op.D.z + 48.0f)) < 96.0f;
            }
        }
    }
    op.K[0]    = o.K[0];
    op.K[1]    = o.K[1];
    op.ok[0]   = o.ok[0];
    op.ok[1]   = o.ok[1];
    op.id      = ++s_opSeq;
    op.team    = team;
    op.phase   = BOP_STACK;
    op.t0      = now;
    op.tPhase  = now;
    op.thrower = controlledEnt->entnum;
    op.det     = m_vNadeDet != vec_zero ? m_vNadeDet : op.D + op.n * 200.0f;
    op.R       = m_solve.R > 0.0f ? m_solve.R : 450.0f;
    m_iOpId    = op.id;
    m_iOpSlot  = -1;
    BotOpLog(op, controlledEnt->entnum, "thrower", "form", "draw", -1);

    // recruit: the buddy first, then the nearest - team bots within 700u of the door, on the near side, not fighting,
    // not busy, whose route goes through it (or within 450u)
    const Container<BotController *>& cl = botManager.getControllerManager().getControllers();
    for (int pass = 0; pass < 2 && op.nm < 2; pass++) {
        BotController *best  = NULL;
        float          bestD = Square(700.0f);
        for (int i = 1; i <= cl.NumObjects(); i++) {
            BotController *c = cl.ObjectAt(i);
            if (!c || c == this || !c->controlledEnt) {
                continue;
            }
            Player *cp = c->controlledEnt;
            if (cp->IsDead() || cp->IsSpectator() || cp->GetTeam() != team || c->m_iOpId || c->m_iNadeState || c->m_iHealState
                || c->m_iCfState || cp->GetLadder() || c->movement.IsOnLadder() || c->movement.IsOnElevatorLink()
                || c->movement.IsInLiftProtocol() || (c->m_iLastEnemySeenAny && now - c->m_iLastEnemySeenAny < 1500)
                || now < c->m_iGrenadeFleeUntil || now < c->m_iBlastCoverUntil) {
                continue;
            }
            const Vector r  = cp->origin - op.D;
            const float  d2 = r.lengthXYSquared();
            if (d2 >= bestD || fabs(r.z) > 96.0f || DotProduct2D(r, op.n) > -16.0f) {
                continue; // too far, another floor, or already in the room / in the doorway
            }
            if (d2 > Square(450.0f) && !c->movement.PathEntersBlast(op.D, 160.0f, 500.0f)) {
                continue; // not coming this way
            }
            const bool bBuddy = (Player *)m_pBuddy == cp && c->BuddyCtl() == this;
            if (pass == 0 && !bBuddy) {
                continue;
            }
            best  = c;
            bestD = d2;
        }
        if (!best) {
            continue;
        }
        pass--; // (the same pass again: maybe a second one)
        // its spot: on the jamb of ITS side, 56u back (then 112), 36u out from the frame (then 60)
        Player     *bp  = best->controlledEnt;
        const float lat = DotProduct2D(bp->origin - op.D, op.t);
        const int   s   = (lat >= 0.0f) ? 1 : -1;
        int         onSide = 0;
        for (int k = 0; k < op.nm; k++) {
            onSide += (op.side[k] == s) ? 1 : 0;
        }
        Vector spot;
        bool   ok = false;
        for (int j = 0; j < 4 && !ok; j++) {
            // (a DOOR: 96u+ back, out of the leaf's swing from either jamb - rc1 soak: stackers 56u back jammed a
            // rotating door and blocked the bots behind them 68 times vs 6)
            const float base  = bDoor ? 96.0f : 56.0f;
            const float depth = (onSide ? base + 56.0f : base) + ((j & 2) ? 56.0f : 0.0f);
            const float out   = op.w * 0.5f + ((j & 1) ? 60.0f : 36.0f);
            spot              = op.D - op.n * depth + op.t * (s * out);
            ok                = BotStackSpotOk(spot, op, bp);
        }
        if (!ok) {
            // no spot on its side: if where it stands is out of the blast and on the near side, it stacks there
            spot = bp->origin;
            ok   = !BotBlastReaches(op.det, op.R, spot, false, bp, NULL);
        }
        if (!ok) {
            best->m_iOpId = -1; // (skip it for the rest of this recruitment)
            continue;
        }
        const int k    = op.nm++;
        op.mem[k]      = bp->entnum;
        op.side[k]     = s;
        op.spot[k]     = spot;
        op.state[k]    = 0;
        op.E[k]        = BotEntryPoint(op, s, bp);
        best->m_iOpId   = op.id;
        best->m_iOpSlot = k;
        best->m_iOpMoveT = now;
        best->movement.CommitMove(0);
        best->movement.SetWhy("stack"); // [HZM bot probe2]
        best->movement.MoveTo(spot);
        BotOpLog(op, bp->entnum, "stack", "recruit", best == this ? "self" : ((Player *)m_pBuddy == bp ? "buddy" : "near"), k);
    }
    for (int i = 1; i <= cl.NumObjects(); i++) {
        BotController *c = cl.ObjectAt(i);
        if (c && c->m_iOpId == -1) {
            c->m_iOpId = 0; // (the skip marks)
        }
    }
    if (op.nm && G_Random(1.0f) < 0.5f) {
        VoiceCallout("*21", 600); // "Squad, move in!" - on the door
    }
    return true;
}

void BotController::OpThrown(float life, const Vector& det)
{
    BotBreachOp *op = BotOpFind(m_iOpId);
    if (!op || m_iOpSlot >= 0 || op->phase != BOP_STACK) {
        return;
    }
    op->phase  = BOP_BLAST;
    op->tPhase = level.inttime;
    op->detAt  = level.inttime + (int)(life * 1000.0f);
    op->det    = det;
    BotOpLog(*op, controlledEnt->entnum, "thrower", "thrown", "out", -1);
}

void BotController::OpLeave(const char *why)
{
    BotBreachOp *op   = BotOpFind(m_iOpId);
    const int    slot = m_iOpSlot;
    m_iOpId           = 0;
    m_iOpSlot         = -1;
    if (op) {
        if (slot >= 0 && slot < 2) {
            op->state[slot] = 3;
        } else if (op->phase == BOP_STACK) {
            op->phase = BOP_DONE; // the thrower dropped out before the throw: the op is off
            BotOpLog(*op, controlledEnt ? controlledEnt->entnum : -1, "thrower", "abort", why, -1);
        }
        BotOpLog(*op, controlledEnt ? controlledEnt->entnum : -1, slot >= 0 ? "stack" : "thrower", "leave", why, slot);
    }
    if (slot >= 0 && controlledEnt) {
        if (movement.GetHoldUntil() <= level.inttime + 300) {
            movement.HoldFor(0); // our 300ms stack hold (a longer blast hold is left alone)
        }
        if (!MovementLocked()) {
            movement.CommitMove(0);
        }
    }
}

void BotController::OpThink(void)
{
    if (!m_iOpId) {
        return;
    }
    BotBreachOp *op  = BotOpFind(m_iOpId);
    const int    now = level.inttime;
    if (!op || op->phase >= BOP_DONE || op->t0 > now || !BotRoomClearOn(controlledEnt) || controlledEnt->IsDead()) {
        OpLeave(op ? "over" : "gone");
        return;
    }
    // the op's clock (whoever thinks first this frame moves it on)
    if (now - op->t0 > 14000) {
        op->phase = BOP_DONE;
        BotOpLog(*op, controlledEnt->entnum, "op", "done", "cap", -1);
    } else if (op->phase == BOP_STACK) {
        gentity_t     *tg = (op->thrower >= 0 && op->thrower < game.maxclients) ? &g_entities[op->thrower] : NULL;
        BotController *tc = (tg && tg->inuse && tg->entity) ? botManager.getControllerManager().findController(tg->entity) : NULL;
        if (!tc || tc->m_iOpId != op->id || tg->entity->IsDead()) {
            op->phase = BOP_DONE;
            BotOpLog(*op, controlledEnt->entnum, "op", "abort", "thrower", -1);
        }
    } else if (op->phase == BOP_BLAST && now >= op->detAt + 300) {
        op->phase   = BOP_ENTRY;
        op->tPhase  = now;
        op->entryT  = now;
        int order   = 0;
        for (int k = 0; k < op->nm; k++) {
            if (op->state[k] <= 1) {
                op->enterAt[k] = now + 600 * order++;
            }
        }
        op->throwerGo = now + 600 * order + 300;
        BotOpLog(*op, controlledEnt->entnum, "op", "entry", "det", -1);
    } else if (op->phase == BOP_ENTRY) {
        bool bAll = now >= op->throwerGo;
        for (int k = 0; k < op->nm && bAll; k++) {
            bAll = op->state[k] >= 3;
        }
        if (bAll || now - op->entryT > 4500) {
            op->phase = BOP_DONE;
            BotOpLog(*op, controlledEnt->entnum, "op", "done", bAll ? "in" : "time", -1);
        }
    }
    if (op->phase >= BOP_DONE) {
        OpLeave("done");
        return;
    }
    if (m_iOpSlot < 0) {
        // the THROWER goes in last (unless getting out of the blast owns its feet)
        if (op->phase == BOP_ENTRY) {
            if (now >= op->throwerGo) {
                OpLeave("go");
            } else if (now >= m_iGrenadeFleeUntil && now >= m_iBlastCoverUntil && movement.GetHoldUntil() < now + 150) {
                movement.HoldFor(300);
            }
        }
        return;
    }
    const int    k   = m_iOpSlot;
    const Vector org = controlledEnt->origin;
    // a stacker that can shoot an enemy fights from its stack spot (State_Attack aims / fires; MovementLocked keeps the
    // feet): past 1.5s of it, it is a fight, not a room clear any more
    if (m_iLastSeenTime && now - m_iLastSeenTime < 100 && now - op->t0 > 1500) {
        OpLeave("enemy");
        return;
    }
    if (m_iNadeState || movement.IsOnLadder() || movement.IsInLiftProtocol() || movement.IsOnElevatorLink()) {
        OpLeave("busy");
        return;
    }
    if (op->phase == BOP_STACK || op->phase == BOP_BLAST) {
        if (op->state[k] == 0) {
            if ((org - op->spot[k]).lengthXYSquared() < Square(28.0f)) {
                op->state[k] = 1;
                BotOpLog(*op, controlledEnt->entnum, "stack", "stacked", "spot", k);
            } else if (now - m_iOpMoveT > 5000) {
                // could not get there in time: stack right here if the blast cannot reach us, else drop out
                const bool bDuck = controlledEnt->client && (controlledEnt->client->ps.pm_flags & PMF_DUCKED);
                if (!BotBlastReaches(op->det, op->R, org, bDuck, controlledEnt, NULL) && DotProduct2D(org - op->D, op->n) < -16.0f) {
                    op->state[k] = 1;
                    op->spot[k]  = org;
                    BotOpLog(*op, controlledEnt->entnum, "stack", "stacked", "here", k);
                } else {
                    OpLeave("nospot");
                    return;
                }
            } else {
                if (!movement.IsMoving() || !movement.PathReaches(op->spot[k], 32.0f)) {
                    movement.CommitMove(0);
                    movement.SetWhy("stack"); // [HZM bot probe2]
                    movement.MoveTo(op->spot[k]);
                }
                movement.CommitMove(600); // (refreshed: the states' re-paths wait)
            }
        }
        if (op->state[k] == 1) {
            if (movement.GetHoldUntil() < now + 150) {
                movement.HoldFor(300);
            }
            m_bWantCrouch = true;
        }
        return;
    }
    // ENTRY
    if (op->state[k] <= 1) {
        if (now < op->enterAt[k]) {
            if (movement.GetHoldUntil() < now + 150) {
                movement.HoldFor(300);
            }
            m_bWantCrouch = op->state[k] == 1;
            return;
        }
        op->state[k] = 2;
        movement.HoldFor(0);
        movement.CommitMove(0);
        if (op->E[k] != vec_zero) {
            movement.SetWhy("entry"); // [HZM bot probe2]
            movement.MoveTo(op->E[k]);
            movement.CommitMove(2500);
        }
        CrossBegin(4, 1, op->D); // (T2's 3s outcome window: what happened going in - BOTCORNEROUT kind=4)
        BotOpLog(*op, controlledEnt->entnum, "stack", "enter", op->E[k] != vec_zero ? "point" : "route", k);
        if (k == 0 && G_Random(1.0f) < 0.5f) {
            VoiceCallout("*21", 600); // "Squad, move in!"
        }
    }
    if (op->state[k] == 2
        && (op->E[k] == vec_zero || (org - op->E[k]).lengthXYSquared() < Square(40.0f) || now - op->enterAt[k] > 3000
            || !movement.IsMoving())) {
        op->state[k] = 3;
        OpLeave("entered");
    }
}

bool BotController::OpAim(void)
{
    if (!m_iOpId || m_iOpSlot < 0) {
        return false;
    }
    BotBreachOp *op = BotOpFind(m_iOpId);
    if (!op) {
        return false;
    }
    const int k = m_iOpSlot;
    const int s = op->side[k];
    Vector    aim;
    if (op->phase == BOP_ENTRY && op->state[k] == 2) {
        const int c = (-s > 0) ? 0 : 1; // the corner on the side it crosses to
        aim         = op->ok[c] ? op->K[c] : (op->ok[1 - c] ? op->K[1 - c] : op->D + op->n * 200.0f + Vector(0, 0, 48));
    } else if (op->state[k] >= 1) {
        // stacked: the far edge of the opening, where a defender would show
        aim = op->D + op->t * (-s * op->w * 0.5f) + op->n * 48.0f + Vector(0, 0, 48);
    } else {
        return false; // walking to the spot: the route aim (and its corner checks)
    }
    const Vector org = controlledEnt->origin;
    Vector       a   = (aim - (org + Vector(0, 0, controlledEnt->viewheight))).toAngles();
    const float  px  = (a.x > 180.0f) ? a.x - 360.0f : a.x;
    a.x              = px * 0.35f;
    a.z              = 0;
    if (op->state[k] == 2 && movement.IsMoving()) {
        const float travel = movement.GetCurrentPathDirection().toYaw();
        a.y                = anglemod(travel + Q_clamp_float(AngleSubtract(a.y, travel), -60.0f, 60.0f));
        movement.SetLookComp(400);
    }
    rotation.SetTargetAngles(a);
    m_iCornerAimT = level.inttime;
    return true;
}

void BotController::DefendThink(void)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_breachDefend", "0", 0);
    }
    if (!m_iDefendUntil) {
        return;
    }
    const int now = level.inttime;
    const char *why = NULL;
    if (!BotTeamGate(s_on->integer, controlledEnt) || controlledEnt->IsDead()) {
        why = "off";
    } else if (now >= m_iDefendUntil) {
        why = "time";
    } else if (now < m_iDefendAt) {
        return; // the grenade has not gone off: getting out of it is CheckGrenadeThreatSafe's
    } else if (m_iLastEnemySeenAny && now - m_iLastEnemySeenAny < 800) {
        why = "enemy"; // fight
    } else if (m_iNadeState || m_iHealState || m_iOpId || movement.IsOnLadder() || movement.IsInLiftProtocol()
               || movement.IsOnElevatorLink()) {
        why = "busy";
    }
    if (why) {
        if (BotNadeProbeOn()) {
            gi.Printf(
                "^~^~^ BOTDEFEND e=%d ev=end why=%s mode=%s dur=%d\n", controlledEnt->entnum, why, m_bDefendPush ? "push" : "hold",
                now - m_iDefendAt
            );
        }
        m_iDefendUntil = 0;
        return;
    }
    if (m_bDefendPush) {
        // the bold: go and see where it came from, gun up (State_Curious' investigate)
        m_vInvestigatePos   = m_vDefendFrom;
        m_iInvestigateUntil = now + 6000;
        m_iCuriousTime      = Q_max(m_iCuriousTime, now + 6000);
        m_vNewCuriousPos    = m_vDefendFrom;
        m_vAlertPos         = m_vDefendFrom + Vector(0, 0, 40);
        m_iAlertUntil       = now + 1500;
        if (BotNadeProbeOn()) {
            gi.Printf(
                "^~^~^ BOTDEFEND e=%d ev=push at=(%.0f %.0f %.0f) from=(%.0f %.0f %.0f)\n", controlledEnt->entnum,
                controlledEnt->origin.x, controlledEnt->origin.y, controlledEnt->origin.z, m_vDefendFrom.x, m_vDefendFrom.y,
                m_vDefendFrom.z
            );
        }
        m_iDefendUntil = 0;
        return;
    }
    // hold the angle: crouched, gun on where it came from (BrainThink faces it, in the attack state too)
    if (!MovementLocked() && movement.GetHoldUntil() < now + 150) {
        movement.HoldFor(300);
    }
    m_bWantCrouch = true;
    m_vAlertPos   = m_vDefendFrom + Vector(0, 0, 40);
    m_iAlertUntil = now + 300;
}

void BotController::BrainThink(void)
{
    if (movement.IsOnLadder()) {
        return; // the ladder owns the view
    }
    static cvar_t *s_botDanger = NULL;
    static cvar_t *s_botGlance = NULL;
    if (!s_botDanger) {
        s_botDanger = gi.Cvar_Get("bot_danger", "1", 0);
        s_botGlance = gi.Cvar_Get("bot_glance", "1", 0);
    }
    // [HZM bot A4] waiting at a closed door: hands off the trigger, or Player::DoUse refuses the USE
    if (movement.IsWaitingForDoor()) {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        // [bot_doorFace] ...and LOOK at it: DoUse finds the door along the view (64u), and glances, corner checks and the
        // path aim can all point past a door the bot is standing at. Not while it can see an enemy.
        static cvar_t *s_face = NULL;
        if (!s_face) {
            s_face = gi.Cvar_Get("bot_doorFace", "0", 0);
        }
        Vector fp;
        if (s_face->integer && movement.GetDoorFacePoint(fp)
            && !(m_iLastEnemySeenAny && level.inttime - m_iLastEnemySeenAny < 800)) {
            m_vAlertPos    = fp + Vector(0, 0, 16); // the alert aim pre-aims 16u low: this lands at eye height
            m_iAlertUntil  = level.inttime + 200;
            m_iGlanceUntil = 0;
        }
    }
    // [HZM bot B3] known danger ground (heat-map death cells): WALK it, eyes up
    // [2026-09-25] NOT by dropping BUTTON_RUN: with coop_sprint a clear RUN bit is the SPRINT key (player.cpp ~4877), so
    // this made bots SPRINT across danger ground. A true walk (BUTTON_COOPWALK) sits near the 60u/s "blocked" line and
    // would re-create the door stall, so danger ground is crossed at the normal run, eyes up (the glance below).
    if (s_botDanger->integer && !m_iAttackTime && NavDanger_IsInside((const float *)controlledEnt->origin)) {
        if (m_iGlanceNext > level.inttime + 1200) {
            m_iGlanceNext = level.inttime + 400;
        }
    }
    if (m_iHealState >= 1 && m_iHealState <= 3) {
        // [user 2026-09-25] bandaging: face the threat axis, level, no glances; crouch if only a crouched eye is hidden
        rotation.SetTargetAngles(Vector(0, m_fHealYaw, 0));
        m_bWantCrouch = m_bHealCrouch;
    } else if (m_iDefendUntil && level.inttime >= m_iDefendAt && !m_bDefendPush && !m_iNadeState
               && level.inttime >= m_iAttackStopAimTime && !(m_iLastEnemySeenAny && level.inttime - m_iLastEnemySeenAny < 800)) {
        // [room-clear step 5] holding the angle an enemy grenade came from - in the attack state too (the alert branch
        // below only runs without one)
        const Vector d = m_vAlertPos - (controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight - 16));
        if (d.lengthSquared() > 1.0f) {
            Vector a = d.toAngles();
            a.x      = Q_clamp_float(a.x > 180.0f ? a.x - 360.0f : a.x, -35.0f, 35.0f);
            rotation.SetTargetAngles(a);
        }
    } else if (m_bB2B && !m_iNadeState
               && (m_bBuddyLead ? level.inttime >= m_iAlertUntil : !(m_iHeardTime && level.inttime - m_iHeardTime < 1500))) {
        // [T3] BACK TO BACK: both holding - the leader faces the threat axis, the partner the other way. The glance stays
        // inside the sector (+/-50, no over-the-shoulder check: the partner has the rear) and is re-based every frame -
        // a standing bot has nothing else resetting the view the glance adds to (vet T3)
        if (level.inttime >= m_iGlanceNext) {
            m_fGlanceYaw   = G_CRandom(50.0f);
            m_iGlanceUntil = level.inttime + 600 + (int)G_Random(700.0f);
            m_iGlanceNext  = m_iGlanceUntil + 1500 + (int)G_Random(2500.0f);
        }
        rotation.SetTargetAngles(Vector(0, anglemod(m_fB2BYaw + (level.inttime < m_iGlanceUntil ? m_fGlanceYaw : 0.0f)), 0));
        if (m_iBuddyStillT && level.inttime - m_iBuddyStillT > 2500) {
            m_bWantCrouch = true; // settled in: get small
        }
    } else if (!m_iAttackTime && !m_iNadeState) {
        if (level.inttime < m_iAlertUntil) {
            // [HZM bot B1/B2] face the heard / lost enemy (pre-aim at chest height)
            Vector d = m_vAlertPos - (controlledEnt->origin + Vector(0, 0, controlledEnt->viewheight - 16));
            if (d.lengthSquared() > 1.0f) {
                Vector a = d.toAngles();
                a.x      = Q_clamp_float(a.x > 180.0f ? a.x - 360.0f : a.x, -35.0f, 35.0f);
                rotation.SetTargetAngles(a);
            }
        } else if (s_botGlance->integer && level.inttime - m_iCornerAimT > 150) {
            // [HZM bot D2] look around like a person: short glances, now and then a check over the shoulder
            // ([T2] not on top of a corner check - the glance ADDS its offset to whatever the target is)
            if (level.inttime >= m_iGlanceNext) {
                const bool bStill = controlledEnt->velocity.lengthXYSquared() < Square(40.0f);
                m_fGlanceYaw      = (bStill && G_Random(1.0f) < 0.25f) ? (G_Random(1.0f) < 0.5f ? 150.0f : -150.0f)
                                                                       : G_CRandom(50.0f);
                m_iGlanceUntil    = level.inttime + 450 + (int)G_Random(500.0f);
                m_iGlanceNext     = m_iGlanceUntil + 2200 + (int)G_Random(3800.0f);
            }
            if (level.inttime < m_iGlanceUntil) {
                Vector a = rotation.GetTargetAngles();
                a.y += m_fGlanceYaw;
                // [user 2026-09-25] "bots will sometimes stand around and just aim up high ... and look around": a bot
                // standing still keeps whatever pitch it last had (AimAtAimNode only levels the view while MOVING), so
                // after an alert at a balcony / upper floor it idled glancing round with its gun raised. A glance
                // brings the eyes back toward level.
                const float px = (a.x > 180.0f) ? a.x - 360.0f : a.x;
                a.x            = px * 0.35f;
                rotation.SetTargetAngles(a);
                // [2026-09-26, bug-2986 finding] a glance while MOVING turned the eyes up to 50 deg off the route and, with
                // a unit move input, PM_CmdScale's largest-axis magnitude dropped the bot to ~65-70% pace for the glance -
                // an unintended slowdown, not a speed choice. The move input is rescaled for it like a corner look
                // (BotMovement::SetLookComp: back to the bot's normal pace, never above it). bot_glanceComp 0 = old.
                static cvar_t *s_glanceComp = NULL;
                if (!s_glanceComp) {
                    s_glanceComp = gi.Cvar_Get("bot_glanceComp", "1", 0);
                }
                if (s_glanceComp->integer) {
                    movement.SetLookComp(300);
                }
            }
        }
    }
    // [HZM bot D4] crouch where a tactic asked for it - and never let the held crouch arm the 0.5s PRONE (coop_prone runs
    // in MP too; the engine clears the block itself once upmove >= 0)
    static cvar_t *s_botCrouch = NULL;
    if (!s_botCrouch) {
        s_botCrouch = gi.Cvar_Get("bot_crouch", "1", 0);
    }
    if (m_iNadeState && (m_iNadeMode == 1 || BotNadeSafe())) {
        m_bWantCrouch = false; // [HZM bot breach] the arc was solved from standing eye height ([step 2] lobs too, vet C8)
    }
    // [HZM bug-2900] a crouch that stops the bot getting anywhere is dropped: t2l4 round 12, a bot crouch-walking up a
    // slope (hold-front / timid-attack crouch while its route ran on) pushed at 1-9u/s for two minutes, never able to
    // climb it crouched. Crouched + wanting to move + under 30u/s for 0.8s -> stand for 3s.
    if (m_bWantCrouch && movement.IsMoving() && (abs(m_botCmd.forwardmove) > 20 || abs(m_botCmd.rightmove) > 20)
        && controlledEnt->velocity.lengthXYSquared() < Square(30.0f)) {
        if (!m_iCrouchStuckSince) {
            m_iCrouchStuckSince = level.inttime;
        } else if (level.inttime - m_iCrouchStuckSince > 800) {
            m_iNoCrouchUntil    = level.inttime + 3000;
            m_iCrouchStuckSince = 0;
        }
    } else {
        m_iCrouchStuckSince = 0;
    }
    if (level.inttime < m_iNoCrouchUntil) {
        m_bWantCrouch = false;
    }
    // [HZM bug-2918] CROUCH IS A TOGGLE. Every crouch transition in player_legs.st keys on the RISING edge of the crouch
    // press - STAND -> CROUCH_IDLE on "+CROUCH", and every CROUCH_* state back to STAND on "+CROUCH" - so releasing
    // the key does NOT stand a player up. Bots held upmove -127 while a tactic wanted a crouch and let go after, which
    // left them crouched until the NEXT tactical crouch - whose press then stood them up (user: "some of them also
    // walk around everywhere crouched"; soak: crouched in 6.7% of moving samples, 96% of those with no crouch input,
    // runs of 10s+; it is also why bug-2900's stand-up never took). Tap - one frame, an edge - whenever the stance
    // the tactics want differs from the stance the bot is in, at most every 400ms (the state machine and the hull
    // settle in between). A one-frame press can never arm the held-crouch PRONE either.
    {
        const bool bWant = s_botCrouch->integer && m_bWantCrouch;
        const bool bDuck = controlledEnt->client && (controlledEnt->client->ps.pm_flags & PMF_DUCKED)
                        && !(controlledEnt->client->ps.pm_flags & PMF_VIEW_PRONE);
        if (m_botCmd.upmove < 0) {
            m_botCmd.upmove = 0; // nothing below this point holds crouch - only the tap does
        }
        if (bWant != bDuck && level.inttime >= m_iCrouchTapNext && controlledEnt->groundentity) {
            m_botCmd.upmove                     = -127;
            m_iCrouchTapNext                    = level.inttime + 400;
            controlledEnt->m_bCoopProneKeyBlock = true;
        }
    }
}

void BotController::GotKill(const Event& ev)
{
    ClearEnemy();
    m_iCuriousTime = 0;
    if (m_iCrossT) {
        m_iCrossKill++; // [T2]
    }
    if (m_pBuddy) {
        m_iBuddyKills++; // [T3]
    }
    if (BotNadeProbeOn() && controlledEnt) {
        gi.Printf("^~^~^ BOTKILL e=%d paired=%d\n", controlledEnt->entnum, m_pBuddy ? 1 : 0); // [T3] K/D paired vs alone
    }

    if ((rand() % 3) == 0) {
        VoiceCallout("*46", 10000); // [HZM bot D3] "Area clear."
        return;
    }

    if (level.inttime >= m_iNextTauntTime && (rand() % 5) == 0) {
        //
        // Randomly play a taunt
        //
        Event event("dmmessage");

        event.AddInteger(0);

        if (g_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
            event.AddString("*5" + str(1 + (rand() % 8)));
        } else {
            event.AddString("*4" + str(1 + (rand() % 9)));
        }

        controlledEnt->ProcessEvent(event);

        m_iNextTauntTime = level.inttime + 5000;
    }
}

void BotController::EventStuffText(const str& text)
{
    SendCommand(text);
}

void BotController::setControlledEntity(Player *player)
{
    controlledEnt = player;
    movement.SetControlledEntity(player);
    rotation.SetControlledEntity(player);
    RollPersonality(); // [HZM bot D1]

    delegateHandle_gotKill =
        player->delegate_gotKill.Add(std::bind(&BotController::GotKill, this, std::placeholders::_1));
    delegateHandle_killed = player->delegate_killed.Add(std::bind(&BotController::Killed, this, std::placeholders::_1));
    delegateHandle_damage = player->delegate_damage.Add(std::bind(&BotController::Pain, this, std::placeholders::_1));
    delegateHandle_stufftext =
        player->delegate_stufftext.Add(std::bind(&BotController::EventStuffText, this, std::placeholders::_1));
    delegateHandle_spawned = player->delegate_spawned.Add(std::bind(&BotController::Spawned, this));
}

Player *BotController::getControlledEntity() const
{
    return controlledEnt;
}

BotController *BotControllerManager::createController(Player *player)
{
    BotController *controller = new BotController();
    controller->setControlledEntity(player);

    controllers.AddObject(controller);

    return controller;
}

void BotControllerManager::removeController(BotController *controller)
{
    controllers.RemoveObject(controller);
    delete controller;
}

BotController *BotControllerManager::findController(Entity *ent)
{
    int i;

    for (i = 1; i <= controllers.NumObjects(); i++) {
        BotController *controller = controllers.ObjectAt(i);
        if (controller->getControlledEntity() == ent) {
            return controller;
        }
    }

    return nullptr;
}

const Container<BotController *>& BotControllerManager::getControllers() const
{
    return controllers;
}

BotControllerManager::~BotControllerManager()
{
    Cleanup();
}

void BotControllerManager::Init()
{
    BotController::Init();
}

void BotControllerManager::Cleanup()
{
    int i;

    BotController::Init();

    for (i = 1; i <= controllers.NumObjects(); i++) {
        BotController *controller = controllers.ObjectAt(i);
        delete controller;
    }

    controllers.FreeObjectList();
}

void BotControllerManager::ThinkControllers()
{
    int i;

    // Delete controllers that don't have associated player entity
    // This cannot happen unless some mods remove them
    for (i = controllers.NumObjects(); i > 0; i--) {
        BotController *controller = controllers.ObjectAt(i);
        if (!controller->getControlledEntity()) {
            gi.DPrintf(
                "Bot %d has no associated player entity. This shouldn't happen unless the entity has been removed by a "
                "script. The controller will be removed, please fix.\n",
                i
            );

            // Remove the controller, it will be recreated later to match `sv_numbots`
            delete controller;
            controllers.RemoveObjectAt(i);
        }
    }

    for (i = 1; i <= controllers.NumObjects(); i++) {
        BotController *controller = controllers.ObjectAt(i);
        controller->Think();
    }
}
