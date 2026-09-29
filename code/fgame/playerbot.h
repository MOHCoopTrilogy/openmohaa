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
class Projectile; // [HZM bot room-clear step 1] OnNadeSpawn

// [HZM bug-2912] lift protocol states (BotMovement::LiftThink)
enum { LIFT_NONE = 0, LIFT_WAIT, LIFT_ALIGN, LIFT_BOARD, LIFT_RIDE, LIFT_EXIT, LIFT_SHAFT };
struct navLift_t;

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
    bool PathReaches(const Vector& vPos, float fTol); // [HZM bug-2849] false = partial path (goal cut off)
    bool IsOnElevatorLink() const; // [HZM bug-2866] boarding / riding a navlinks "elevator" link
    // [HZM bug-2912] lift protocol state for other bots (let-off / capacity): LIFT_* and the end it is bound for
    int  GetLiftState(int& destEnd, const navLift_t *& liftRec) const;
    bool IsInLiftProtocol() const { return m_iLiftState != LIFT_NONE; } // [2026-09-25] any lift state (HealThink)
    int  GetLiftSlot() const { return m_iLiftSlot; } // [HZM bug-2949] its place in the cab grid (ALIGN/BOARD/RIDE)
    void LiftAbort(const char *why); // [HZM bug-2912] leave the lift protocol (also called on spawn)
    bool IsOnLadder() const;       // [HZM bug-2871] on a ladder route: walking onto / climbing a func_ladder link
    bool GetLadderSteer(Vector& dir, float& pitch) const; // [HZM bug-2871] view yaw dir + pitch to mount/climb
    void   CommitMove(int iMs);            // [HZM bot B4/C3] hold the current path iMs against MoveTo/ClearMove (0 = release)
    Vector GetPathDestination() const;     // [HZM bot C1] where the current path ends
    bool   IsWaitingForDoor() const;       // [HZM bot A4] stopped at a closed door, letting it open
    bool   GetDoorFacePoint(Vector& out) const; // [bot_doorFace] where to look while waiting at a closed door
    void   HoldFor(int iMs);               // [HZM bot breach] stand still, keeping the path, for iMs (0 = release)
    int    GetHoldUntil() const { return m_iHoldUntil; }
    bool   IsHeld() const;
    bool   IsDirectSteering() const { return level.inttime < m_iDirectSteerUntil; } // [bug-2879] off-mesh steer
    void   SetCalm(bool b) { m_bCalm = b; } // [HZM bug-2899] no fight on: the no-progress learner may run
    // [T2 corner checks] the brain's eyes are off the travel line for iMs: MoveThink rescales the move input so the pace
    // stays the one the bot would have looking where it goes (PM_CmdScale takes the largest axis as the magnitude)
    void   SetLookComp(int iMs) { m_iLookCompUntil = level.inttime + iMs; }
    int    GetSepT() const { return m_iSepT; } // [T2] last frame the bot-bot separation sidestep fired (view-relative)
    bool   IsLookComp() const { return level.inttime < m_iLookCompUntil; } // [probe] the rescale is live
    bool   IsStruggling() const { return m_iStuckPushTime != 0 || m_iTempAwayState != 0 || level.inttime < m_iDirectSteerUntil; }
    int    GetPathCorners(Vector *out, int maxCorners) const; // [HZM bot breach] corners ahead (see IPather::GetCorners)
    bool   GetApproachingLadder(Vector& from, Vector& to) const; // [HZM bot breach] a ladder link <128u ahead, not yet on it
    // [HZM bot breach] path walks into it. [room-clear step 2] mask/eyeZ: bot_nadeSafe tests the engine's own blast
    // line (MASK_EXPLOSION to the centroid, +47); the defaults are the step-0 test (MASK_SOLID from +32)
    bool   PathEntersBlast(const Vector& c, float r, float lookahead, int mask = MASK_SOLID, float eyeZ = 32.0f) const;
    // [HZM bot probe2] WHY does the bot go where it goes: the brain tags the next route request (SetWhy), every route
    // change / clear / jump is logged with that tag (^~^~^ BOTGOAL / BOTJUMP, bot_probe on), and the probe line carries
    // the tag of the route being walked (GetGoalWhy) - "running the other way" and loops become attributable
    void        SetWhy(const char *why) { m_szWhy = why; }
    const char *GetGoalWhy() const { return m_bPathing ? m_szGoalWhy : "none"; }
    bool        GetObjective(Vector& out) const;
    bool        AtObjective() const; // [HZM bug-2884] standing on this bot's objective spot (the stay)
    unsigned char GetApproachArea() const { return m_pPath ? m_pPath->GetApproachingArea() : 0; }
    void        Jump(usercmd_t& botcmd, const char *why);

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
    Vector AvoidPlayersAhead(const Vector& dir, float& closeness) const; // [HZM bot A1] anticipatory bot-bot avoidance
    bool   LowObstacleAhead(const Vector& dir) const;                     // [HZM bot A2] a hop would clear what blocks us
    void   CheckAttractiveNodes();
    void   CheckEndPos(Entity *entity);
    void   CheckJump(usercmd_t& botcmd);
    void   CheckJumpOverEdge(usercmd_t& botcmd);
    float  WhiskerClear(const Vector& dirAngles, float degOff, float len); // [HZM bot feelers] trace clearance 0..1
    void   CheckLookaheadReroute();                                        // [HZM bot lookahead] ranged go-around of solid OBJECTS ahead
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
    int    m_iBlockLogTime;     // [HZM bot blocker-probe] 3s sample clock for ^~^~^ BOTBLOCK (bot_blocklog)
    Vector m_vBlockCheckPos;    // [HZM bot blocker-probe] origin at the last sample (position-based stuck detect)
    int    m_iFeelerCommitTime; // [HZM bot feelers] hold the chosen steer side until this inttime (anti-weave)
    int    m_iFeelerSide;       // [HZM bot feelers] committed steer sign for rightmove (+1 right / -1 left)
    int    m_iExploreCommitTime; // [HZM bot stuck-escape] hold the explore-burst goal until this inttime (see MoveThink)
    int    m_iElevatorStart;     // [HZM bug-2862] when this bot began waiting for / riding an elevator link (0 = not)
    // [HZM bug-2912] LIFT PROTOCOL (navlinks elevator lines that name their cab/gates - see LiftThink)
    bool   LiftThink(usercmd_t& botcmd);
    const navLift_t *m_pLiftRec;  // the lift being used (NULL = not in the protocol)
    SafePtr<Entity> m_pLiftEnt[3];        // cab, gate at end 0, gate at end 1
    int    m_iLiftState;         // LIFT_* (0 = none)
    int    m_iLiftFrom;          // landing index we board at (0/1)
    int    m_iLiftTo;            // landing index we leave at
    int    m_iLiftProgressT;     // last progress (state change / a boarding chance) - 45s give-up from here
    int    m_iLiftOpenT;         // when the cab + our gate were first seen open at our landing this stop (0 = not)
    int    m_iLiftStateT;        // when the current state began
    int    m_iLiftLogT;          // BOTLIFT probe throttle
    int    m_iLiftSide;          // +1/-1: which side of the door this bot queues / stands on (entnum parity)
    int    m_iLiftSlot;          // 0..3: its place in the cab's 2x2 grid, taken when it steps up to board
    int    m_iLiftBlockT;        // [HZM bug-2949] pushing toward a lift spot without moving since (0 = moving / there)
    Vector m_vJumpLinkA;         // [HZM bug-2913] the JUMP link this bot is hopping at (its two ends)
    Vector m_vJumpLinkB;
    int    m_iJumpLinkHops;      // hops at it since m_iJumpLinkT without getting across
    int    m_iJumpLinkT;
    Vector m_vLoopPos[12];       // [HZM bug-2958 probe] positions every 500ms (loop detector)
    int    m_iLoopIdx;
    int    m_iLoopT;
    int    m_iLoopLogT;
    Vector m_vLinkLogA;          // [HZM bug-2964 probe] BOTLINK: the off-mesh link being crossed (ends, area, start time,
    Vector m_vLinkLogB;          //   blocks and origin at its start, the last origin - a respawn teleport drops the record)
    Vector m_vLinkLogO;
    Vector m_vLinkLogLast;
    int    m_iLinkLogT0;
    int    m_iLinkLogArea;
    int    m_iLinkLogBlk;
    Vector m_vLinkReachA;        // [HZM bug-2957] the JUMP link take-off being approached, the closest we got, since when
    float  m_fLinkReachBest;
    int    m_iLinkReachT;
    Vector m_vLiftGoal;          // the goal we were routing to - re-issued once we are off the lift
    Vector m_vLiftQueue;         // cached queue spot beside our door (validated by trace)
    int    m_iLiftQueueT;        // when m_vLiftQueue was chosen (re-validated every 2s)
    int    m_iLadderStart;       // [HZM bug-2871] when the current ladder traversal began (0 = none)
    int    m_iLadderDir;         // [HZM bug-2871] +1 climbing up, -1 climbing down, 0 not on a ladder route
    bool   m_bWasOnLadder;       // [HZM bug-2871] for the ^~^~^ BOTLADDER mount/dismount probe line
    int    m_iLadderQLog;        // [HZM bug-2871] throttle for the ^~^~^ BOTLQ queue-wait probe line
    int    m_iLadderWaitStart;   // [HZM bug-2871] when this bot started waiting in a ladder queue (0 = not waiting)
    int    m_iDoorWaitStart;     // [HZM bot A4] when this bot stopped at a closed door (0 = none)
    int    m_iDoorWaitUntil;     // [HZM bot A4] still waiting for the door this frame
    int    m_iLeafRoundNext;     // [bot_doorLeaf] next time a go-round of an open door leaf may be issued
    int    m_iLeafTouchT;        // [bot_doorLeaf] start of the current contact episode with an open leaf (0 = none)
    int    m_iLeafLastT;         // [bot_doorLeaf] last frame the open leaf was touched
    int    m_iLeafSteerLogT;     // [bot_doorLeaf 2] probe throttle
    // [bug-3086 bot_runJump] run-up jump at a steep lip a standing hop failed on
    Vector m_vRJFrom;            // take-off point of the last stuck hop
    Vector m_vRJDir;             // its heading (flat, unit)
    float  m_fRJZ;               // its height
    int    m_iRJHopT;            // when it jumped (0 = not watching)
    int    m_iRJPhase;           // 0 none, 1 back off, 2 run, 3 in the air
    int    m_iRJPhaseT;          // phase start
    Vector m_vRJSpot;            // the spot the tries are counted for
    int    m_iRJSpotT;
    int    m_iRJTries;
    bool   SteepLipAhead(const Vector& dir, float& nz);
    Vector LeafDeflect(const Vector& delta); // [bot_doorLeaf 2] steer round an open door leaf the next leg crosses
    Vector m_vDoorFace;          // [bot_doorFace] the closed door's centre at eye height, while waiting at it
    int    m_iDoorLogged;        // [bot_doorFace probe] 1 = wait start logged, 2 = give-up logged
    int    m_iHoldUntil;         // [HZM bot breach] standing still (path kept) until this inttime
    const char *m_szWhy;         // [HZM bot probe2] tag for the NEXT route request (consumed by it)
    const char *m_szGoalWhy;     // [HZM bot probe2] tag of the route being walked
    const char *m_szGoalLogWhy;  // [HZM bot probe2] BOTGOAL de-dup: last logged tag / target / time
    Vector      m_vGoalLogPos;
    int         m_iGoalLogT;
    const char *m_szJumpLogWhy;  // [HZM bot probe2] BOTJUMP throttle
    int         m_iJumpLogT;
    int         m_iPathJumpNext;     // [HZM bug-2879] no path-hop before this (one per 1.5s at most)
    int         m_iRerouteNext;      // [HZM bug-2879] no new object go-around before this
    int         m_iObjLogT;          // [HZM bug-2879] BOTOBJ throttle
    Vector      m_vObjArrivePos;     // [HZM bug-2888] where the route toward the objective ENDED (goal off-mesh)
    int         m_iObjArriveT;       // 0 = none
    Vector      m_vWedgePos;         // [HZM bug-2885] the current wedge episode: where, when, how many hops so far
    int         m_iWedgeT;
    int         m_iWedgeHops;
    bool        m_bCalm;             // [HZM bug-2899] set by the brain each frame: no enemy / shot / cover fight / nade
    int         m_iLookCompUntil;    // [T2 corner checks] rescale the move input while the eyes are off the travel line
    int         m_iSepT;             // [T2] the bot-bot separation sidestep last fired at
    Vector      m_vProgPos;          // [HZM bug-2899] no-progress window: where it began, when (0 = not running)
    int         m_iProgT;
    Vector      m_vSolidPos;         // [HZM bug-2891] frozen-origin sampler: where / when / how many still samples
    int         m_iSolidT;
    int         m_iSolidN;
    int         m_iSolidLogT;
    int         m_iSlideLogT;        // [bug-2966 probe] BOTSLIDE throttle
    int         m_iSlideT;           // [bug-2966] sliding (wants to move, ground too steep to walk) since (0 = not)
    Vector      m_vSlidePos;         // [bug-2966] where that began
    int         m_iSlideSampT;       // [bug-2966] slide learner: 500ms samples, the last 8 (4s) as bits + positions
    int         m_iSlideRing;
    int         m_iSlideIdx;
    Vector      m_vSlideRingPos[8];
    int         m_iPathEndCount;     // [HZM bug-2879] routes that ended the moment they were made, in this window
    int         m_iPathEndWindow;
    Vector      m_vPathEndPos;
    int         m_iDirectSteerUntil; // [HZM bug-2879] walking straight at the goal (off the navmesh) until
    Vector      m_vDirectSteerDir;
    void        LogGoal(const char *kind, const char *why, const Vector& to, bool ok);
    Vector m_vRouteWaypoint;     // [HZM bot route detour] per-bot approach waypoint that forces a non-shortest road (x=1e9 = none)
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
    void          SetCombatTurn(bool bCombat) { m_bCombatTurn = bCombat; } // [HZM] slower, human turn while aiming
    void          SetTurnMul(float f) { m_fTurnMul = f; }                   // [HZM bot D1] per-bot combat turn speed
    void          SetPrecise(bool b) { m_bPrecise = b; }                    // [HZM bot breach] converge fully (per frame)

