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
// playerbot.h: Multiplayer bot system.

#pragma once

#include "player.h"
#include "navigate.h"
#include "navigation_path.h"

#define MAX_BOT_FUNCTIONS 5

typedef struct nodeAttract_s {
    float             m_fRespawnTime;
    AttractiveNodePtr m_pNode;
} nodeAttract_t;

class BotController;

class BotMovement
{
public:
    BotMovement();
    ~BotMovement();

    void SetControlledEntity(Player *newEntity);

    void MoveThink(usercmd_t& botcmd);

    void AvoidPath(
        Vector vPos,
        float  fAvoidRadius,
        Vector vPreferredDir = vec_zero,
        float *vLeashHome    = NULL,
        float  fLeashRadius  = 0.0f
    );
    void MoveNear(Vector vNear, float fRadius, float *vLeashHome = NULL, float fLeashRadius = 0.0f);
    void MoveTo(Vector vPos, float *vLeashHome = NULL, float fLeashRadius = 0.0f);
    bool MoveToBestAttractivePoint(int iMinPriority = 0);

    bool CanMoveTo(Vector vPos);
    bool MoveDone();
    bool IsMoving(void);
    void ClearMove(void);

    Vector GetCurrentGoal() const;
    Vector GetCurrentPathDirection() const;

    // [HZM bot probe] read-only accessors for the behavioural telemetry in BotController::ProbeThink
    Vector GetTargetPos() const { return m_vTargetPos; }
    int    GetNumBlocks() const { return m_iNumBlocks; }
    bool   IsJumping() const { return m_bJump; }
    bool   IsPathing() const { return m_bPathing; }

private:
    Vector CalculateDir(const Vector& delta) const;
    Vector CalculateRelativeWishDirection(const Vector& dir) const;
    void   CheckAttractiveNodes();
    void   CheckEndPos(Entity *entity);
    void   CheckJump(usercmd_t& botcmd);
    void   CheckJumpOverEdge(usercmd_t& botcmd);
    float  WhiskerClear(const Vector& dirAngles, float degOff, float len); // [HZM bot feelers] trace clearance 0..1
    void   NewMove();
    Vector FixDeltaFromCollision(const Vector& delta);
    void   CalculateBestFrontAvoidance(
          const Vector& targetOrg,
          float         maxDist,
          const Vector& forward,
          const Vector& right,
          float&        bestFrac,
          Vector&       bestPos
      );

private:
    SafePtr<Player>            controlledEntity;
    AttractiveNodePtr          m_pPrimaryAttract;
    Container<nodeAttract_t *> m_attractList;
    IPather                   *m_pPath;
    int                        m_iLastMoveTime;

    Vector m_vCurrentOrigin;
    Vector m_vTargetPos;
    Vector m_vCurrentGoal;
    Vector m_vCurrentDir;
    Vector m_vLastCheckPos[2];
    float  m_fAttractTime;
    Vector m_vAttractScatterGoal; // [HZM] per-bot scattered destination around the primary attract node
    Vector m_vScatterAnchor;      // [HZM] node origin the scatter goal was rolled around (re-roll only when it moves)
    int    m_iTempAwayTime;
    int    m_iNumBlocks;
    int    m_iStuckPushTime;    // [HZM bot wall-slide] inttime the bot began pushing-but-not-moving (0 = not stuck)
    int    m_iFeelerCommitTime; // [HZM bot feelers] hold the chosen steer side until this inttime (anti-weave)
    int    m_iFeelerSide;       // [HZM bot feelers] committed steer sign for rightmove (+1 right / -1 left)
    int    m_iCheckPathTime;
    int    m_iLastBlockTime;
    int    m_iTempAwayState;
    bool   m_bPathing;

    ///
    /// Collision detection
    ///

    bool   m_bAvoidCollision;
    int    m_iCollisionCheckTime;
    Vector m_vTempCollisionAvoidance;

    ///
    /// Jump detection
    ///

    bool   m_bJump;
    int    m_iJumpCheckTime;
    Vector m_vJumpLocation;
};

class BotRotation
{
public:
    BotRotation();

    void SetControlledEntity(Player *newEntity);

    void          TurnThink(usercmd_t& botcmd, usereyes_t& eyeinfo);
    const Vector& GetTargetAngles() const;
    void          SetTargetAngles(Vector vAngles);
    void          AimAt(Vector vPos);

private:
    SafePtr<Player> controlledEntity;

    Vector m_vTargetAng;
    Vector m_vCurrentAng;
    Vector m_vAngDelta;
    Vector m_vAngSpeed;
};

class BotState
{
public:
    virtual bool CheckCondition() const = 0;
    virtual void Begin()                = 0;
    virtual void End()                  = 0;
    virtual void Think()                = 0;
};

class BotController : public Listener
{
public:
    struct botfunc_t {
        bool (BotController::*CheckCondition)(void);
        void (BotController::*BeginState)(void);
        void (BotController::*EndState)(void);
        void (BotController::*ThinkState)(void);
    };

private:
    static botfunc_t botfuncs[];

    BotMovement movement;
    BotRotation rotation;

