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
#include "windows.h"
#include "g_bot.h"

// We assume that we have limited access to the server-side
// and that most logic come from the playerstate_s structure

cvar_t *bot_manualmove;

CLASS_DECLARATION(Listener, BotController, NULL) {
    {NULL, NULL}
};

BotController::botfunc_t BotController::botfuncs[MAX_BOT_FUNCTIONS];

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
    m_iAttackTime         = 0;
    m_iEnemyEyesTag       = -1;
    m_iContinuousFireTime = 0;
    m_iLastSeenTime       = 0;
    m_iLastUnseenTime     = 0;
    m_iLastBurstTime      = 0;
    m_iLastPainTime       = 0; // [HZM] cover-seek gate
    m_iEnemyLockTime      = 0; // [HZM Phase 5b] aim-convergence clock

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

    CheckStates();

    movement.MoveThink(m_botCmd);
    rotation.TurnThink(m_botCmd, m_botEyes);
    CheckUse();

    CheckValidWeapon();
}

void BotController::CheckUse(void)
{
    Vector  dir;
    Vector  start;
    Vector  end;
    trace_t trace;

    if (controlledEnt->GetLadder()) {
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

    if (!com_token) {
        return;
    }

    controlledEnt->m_lastcommand = com_token;

    if (!Event::GetEvent(com_token)) {
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

    if (pEnt->IsSubclassOfSentient()) {
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
            float maxDist = Q_min(world->m_fAIVisionDistance, world->farplane_distance * 0.828);
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
    if (CheckWindows()) {
        m_botCmd.buttons ^= BUTTON_ATTACKLEFT;
        m_iLastFireTime = level.inttime;
    } else {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        CheckReload();
    }

    AimAtAimNode();

    if (!movement.MoveToBestAttractivePoint() && !movement.IsMoving()) {
        if (m_vLastDeathPos != vec_zero) {
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

    AimAtAimNode();

    if (!movement.MoveToBestAttractivePoint(3) && (!movement.IsMoving() || m_vLastCuriousPos != m_vNewCuriousPos)) {
        movement.MoveTo(m_vNewCuriousPos);
        m_vLastCuriousPos = m_vNewCuriousPos;
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

        maxDistance = Q_min(world->m_fAIVisionDistance, world->farplane_distance * 0.828);

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

            if (!m_pEnemy) {
                m_iLastUnseenTime = level.inttime;
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

    if (!m_pEnemy || !IsValidEnemy(m_pEnemy)) {
        // Ignore dead enemies
        m_iAttackTime = 0;
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
        controlledEnt->CanSee(m_pEnemy, fFovFire, Q_min(world->m_fAIVisionDistance, world->farplane_distance * 0.828), false);

    if (bCanSee) {
        if (!pWeap) {
            return;
        }

        bCanAttack = true;
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
        }
    } else {
        m_botCmd.buttons &= ~(BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT);
        fMinDistanceSquared = 0;

        if (level.inttime > m_iLastSeenTime + 2000) {
            m_iLastUnseenTime = level.inttime;
        }
    }

    if (bCanSee || level.inttime < m_iAttackStopAimTime) {
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
        // misses read as suppression) and tightens over ~1.4s to a residual-jitter floor - this removes the
        // "instant lock-on" aimbot tell. Only the horizontal miss spread ([0]/[1]) scales; the vertical aim
        // point ([2], body height) is left alone. bot_combat_realism 0 = the stock flat spread.
        static cvar_t *s_botCombat = NULL;
        if (!s_botCombat) {
            s_botCombat = gi.Cvar_Get("bot_combat_realism", "1", CVAR_ARCHIVE);
        }
        if (m_pEnemy != m_pLastAimEnemy) {
            m_pLastAimEnemy  = m_pEnemy;
            m_iEnemyLockTime = level.inttime;
        }
        float fAimConv = 1.0f;
        if (s_botCombat->integer) {
            fAimConv = 1.7f - (float)(level.inttime - m_iEnemyLockTime) / 1400.0f;
            if (fAimConv < 0.35f) {
                fAimConv = 0.35f;
            }
            if (fAimConv > 1.7f) {
                fAimConv = 1.7f;
            }
        }

        if (level.inttime >= m_iLastAimTime + 100) {
            if (m_iEnemyEyesTag != -1) {
                m_vAimOffset[0] = G_CRandom((m_pEnemy->maxs.x - m_pEnemy->mins.x) * 0.5) * fAimConv;
                m_vAimOffset[1] = G_CRandom((m_pEnemy->maxs.y - m_pEnemy->mins.y) * 0.5) * fAimConv;
                m_vAimOffset[2] = -G_Random(m_pEnemy->maxs.z * 0.5);
            } else {
                m_vAimOffset[0] = G_CRandom((m_pEnemy->maxs.x - m_pEnemy->mins.x) * 0.5) * fAimConv;
                m_vAimOffset[1] = G_CRandom((m_pEnemy->maxs.y - m_pEnemy->mins.y) * 0.5) * fAimConv;
                m_vAimOffset[2] = 16 + G_Random(m_pEnemy->viewheight - 16);
            }
            m_iLastAimTime = level.inttime;
        }

        rotation.AimAt(vTarget + m_vAimOffset);
    } else {
        AimAtAimNode();
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

        // Move to cover only when a VALIDATED cover spot exists. Critical health tries hard; a hurt+exposed
        // bot peeks (committed via !IsMoving so it does not thrash). No cover -> keep the stock fight logic.
        if ((fHealthFrac <= 0.35f) || (fHealthFrac < 0.6f && bRecentlyShot && bCanSee && !movement.IsMoving())) {
            Vector vCover;
            if (FindCoverPosition(m_vLastEnemyPos, vCover)) {
                movement.MoveTo(vCover);
                m_iAttackTime = level.inttime + 1000;
                return;
            }
        }
    }

    if (bNoMove) {
        return;
    }

    fEnemyDistanceSquared = (controlledEnt->origin - m_vLastEnemyPos).lengthSquared();

    if ((!movement.MoveToBestAttractivePoint(5) && !movement.IsMoving())
        || (m_vOldEnemyPos != m_vLastEnemyPos && !movement.MoveDone()) || fEnemyDistanceSquared < fMinDistanceSquared) {
        if (!bMelee || !bCanSee) {
            if (fEnemyDistanceSquared < fMinDistanceSquared) {
                Vector vDir = controlledEnt->origin - m_vLastEnemyPos;
                VectorNormalizeFast(vDir);

                movement.AvoidPath(m_vLastEnemyPos, fMinDistance, Vector(controlledEnt->orientation[1]) * 512);
            } else {
                movement.MoveTo(m_vLastEnemyPos);
            }

            if (!bCanSee && movement.MoveDone()) {
                // Lost track of the enemy
                ClearEnemy();
                return;
            }
        } else {
            movement.MoveTo(m_vLastEnemyPos);
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
    // FIXME: TODO
    return false;
}

void BotController::State_Grenade(void)
{
    // FIXME: TODO
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

    gi.Printf(
        "^~^~^ BOTPROBE e=%d tm=%c al=%d hp=%d px=%.0f py=%.0f pz=%.0f spd=%.0f fm=%d rm=%d um=%d fire=%d "
        "blk=%d jmp=%d pth=%d gd=%.0f en=%d ed=%.0f seen=%d pain=%d\n",
        controlledEnt->entnum, tm, alive, (int)controlledEnt->health,
        org.x, org.y, org.z, spd,
        (int)ucmd.forwardmove, (int)ucmd.rightmove, (int)ucmd.upmove, fire,
        movement.GetNumBlocks(), movement.IsJumping() ? 1 : 0, movement.IsPathing() ? 1 : 0, goalDist,
        enemyEnt, enemyDist, seenAgo, painAgo
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
    if (!attacker || !attacker->IsSubclassOfSentient()) {
        return;
    }

    sent = static_cast<Sentient *>(attacker);
    if (!IsValidEnemy(sent)) {
        return;
    }

    // Remember we were just shot (drives cover-seeking in State_Attack), even during an ongoing fight.
    m_iLastPainTime = level.inttime;

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

void BotController::GotKill(const Event& ev)
{
    ClearEnemy();
    m_iCuriousTime = 0;

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