private:
    SafePtr<Player> controlledEntity;

    Vector m_vTargetAng;
    Vector m_vCurrentAng;
    Vector m_vAngDelta;
    Vector m_vAngSpeed;
    bool   m_bCombatTurn;
    float  m_fTurnMul; // [HZM bot D1]
    bool   m_bPrecise; // [HZM bot breach]
};

// [HZM bot room-clear step 2] bot_nadeSafe: one grenade throw solved incrementally over a few frames (a pitch sweep x a
// few charges, each arc simulated to DETONATION with the engine's toss rules), under a server-wide trace budget
struct BotSolveJob {
    int    state; // 0 idle, 1 running, 2 solved, 3 no arc
    int    kind;  // 0 lob, 1 door, 2 hatch up, 3 hatch down
    Vector target[2];  // [0] scores every arc; [1] (door) the straight-through aim point
    int    nTargets;   // aim lines swept (yaw[])
    float  yaw[8];
    Vector gate, D, n; // gate: what the arc must get past; D/n: the opening and the way through it (kind 1)
    Vector eye, T;     // where it is thrown from, where the thrower stands
    int    ti, fi, pi; // sweep position: target (yaw), charge, pitch
    int    arcs, traces, frames, startT;
    bool   okT, okC;         // best arc with the thrower safe where it stands / best that needs it to take cover
    float  errT, errC, fT, fC, lifeT, lifeC;
    Vector angT, angC, detT, detC, landT, landC;
    Vector ang, det, land, S; // result: view angles, detonation, first impact, where the thrower stands at detonation
    float  f, hold, fuse, R;
    bool   needCover;
    int    rj[12];
};