    // States
    int    m_iCuriousTime;
    int    m_iAttackTime;
    int    m_iAttackStopAimTime;
    int    m_iLastBurstTime;
    int    m_iLastSeenTime;
    int    m_iLastUnseenTime;
    int    m_iContinuousFireTime;
    Vector m_vAimOffset;
    int    m_iLastAimTime;

    Vector            m_vLastCuriousPos;
    Vector            m_vNewCuriousPos;
    Vector            m_vOldEnemyPos;
    Vector            m_vLastEnemyPos;
    Vector            m_vLastDeathPos;
    SafePtr<Sentient> m_pEnemy;
    SafePtr<Sentient> m_pLastAimEnemy; // [HZM] last enemy we aimed at, to detect a fresh lock for aim convergence
    int               m_iEnemyEyesTag;

    // Input
    usercmd_t  m_botCmd;
    usereyes_t m_botEyes;

    // States
    int               m_StateCount;
    unsigned int      m_StateFlags;
    ScriptThreadLabel m_RunLabel;

    // Taunts
    int m_iNextTauntTime;
    int m_iLastFireTime;
    int m_iLastPainTime;  // [HZM] last time the bot took damage (set in Pain) - gates cover-seeking
    int m_iEnemyLockTime; // [HZM] when the current enemy was first locked - drives aim convergence
    int m_iProbeLastTime; // [HZM bot probe] throttle: last inttime ProbeThink logged for this bot

    void ProbeThink(const usercmd_t& ucmd); // [HZM bot probe] per-bot behavioural telemetry (bot_probe 1)

private:
    DelegateHandle delegateHandle_gotKill;
    DelegateHandle delegateHandle_killed;
    DelegateHandle delegateHandle_stufftext;
    DelegateHandle delegateHandle_spawned;
    DelegateHandle delegateHandle_damage;

private:
    Weapon *FindWeaponWithAmmo(void);
    Weapon *FindMeleeWeapon(void);
    void    UseWeaponWithAmmo(void);

    void CheckUse(void);
    bool CheckWindows(void);
    void CheckValidWeapon(void);

    void State_DefaultBegin(void);
    void State_DefaultEnd(void);
    void State_Reset(void);

    static void InitState_Idle(botfunc_t *func);
    bool        CheckCondition_Idle(void);
    void        State_BeginIdle(void);
    void        State_EndIdle(void);
    void        State_Idle(void);

    static void InitState_Curious(botfunc_t *func);
    bool        CheckCondition_Curious(void);
    void        State_BeginCurious(void);
    void        State_EndCurious(void);
    void        State_Curious(void);

    static void InitState_Attack(botfunc_t *func);
    bool        CheckCondition_Attack(void);
    void        State_BeginAttack(void);
    void        State_EndAttack(void);
    void        State_Attack(void);
    bool        IsValidEnemy(Sentient *sent) const;
    // [HZM Phase 1] find a nearby, reachable spot that breaks line of sight to threatPos (cover). Returns
    // false if none of the sampled spots qualify. Used by State_Attack for cover-peek + fall-back.
    bool        FindCoverPosition(const Vector& threatPos, Vector& outCover);

    static void InitState_Grenade(botfunc_t *func);
    bool        CheckCondition_Grenade(void);
    void        State_BeginGrenade(void);
    void        State_EndGrenade(void);
    void        State_Grenade(void);

    static void InitState_Weapon(botfunc_t *func);
    bool        CheckCondition_Weapon(void);
    void        State_BeginWeapon(void);
    void        State_EndWeapon(void);
    void        State_Weapon(void);

    void CheckStates(void);

public:
    CLASS_PROTOTYPE(BotController);

    BotController();
    ~BotController();

    static void Init(void);

    void GetEyeInfo(usereyes_t *eyeinfo);
    void GetUsercmd(usercmd_t *ucmd);

    void UpdateBotStates(void);
    void CheckReload(void);

    void AimAtAimNode(void);

    void NoticeEvent(Vector vPos, int iType, Entity *pEnt, float fDistanceSquared, float fRadiusSquared);
    void ClearEnemy(void);

    void SendCommand(const char *text);

    void Think();

    void Spawned(void);

    void Killed(const Event& ev);
    void GotKill(const Event& ev);
    void Pain(const Event& ev);
    void EventStuffText(const str& text);

    BotMovement& GetMovement();

public:
    void    setControlledEntity(Player *player);
    Player *getControlledEntity() const;

private:
    SafePtr<Player> controlledEnt;
};

class BotControllerManager : public Listener
{
public:
    CLASS_PROTOTYPE(BotControllerManager);

public:
    ~BotControllerManager();

    BotController                    *createController(Player *player);
    void                              removeController(BotController *controller);
    BotController                    *findController(Entity *ent);
    const Container<BotController *>& getControllers() const;

    void Init();
    void Cleanup();
    void ThinkControllers();

private:
    Container<BotController *> controllers;
};

class BotManager : public Listener
{
public:
    CLASS_PROTOTYPE(BotManager);

public:
    BotControllerManager& getControllerManager();

    void Init();
    void Cleanup();
    void Frame();
    void BroadcastEvent(Entity *originator, Vector origin, int iType, float radius);

private:
    BotControllerManager botControllerManager;
};

extern BotManager botManager;