// [user 2026-09-25, team tactics T2b] an OPENING the route passes through (a doorway / archway / gap in a wall), found by
// BotController::ScanOpening, with the two hard corners just inside it. Shared with the room-clear op (entry aims).
struct BotOpening {
    int    state;   // 0 none, 1 tracking (approaching / crossing it)
    int    kind;    // 1 a door entity's doorway, 2 a gap in a wall
    int    id;      // probe id
    Vector D;       // centre of the opening at floor height
    Vector n;       // the way through it (into the room)
    Vector t;       // along the wall: +t is the right of n
    float  w;       // width
    Vector K[2];    // aim points in the two hard corners: [0] on the +t side, [1] on the -t side
    bool   ok[2];   // that side has a hard corner (not an open hall, not blocked)
    float  c[2];    // how far along the wall each corner is (probe)
    int    first;   // corner looked at first (the far one - or the one a team-mate did not take)
    int    t0;      // tracking since
    int    aimed;   // frames the eyes were really put on a corner (0 in shadow mode)
    int    nearT;   // v2: crossing began (the near-corner look is 0.7s, the far one 0.5s after it)
    int    visT[2]; // v2: each corner is only looked at while the eye can see it (re-tested every 200ms)
    bool   vis[2];
    bool   crossed; // the bot has passed the opening's plane
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
    int m_iReactUntil;    // [HZM] no turning-to-aim or firing at a freshly locked enemy before this (reaction time)
    Vector m_vCoverPos;       // [HZM] committed cover spot (FindCoverPosition samples around the bot, so it drifts)
    int    m_iCoverUntil;     // [HZM] hold m_vCoverPos until this inttime; 0 = no commitment
    int    m_iCoverRetryTime; // [HZM] no cover found: don't re-run the 7-trace search before this
    int m_iProbeLastTime; // [HZM bot probe] throttle: last inttime ProbeThink logged for this bot

    // [HZM bot B-D] awareness, tactics, personality (user 2026-09-24). Each piece has its own bot_* cvar.
    int    m_iInvestigateUntil; // B1/B2: go and look at a heard / last-seen enemy until this time
    Vector m_vInvestigatePos;
    Vector m_vInvestigateIssued;
    int    m_iAlertUntil;       // B1/B2: face this spot (heard / lost enemy) until this time
    Vector m_vAlertPos;
    int    m_iGlanceNext;       // D2: idle look-around
    int    m_iGlanceUntil;
    float  m_fGlanceYaw;
    int    m_iVoiceNext;        // D3: this bot's next allowed callout
    float  m_fSkillReact;       // D1: personality multipliers
    float  m_fSkillAim;
    float  m_fSkillAggro;
    float  m_fSkillTurn;
    int    m_iLastEnemySeenAny; // D3: for the "enemy spotted" callout
    int    m_iNadeScanTime;     // B4: grenade flee scan throttle
    int    m_iGrenadeFleeUntil;
    int    m_iCrouchStuckSince;  // [HZM bug-2900] crouched, wanting to move, and not moving: since when (0 = not)
    int    m_iNoCrouchUntil;     // [HZM bug-2900] stand up until then
    int    m_iCrouchTapNext;     // [HZM bug-2918] next crouch TAP allowed (crouch is a toggle - see BrainThink)
    int    m_iNadeState;        // B4: throw sequence (0 none, 1 switching, 2 cooking, 3 released)
    int    m_iNadeTime;
    int    m_iNadeStart;
    int    m_iNadeNext;
    float  m_fNadeHold;
    Vector m_vNadeAim;          // [HZM bot room-clear step 1] the view State_Grenade asked for this frame
    int    m_iNadeClobbered;    // [HZM bot room-clear step 1] 1 trigger / 2 aim overwritten mid-throw (logged once)
    Vector m_vNadeTarget;
    SafePtr<Weapon> m_pNadeWeapon;
    int    m_iLaneState;        // C1: 0 undecided, 1 on the lane leg, 2 done
    int    m_iLaneSince;
    Vector m_vLanePoint;
    int    m_iRetreatUntil;     // C3
    int    m_iRetreatNext;
    bool   m_bHolder;           // C4: this bot holds the team's front line
    bool   m_bHolding;
    int    m_iHoldCheck;
    Vector m_vHoldPos;
    Vector m_vHoldIssued;
    int    m_iHoldSince;        // C4: when this holder reached the line
    int    m_iHoldCooldown;     // C4: attacking instead of holding until this time
    bool   m_bWantCrouch;       // D4: a tactic asked for a crouch this frame
    Vector m_vHoldCover;        // C4: the low-cover spot this holder set up behind (vec_zero = none / in the open)
    int    m_iHoldCoverTry;     // C4: looked for cover at this hold already
    int    m_iHoldZone;         // [bug-2959] the zone the current hold point was taken from (bot_holdFrontLine)

    // [HZM bot breach] grenade a door / ladder hatch before going through it (user 2026-09-24)
    int    m_iNadeMode;         // 0 = lob at a hidden enemy, 1 = breach (clearing throw)
    int    m_iBreachNext;       // next breach look-ahead check
    int    m_iBreachKeyEnt;     // the door entnum (or -1 for a ladder) the current roll is about
    Vector m_vBreachKey;        // the ladder foot / door centre the current roll is about
    int    m_iBreachKeyTime;
    bool   m_bBreachWant;       // the roll said "clear it"
    int    m_iBreachKind;       // 1 door, 2 ladder up, 3 ladder down
    Vector m_vBreachGate;       // doorway / ladder end the grenade must get past
    Vector m_vBreachAng;        // solved throw view angles
    Vector m_vBreachLand;       // predicted first impact
    float  m_fBreachFuse;       // predicted fuse (s) for the solved charge
    int    m_iBreachGoAt;       // thrower: call "move in" at this time (0 = none)
    int    m_iBlastCoverId;     // the team blast this bot already took cover from
    int    m_iBlastWaitId;      // the team blast this bot is already holding back for
    Vector m_vHeardPos;         // B1: the last enemy heard (for "is that room occupied?")
    int    m_iHeardTime;

    // [HZM bot room-clear step 2] GRENADE SAFETY (bot_nadeSafe 1; 0 = the step-1 code path). m_iNadeState gains 4 = SOLVE
    // (standing still, grenade not drawn yet, the throw solved over a few frames); 1 draw + aim, 2 cook, 3 released.
    BotSolveJob m_solve;
    int    m_iNadeKind;         // the solve kind of this throw (0 lob, 1 door, 2/3 hatch)
    Vector m_vNadeTarget2;      // a second aim line (door: straight through the middle of the doorway)
    Vector m_vNadeD;            // the opening (kind 1) and the way through it
    Vector m_vNadeN;
    Vector m_vNadeT;            // where the throw was solved from (the thrower is held there)
    Vector m_vNadeDet;          // the solver's predicted detonation point
    int    m_iNadeButtons;      // the trigger State_Grenade wants this frame - re-asserted after BrainThink (vet B1)
    int    m_iNadeSolveT;       // entered SOLVE at
    int    m_iNadeReadyT;       // the grenade came up (ready to fire) at
    int    m_iNadeCookT;        // the torso entered CHARGE_ATTACK_GRENADE at (the charge clock runs from there, 0 = not yet)
    int    m_iNadeHoldUntil;    // the movement hold WE set (only released while it is still ours)
    int    m_iNadeSpawnId;      // BOTNADE id of the projectile this throw spawned (0 = not out yet)
    int    m_iNadeSpawnT;
    int    m_iBreachRetry;      // no-arc solves at this door so far (walk closer, try again)
    int    m_iBlastCoverUntil;  // moving to / standing on a blast-cover spot until
    Vector m_vBlastCoverSpot;
    int    m_iNadeCrouchUntil;  // no blast cover reachable in time: crouch where we stand until
    float  m_fPace;             // measured running pace (u/s): can it reach cover within the fuse?

    // [HZM bot cover] FIGHTING FROM COVER (user 2026-09-24): hide / peek / lean / blind fire, and bound cover to cover
    int    m_iCfState;          // 0 none, 1 running to a cover spot, 2 hidden, 3 peeking
    // [user 2026-09-25] bandage when out of harm's way (HealThink)
    int    m_iHealState;        // 0 none, 1 stop + face, 2 press USE, 3 channel, 4 side-step out of an aborted channel
    int    m_iHealT;            // entered the current heal state at
    int    m_iHealNext;         // no new attempt before this
    int    m_iHealFails;        // failed attempts this life
    bool   m_bHealOut;          // the heal never started (no bandage left / Medkits off / refused): stop for this life
    int    m_iHealHoldUntil;    // the movement hold WE set - only released while it is still ours
    int    m_iHealStepUntil;
    int    m_iHealStepDir;
    int    m_iHealDmgT;         // last time health dropped (any source)
    int    m_iHealSlowSince;    // XY speed under 5u/s since (0 = not)
    int    m_iHealSafeT;        // last safety evaluation (traces at most every 500ms)
    int    m_iHealYawTry;       // turned away from something usable in front this many times
    float  m_fHealHp0;
    float  m_fHealLastHp;
    float  m_fHealYaw;          // facing while healing: toward the threat axis (plus the usable-avoid offset)
    bool   m_bHealCrouch;       // the cover only hides a crouched eye
    Vector m_vHealThreat;       // the threat position the safety test used (probe distance)
    int    m_iCfType;           // 1 LOW (crouch behind it, stand up over it to shoot), 2 CORNER (step out + lean round it)
    int    m_iCfLean;           // corner: +1 the open side is on our right, -1 on our left
    Vector m_vCfPos;            // the hide spot
    int    m_iCfUntil;          // the current hide / peek phase ends
    int    m_iCfCycles;
    int    m_iCfMaxCycles;
    int    m_iCfSeenInPeek;     // this peek saw the enemy at least once
    int    m_iCfBlindPeeks;     // peeks in a row that saw nobody
    int    m_iCfExposedSince;   // hidden but the enemy can see us (flanked) since
    int    m_iCfSearchNext;     // next bound / cover search
    int    m_iCfTakeNext;       // next take-cover-on-contact search
    int    m_iCfNoSession;      // no new session before (just left one)
    int    m_iCfMoveUntil;      // state 1 gives up
    int    m_iCfLastTick;       // CoverFight ran (stale session guard)
    bool   m_bCfBlind;          // this hide phase blind-fires over the top
    int    m_iCfStrafe;         // this frame's positional correction (applied after MoveThink)
    int    m_iNoObjSince;       // [HZM bug-2879] idle with no objective since (0 = has one)
    int    m_iAtObjSince;       // [HZM bug-2884] standing at the objective since
    int    m_iHuntUntil;        // [HZM bug-2884] out hunting the fight until
    int    m_iHuntNext;
    int    m_iSightMarkNext;    // [HZM bug-2884] team sighting mark throttle
    bool   m_bHuntDefend;       // [HZM bug-2890] this hunt is a no-knowledge DEFEND of our own end: hold on arrival
    bool   FindHuntTarget(Vector& out, int& kind);
    int    m_iCfSteerStall;     // CoverSteerTo: pushing without moving since (can't get closer: stop pushing)
    int    m_iCfFwd;

    // [user 2026-09-25, team tactics T2] CORNER CHECKS while pathing (bot_cornerCheck)
    BotOpening m_open;          // the opening the route is passing through (T2b)
    int    m_iOpenScanNext;     // next opening scan
    int    m_iCornerAimT;       // last frame a corner / sector aim owned the view (BrainThink then skips the glance)
    Vector m_vPieCorner;        // T2a: the route corner being pied, and since when (0 = none)
    int    m_iPieT;
    int    m_iPieAimed;
    int    m_iPieKey;           // T2a probe sampling
    int    m_iPieVisT;          // T2a v2: is the corner blind (the next leg hidden from here) - re-tested every 250ms
    bool   m_bPieBlind;
    // the 3s after crossing an opening / rounding a route corner: what happened (BOTCORNEROUT) - both arms of the A/B
    int    m_iCrossT;           // crossed at (0 = no window open)
    int    m_iCrossKind;        // 1 door, 2 gap, 3 route corner
    int    m_iCrossAim;         // the eyes were put on its corners (0 = shadow / not possible)
    int    m_iCrossId;
    int    m_iCrossHurt;        // ms after crossing of the first enemy hit (-1 none)
    int    m_iCrossAcq;         // ms after crossing of the first enemy acquired (-1 none)
    float  m_fCrossAcqAng;      // view-to-enemy angle at that acquisition
    int    m_iCrossKill;
    bool   CornerThink(void);   // T2: sets the view on a corner this frame (true), or leaves it to the caller
    bool   ScanOpening(BotOpening& o);
    void   CornerCorners(BotOpening& o) const;
    void   CrossBegin(int kind, int aimed, const Vector& at);
    void   CrossEnd(const char *why);
    void   AimAtPath(void);     // AimAtAimNode with the corner / sector checks in front of it (non-combat callers)

    // [user 2026-09-25, team tactics T3] BUDDY PAIRS (bot_buddy): friendlies close by + enemy heard / seen -> pair up for a
    // while: split the aim sectors on the move, back to back when both are holding, alternate the bounds in a fight
    SafePtr<Player> m_pBuddy;
    bool   m_bBuddyLead;        // the lower entnum leads (the ladder / lift queues use the same order)
    int    m_iBuddyT0;          // paired at
    int    m_iBuddyNext;        // next pairing attempt
    int    m_iBuddyStillT;      // both standing since (back to back after 1.5s)
    int    m_iBuddySectT;       // follower: the current look (flank / ahead) ends at
    bool   m_bBuddyFlank;       // follower: looking out to the flank in this slice
    int    m_iBuddySide;        // follower: +1 right / -1 left flank
    float  m_fB2BYaw;           // back to back: the sector this bot faces
    bool   m_bB2B;              // back to back this frame
    int    m_iB2BLogT;
    int    m_iBuddyKills;       // while paired (BOTBUDDY split line)
    int    m_iOverwatchLogT;
    BotController *BuddyCtl(void) const; // the partner's controller when the pair is still valid (alive, same side, mutual)
    void   BuddyThink(void);
    void   BuddySplit(const char *why);
    bool   BuddySectorAim(void);
    bool   BuddyTraveling(Vector& dir); // moving along a route: its XY direction

    // [user 2026-09-25, room-clear steps 3-5] STACK AND CLEAR: a team-mate's clearing grenade going into a room -> the bots
    // near the door stack beside it, out of the blast, and after it has gone off enter together, each clearing a corner
    int    m_iOpId;             // the op this bot is in (0 none) - thrower or stacker
    int    m_iOpSlot;           // stacker slot (-1 = the thrower)
    int    m_iOpMoveT;          // stacker: set off for its spot at
    bool   m_bSawLast;          // [step 3] could see its enemy last frame (VANISH: the spot it ducked out of sight)
    bool   m_bBreachRollSus;    // [step 3] the current door roll was made WITH evidence (a no-evidence "no" re-rolls on new evidence)
    int    m_iDefendAt;         // [step 5] an enemy grenade came in: hold the angle it came from, from ... to ...
    int    m_iDefendUntil;
    Vector m_vDefendFrom;
    int    m_iDefendNade;       // the projectile (entnum + 1) it is about
    bool   m_bDefendPush;
    bool   OpForm(void);        // the thrower, at the draw
    void   OpThink(void);       // every frame: stacker / thrower / non-member side of the live ops
    void   OpLeave(const char *why);
    bool   OpAim(void);         // the stacker's / entrant's eyes (AimAtPath)
    void   OpThrown(float life, const Vector& det); // the thrower's grenade has left the hand
    void   DefendThink(void);

    void RollPersonality(void);
    void VoiceCallout(const char *code, int iCooldownMs);
    void BrainThink(void);
    void CheckGrenadeThreat(void);
    bool FindFragGrenade(Weapon *&out);
    bool IdleHoldFront(void);
    bool IdleLane(void);
    bool FindLowCover(const Vector& centre, const Vector& threatDir, float maxFromCentre, Vector& out);
    bool CheckNadeLob(void);
    bool CheckBreach(void);
    bool BreachSuspect(const Vector& p) const;
    bool SolveThrow(Weapon *w, const Vector& target, int kind, const Vector& gate, Vector& ang, float& hold, Vector& land, float& fuse);
    void CheckFriendlyBlast(void);
    // [HZM bot room-clear step 2] bot_nadeSafe
    void State_GrenadeSafe(void);
    void NadeSafeEnd(const char *why);
    bool CheckNadeLobSafe(void);
    bool BreachStartSafe(Weapon *w, int kind, const Vector *tl, int nt, const Vector& gate, const Vector& D, const Vector& n);
    void SolveStart(void);
    void SolveThink(void);
    void SolveArc(float f, float elev, float yaw, int ti);
    void SolveFinish(void);
    bool FindBlastCover(
        const Vector& det, float R, const Vector& from, float timeLeft, Vector& out, float *travel, Vector *pFar = NULL
    );
    bool MovementLocked(void) const;
    void CheckGrenadeThreatSafe(void);
    void CheckFriendlyBlastSafe(void);
    bool CoverFight(bool bCanSee, Weapon *pWeap);
    bool StartCoverSession(const Vector& threat, const char *why, const Vector *spot = NULL);
    void EndCoverSession(const char *why);
    void HealThink(void);                           // [user 2026-09-25] bandage when out of harm's way
    float VisionDistance(void) const;               // [user 2026-09-25] AI vision cap, longer for rifles / scopes
    bool HealSafe(Vector& threat, bool& crouch);
    void HealEnd(const char *ev, const char *why);
    int  DetectCoverAt(const Vector& pos, const Vector& threat, int& lean) const;
    bool FindFightCover(const Vector& threat, const Vector& toward, bool bAdvance, Vector& out);
    bool TryBound(bool bCanSee);
    bool TryTakeCover(bool bCanSee);
    void CoverSteerTo(const Vector& spot, float tol);

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
    void OnNadeSpawn(Projectile *proj, Weapon *weap, float fraction, float life, const char *how); // telemetry
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
