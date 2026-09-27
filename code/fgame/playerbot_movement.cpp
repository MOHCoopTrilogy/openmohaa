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
// playerbot_movement.cpp: Manages bot movements

#include "playerbot.h"
#include "doors.h" // [HZM bot A4]
#include "debuglines.h"
#include "navigation_recast_config_ext.h" // [HZM bug-2862] RECAST_AREA_ELEVATOR
#include "navigation_recast_load_ext.h"   // [HZM bug-2898] NavLearn_MarkSteep

static int maxFallHeight = 400;

BotMovement::BotMovement()
{
    controlledEntity = NULL;

    m_pPath         = IPather::CreatePather();
    m_iLastMoveTime = 0;

    m_bPathing       = false;
    m_iTempAwayState = 0;
    m_fAttractTime   = 0;

    m_iCheckPathTime = 0;
    m_iTempAwayTime  = 0;
    m_iNumBlocks       = 0;
    m_iStuckPushTime   = 0; // [HZM bot wall-slide]
    m_iBlockLogTime    = 0; // [HZM bot blocker-probe]
    m_vBlockCheckPos   = Vector(0, 0, 0); // [HZM bot blocker-probe]
    m_iFeelerCommitTime = 0; // [HZM bot feelers]
    m_iExploreCommitTime = 0; // [HZM bot stuck-escape]
    m_iElevatorStart     = 0; // [HZM bug-2862]
    m_pLiftRec           = NULL; // [HZM bug-2912]
    m_iLiftState         = LIFT_NONE;
    m_iLiftFrom          = 0;
    m_iLiftTo            = 1;
    m_iLiftProgressT     = 0;
    m_iLiftOpenT         = 0;
    m_iLiftStateT        = 0;
    m_iLiftLogT          = 0;
    m_iLiftSide          = 1;
    m_iLiftSlot          = 0;
    m_iLiftBlockT        = 0; // [HZM bug-2949]
    m_iLoopIdx           = 0; // [HZM bug-2958 probe]
    m_iLoopT             = 0;
    m_iLoopLogT          = 0;
    m_vLinkLogA          = vec_zero; // [HZM bug-2964 probe]
    m_vLinkLogB          = vec_zero;
    m_vLinkLogO          = vec_zero;
    m_vLinkLogLast       = vec_zero;
    m_iLinkLogT0         = 0;
    m_iLinkLogArea       = 0;
    m_iLinkLogBlk        = 0;
    m_vLinkReachA        = vec_zero; // [HZM bug-2957]
    m_fLinkReachBest     = 0;
    m_iLinkReachT        = 0;
    m_iJumpLinkHops      = 0; // [HZM bug-2913]
    m_iJumpLinkT         = 0;
    m_iLiftQueueT        = 0;
    m_iLadderStart       = 0; // [HZM bug-2871]
    m_iLadderDir         = 0;
    m_bWasOnLadder       = false;
    m_iLadderQLog        = 0;
    m_iLadderWaitStart   = 0;
    m_iDoorWaitStart     = 0; // [HZM bot A4]
    m_iDoorWaitUntil     = 0;
    m_iLeafRoundNext     = 0;
    m_iLeafTouchT        = 0;
    m_iLeafLastT         = 0;
    m_iLeafSteerLogT     = 0;
    m_iDoorLogged        = 0;
    m_iHoldUntil         = 0; // [HZM bot breach]
    m_szWhy              = NULL; // [HZM bot probe2]
    m_szGoalWhy          = "none";
    m_szGoalLogWhy       = NULL;
    m_vGoalLogPos        = vec_zero;
    m_iGoalLogT          = 0;
    m_szJumpLogWhy       = NULL;
    m_iJumpLogT          = 0;
    m_iPathJumpNext      = 0;
    m_iRerouteNext       = 0;
    m_iObjLogT           = 0;
    m_vObjArrivePos      = vec_zero;
    m_iObjArriveT        = 0;
    m_vWedgePos          = vec_zero;
    m_iWedgeT            = 0;
    m_iWedgeHops         = 0;
    m_iSolidT            = 0;
    m_bCalm              = false;
    m_iLookCompUntil     = 0; // [T2 corner checks]
    m_iSepT              = 0;
    m_iProgT             = 0;
    m_iSolidN            = 0;
    m_iSolidLogT         = 0;
    m_iSlideLogT         = 0; // [bug-2966]
    m_iSlideT            = 0;
    m_vSlidePos          = vec_zero;
    m_iSlideSampT        = 0;
    m_iSlideRing         = 0;
    m_iSlideIdx          = 0;
    m_iPathEndCount      = 0;
    m_iPathEndWindow     = 0;
    m_vPathEndPos        = vec_zero;
    m_iDirectSteerUntil  = 0;
    m_vDirectSteerDir    = vec_zero;
    m_vRouteWaypoint = Vector(1.0e9f, 1.0e9f, 1.0e9f); // [HZM bot lookahead] none
    m_iFeelerSide       = 1;

    m_bAvoidCollision     = false;
    m_iCollisionCheckTime = 0;

    // [HZM Phase 2] sentinel far from any real map coord so the FIRST attract selection always rolls a scatter
    // goal; thereafter it is re-rolled only when the node origin actually moves (see MoveToBestAttractivePoint).
    m_vScatterAnchor = Vector(1.0e9f, 1.0e9f, 1.0e9f);
}

BotMovement::~BotMovement()
{
    delete m_pPath;
}

void BotMovement::SetControlledEntity(Player *newEntity)
{
    controlledEntity = newEntity;
}

// [HZM bot feelers] Clearance (trace fraction 0..1) of a whisker cast from the bot along dirAngles rotated by
// degOff degrees of yaw, out to len units, using the bot's own collision box. 1 = fully clear, near 0 = wall
// right in front. This is the "always aware of surroundings" sense the reactive block-recovery lacked (that
// one only looked 32u ahead every 250ms; these look ~112u every frame). Bot-only.
// [bot_slopeWalk] a horizontal step-height probe that stops on a plane pmove can WALK (normal z >= MIN_WALK_NORMAL) hit
// the ground rising ahead - a hillside, a ramp - not an obstacle: the bot walks it (PM_ClipVelocity keeps the horizontal
// speed on walkable ground). On a 30-38 deg slope the ground rises more than a step within the probes' 30-150u, so the
// feelers eased the throttle and steered off the line, the wall-slide called it blocked and strafe-hopped, and CheckJump
// hopped - each hop losing the climb (t2l1 (-352,-3504): 2201 stuck samples in 22 soaks, bots hopping in place on a
// normal-0.75-0.81 hillside). bot_slopeWalk 0 = stock.
static bool BotProbeHitWalkable(const trace_t& t)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_slopeWalk", "0", 0);
    }
    return s_on->integer && t.fraction < 1.0f && !t.startsolid && !t.allsolid && t.plane.normal[2] >= MIN_WALK_NORMAL;
}

float BotMovement::WhiskerClear(const Vector& dirAngles, float degOff, float len)
{
    Vector a = dirAngles;
    a[1] += degOff; // yaw
    Vector f, r, u;
    AngleVectors(a, f, r, u);
    f.z = 0;
    if (f.lengthSquared() < 0.01f) {
        return 1.0f;
    }
    VectorNormalize2D(f);

    Vector mins = controlledEntity->mins;
    Vector maxs = controlledEntity->maxs;
    maxs.z -= STEPSIZE;
    Vector base = controlledEntity->origin + Vector(0, 0, STEPSIZE);

    trace_t t = G_Trace(base, mins, maxs, base + f * len, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotFeeler");
    if (BotProbeHitWalkable(t)) {
        return 1.0f; // [bot_slopeWalk] the ground rises ahead: walkable, not a wall
    }

    // [HZM bot feelers] EXPECT GROUND (user): an opening with no floor under it is a pit, not a safe path - the
    // feeler must not steer the bot off a ledge. Drop a trace at the reached point; if no ground within a
    // survivable step-down (~STEPSIZE + 200), treat this direction as (almost) blocked so the steering avoids it.
    Vector  fwdPt = t.endpos;
    trace_t g     = G_Trace(
        fwdPt, mins, maxs, fwdPt - Vector(0, 0, STEPSIZE + 200.0f), controlledEntity, MASK_PLAYERSOLID, qtrue,
        "BotFeelerGround"
    );
    if (g.fraction >= 1.0f) {
        return Q_min(t.fraction, 0.15f);
    }
    return t.fraction;
}

/*
====================
CheckLookaheadReroute

[user 2026-09-21] PROACTIVE RANGED OBSTACLE AVOIDANCE. "A human wouldn't walk straight into a tank they saw
50+ feet away - they'd go around it." The stock recovery only reacts AFTER the bot is already grinding the
obstacle; the old close-range feelers reacted to WALLS and weaved (so they're disabled). The clean distinction:
a tank / turret / vehicle / mover is an ENTITY; walls and terrain are WORLD, which the navmesh path already
routes around. So we trace the bot's box FORWARD along the way it is actually moving, out to bot_lookahead
units, and reroute ONLY when a solid ENTITY (not world, not a teammate) looms ahead at range - picking the
clearer side, pushing an approach waypoint out past it, and committing to it so the bot arcs around well before
contact instead of walking into it. World hits are ignored, so it never re-plans around ordinary walls (no
weaving). bot_lookahead 0 restores stock. Bot-only (coop instantiates no BotMovement); no RecastPather change.
====================
*/
void BotMovement::CheckLookaheadReroute()
{
    static cvar_t *s_botLookahead = NULL;
    if (!s_botLookahead) {
        s_botLookahead = gi.Cvar_Get("bot_lookahead", "460", CVAR_ARCHIVE);
    }
    if (s_botLookahead->value < 1.0f || !controlledEntity) {
        return;
    }
    if (level.inttime < m_iExploreCommitTime) {
        return; // already detouring/escaping - let that commitment run
    }
    if (level.inttime < m_iRerouteNext) {
        return; // [HZM bug-2879] just went round something: no chain of detours
    }
    if (!m_pPath->GetNodeCount()) {
        return; // no path to look along
    }

    // Forward = the PATH's own current direction (the way the bot is actually travelling this instant). Using the
    // path delta, not a straight line to the far goal, means a path that curves around a wall points AWAY from
    // that wall, so we never react to geometry the navmesh already handled.
    Vector fdir = m_pPath->GetCurrentDelta();
    fdir.z = 0;
    if (fdir.lengthSquared() < Square(16)) {
        return;
    }
    VectorNormalize2D(fdir);

    Vector mins = controlledEntity->mins;
    Vector maxs = controlledEntity->maxs;
    maxs.z -= STEPSIZE;
    Vector base = controlledEntity->origin + Vector(0, 0, STEPSIZE);
    float  len  = s_botLookahead->value;
    {
        // [HZM bug-2879] look only along the route's CURRENT LEG (to its next corner): the straight 460u line ran past
        // the corner into objects the route never goes near - open door leaves, props beside a doorway - and each hit
        // committed a 2s sideways detour (probe2 e1l1: 'routewp' 18% of bot time, 20 reversals, 18 loops)
        Vector c[1];
        if (m_pPath->GetCorners(c, 1) == 1) {
            len = Q_min(len, (c[0] - controlledEntity->origin).lengthXY() + 32.0f);
        }
        if (len < 96.0f) {
            return;
        }
    }
    trace_t t =
        G_Trace(base, mins, maxs, base + fdir * len, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotLookahead");

    if (t.fraction >= 1.0f) {
        return; // clear ahead
    }
    // ONLY react to a solid OBJECT (an entity). World geometry (walls/terrain) is the navmesh's job.
    if (!t.ent || !t.ent->entity || t.ent->entity == world) {
        return;
    }
    if (t.ent->entity->IsSubclassOfPlayer()) {
        return; // a teammate ahead - the per-frame bot-bot separation owns that
    }
    if (t.ent->entity->IsSubclassOfDoor()) {
        return; // [HZM bug-2879] a door is walked THROUGH (BotMovement A4 waits for a shut one), not round
    }
    float blockDist = t.fraction * len;
    if (blockDist < 80.0f) {
        return; // basically on top of it - the reactive wall-slide owns point-blank
    }

    // Choose the clearer side to arc around the object.
    Vector fa = fdir.toAngles();
    float  lC = WhiskerClear(fa, 55.0f, len);  // +yaw = left
    float  rC = WhiskerClear(fa, -55.0f, len); // -yaw = right
    if (lC < 0.5f && rC < 0.5f) {
        // both forward diagonals also closing - probe wider
        lC = WhiskerClear(fa, 90.0f, len * 0.8f);
        rC = WhiskerClear(fa, -90.0f, len * 0.8f);
    }
    float bestClear = (lC >= rC) ? lC : rC;
    if (bestClear < 0.4f) {
        return; // boxed on every side - leave it to the stuck-escape / block recovery
    }
    float sideDeg = (lC >= rC) ? 72.0f : -72.0f;

    // Waypoint: out to the clear side, far enough to clear the object's width, a touch before the block so the
    // bot commits to the go-around lane. It re-paths to the real objective once it arrives (commit release).
    Vector sa = fa;
    sa[1] += sideDeg;
    Vector sfwd, sr, su;
    AngleVectors(sa, sfwd, sr, su);
    sfwd.z = 0;
    VectorNormalize2D(sfwd);
    Vector wp = controlledEntity->origin + sfwd * (blockDist * bestClear + 150.0f);

    // Commit to the detour: zero first so this MoveNear isn't blocked by the guard, then hold it so the
    // objective re-path can't yank the bot back into the object before it clears (same mechanism as the escape).
    m_iExploreCommitTime = 0;
    const Vector      vKeep   = m_vTargetPos; // [HZM bug-2879] the route we had, in case the go-around has none
    const char *const szKeep  = m_szGoalWhy;
    SetWhy("routewp");
    MoveNear(wp, 96.0f);
    if (IsMoving()) {
        static cvar_t *s_commit = NULL;
        if (!s_commit) {
            s_commit = gi.Cvar_Get("bot_stuckCommit", "2000", CVAR_ARCHIVE);
        }
        m_iExploreCommitTime = level.inttime + s_commit->integer;
        m_vRouteWaypoint     = wp;
        m_iRerouteNext       = level.inttime + s_commit->integer + 3000;
    } else {
        // [HZM bug-2879] no route round it: the failed request had already dropped the bot's route, and the next
        // frame rebuilt it and tried again - 59 failed go-arounds per bot-minute on The Rail Yard (probe2). Put the
        // old route back and leave it 2s.
        SetWhy(szKeep);
        MoveTo(vKeep);
        m_iRerouteNext = level.inttime + 2000;
    }
}

void BotMovement::MoveThink(usercmd_t& botcmd)
{
    m_szWhy = NULL; // [HZM bot probe2] a tag the brain set but no route call consumed this frame must not leak onto the next
    Vector vAngles;
    Vector vWishDir;
    Vector vDelta;

    botcmd.forwardmove = 0;
    botcmd.rightmove   = 0;

    CheckAttractiveNodes();

    // [HZM bug-2871] LADDERS. The navmesh's func_ladder links (NavigationMapExtension_Ladders, area 32) were routes
    // the bots could not complete: nothing held the climb inputs, and every stuck heuristic below (wall-slide strafe-
    // JUMP, CheckJump, the blocked re-path) fired at the foot of the ladder - and +JUMP is the ladder state machine's
    // "jump off" (player_Torso.st JUMP_OFF_LADDER). m3l1b's allies jump-spammed at bunker ladder *21 for 23% of three
    // heat rounds. Climbing is purely input-driven (player_Torso.st): mount = ONGROUND FORWARD + looking up >35 deg at
    // the bottom / down >35 at the top (or +USE, BotController::CheckUse); climb = FORWARD looking up (down to descend);
    // the state machine dismounts by itself at the far end (CAN_GET_OFF_LADDER_TOP/BOTTOM). So on a ladder route:
    // hold FORWARD, never jump or strafe, never re-path, and let BotController pitch the view (GetLadderSteer).
    const bool bLadderLink = (m_pPath->GetTraversingArea() == RECAST_AREA_LADDER);
    const bool bOnLadder   = controlledEntity->GetLadder() != NULL;
    if (bOnLadder != m_bWasOnLadder) {
        static cvar_t *s_probeL = NULL;
        if (!s_probeL) {
            s_probeL = gi.Cvar_Get("bot_probe", "0", 0);
        }
        if (s_probeL->integer) {
            gi.Printf(
                "^~^~^ BOTLADDER e=%d %s at=(%.0f %.0f %.0f) dir=%d link=%d\n", controlledEntity->entnum,
                bOnLadder ? "mount" : "dismount", controlledEntity->origin.x, controlledEntity->origin.y,
                controlledEntity->origin.z, m_iLadderDir, bLadderLink ? 1 : 0
            );
        }
        m_bWasOnLadder = bOnLadder;
    }
    if (bLadderLink) {
        const Vector d = m_pPath->GetCurrentDelta(); // toward the link's far end
        if (d.z > 16.0f) {
            m_iLadderDir = 1;
        } else if (d.z < -16.0f) {
            m_iLadderDir = -1;
        }
    } else if (!bOnLadder) {
        m_iLadderDir = 0;
    }
    const bool bLadder = (bLadderLink || bOnLadder) && m_iLadderDir != 0;
    // approaching a ladder link (<128u): no strafe-jump/wall-slide at the foot - a bot queueing behind a climber stands
    const bool bNearLadder = !bLadder && m_pPath->GetApproachingArea() == RECAST_AREA_LADDER;
    if (bOnLadder || (!bNearLadder && !bLadderLink)) {
        m_iLadderWaitStart = 0; // mounted, or left the ladder's approach: the next queue starts a fresh timer
    }
    if (bLadder) {
        if (!m_iLadderStart) {
            m_iLadderStart = level.inttime;
        } else if (level.inttime - m_iLadderStart > 20000) {
            // a climb that has not finished in 20s is not going to: jump off (on a ladder) and drop the route
            m_iLadderStart       = 0;
            m_iLadderDir         = 0;
            m_iExploreCommitTime = 0;
            SetWhy("ladderbail");
            ClearMove();
            if (bOnLadder) {
                Jump(botcmd, "ladderbail");
            }
            return;
        }
        m_iLastMoveTime      = level.inttime; // no 5s re-path mid-climb
        m_iCheckPathTime     = level.inttime; // no 1s blocked check
        m_iStuckPushTime     = 0;             // no wall-slide
        m_iExploreCommitTime = level.inttime + 500; // the brain's MoveTo/ClearMove must not yank the route mid-climb
        if (bOnLadder && !bLadderLink) {
            // the link already completed (we are at the far end but not yet stepped off): keep climbing to dismount
            botcmd.forwardmove = 127;
            return;
        }
    } else {
        m_iLadderStart = 0;
        if (bOnLadder) {
            // on a ladder with NO ladder route. player_Torso.st AUTO-mounts on FORWARD + looking down >35 deg at a ladder
            // top - exactly a bot aiming at someone below while walking past the edge (m3l1b ladder test: bots mounted
            // at (1819,-1496,268) mid-fight). Jumping off there dropped them down the shaft; instead climb off at the
            // NEAR end, as a player would (the state machine's GET_OFF_LADDER_TOP / _BOTTOM).
            const Entity *lad = controlledEntity->GetLadder();
            const float   mid = (lad->absmin.z + lad->absmax.z) * 0.5f;
            m_iLadderDir        = (controlledEntity->origin.z > mid) ? 1 : -1;
            m_iLadderStart      = level.inttime;
            botcmd.forwardmove  = 127;
            return;
        }
    }

    // [HZM bot breach] HOLD: stand where we are, keeping the route - lining up a clearing throw at a door / ladder hatch,
    // or letting a team-mate's clearing grenade go off before walking into the room. Nothing below may read the stop as
    // "stuck" (no re-path, no wall-slide, no ladder give-up). Never while physically on a ladder: a climber finishes.
    if (level.inttime < m_iDirectSteerUntil && !bOnLadder && !bLadder) {
        // [HZM bug-2879] off-mesh recovery (see the pathend clear): walk the chosen bearing, hop a lip if it clears
        Vector f, r;
        AngleVectors(Vector(0, controlledEntity->GetViewAngles().y, 0), f, r, NULL);
        const float df = m_vDirectSteerDir.x * f.x + m_vDirectSteerDir.y * f.y;
        const float dr = m_vDirectSteerDir.x * r.x + m_vDirectSteerDir.y * r.y;
        botcmd.forwardmove = (signed char)(df * 127.0f);
        botcmd.rightmove   = (signed char)(dr * 127.0f);
        if (LowObstacleAhead(m_vDirectSteerDir) && level.inttime >= m_iPathJumpNext) {
            m_iPathJumpNext = level.inttime + 1500;
            Jump(botcmd, "offmesh");
        }
        m_iCheckPathTime = level.inttime;
        m_iLastMoveTime  = level.inttime;
        return;
    }

    // [HZM bot room-clear step 2, vet B3] a hold never pre-empts the LIFT PROTOCOL once the bot is boarding, riding,
    // getting off or escaping the shaft: a bot held in the gate stops the lift for everyone (bug-2912). bot_nadeSafe 0 =
    // the step-1 order (hold first).
    static cvar_t *s_nadeSafeMv = NULL;
    if (!s_nadeSafeMv) {
        s_nadeSafeMv = gi.Cvar_Get("bot_nadeSafe", "1", 0);
    }
    const bool bLiftLive = s_nadeSafeMv->integer
                        && (m_iLiftState == LIFT_BOARD || m_iLiftState == LIFT_RIDE || m_iLiftState == LIFT_EXIT
                            || m_iLiftState == LIFT_SHAFT);
    if (level.inttime < m_iHoldUntil && !bOnLadder && !bLiftLive) {
        m_iCheckPathTime = level.inttime;
        m_iLastMoveTime  = level.inttime;
        m_iStuckPushTime = 0;
        // [2026-09-25] a hold is not "no progress": the no-progress learner and the embedded sampler must not count its
        // seconds toward their windows when the bot moves on (an 8s+ breach / bandage hold would mark the spot steep)
        m_iProgT  = 0;
        m_iSolidN = 0;
        if (m_iLadderStart) {
            m_iLadderStart = level.inttime;
        }
        return;
    }

    // [HZM bug-2912] LIFT PROTOCOL: a lift whose movers the navlinks line names is run start to finish by LiftThink
    // (queue beside the door, board on its centre line, ride, get off, never stand in a gate or under the cab). It
    // steers directly, so nothing below - stuck checks, wall-slide, sidestep, collision steering - may fight it.
    if (!bOnLadder && LiftThink(botcmd)) {
        m_iCheckPathTime = level.inttime;
        m_iLastMoveTime  = level.inttime;
        m_iStuckPushTime = 0;
        m_iNumBlocks     = 0;
        return;
    }

    {
        // [HZM bug-2879] RIDING a moving platform (the Push lift cab): stand still until it stops. The lift link can
        // already count as traversed once the bot reached the cab centre, and then the ordinary route-following ran
        // mid-ride - bots walked round the moving cab and the block logic (no horizontal progress) fired the stuck
        // escape out of it (probe2 Rail Yard: 12 escape samples on the cab; user: "glitched around and in it").
        // BUT only well INSIDE its footprint: frozen at the cab's doorway edge the rising floor carried the bot into the
        // lintel and the lift STALLED, crushing it for 26s (probe2 round 2: 17 MPELEV STALL, a bot 52u off the cab
        // centre on the exit side). On the edge of a moving platform: step IN toward its centre, then ride.
        const Entity *gnd = controlledEntity->groundentity ? controlledEntity->groundentity->entity : NULL;
        if (gnd && gnd != world) {
            // [HZM bug-2879] a THIN mover underfoot - a lift gate's top edge (probe2 round 3: a bot waiting on
            // $elevator_gate_2 was lifted into the lintel as it closed: stall + crush). Never stand on one: step off
            // along its thin axis, to the LANDING side when the route is at a lift (never into the shaft), else to
            // the side we are already on.
            const float ex = gnd->absmax.x - gnd->absmin.x;
            const float ey = gnd->absmax.y - gnd->absmin.y;
            const bool bAtLift = m_pPath->GetApproachingArea() == RECAST_AREA_ELEVATOR
                              || m_pPath->GetTraversingArea() == RECAST_AREA_ELEVATOR;
            // (only a thin thing that MOVES or sits at a lift: a narrow scripted plank bridge is walked, not left)
            if (Q_min(ex, ey) < 64.0f && (bAtLift || gnd->velocity.lengthSquared() > 1.0f)) {
                const Vector c((gnd->absmin.x + gnd->absmax.x) * 0.5f, (gnd->absmin.y + gnd->absmax.y) * 0.5f, 0);
                const Vector axis = (ex < ey) ? Vector(1, 0, 0) : Vector(0, 1, 0);
                const Vector org  = controlledEntity->origin;
                float        side = (org.x - c.x) * axis.x + (org.y - c.y) * axis.y;
                Vector       lf, lt;
                if (m_pPath->GetApproachingArea() == RECAST_AREA_ELEVATOR && m_pPath->GetApproachingLink(lf, lt)) {
                    side = (lf.x - c.x) * axis.x + (lf.y - c.y) * axis.y; // the landing we queue at
                }
                const Vector d = axis * (side >= 0.0f ? 1.0f : -1.0f);
                Vector       f, r;
                AngleVectors(Vector(0, controlledEntity->GetViewAngles().y, 0), f, r, NULL);
                botcmd.forwardmove = (signed char)((d.x * f.x + d.y * f.y) * 127.0f);
                botcmd.rightmove   = (signed char)((d.x * r.x + d.y * r.y) * 127.0f);
                m_iCheckPathTime   = level.inttime;
                m_iLastMoveTime    = level.inttime;
                m_iStuckPushTime   = 0;
                return;
            }
        }
        bool bCabRoute = false;
        if (gnd && gnd != world && (gnd->absmax.x - gnd->absmin.x) >= 64.0f && (gnd->absmax.y - gnd->absmin.y) >= 64.0f) {
            // ... but only until the cab is at the level the lift route goes TO - there the bot must walk out
            Vector lf, lt;
            if (m_pPath->GetApproachingArea() == RECAST_AREA_ELEVATOR && m_pPath->GetApproachingLink(lf, lt)) {
                bCabRoute = fabs(controlledEntity->origin.z - lt.z) > 48.0f;
            } else if (m_pPath->GetTraversingArea() == RECAST_AREA_ELEVATOR && m_pPath->GetElevatorEnd(lt)) {
                bCabRoute = fabs(controlledEntity->origin.z - lt.z) > 48.0f;
            }
        }
        // [HZM bug-2886] a bot already standing IN the lift cab whose route takes the lift walked OUT to the landing to
        // "start" the link, met a bot walking in, and both jammed on the bottom gate's edge as it closed (probe2 val4:
        // gate2close stall, one bot in the cab doorway heading out, one on the gate heading in). In the cab with a lift
        // route = already where the link would take it: stay in (centred), moving or not.
        if (gnd && gnd != world && (bCabRoute || gnd->velocity.lengthSquared() > Square(4.0f))) {
            m_iCheckPathTime = level.inttime;
            m_iLastMoveTime  = level.inttime;
            m_iStuckPushTime = 0;
            m_iNumBlocks     = 0;
            botcmd.forwardmove = 0;
            botcmd.rightmove   = 0;
            const float  inset = controlledEntity->maxs.x + 12.0f;
            const Vector org   = controlledEntity->origin;
            const bool   bInside = org.x > gnd->absmin.x + inset && org.x < gnd->absmax.x - inset
                              && org.y > gnd->absmin.y + inset && org.y < gnd->absmax.y - inset;
            if (!bInside) {
                Vector c((gnd->absmin.x + gnd->absmax.x) * 0.5f, (gnd->absmin.y + gnd->absmax.y) * 0.5f, 0);
                Vector d = c - org;
                d.z      = 0;
                if (d.lengthSquared() > 1.0f) {
                    VectorNormalize2D(d);
                    Vector f, r;
                    AngleVectors(Vector(0, controlledEntity->GetViewAngles().y, 0), f, r, NULL);
                    botcmd.forwardmove = (signed char)((d.x * f.x + d.y * f.y) * 127.0f);
                    botcmd.rightmove   = (signed char)((d.x * r.x + d.y * r.y) * 127.0f);
                }
            }
            return;
        }
    }

    if (!IsMoving()) {
        return;
    }

    // [HZM bug-2862] ELEVATOR link (navlinks "elevator"): the bot has reached the landing and is heading for the other
    // landing THROUGH the lift - pressing a closed shaft gate until the cab arrives, then standing in the cab while the
    // Push auto-elevator carries it. Every stuck heuristic reads that as "blocked" (no speed, no progress) and would
    // back it off, re-path it from mid-shaft or strafe-jump it out of the cab - so hold them off while on the link,
    // and give up only after a full cycle has clearly failed (45s). The object-avoidance layers below (look-ahead
    // re-route, FixDeltaFromCollision) must also stand down: the CAB is a solid mover straight ahead, and they
    // steered bots sideways along the open doorway instead of into it (soak ELEVDBG: bot slid x-4665 -> -4720).
    // Only once actually ON the link (trigger radius 96u = the whole landing). Suppressing while merely approaching
    // left two bots body-blocking each other at the landing with no recovery to separate them (soak ELEVDBG).
    const bool bOnElevator = (m_pPath->GetTraversingArea() == RECAST_AREA_ELEVATOR);
    // [HZM bug-2879] LIFT QUEUE: walking up to the lift link (start < 128u) with others there is waiting in line - no
    // stuck escape, no wall-slide hop onto the gate. Probe2 round 3: 24% of The Rail Yard's stuck escapes fired at
    // the lift landing ("glitched around it"). A closer team-mate standing on the way in: wait behind them.
    const bool bNearLift = !bOnElevator && m_pPath->GetApproachingArea() == RECAST_AREA_ELEVATOR;
    if (bNearLift) {
        m_iCheckPathTime = level.inttime;
        m_iLastMoveTime  = level.inttime;
        m_iStuckPushTime = 0;
        m_iNumBlocks     = 0;
    }
    if (bOnElevator) {
        if (!m_iElevatorStart) {
            m_iElevatorStart = level.inttime;
        } else if (level.inttime - m_iElevatorStart > 45000) {
            m_iElevatorStart = 0;
            SetWhy("liftgiveup");
            ClearMove();
            return;
        }
        m_iLastMoveTime  = level.inttime; // no 5s "new origin" re-path while the cab moves
        m_iCheckPathTime = level.inttime; // no 1s blocked check
        m_iStuckPushTime = 0;             // no wall-slide
        {
            // [HZM bug-2886] a SHUT gate ahead (a thin mover): wait a step back from it. Pressing the gate face as the
            // stock boarding does put the bot in the gate's own path - it jammed the top gate as it slid down to open
            // and was crushed for 20s+ (probe2 val4: gate1open stall).
            Vector wd = m_pPath->GetCurrentDelta();
            wd.z      = 0;
            if (wd.lengthSquared() > 1.0f) {
                VectorNormalize2D(wd);
                Vector       smins = controlledEntity->mins;
                Vector       smaxs = controlledEntity->maxs;
                smaxs.z -= STEPSIZE;
                const Vector sb = controlledEntity->origin + Vector(0, 0, STEPSIZE);
                trace_t      gt = G_Trace(sb, smins, smaxs, sb + wd * 40.0f, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotLiftGate");
                if (gt.fraction < 1.0f && gt.ent && gt.ent->entity && gt.ent->entity != world
                    && !gt.ent->entity->IsSubclassOfPlayer()) {
                    // any solid mover ahead on the lift route (the shut gate - its box need not be thin: round 9 still had
                    // a bot pressing $elevator_gate_2 as it opened, 3 short stalls); the open cab doorway traces clear
                    botcmd.forwardmove = 0;
                    botcmd.rightmove   = 0;
                    return;
                }
            }
        }
    } else {
        m_iElevatorStart = 0;
    }


    // [user 2026-09-21] PROACTIVE ranged go-around of solid OBJECTS ahead (tanks/turrets/vehicles/movers),
    // before the bot walks into them - see CheckLookaheadReroute. Runs before path-following so a reroute it
    // issues is followed THIS frame. World geometry is ignored (the navmesh owns walls), so no weaving.
    if (!bOnElevator && !bLadder) {
        CheckLookaheadReroute();
    }

    if (!IsMoving()) {
        return; // a reroute that found no path cleared the move; nothing to follow this frame
    }


    if (m_pPath->GetNodeCount()) {
        m_vTargetPos = m_pPath->GetDestination();
    }

    if (m_pPath->IsQuerying()) {
        m_iLastMoveTime = level.inttime;
    }

    if (level.inttime >= m_iLastMoveTime + 5000 && m_vCurrentOrigin != controlledEntity->origin) {
        m_vCurrentOrigin = controlledEntity->origin;

        if (m_pPath->GetNodeCount() && !controlledEntity->GetLadder()) {
            // recalculate paths because of a new origin

            PathSearchParameter parameters;
            parameters.entity     = controlledEntity;
            parameters.fallHeight = maxFallHeight;
            m_pPath->FindPath(controlledEntity->origin, m_pPath->GetDestination(), parameters);
        }

        m_iLastMoveTime = level.inttime;
    }

    if (m_iTempAwayState == 2 && level.inttime >= m_iTempAwayTime + 750) {
        m_iTempAwayState = 0;

        PathSearchParameter parameters;
        parameters.entity     = controlledEntity;
        parameters.fallHeight = maxFallHeight;
        m_pPath->FindPath(controlledEntity->origin, m_vTargetPos, parameters);

        m_iLastMoveTime  = level.inttime;
        m_iCheckPathTime = level.inttime;
    }

    vDelta = m_pPath->GetCurrentDelta();
    if (!bOnElevator && !bLadder) {
        vDelta = LeafDeflect(vDelta);           // [bot_doorLeaf 2] round an open door leaf the leg crosses
        vDelta = FixDeltaFromCollision(vDelta); // [HZM bug-2862] not into the lift cab (nor steered off a ladder)
    }

    if (m_pPath->GetNodeCount()) {
        m_pPath->UpdatePos(controlledEntity->origin);

        m_vCurrentGoal = controlledEntity->origin;
        VectorAdd2D(m_vCurrentGoal, vDelta, m_vCurrentGoal);

        if (MoveDone()) {
            // Clear the path
            m_pPath->Clear();
        }
    }

    if (ai_debugpath->integer) {
        G_DebugLine(controlledEntity->centroid, m_vCurrentGoal + Vector(0, 0, 36), 1, 1, 0, 1);
    }

    // Check if we're blocked
    if (level.inttime >= m_iCheckPathTime + 1000 && m_iTempAwayState != 2) {
        bool blocked = false;

        m_iCheckPathTime = level.inttime;

        // [user 2026-09-21] STUCK FAILSAFE - a bot must NEVER give up and stand still. Stock ClearMove() here
        // abandoned the move after ~5s of blocking, leaving the bot parked forever (the "stuck at the tank",
        // even with another road right there). Instead, ESCALATE to an explore burst: jump (to climb a low
        // blocker like a tank track or rubble lip) and fling the goal to a far point in a fresh random
        // direction, clearing the jammed path so the bot wanders off the choke, hits open ground, and re-paths
        // to the objective from there. The counter is knocked back (not zeroed), so if it is STILL stuck it
        // escapes again ~2s later, never terminating. This also spreads bots across routes (a jammed lane's
        // bots scatter onto the alternates). bot_stuckExplore 0 restores the stock give-up. Bot-only.
        static cvar_t *s_botStuckExplore = NULL;
        if (!s_botStuckExplore) {
            s_botStuckExplore = gi.Cvar_Get("bot_stuckExplore", "1", CVAR_ARCHIVE);
        }
        if (m_iNumBlocks >= 5) {   // [user 2026-09-21] REVERTED 3->5: the lower value fired the escape while a bot
                                   // was still legitimately navigating a tight lane, so it flung EVERY bot off the
                                   // tank route at once and they all re-converged on the same choke - "all bots
                                   // stick on the tank". 5 is the stock give-up threshold; keep it.
            if (s_botStuckExplore->integer) {
                // PATH to a far point in a random direction so the bot actually NAVIGATES off the choke.
                // (A bare goal with no path just stalls and the objective re-route drags it straight back
                // into the tank - the reason the earlier version didn't work.) MoveNear runs a real
                // FindPathNear to a reachable spot; the jump clears a low lip (tank track / rubble). Knock
                // the counter back, not to zero, so if it is STILL stuck it explores again ~2s later and
                // never permanently gives up. Over successive escalations the random directions fan the
                // bot onto the other roads.
                // [HZM bug-2879] not a RANDOM direction (half of those point back the way the bot came): score 8
                // bearings by how clear they are and how much they still lead toward the goal, and take the best
                Vector toGoal = m_vTargetPos - controlledEntity->origin;
                toGoal.z      = 0;
                if (toGoal.lengthSquared() > 1.0f) {
                    VectorNormalize2D(toGoal);
                }
                Vector vOff;
                float  bestS = -1.0f;
                for (int k = 0; k < 8; k++) {
                    const float  yaw = 45.0f * k + G_CRandom(15.0f);
                    const Vector d(cos(DEG2RAD(yaw)), sin(DEG2RAD(yaw)), 0);
                    const float  clear = WhiskerClear(Vector(0, yaw, 0), 0.0f, 400.0f);
                    const float  lead  = Q_max(0.0f, d.x * toGoal.x + d.y * toGoal.y);
                    const float  s     = clear * (0.35f + 0.65f * lead) + G_Random(0.1f);
                    if (s > bestS) {
                        bestS = s;
                        vOff  = d;
                    }
                }
                // [user 2026-09-21] COMMIT the escape. Root cause the earlier escape "did nothing": the brain
                // re-issues MoveTo(enemy)/AvoidPath every time the enemy moves and ClearMove() every time the
                // weapon wants the bot to stop-and-fire (playerbot.cpp combat think), so the escape's MoveNear
                // path was overwritten/cleared on the very next think and the bot snapped straight back to the
                // tank. Zero the commit so THIS call's own MoveNear isn't blocked by the guard, run it, then hold
                // the goal for bot_stuckCommit ms - during which MoveTo/MoveNear/AvoidPath/ClearMove no-op, so the
                // bot follows the escape path clear of the choke before the objective can pull it back.
                m_iExploreCommitTime = 0;
                SetWhy("stuckescape");
                MoveNear(controlledEntity->origin + vOff * (300.0f + G_Random(200.0f)), 160.0f);
                if (LowObstacleAhead(m_vCurrentDir)) {
                    Jump(botcmd, "escape"); // jump over a low blocker - [HZM bot A2] only when a hop actually clears it
                }
                m_iNumBlocks    = 3;
                m_iLastMoveTime = level.inttime;
                if (IsMoving()) {   // only commit if FindPathNear actually produced a route (else re-try next tick)
                    static cvar_t *s_botStuckCommit = NULL;
                    if (!s_botStuckCommit) {
                        s_botStuckCommit = gi.Cvar_Get("bot_stuckCommit", "2000", CVAR_ARCHIVE);
                    }
                    m_iExploreCommitTime = level.inttime + s_botStuckCommit->integer;
                }
            } else {
                // Give up (stock)
                ClearMove();
            }
        }

        if (!m_pPath->IsQuerying() && !controlledEntity->GetLadder()) {
            // [HZM bug-2879] a collision while SLIDING along a wall, clipping a corner or brushing a mate is not being
            // blocked. MOVERESULT_BLOCKED alone read ~half of all healthy moving samples as blocked, and every read
            // stepped the bot 128u off its route, re-pathed it and, five in a row, flung it 400-720u in a RANDOM
            // direction (probe2 on The Rail Yard: blockrepath every ~2.5s on a bot closing at 115u/s; stuckescape 5%
            // of all time) - the "back and forth", "running the opposite way" of the user's playtest.
            const float v2 = controlledEntity->velocity.lengthXYSquared();
            if ((controlledEntity->GetMoveResult() >= MOVERESULT_BLOCKED && v2 <= Square(60)) || v2 <= Square(8)) {
                blocked = true;
            } else if ((controlledEntity->origin - m_vLastCheckPos[0]).lengthSquared() <= Square(64)
                       && (controlledEntity->origin - m_vLastCheckPos[1]).lengthSquared() <= Square(64)) {
                blocked = true;
            }
        }

        if (!blocked) {
            m_iTempAwayState = 0;
            m_iNumBlocks     = 0;

            if (!m_pPath->GetNodeCount()) {
                m_vTargetPos   = controlledEntity->origin + Vector(G_CRandom(512), G_CRandom(512), G_CRandom(512));
                m_vCurrentGoal = m_vTargetPos;
            }
        } else if (m_iTempAwayState == 0) {
            m_iLastBlockTime = level.inttime;
            m_iTempAwayState = 1;
        }

        if (m_iTempAwayState && level.inttime >= m_iLastBlockTime + 1000) {
            Vector delta;
            Vector dir;

            m_iTempAwayState = 2;
            m_iTempAwayTime  = level.inttime;
            m_iNumBlocks++;

            // Try to backward a little
            if (m_pPath->GetNodeCount()) {
                delta = m_pPath->GetCurrentDelta();
            } else {
                delta = m_vTargetPos - controlledEntity->origin;
            }

            m_pPath->Clear();

            if (m_iNumBlocks < 2) {
                dir   = -delta;
                dir.z = 0;
                dir.normalize();

                if (dir.x < -0.5 || dir.x > 0.5) {
                    dir.x *= 4;
                    dir.y /= 4;
                } else if (dir.y < -0.5 || dir.y > 0.5) {
                    dir.x /= 4;
                    dir.y *= 4;
                } else {
                    dir.x = G_CRandom(2);
                    dir.y = G_CRandom(2);
                }

                m_vCurrentGoal = controlledEntity->origin + delta + dir * 128;
            } else {
                // [HZM Phase 4a] before the blind random jump (and the eventual give-up at m_iNumBlocks>=5),
                // aim for a REACHABLE point near the real goal, with the accept radius growing per block, so a
                // bot stuck pathing to an exact unreachable spot re-routes to solid ground instead of grinding
                // the wall. bot_nav_reroute 0 restores the stock random escape. Bot-only (coop instantiates no
                // BotMovement); no shared/RecastPather change.
                static cvar_t *s_botReroute = NULL;
                if (!s_botReroute) {
                    s_botReroute = gi.Cvar_Get("bot_nav_reroute", "1", CVAR_ARCHIVE);
                }
                if (s_botReroute->integer) {
                    SetWhy("blockrepath");
                    MoveNear(m_vTargetPos, 128.0f + 96.0f * m_iNumBlocks);
                } else {
                    m_vCurrentGoal =
                        controlledEntity->origin + Vector(G_CRandom(512), G_CRandom(512), G_CRandom(512));
                }
            }
        }

        // [HZM bug-2882] WEDGED: trying to move, yet not one unit of progress over the last two checks (~2s). A bot
        // jammed into a terrain seam / brush edge reports full velocity while its origin never changes (probe2 t2l1:
        // spd 148 at an identical position for a minute+), so the speed test cannot see it and the escape's re-route
        // cannot free it - a hop plus a short sidestep usually does.
        // (velocity > 60u/s: JAMMED - pushing at full speed yet going nowhere. A bot simply standing still is blocked by
        // something ordinary - a team-mate, a wall - and the stuck handling owns it; hopping there made 1982 of round 9's
        // 3075 'wedged' hops, the "random jumping" again)
        // [HZM bug-2891] 30u/s, not 60: t2l3 round 10 had a bot jammed at a steady 50u/s for five minutes that the 60 gate
        // never let hop (round 9, ungated, freed the same spot in seconds); standing-still bots read <10
        if (blocked && level.inttime >= m_iPathJumpNext && !controlledEntity->GetLadder()
            && controlledEntity->velocity.lengthXYSquared() > Square(30.0f)
            && (controlledEntity->origin - m_vLastCheckPos[1]).lengthSquared() < Square(8.0f)
            && (controlledEntity->origin - m_vLastCheckPos[0]).lengthSquared() < Square(8.0f)) {
            // [HZM bug-2885] at most TWO hops per wedge: on e3l1 the unhelped hop repeated every 1.5s (677 'wedged' hops,
            // 254 at one spot) - hopping in place is worse to watch than the wedge. Then back OUT the way we came for
            // 1s and route again with a wide approach; after that the ordinary stuck handling owns it.
            // (an episode is 15s from its START: two hops + a back-out per 15s at most, and a bot still wedged after that
            // gets another go - t2l3 round 10 had one refreshing the same episode for five minutes, never hopping again)
            if ((controlledEntity->origin - m_vWedgePos).lengthSquared() > Square(64) || level.inttime - m_iWedgeT > 15000) {
                m_vWedgePos  = controlledEntity->origin;
                m_iWedgeHops = 0;
                m_iWedgeT    = level.inttime;
            }
            if (m_iWedgeHops < 2) {
                m_iWedgeHops++;
                m_iPathJumpNext = level.inttime + 1500;
                Jump(botcmd, "wedged");
                const float yaw = controlledEntity->GetViewAngles().y + ((controlledEntity->entnum + level.inttime / 1000) & 1 ? 90.0f : -90.0f);
                m_vDirectSteerDir   = Vector(cos(DEG2RAD(yaw)), sin(DEG2RAD(yaw)), 0);
                m_iDirectSteerUntil = level.inttime + 600;
            } else if (m_iWedgeHops == 2) {
                m_iWedgeHops++;
                Vector back = Vector(0, 0, 0) - m_vCurrentDir;
                back.z      = 0;
                if (back.lengthSquared() > 0.01f) {
                    VectorNormalize2D(back);
                    m_vDirectSteerDir   = back;
                    m_iDirectSteerUntil = level.inttime + 1000;
                }
                SetWhy("wedgerepath");
                MoveNear(m_vTargetPos, 256.0f);
            }
        }

        m_vLastCheckPos[1] = m_vLastCheckPos[0];
        m_vLastCheckPos[0] = controlledEntity->origin;
    }

    if (ai_debugpath->integer) {
        int i;
        int nodecount = m_pPath->GetNodeCount();

        for (i = 0; i < nodecount - 1; i++) {
            PathNav      node1  = m_pPath->GetNode(i);
            PathNav      node2  = m_pPath->GetNode(i + 1);
            const Vector vStart = node1.origin + Vector(0, 0, 32);
            const Vector vEnd   = node2.origin + Vector(0, 0, 32);

            G_DebugLine(vStart, vEnd, 1, 0, 0, 1);
        }
    }

    if (m_pPath->GetNodeCount() || m_iTempAwayState != 0) {
        if ((m_vTargetPos - controlledEntity->origin).lengthSquared() <= Square(16)) {
            m_iExploreCommitTime = 0; // [HZM] reached the goal (incl. an escape target): release the stuck-escape commit
            if (m_pPrimaryAttract && (m_vAttractScatterGoal - controlledEntity->origin).lengthXYSquared() < Square(256)) {
                m_vObjArrivePos = controlledEntity->origin; // [HZM bug-2888]
                m_iObjArriveT   = level.inttime;
            }
            SetWhy("arrived");
            ClearMove();
        }
    } else {
        //if ((m_vTargetPos - controlledEntity->origin).lengthXYSquared() <= Square(16)) {
        m_iExploreCommitTime = 0; // [HZM] release the stuck-escape commit on this internal stop
        if (m_pPrimaryAttract && (m_vAttractScatterGoal - controlledEntity->origin).lengthXYSquared() < Square(256)) {
            m_vObjArrivePos = controlledEntity->origin; // [HZM bug-2888]
            m_iObjArriveT   = level.inttime;
        }
        SetWhy("pathend");
        ClearMove();
        // [HZM bug-2879] a route that ends the moment it is made, over and over, with the bot standing still: the bot is
        // OFF the navmesh (on a prop, a ledge lip) where the corridor cannot hold it. Stock re-pathed every frame for
        // the rest of the round (probe2 e1l1: one bot, 7350 route requests, 3+ minutes frozen). Walk straight at the
        // goal for ~1.2s (clearest bearing that still leads there) to get back onto the mesh, then route again.
        if (level.inttime - m_iPathEndWindow > 2000 || (controlledEntity->origin - m_vPathEndPos).lengthSquared() > Square(48)) {
            m_iPathEndWindow = level.inttime;
            m_iPathEndCount  = 0;
            m_vPathEndPos    = controlledEntity->origin;
        }
        // (not a bot that is AT its goal - there a route ends at once because it is already there: bug-2883)
        if ((m_vTargetPos - controlledEntity->origin).lengthXYSquared() > Square(96) && ++m_iPathEndCount >= 8
            && level.inttime >= m_iDirectSteerUntil) {
            Vector toGoal = m_vTargetPos - controlledEntity->origin;
            toGoal.z      = 0;
            if (toGoal.lengthSquared() > 1.0f) {
                VectorNormalize2D(toGoal);
            }
            float bestS = -1.0f;
            for (int k = 0; k < 8; k++) {
                const float  yaw   = 45.0f * k;
                const Vector d(cos(DEG2RAD(yaw)), sin(DEG2RAD(yaw)), 0);
                const float  clear = WhiskerClear(Vector(0, yaw, 0), 0.0f, 160.0f);
                const float  s     = clear * (0.3f + 0.7f * Q_max(0.0f, d.x * toGoal.x + d.y * toGoal.y)) + G_Random(0.15f);
                if (s > bestS) {
                    bestS             = s;
                    m_vDirectSteerDir = d;
                }
            }
            m_iDirectSteerUntil = level.inttime + 1200;
            m_iPathEndCount     = 0;
            static cvar_t *s_probeO = NULL;
            if (!s_probeO) {
                s_probeO = gi.Cvar_Get("bot_probe", "0", 0);
            }
            if (s_probeO->integer) {
                gi.Printf(
                    "^~^~^ BOTEV e=%d tm=%c ev=offmesh at=(%.0f %.0f %.0f)\n", controlledEntity->entnum,
                    controlledEntity->GetTeam() == TEAM_ALLIES ? 'a' : 'x', controlledEntity->origin.x,
                    controlledEntity->origin.y, controlledEntity->origin.z
                );
            }
        }
        //}
    }

    // Rotate the dir
    if (m_pPath->GetNodeCount()) {
        m_vCurrentDir = CalculateDir(vDelta);
    } else {
        m_vCurrentDir = CalculateDir(m_vCurrentGoal - controlledEntity->origin);
    }

    // [HZM bot A1] ANTICIPATORY avoidance: ease round a player standing / coming in our lane BEFORE bumping (the reactive
    // BotSep below only fires once jammed nose-to-nose). [bot A3] and ease off for a SHARP turn at the next corner so the
    // bot rounds it instead of overshooting and scraping the wall.
    float fPace = 1.0f;
    if (!bLadder && !bOnElevator) {
        static cvar_t *s_botAvoid  = NULL;
        static cvar_t *s_botCorner = NULL;
        if (!s_botAvoid) {
            s_botAvoid  = gi.Cvar_Get("bot_avoid", "1", 0);
            s_botCorner = gi.Cvar_Get("bot_cornerSlow", "1", 0);
        }
        if (s_botAvoid->integer) {
            float closeness = 0.0f;
            m_vCurrentDir   = AvoidPlayersAhead(m_vCurrentDir, closeness);
            if (closeness > 0.6f) {
                fPace = Q_min(fPace, 0.7f); // someone right ahead: slow while sidestepping
            }
        }
        Vector vNext;
        if (s_botCorner->integer && m_pPath->GetNodeCount() && m_pPath->GetCornerAfterNext(vNext)) {
            const Vector here = m_pPath->GetCurrentDelta(); // to the corner we are steering at
            if (here.lengthXYSquared() < Square(110)) {
                Vector turn = vNext - (controlledEntity->origin + here);
                turn.z      = 0;
                if (turn.lengthSquared() > 1.0f) {
                    VectorNormalize2D(turn);
                    const float dot = m_vCurrentDir.x * turn.x + m_vCurrentDir.y * turn.y;
                    // [bot_slopeWalk 2] ...not while CLIMBING to it (the corner 16u+ above): uphill a bot cannot
                    // overshoot, and the brake held it at 65% input for a whole hillside leg (t2l1 (-352,-3504))
                    static cvar_t *s_slopeB = NULL;
                    if (!s_slopeB) {
                        s_slopeB = gi.Cvar_Get("bot_slopeWalk", "0", 0);
                    }
                    const bool bClimb = s_slopeB->integer >= 2 && here.z > 16.0f;
                    if (dot < 0.57f && !bClimb) { // sharper than ~55 deg
                        fPace = Q_min(fPace, 0.65f);
                    }
                }
            }
        }
    }

    vWishDir = CalculateRelativeWishDirection(m_vCurrentDir);

    // Forward to the specified direction
    float x = vWishDir.x * 127 * fPace;
    float y = -vWishDir.y * 127 * fPace;
    if (level.inttime < m_iLookCompUntil) {
        // [T2 corner checks] the brain is looking at a corner / a flank, off the travel line. PM_CmdScale takes the LARGEST
        // axis as the input magnitude (x the blended strafe / backpedal limit), so this unit-length input ran at ~65% at
        // 45 deg off the view and ~78% at 60. Rescale it so the pace is the one the bot would have looking where it goes
        // (never more: a straight input is already the full 127 x fPace, and the strafe limit still applies).
        const float ax = fabs(x), ay = fabs(y), l1 = ax + ay, m = Q_max(ax, ay);
        if (l1 > 1.0f && m > 1.0f) {
            const float fDir = (ax / l1) * (x < 0.0f ? 0.72f : 1.0f) + (ay / l1) * 0.85f; // bg_pmove pm_backspeed / strafe
            const float want = Q_min(127.0f, 127.0f * fPace / Q_max(fDir, 0.5f));
            if (want > m) {
                x *= want / m;
                y *= want / m;
            }
        }
    }

    botcmd.forwardmove = (signed char)Q_clamp(x, -127, 127);
    botcmd.rightmove   = (signed char)Q_clamp(y, -127, 127);
    botcmd.upmove      = 0;

    // [HZM bot feelers] Proactive surroundings awareness (user: "always aware of surroundings", "whiskers long
    // enough they won't brush walls until they find the opening", "360 and up and down", "use their whiskers the
    // whole way towards the enemy spawn"). Every frame while the bot wants to move: feel FAR ahead along the goal
    // direction; if that is closing, feel the two forward diagonals and steer toward the clearer one BEFORE
    // touching the wall (arcing into the opening). If the whole forward arc is boxed, sweep a wide ring to find
    // ANY open heading and peel toward it. Vertical awareness (ledge up / drop) stays in CheckJump /
    // CheckJumpOverEdge below. This is the preventative layer; the reactive wall-slide after it only fires if a
    // bot is still fully pinned. bot_feelers 0 restores stock. Bot-only (coop instantiates no BotMovement).
    {
        // NOTE default 0: the always-on whisker steer over-fired on winding maps (constant re-steer => weaving,
        // slower, MORE stuck in the bot study). Kept for tuning; the targeted reactive wall-slide below is the
        // movement win. The dominant Push-bot problem is convergence (teams never reach LOS), fixed elsewhere.
        static cvar_t *s_botFeel = NULL;
        if (!s_botFeel) {
            s_botFeel = gi.Cvar_Get("bot_feelers", "0", CVAR_ARCHIVE);
        }
        bool bWants = (botcmd.forwardmove > 40 || botcmd.forwardmove < -40 || botcmd.rightmove > 40
                       || botcmd.rightmove < -40);
        // NOTE: this block always runs (when moving) so BOT-BOT SEPARATION below stays active regardless of
        // bot_feelers; only the whisker STEER is gated on bot_feelers (via c below).
        if (controlledEntity && bWants) {
            Vector gdir = m_vCurrentGoal - controlledEntity->origin;
            gdir.z = 0;
            if (gdir.lengthSquared() > 1.0f) {
                Vector ga = gdir.toAngles();

                // [HZM bot feelers] BOT-BOT SEPARATION (user: "if bots are stuck in each other the same whiskers
                // should slightly nudge them out of each other"). If a PLAYER (teammate/bot) is jammed right
                // ahead, peel sideways on a per-bot deterministic side (odd/even entnum -> opposite sides) so two
                // bots wedged into each other pick different ways and pop apart instead of both grinding the same
                // way. Takes priority over wall-steering for this frame.
                bool bSep = false;
                {
                    Vector fdir, fr, fu;
                    AngleVectors(ga, fdir, fr, fu);
                    fdir.z = 0;
                    if (fdir.lengthSquared() > 0.01f) {
                        VectorNormalize2D(fdir);
                        Vector smins = controlledEntity->mins;
                        Vector smaxs = controlledEntity->maxs;
                        smaxs.z -= STEPSIZE;
                        Vector  sbase = controlledEntity->origin + Vector(0, 0, STEPSIZE);
                        trace_t bt    = G_Trace(
                            sbase, smins, smaxs, sbase + fdir * 56.0f, controlledEntity, MASK_PLAYERSOLID, qtrue,
                            "BotSep"
                        );
                        if (bt.fraction < 1.0f && bt.ent && bt.ent->entity && bt.ent->entity != world
                            && bt.ent->entity->IsSubclassOfPlayer()) {
                            int side         = (controlledEntity->entnum & 1) ? 1 : -1;
                            m_iSepT          = level.inttime; // [T2] (view-relative: the brain keeps the eyes on the route)
                            botcmd.rightmove = (signed char)(side * 110);
                            if (botcmd.forwardmove > 60) {
                                botcmd.forwardmove = 60; // keep some push so they slide past, not just apart
                            }
                            bSep = true;
                        }
                    }
                }

                // feelers OFF (default) or separating this frame => c=1 so the whisker STEER below is skipped.
                // TIGHTER trigger (only when a wall is genuinely CLOSE, < ~0.45 of a 150u look) - the old 0.85
                // fired almost constantly on winding maps and caused weaving/slowdown.
                float c = (bSep || !s_botFeel->integer) ? 1.0f : WhiskerClear(ga, 0.0f, 150.0f);
                if (!bSep && c < 0.45f) {
                    // COMMITMENT (the anti-weave fix): only RE-decide the steer side when the previous commit has
                    // expired, then HOLD it ~450ms so the bot arcs smoothly around the obstruction instead of
                    // flip-flopping every frame (which slowed and stuck it before).
                    if (level.inttime >= m_iFeelerCommitTime) {
                        float l1 = WhiskerClear(ga, 45.0f, 130.0f);  // +yaw = left
                        float r1 = WhiskerClear(ga, -45.0f, 130.0f); // -yaw = right
                        if (l1 < 0.4f && r1 < 0.4f) {
                            // both diagonals also closing: probe wider for any opening and pick the better side
                            float lw      = WhiskerClear(ga, 80.0f, 110.0f);
                            float rw      = WhiskerClear(ga, -80.0f, 110.0f);
                            m_iFeelerSide = (rw >= lw) ? 1 : -1;
                        } else {
                            m_iFeelerSide = (r1 >= l1) ? 1 : -1; // toward the clearer diagonal (+1 = right)
                        }
                        m_iFeelerCommitTime = level.inttime + 450;
                    }
                    // apply the committed steer, blended onto the path's own rightmove
                    float newR = (float)botcmd.rightmove + (float)(m_iFeelerSide * 100);
                    Q_clamp(newR, -127.0f, 127.0f);
                    botcmd.rightmove = (signed char)newR;
                    if (c < 0.28f && botcmd.forwardmove > 85) {
                        botcmd.forwardmove = 85; // wall very close: ease throttle so the steer arcs, not rams
                    }
                }
            }
        }
    }

    // [HZM bot A4] a closed / still-opening DOOR right ahead: stop and let it open. Walking into a swinging door blocks it
    // and makes it swing back (doors.cpp), and the stuck logic then strafe-hopped at it. BotController::CheckUse presses
    // USE while we face it (BotController::BrainThink releases the trigger - Player::DoUse refuses with attack held).
    // Locked / jammed / trigger-only doors are carved out of the bot navmesh (navigation_recast_obstacle.cpp) so a route
    // never asks to walk through one. Give up after 3s so a door that will not open cannot freeze the bot.
    if (!bLadder && !bOnElevator) {
        static cvar_t *s_botDoors = NULL;
        if (!s_botDoors) {
            s_botDoors = gi.Cvar_Get("bot_doors", "1", 0);
        }
        bool bDoor     = false;
        bool bLeafSeen = false; // [bot_doorLeaf] the open-leaf contact timer runs only while a leaf is hit
        if (s_botDoors->integer && m_vCurrentDir.lengthSquared() > 0.01f) {
            Vector smins = controlledEntity->mins;
            Vector smaxs = controlledEntity->maxs;
            smaxs.z -= STEPSIZE;
            const Vector sb = controlledEntity->origin + Vector(0, 0, STEPSIZE);
            Vector       wd(m_vCurrentDir.x, m_vCurrentDir.y, 0);
            VectorNormalize2D(wd);
            trace_t dt = G_Trace(sb, smins, smaxs, sb + wd * 48.0f, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotDoor");
            if (dt.fraction < 1.0f && dt.ent && dt.ent->entity && dt.ent->entity->IsSubclassOfDoor()) {
                Door *door = static_cast<Door *>(dt.ent->entity);
                // [bot_doorLeaf] an OPEN rotating door's LEAF across the way. The mesh is world-only and the lookahead
                // ignores door leaves on purpose (bug-2879: detours round leaves the route never touched), so a route
                // that cuts past the hinge of a door standing open runs straight into the leaf - most rotating doors on
                // these maps are "wait -1": opened once, they stay open for the round. m2l1 *27, m4l2 *103 and
                // bunkerdoor1, e1l1 *303: 20-90 BOTBLOCK samples a run, one bot pinned ~2 min. Point-blank and slow:
                // go round the leaf's FREE end (the far end from the hinge - a rotating door's origin is its hinge)
                // and let the route resume from there, committed like the lookahead's detour.
                static cvar_t *s_leaf = NULL;
                if (!s_leaf) {
                    s_leaf = gi.Cvar_Get("bot_doorLeaf", "2", 0);
                }
                // a contact EPISODE first (touching it again and again, gaps under 0.75s, for 1.5s): a jammed bot
                // jitters along the leaf at 15-110 u/s rather than standing still, and most single contacts are
                // brushes that clear by themselves
                const bool bHeld = s_leaf->integer == 1 && door->isOpen() && door->isSubclassOf(RotatingDoor) && dt.fraction < 0.5f;
                bLeafSeen        = bHeld;
                if (bHeld) {
                    if (!m_iLeafTouchT || level.inttime - m_iLeafLastT > 750) {
                        m_iLeafTouchT = level.inttime;
                    }
                    m_iLeafLastT = level.inttime;
                }
                if (bHeld && level.inttime - m_iLeafTouchT >= 1500 && level.inttime >= m_iLeafRoundNext) {
                    m_iLeafRoundNext = level.inttime + 2500;
                    const Vector hinge = door->origin;
                    Vector       c     = (door->absmin + door->absmax) * 0.5f;
                    Vector       d     = c - hinge;
                    d.z                = 0;
                    const float  half  = d.length();
                    if (half > 8.0f) {
                        d *= 1.0f / half;
                        Vector wp = hinge + d * (2.0f * half + 40.0f);
                        wp.z      = controlledEntity->origin.z;
                        m_iExploreCommitTime = 0;
                        const Vector      vKeep  = m_vTargetPos;
                        const char *const szKeep = m_szGoalWhy;
                        SetWhy("leafround");
                        MoveNear(wp, 48.0f);
                        const bool ok = IsMoving();
                        if (ok) {
                            m_iExploreCommitTime = level.inttime + 1500;
                            m_iRerouteNext       = level.inttime + 3000;
                        } else {
                            SetWhy(szKeep);
                            MoveTo(vKeep);
                        }
                        static cvar_t *s_probeD = NULL;
                        if (!s_probeD) {
                            s_probeD = gi.Cvar_Get("bot_probe", "0", 0);
                        }
                        if (s_probeD->integer) {
                            gi.Printf(
                                "^~^~^ BOTLEAF e=%d at=(%.0f %.0f %.0f) door=%d model=%s hinge=(%.0f %.0f) tip=(%.0f %.0f) ok=%d\n",
                                controlledEntity->entnum, controlledEntity->origin.x, controlledEntity->origin.y,
                                controlledEntity->origin.z, door->entnum, door->model.c_str(), hinge.x, hinge.y, wp.x, wp.y,
                                ok ? 1 : 0
                            );
                        }
                        if (ok) {
                            return;
                        }
                    }
                }
                if (!door->isOpen()) {
                    bDoor = true;
                    static cvar_t *s_face = NULL;
                    if (!s_face) {
                        s_face = gi.Cvar_Get("bot_doorFace", "0", 0);
                    }
                    if (!m_iDoorWaitStart) {
                        m_iDoorWaitStart = level.inttime;
                        m_iDoorLogged    = 0;
                    } else if (s_face->integer && level.inttime - m_iDoorWaitStart >= 6000) {
                        m_iDoorWaitStart = level.inttime; // [bot_doorFace] still the same shut door: wait (and USE) again
                        m_iDoorLogged    = 0;
                    }
                    m_vDoorFace   = (door->absmin + door->absmax) * 0.5f;
                    m_vDoorFace.z = controlledEntity->origin.z + controlledEntity->viewheight;
                    {
                        // [bot_doorFace probe] at the wait's start and at its give-up: can a USE from here reach it?
                        static cvar_t *s_probeU = NULL;
                        if (!s_probeU) {
                            s_probeU = gi.Cvar_Get("bot_probe", "0", 0);
                        }
                        const int phase = (level.inttime - m_iDoorWaitStart < 3000) ? 1 : 2;
                        if (s_probeU->integer && m_iDoorLogged < phase
                            && (phase == 1 ? level.inttime - m_iDoorWaitStart >= 1000 : true)) {
                            m_iDoorLogged = phase;
                            int       touch[64];
                            const int nu  = controlledEntity->getUseableEntities(touch, 64, true);
                            int       reach = 0;
                            for (int k = 0; k < nu; k++) {
                                reach |= (touch[k] == door->entnum);
                            }
                            gi.Printf(
                                "^~^~^ BOTDOOR e=%d ev=%s at=(%.0f %.0f %.0f) door=%d model=%s st=%d lk=%d hp=%.0f sf=%d usereach=%d "
                                "yaw=%.0f\n",
                                controlledEntity->entnum, phase == 1 ? "wait" : "giveup", controlledEntity->origin.x,
                                controlledEntity->origin.y, controlledEntity->origin.z, door->entnum, door->model.c_str(),
                                door->isCompletelyClosed() ? 0 : 2, door->locked ? 1 : 0, door->health, door->spawnflags,
                                reach, controlledEntity->angles[YAW]
                            );
                        }
                    }
                    if (level.inttime - m_iDoorWaitStart < 3000) {
                        botcmd.forwardmove = 0;
                        botcmd.rightmove   = 0;
                        botcmd.upmove      = 0;
                        // [bot_doorSwing] a rotating door on the move with the bot inside its swing: step back out of the
                        // leaf's way (it opens toward whoever stands on that side unless "alwaysaway"; a leaf that meets a
                        // player is blocked and shuts again - DoorBlocked). Away from the hinge, along the floor.
                        static cvar_t *s_swing = NULL;
                        if (!s_swing) {
                            s_swing = gi.Cvar_Get("bot_doorSwing", "0", 0);
                        }
                        if (s_swing->integer && !door->isCompletelyClosed() && door->isSubclassOf(RotatingDoor)) {
                            const float leaf = Q_max(door->maxs.x - door->mins.x, door->maxs.y - door->mins.y);
                            Vector      away = controlledEntity->origin - door->origin;
                            away.z           = 0;
                            const float dh   = away.length();
                            if (dh > 1.0f && dh < leaf + 24.0f) {
                                away *= 1.0f / dh;
                                Vector f, r;
                                AngleVectors(Vector(0, controlledEntity->GetViewAngles().y, 0), f, r, NULL);
                                botcmd.forwardmove = (signed char)((away.x * f.x + away.y * f.y) * 127.0f);
                                botcmd.rightmove   = (signed char)((away.x * r.x + away.y * r.y) * 127.0f);
                            }
                        }
                        m_iDoorWaitUntil   = level.inttime + 150;
                        m_iCheckPathTime   = level.inttime;
                        m_iLastMoveTime    = level.inttime;
                        m_iStuckPushTime   = 0;
                        return;
                    }
                }
            }
        }
        if (!bDoor) {
            m_iDoorWaitStart = 0;
        }
        if (!bLeafSeen && m_iLeafTouchT && level.inttime - m_iLeafLastT > 750) {
            m_iLeafTouchT = 0; // the episode is over
        }
    }

    // [HZM bug-2871] on a ladder route: straight FORWARD only (the view is pitched/yawed by BotController)
    if (bLadder) {
        botcmd.forwardmove = 127;
        botcmd.rightmove   = 0;
        botcmd.upmove      = 0;
    }

    // [HZM bug-2871] LADDER QUEUE. One ladder, several bots: they crowded the mount spot and blocked each other's mount
    // (FuncLadder::CanUseLadder traces a player box at the foot - m3l1b ladder retest logged "ladder start position is
    // blocked by a solid object" 3340 times in 6 min, 929 stuck samples at the foot). Approaching a ladder link, WAIT a
    // body-length back - standing, not pushing - while someone is on the ladder or a closer player holds the mount spot.
    if (bNearLadder || (bLadderLink && !bOnLadder)) {
        // approaching: the next corner IS the link start (the foot); already on the link (inside the 32u trigger):
        // we stand at the foot ourselves - then only "someone is on the ladder" can make us wait
        const Vector foot = bNearLadder ? controlledEntity->origin + m_pPath->GetCurrentDelta() : controlledEntity->origin;
        const float  my2  = (controlledEntity->origin - foot).lengthXYSquared();
        bool         wait = false;
        int          why = 0, whoE = -1;
        for (int i = 0; i < game.maxclients && !wait; i++) {
            gentity_t *ge = &g_entities[i];
            if (!ge->inuse || !ge->entity || ge->entity == controlledEntity || !ge->entity->IsSubclassOfPlayer()) {
                continue;
            }
            Player *o = static_cast<Player *>(ge->entity);
            if (o->IsDead() || o->IsSpectator()) {
                continue;
            }
            const float o2 = (o->origin - foot).lengthXYSquared();
            // [ladder-queue fix] only a TEAMMATE at the SAME END (|dz| < 64) is "ahead in line": the first version
            // counted an axis defender camping the ladder TOP as closer to the allies' FOOT (2D, no team check), and
            // the whole allied team stood at the bottom for the rest of the round (BOTLQ: all waits 'closer', 0 mounts)
            const bool mate = o->GetTeam() == controlledEntity->GetTeam() && fabs(o->origin.z - foot.z) < 64.0f;
            if (o->GetLadder() && o2 < Square(96)) {
                wait = true; // the ladder is in use
                why  = 1;
                whoE = o->entnum;
            } else if (mate && o2 < Square(40) && o2 < my2) {
                wait = true; // someone closer is on the mount spot
                why  = 2;
                whoE = o->entnum;
            } else if (mate && my2 < 1.0f && o2 < Square(48) && o->entnum < controlledEntity->entnum) {
                // [HZM bug-2897] BOTH already at the foot (inside the link trigger, where "foot" is our own origin, so
                // nobody is ever "closer"): each stands in the other's mount box and FuncLadder::CanUseLadder refuses
                // both - m3l1b round 11: two bots side by side at the bunker ladder pushing for minutes, 3459 "ladder
                // start position is blocked" in 6 min. Lower entnum climbs first; the other backs off and queues.
                wait = true;
                why  = 2;
                whoE = o->entnum;
            }
        }
        // and never wait for good: after 8s in line (a climb takes ~4-5s), go (whatever holds the spot - a stuck climber, a fight - it
        // is not coming free on its own)
        if (wait && my2 < Square(120)) {
            if (!m_iLadderWaitStart) {
                m_iLadderWaitStart = level.inttime;
            } else if (level.inttime - m_iLadderWaitStart > 8000) {
                wait = false;
            }
        }
        if (wait && my2 < Square(120)) {
            static cvar_t *s_probeQ = NULL;
            if (!s_probeQ) {
                s_probeQ = gi.Cvar_Get("bot_probe", "0", 0);
            }
            if (s_probeQ->integer && level.inttime - m_iLadderQLog > 2000) {
                m_iLadderQLog = level.inttime;
                gi.Printf(
                    "^~^~^ BOTLQ e=%d wait=%s other=%d my=%.0f link=%d near=%d\n", controlledEntity->entnum,
                    why == 1 ? "onladder" : "closer", whoE, sqrt(my2), bLadderLink ? 1 : 0, bNearLadder ? 1 : 0
                );
            }
            // stand ASIDE of the mount spot: FuncLadder::CanUseLadder box-traces the climber's start position, so a bot
            // waiting on it blocks the mount (queue v2 still logged 'ladder start position is blocked' x2006) - back
            // off to ~64u (the view faces the foot, so backward is away from it)
            botcmd.forwardmove = (my2 < Square(64)) ? -100 : 0;
            botcmd.rightmove   = 0;
            botcmd.upmove      = 0;
            m_iCheckPathTime   = level.inttime; // waiting in line is not "blocked"
            m_iLastMoveTime    = level.inttime;
            m_iStuckPushTime   = 0;
            return;
        }
    }

    // [HZM bot wall-slide] Reactive unstick. The stock block-recovery only re-checks every ~1s and takes another
    // ~1s to react, so a bot pushing into a wall / thin opening grinds in place for up to ~2s each time - the
    // dominant "running-in-place" seen in the Push bot study. Here, the MOMENT the bot is pushing hard (high
    // forwardmove) but its horizontal speed is near zero, we inject a strafe so it slides ALONG the obstruction
    // to find the gap, alternating side every ~600ms so it probes both ways. This composes with (does not
    // replace) the slower reroute/give-up logic above. bot_wallslide 0 restores stock. Bot-only (coop
    // instantiates no BotMovement); no shared RecastPather change.
    {
        static cvar_t *s_botSlide = NULL;
        if (!s_botSlide) {
            s_botSlide = gi.Cvar_Get("bot_wallslide", "1", CVAR_ARCHIVE);
        }
        bool bWantsMove = (botcmd.forwardmove > 60 || botcmd.forwardmove < -60 || botcmd.rightmove > 60
                           || botcmd.rightmove < -60);
        if (s_botSlide->integer && controlledEntity && bWantsMove && !bOnElevator && !bLadder && !bNearLadder) {
            float velH2 = controlledEntity->velocity.x * controlledEntity->velocity.x
                        + controlledEntity->velocity.y * controlledEntity->velocity.y;
            // [user 2026-09-21] Only treat "near-stationary" as STUCK when something is actually BLOCKING the way
            // the bot is trying to go. A WOUNDED bot momentarily slowed/staggering in the open was tripping this
            // and strafe-JUMPING in place "on its own blood trail" (the blood FX is non-solid SOLID_NOT - it was
            // never a collision, it was this false trigger). A short forward box-probe along the movement goal
            // gates it: clear ahead => not wall-stuck, so reset and let the stagger resolve instead of jittering.
            bool bBlockedAhead = false;
            {
                Vector wishd = m_vCurrentGoal - controlledEntity->origin;
                wishd.z = 0;
                if (wishd.lengthSquared() > Square(8)) {
                    VectorNormalize2D(wishd);
                    Vector smins = controlledEntity->mins;
                    Vector smaxs = controlledEntity->maxs;
                    smaxs.z -= STEPSIZE;
                    Vector  sb = controlledEntity->origin + Vector(0, 0, STEPSIZE);
                    trace_t ft = G_Trace(
                        sb, smins, smaxs, sb + wishd * 30.0f, controlledEntity, MASK_PLAYERSOLID, qtrue,
                        "BotStuckProbe"
                    );
                    bBlockedAhead = (ft.fraction < 1.0f) && !BotProbeHitWalkable(ft); // [bot_slopeWalk]
                }
            }
            if (velH2 < Square(20) && bBlockedAhead) {
                if (!m_iStuckPushTime) {
                    m_iStuckPushTime = level.inttime;
                }
                int held = level.inttime - m_iStuckPushTime;
                // grinding for >150ms: run a 3-phase escape, ~450ms each, cycling until we break free -
                //   phase 0: strafe right along the wall   phase 1: strafe left   phase 2: back off the wall
                // The back-off peels a bot out of a dead-end POCKET where both strafes are also blocked (the axis
                // stuck-mode the single-side slide could not fix).
                if (held >= 150) {
                    int phase = (held / 450) % 3;
                    if (phase == 0) {
                        botcmd.rightmove = 127;
                    } else if (phase == 1) {
                        botcmd.rightmove = -127;
                    } else {
                        botcmd.forwardmove = (signed char)(-botcmd.forwardmove * 0.8f); // reverse off the obstruction
                    }
                    // while strafing, ease forward so the lateral motion translates into sideways travel, AND
                    // JUMP: a stuck bot pushing into a low LEDGE/step - too tall for the 18u auto-step and with no
                    // jump link in the world-only navmesh (e.g. the m3l3 log the axis pile against, where real
                    // players must hop up) - clears it with forward momentum + a hop. Harmless bunny-hop if it is
                    // actually a flat wall. CheckJump below only fires along the PATH direction, which is why a
                    // path-less ledge never triggered it.
                    // [HZM bot A2] ...but only HOP when a hop clears it (LowObstacleAhead): jumping at a plain wall was
                    // most of the remaining "jump around" jank (heat soaks: jump-while-still 4-15% on several maps)
                    static cvar_t *s_botJumpSmart = NULL;
                    if (!s_botJumpSmart) {
                        s_botJumpSmart = gi.Cvar_Get("bot_jumpSmart", "1", 0);
                    }
                    Vector wishd2 = m_vCurrentGoal - controlledEntity->origin;
                    wishd2.z      = 0;
                    if (wishd2.lengthSquared() > 1.0f) {
                        VectorNormalize2D(wishd2);
                    }
                    if (phase < 2 && (!s_botJumpSmart->integer || LowObstacleAhead(wishd2))) {
                        Jump(botcmd, "wallslide");
                        if (botcmd.forwardmove > 80) {
                            botcmd.forwardmove = 80;
                        } else if (botcmd.forwardmove < -80) {
                            botcmd.forwardmove = -80;
                        }
                    }
                }
            } else {
                m_iStuckPushTime = 0;
            }
        } else {
            m_iStuckPushTime = 0;
        }
    }

    // [HZM bug-2958 probe] LOOP detector (bot_blocklog): >400u walked in the last 6s but <80u net - a bot running in a
    // small circle is never "blocked" (m4l2: an axis bot circled its spawn room for minutes at 150-215u/s). Log what it
    // steers at, every 3s while it lasts.
    {
        static cvar_t *s_blockLogL = NULL;
        if (!s_blockLogL) {
            s_blockLogL = gi.Cvar_Get("bot_blocklog", "0", 0);
        }
        if (s_blockLogL->integer && controlledEntity && level.inttime - m_iLoopT >= 500) {
            m_iLoopT              = level.inttime;
            m_vLoopPos[m_iLoopIdx % 12] = controlledEntity->origin;
            m_iLoopIdx++;
            if (m_iLoopIdx >= 12) {
                float walked = 0.0f;
                for (int li = 1; li < 12; li++) {
                    walked += (m_vLoopPos[(m_iLoopIdx - li) % 12] - m_vLoopPos[(m_iLoopIdx - li - 1) % 12]).lengthXY();
                }
                const float net = (m_vLoopPos[(m_iLoopIdx - 1) % 12] - m_vLoopPos[m_iLoopIdx % 12]).lengthXY();
                if (walked > 400.0f && net < 80.0f && level.inttime - m_iLoopLogT > 3000) {
                    m_iLoopLogT = level.inttime;
                    Vector    cc[3];
                    const int nc = m_pPath ? m_pPath->GetCorners(cc, 3) : 0;
                    gi.Printf(
                        "^~^~^ BOTLOOP e=%d at=(%.0f %.0f %.0f) walked=%.0f net=%.0f nodes=%d corner0=(%.0f %.0f %.0f) "
                        "corner1=(%.0f %.0f %.0f) dest=(%.0f %.0f %.0f) target=(%.0f %.0f %.0f) trav=%d appr=%d sm=%d "
                        "direct=%d commit=%d hold=%d why=%s\n",
                        controlledEntity->entnum, controlledEntity->origin.x, controlledEntity->origin.y,
                        controlledEntity->origin.z, walked, net, m_pPath ? m_pPath->GetNodeCount() : -1,
                        nc > 0 ? cc[0].x : 0.0f, nc > 0 ? cc[0].y : 0.0f, nc > 0 ? cc[0].z : 0.0f, nc > 1 ? cc[1].x : 0.0f,
                        nc > 1 ? cc[1].y : 0.0f, nc > 1 ? cc[1].z : 0.0f, GetPathDestination().x, GetPathDestination().y,
                        GetPathDestination().z, m_vTargetPos.x, m_vTargetPos.y, m_vTargetPos.z,
                        m_pPath ? (int)m_pPath->GetTraversingArea() : -1, m_pPath ? (int)m_pPath->GetApproachingArea() : -1,
                        m_pPath ? m_pPath->GetSteerMode() : -1, level.inttime < m_iDirectSteerUntil ? 1 : 0,
                        level.inttime < m_iExploreCommitTime ? 1 : 0, level.inttime < m_iHoldUntil ? 1 : 0,
                        m_szGoalWhy ? m_szGoalWhy : "none"
                    );
                }
            }
        }
    }

    // [HZM bug-2964 probe] BOTLINK (bot_blocklog): every off-mesh link crossing, with how it ended - ok=1 when the bot
    // stopped traversing within 48u (xy) / 40u (z) of the link's far end. The traversal-level evidence for which links
    // bots really walk (the straight-link walk check must not drop those).
    {
        static cvar_t *s_blockLogK = NULL;
        if (!s_blockLogK) {
            s_blockLogK = gi.Cvar_Get("bot_blocklog", "0", 0);
        }
        if (s_blockLogK->integer && controlledEntity && m_pPath) {
            const Vector o = controlledEntity->origin;
            if (m_iLinkLogT0 && (o - m_vLinkLogLast).lengthSquared() > Square(200.0f)) {
                m_iLinkLogT0 = 0; // teleported (respawn): not a traversal result
            }
            m_vLinkLogLast = o;
            Vector     la, lb;
            const int  ta = m_pPath->GetTraversingArea();
            const bool tr = ta && m_pPath->GetTraversingLink(la, lb);
            const bool nw = tr && (!m_iLinkLogT0 || (la - m_vLinkLogA).lengthSquared() > 1.0f || (lb - m_vLinkLogB).lengthSquared() > 1.0f);
            if (m_iLinkLogT0 && (!tr || nw)) {
                const Vector de = m_vLinkLogB - o;
                const int    ok = (de.lengthXY() < 48.0f && fabs(de.z) < 40.0f) ? 1 : 0;
                gi.Printf(
                    "^~^~^ BOTLINK e=%d area=%d from=(%.0f %.0f %.0f) to=(%.0f %.0f %.0f) ms=%d ok=%d dend=%.0f blk=%d start=(%.0f %.0f %.0f) why=%s\n",
                    controlledEntity->entnum, m_iLinkLogArea, m_vLinkLogA.x, m_vLinkLogA.y, m_vLinkLogA.z, m_vLinkLogB.x,
                    m_vLinkLogB.y, m_vLinkLogB.z, level.inttime - m_iLinkLogT0, ok, de.length(), m_iNumBlocks - m_iLinkLogBlk,
                    m_vLinkLogO.x, m_vLinkLogO.y, m_vLinkLogO.z, m_szGoalWhy ? m_szGoalWhy : "none"
                );
                m_iLinkLogT0 = 0;
            }
            if (nw) {
                m_vLinkLogA    = la;
                m_vLinkLogB    = lb;
                m_vLinkLogO    = o;
                m_iLinkLogT0   = level.inttime;
                m_iLinkLogArea = ta;
                m_iLinkLogBlk  = m_iNumBlocks;
            }
        }
    }

    // [HZM bug-2957] a JUMP link whose TAKE-OFF the body cannot reach. bug-2913 learns a jump link a bot hops at three
    // times without getting across (upward only, from the take-off); e1l1 had the other failure: the take-off itself sat
    // behind terrain the cylinder cannot cross from the side the route arrives on - bots pushed 25u short of it for
    // minutes (BOTBLOCK2 'appr=34', 'ahead World at 1'), never hopping, so nothing was learned. Approaching a JUMP link,
    // within 96u of its take-off, wanting to move, and not having got 8u closer to it for 4s: cost it last resort (the
    // same rule as bug-2913 - the only way stays a way) and route again. bot_linkReachLearn 1 = on. DEFAULT OFF
    // (2026-09-25): on e1l1 it fired (6 links) but a learned link only drops to JUMP_HARD (cost 60), routes kept using
    // the web of links onto that strip and the spot's stuck rate did not move (35% vs 32% of samples there).
    {
        static cvar_t *s_linkReach = NULL;
        if (!s_linkReach) {
            s_linkReach = gi.Cvar_Get("bot_linkReachLearn", "0", 0);
        }
        Vector     la, lb;
        const bool bWants = botcmd.forwardmove > 40 || botcmd.forwardmove < -40 || botcmd.rightmove > 40
                         || botcmd.rightmove < -40;
        if (s_linkReach->integer && controlledEntity && m_pPath && !bLadder && !bOnElevator
            && m_pPath->GetApproachingArea() == RECAST_AREA_JUMP && m_pPath->GetApproachingLink(la, lb)) {
            const float d = (la - controlledEntity->origin).lengthXY();
            // (inside 16u the pather is already over the link - the triggerRadius - and bug-2913 takes it from there;
            // e1l1's bots got no nearer than 24-34u, so the old 24u reset kept re-arming the clock and it never fired)
            if (d > 96.0f || d < 16.0f || (la - m_vLinkReachA).lengthSquared() > Square(16.0f)) {
                m_vLinkReachA    = la;
                m_fLinkReachBest = d;
                m_iLinkReachT    = level.inttime;
            } else if (d < m_fLinkReachBest - 8.0f || !bWants) {
                m_fLinkReachBest = Q_min(m_fLinkReachBest, d);
                m_iLinkReachT    = level.inttime;
            } else if (level.inttime - m_iLinkReachT > 4000) {
                const int n = NavLearn_DisableJumpLink(la, lb);
                gi.Printf(
                    "^~^~^ BOTLEARN e=%d at=(%.0f %.0f %.0f) takeoff to=(%.0f %.0f %.0f) disabled=%d d=%.0f why=%s\n",
                    controlledEntity->entnum, la.x, la.y, la.z, lb.x, lb.y, lb.z, n, d, m_szGoalWhy ? m_szGoalWhy : "none"
                );
                m_iLinkReachT = level.inttime;
                if (n > 0) {
                    SetWhy("learnrepath");
                    MoveNear(m_vTargetPos, 96.0f);
                }
            }
        } else {
            m_iLinkReachT = level.inttime;
        }
    }

    // [HZM bug-2899] NO PROGRESS, learned. A bot calmly navigating (no enemy, no shooting, no cover fight - the brain says
    // so each frame), headed somewhere >160u away, that is still within 96u of where it was 8s ago, with nobody within
    // 64u to be queueing behind, is circling ground the mesh calls walkable and a player cannot cross: t2l1 round 12, an
    // axis bot 2+ minutes on a rocky hillside (z 1565-1613, hopping, never frozen, so bug-2898's learner never fired).
    // Re-cost the poly it keeps failing to enter (toward its next corner) to last-resort, as the frozen case does, and
    // route again. Everyone on the map avoids it from then on; where it is the only way it is still taken.
    if (controlledEntity && m_bCalm && IsMoving() && !controlledEntity->GetLadder() && level.inttime >= m_iHoldUntil
        && m_pPath->GetTraversingArea() != RECAST_AREA_ELEVATOR && m_pPath->GetApproachingArea() != RECAST_AREA_ELEVATOR
        && (m_vTargetPos - controlledEntity->origin).lengthXYSquared() > Square(160.0f)) {
        if (!m_iProgT || (controlledEntity->origin - m_vProgPos).lengthXYSquared() > Square(96.0f)) {
            m_vProgPos = controlledEntity->origin;
            m_iProgT   = level.inttime;
        } else if (level.inttime - m_iProgT > 8000) {
            bool crowd = false;
            for (int i = 0; i < game.maxclients && !crowd; i++) {
                gentity_t *ge = &g_entities[i];
                if (ge->inuse && ge->entity && ge->entity != controlledEntity && ge->entity->IsSubclassOfPlayer()
                    && !static_cast<Player *>(ge->entity)->IsDead()
                    && (ge->entity->origin - controlledEntity->origin).lengthSquared() < Square(64.0f)) {
                    crowd = true;
                }
            }
            Vector dir = m_vCurrentGoal - controlledEntity->origin;
            dir.z      = 0;
            if (!crowd && dir.lengthSquared() > 1.0f) {
                VectorNormalize2D(dir);
                const int lr = NavLearn_MarkSteep(controlledEntity->origin, dir);
                gi.Printf(
                    "^~^~^ BOTLEARN e=%d at=(%.0f %.0f %.0f) progress marked=%d why=%s\n", controlledEntity->entnum,
                    controlledEntity->origin.x, controlledEntity->origin.y, controlledEntity->origin.z, lr,
                    m_szGoalWhy ? m_szGoalWhy : "none"
                );
                if (lr == 1) {
                    SetWhy("learnrepath");
                    MoveNear(m_vTargetPos, 96.0f);
                }
            }
            m_vProgPos = controlledEntity->origin;
            m_iProgT   = level.inttime;
        }
    } else {
        m_iProgT = 0;
    }

    // [HZM bug-2891] EMBEDDED. A bot that wants to move and whose origin has not changed by half a unit for 1.5s: test
    // the box where it stands. Inside solid (t2l3 round 10: two bots frozen for five minutes, 2u below the ground the
    // others walked over there, the forward probe clear) no route, hop or escape can ever free it - pmove's own
    // PM_CorrectAllSolid only jitters 1u. Push it out to the nearest free spot within a step (18u), the same
    // correction with a reach that covers the sink. Not in solid: log what the ground is, once per 5s, so the next
    // frozen case names itself.
    if (controlledEntity && !controlledEntity->GetLadder() && level.inttime - m_iSolidT >= 500) {
        const bool still = (controlledEntity->origin - m_vSolidPos).lengthSquared() < Square(0.5f);
        m_iSolidT        = level.inttime;
        m_vSolidPos      = controlledEntity->origin;
        m_iSolidN        = (still && IsMoving()) ? m_iSolidN + 1 : 0;
        if (m_iSolidN >= 3) {
            const Vector o  = controlledEntity->origin;
            trace_t      st = G_Trace(
                o, controlledEntity->mins, controlledEntity->maxs, o, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotSolid"
            );
            // ...and a short SWEEP the way it wants to go: round 11 t2l3 froze a bot at the round-10 spot with the box
            // test clear (on world ground, walking, normal 0.88, 49u/s) - pmove's slide move keeps the velocity and
            // stays put when its sweep comes back allsolid, which a start==end box test need not show (terrain seams)
            Vector wish = m_vCurrentGoal - o;
            wish.z      = 0;
            if (wish.lengthSquared() < 1.0f) {
                wish   = controlledEntity->velocity;
                wish.z = 0;
            }
            const bool bWish = wish.lengthSquared() >= 1.0f;
            if (bWish) {
                VectorNormalize2D(wish);
            }
            trace_t sw = st;
            if (bWish) {
                sw = G_Trace(
                    o, controlledEntity->mins, controlledEntity->maxs, o + wish * 8.0f, controlledEntity, MASK_PLAYERSOLID,
                    qtrue, "BotSolidSweep"
                );
            }
            const bool bBox   = st.startsolid || st.allsolid;
            const bool bSweep = bWish && (sw.startsolid || sw.allsolid);
            if (bBox || bSweep) {
                static const float dzs[]    = {1, 2, 3, 4, 6, 9, 12, 18};
                static const float dxys[][2] = {
                    {0, 0},  {4, 0},  {-4, 0},  {0, 4},  {0, -4},  {6, 6},  {-6, 6},  {6, -6},  {-6, -6},
                    {10, 0}, {-10, 0}, {0, 10}, {0, -10}, {16, 0}, {-16, 0}, {0, 16}, {0, -16}
                };
                bool   fixed = false;
                Vector to    = o;
                for (int iz = 0; iz < 8 && !fixed; iz++) {
                    for (int ixy = 0; ixy < 17 && !fixed; ixy++) {
                        const Vector p  = o + Vector(dxys[ixy][0], dxys[ixy][1], dzs[iz]);
                        trace_t      ft = G_Trace(
                            p, controlledEntity->mins, controlledEntity->maxs, p, controlledEntity, MASK_PLAYERSOLID, qtrue,
                            "BotSolidFree"
                        );
                        if (ft.startsolid || ft.allsolid) {
                            continue;
                        }
                        if (bWish) {
                            trace_t fs = G_Trace(
                                p, controlledEntity->mins, controlledEntity->maxs, p + wish * 8.0f, controlledEntity,
                                MASK_PLAYERSOLID, qtrue, "BotSolidFreeSweep"
                            );
                            if (fs.startsolid || fs.allsolid) {
                                continue;
                            }
                        }
                        // settle it back onto the floor (never lift it onto a ledge it could not stand on)
                        trace_t gt = G_Trace(
                            p, controlledEntity->mins, controlledEntity->maxs, p - Vector(0, 0, 40), controlledEntity,
                            MASK_PLAYERSOLID, qtrue, "BotSolidFloor"
                        );
                        if (gt.startsolid || gt.fraction >= 1.0f || gt.plane.normal[2] < 0.7f) {
                            continue;
                        }
                        if (bSweep && !bBox && bWish) { // settled back into the seam?
                            trace_t fs = G_Trace(
                                gt.endpos, controlledEntity->mins, controlledEntity->maxs, Vector(gt.endpos) + wish * 8.0f,
                                controlledEntity, MASK_PLAYERSOLID, qtrue, "BotSolidFreeSweep2"
                            );
                            if (fs.startsolid || fs.allsolid) {
                                continue;
                            }
                        }
                        to    = gt.endpos;
                        fixed = true;
                    }
                }
                const trace_t& ht = bBox ? st : sw;
                Entity        *se = (ht.ent && ht.ent->entity) ? ht.ent->entity : NULL;
                if (!fixed && se && se != world && !se->IsSubclassOfPlayer()) {
                    // [HZM bug-2899] inside a whole ENTITY (e3l2 round 12: a scripted truck's collision box drove onto a
                    // bot, which then sat inside it for five minutes - 195 BOTSOLID fixed=0, nothing free within 18u):
                    // step out just past the side of its box nearest to us, then the far sides
                    const Vector em = se->absmin, eM = se->absmax;
                    Vector       cand[4] = {
                        Vector(em.x - controlledEntity->maxs.x - 2, o.y, o.z), Vector(eM.x - controlledEntity->mins.x + 2, o.y, o.z),
                        Vector(o.x, em.y - controlledEntity->maxs.y - 2, o.z), Vector(o.x, eM.y - controlledEntity->mins.y + 2, o.z)
                    };
                    for (int a = 0; a < 4; a++) { // nearest first
                        for (int b = a + 1; b < 4; b++) {
                            if ((cand[b] - o).lengthSquared() < (cand[a] - o).lengthSquared()) {
                                const Vector tmp = cand[a];
                                cand[a]          = cand[b];
                                cand[b]          = tmp;
                            }
                        }
                    }
                    for (int a = 0; a < 4 && !fixed; a++) {
                        if ((cand[a] - o).lengthXYSquared() > Square(256.0f)) {
                            continue;
                        }
                        for (int iz = 0; iz < 3 && !fixed; iz++) {
                            const Vector p  = cand[a] + Vector(0, 0, iz * 9.0f);
                            trace_t      ft = G_Trace(
                                p, controlledEntity->mins, controlledEntity->maxs, p, controlledEntity, MASK_PLAYERSOLID,
                                qtrue, "BotSolidOut"
                            );
                            if (ft.startsolid || ft.allsolid) {
                                continue;
                            }
                            trace_t gt = G_Trace(
                                p, controlledEntity->mins, controlledEntity->maxs, p - Vector(0, 0, 48), controlledEntity,
                                MASK_PLAYERSOLID, qtrue, "BotSolidOutFloor"
                            );
                            if (gt.startsolid || gt.fraction >= 1.0f || gt.plane.normal[2] < 0.7f) {
                                continue;
                            }
                            to    = gt.endpos;
                            fixed = true;
                        }
                    }
                }
                gi.Printf(
                    "^~^~^ BOTSOLID e=%d at=(%.0f %.0f %.1f) kind=%s in=%s model=%s tn=%s fixed=%d to=(%.0f %.0f %.1f)\n",
                    controlledEntity->entnum, o.x, o.y, o.z, bBox ? "box" : "sweep", se ? se->getClassname() : "?",
                    se ? se->model.c_str() : "", se ? se->targetname.c_str() : "", fixed ? 1 : 0, to.x, to.y, to.z
                );
                if (fixed) {
                    controlledEntity->setOrigin(to);
                    controlledEntity->velocity = vec_zero;
                    m_vSolidPos                = to;
                }
                m_iSolidN = 0;
            } else if (level.inttime >= m_iSolidLogT) {
                m_iSolidLogT = level.inttime + 5000;
                // [HZM bug-2898] frozen at speed against WORLD ground too steep to walk (the sweep's plane): learn it
                if (bWish && sw.fraction < 0.5f && sw.ent && sw.ent->entity == world && sw.plane.normal[2] > 0.05f
                    && sw.plane.normal[2] < 0.7f && controlledEntity->velocity.lengthXYSquared() > Square(30.0f)) {
                    const int lr = NavLearn_MarkSteep(controlledEntity->origin, wish);
                    gi.Printf(
                        "^~^~^ BOTLEARN e=%d at=(%.0f %.0f %.0f) steep n=%.2f marked=%d why=%s\n", controlledEntity->entnum,
                        o.x, o.y, o.z, sw.plane.normal[2], lr, m_szGoalWhy ? m_szGoalWhy : "none"
                    );
                    if (lr == 1) {
                        SetWhy("learnrepath");
                        MoveNear(m_vTargetPos, 96.0f);
                    }
                }
                gclient_t *cl = controlledEntity->client;
                Entity    *we = (sw.ent && sw.ent->entity) ? sw.ent->entity : NULL;
                gi.Printf(
                    "^~^~^ BOTFROZEN e=%d at=(%.0f %.0f %.1f) spd=%.0f ge=%d walk=%d gplane=%d gn=%.2f pmf=%d swf=%.2f "
                    "swn=%.2f swc=%s why=%s\n",
                    controlledEntity->entnum, o.x, o.y, o.z, controlledEntity->velocity.lengthXY(),
                    cl ? cl->ps.groundEntityNum : -1, cl ? (int)cl->ps.walking : -1, cl ? (int)cl->ps.groundPlane : -1,
                    cl ? cl->ps.groundTrace.plane.normal[2] : -1.0f, cl ? cl->ps.pm_flags : -1, bWish ? sw.fraction : -1.0f,
                    bWish ? sw.plane.normal[2] : -2.0f, we ? we->getClassname() : "-", m_szGoalWhy ? m_szGoalWhy : "none"
                );
            }
        }
    }

    // [bug-2966 probe] SLIDING: the bot wants to move and pmove's ground under it is too steep to walk (a ground plane
    // with a normal under 0.7 - pmove slides it back down). Where bots do this on routes the navmesh calls walkable, a
    // patch bank or rock face is steeper in collision than in the mesh (t2l1 (1150,-2070): patch 837 rises 55u over ~64u
    // between two flat areas, axis bots stuck there in every soak, mostly in fights, so the calm-only learner never ran).
    // bot_blocklog 1: one line per bot per 2s while it lasts, with the ground normal and how far it has got.
    {
        static cvar_t *s_slideLog = NULL;
        if (!s_slideLog) {
            s_slideLog = gi.Cvar_Get("bot_blocklog", "0", 0);
        }
        gclient_t *cl     = controlledEntity ? controlledEntity->client : NULL;
        const bool bWants = botcmd.forwardmove > 40 || botcmd.forwardmove < -40 || botcmd.rightmove > 40 || botcmd.rightmove < -40;
        const bool bSteep = cl && cl->ps.groundPlane && cl->ps.groundTrace.plane.normal[2] < 0.7f;
        if (bSteep && bWants && IsMoving() && !controlledEntity->GetLadder()) {
            if (!m_iSlideT) {
                m_iSlideT   = level.inttime;
                m_vSlidePos = controlledEntity->origin;
            }
            if (s_slideLog->integer && level.inttime >= m_iSlideLogT) {
                m_iSlideLogT = level.inttime + 2000;
                gi.Printf(
                    "^~^~^ BOTSLIDE e=%d at=(%.0f %.0f %.0f) gn=%.2f spd=%.0f vz=%.0f for=%d net=%.0f fm=%d rm=%d why=%s\n",
                    controlledEntity->entnum, controlledEntity->origin.x, controlledEntity->origin.y, controlledEntity->origin.z,
                    cl->ps.groundTrace.plane.normal[2], controlledEntity->velocity.lengthXY(), controlledEntity->velocity.z,
                    level.inttime - m_iSlideT, (controlledEntity->origin - m_vSlidePos).lengthXY(), (int)botcmd.forwardmove,
                    (int)botcmd.rightmove, m_szGoalWhy ? m_szGoalWhy : "none"
                );
            }
        } else if (!bSteep) {
            m_iSlideT = 0;
        }
        // [bug-2966] SLIDE LEARNER (bot_slideLearn 1): wanting to move and on ground too steep to walk in 5 of the last 8
        // half-second samples, and under 64u from where it was 4s ago - in a fight or not (sliding back down a bank is
        // not a combat artefact, unlike the milling the calm-only learner guards against): the poly it keeps trying to
        // enter becomes last-resort ground (NavLearn_MarkSteep, x25, never removed) and it routes again. Holds, ladders
        // and lift links are left alone.
        static cvar_t *s_slideLearn = NULL;
        if (!s_slideLearn) {
            s_slideLearn = gi.Cvar_Get("bot_slideLearn", "0", 0);
        }
        if (s_slideLearn->integer && cl && IsMoving() && !controlledEntity->GetLadder() && level.inttime >= m_iHoldUntil
            && m_pPath->GetTraversingArea() != RECAST_AREA_ELEVATOR && m_pPath->GetTraversingArea() != RECAST_AREA_LADDER) {
            if (level.inttime - m_iSlideSampT >= 500 || level.inttime < m_iSlideSampT) {
                m_iSlideSampT = level.inttime;
                // [bug-2966 v2] or PINNED against a steep world face: standing on walkable ground, all but stopped,
                // and 16u toward the steering goal is a WORLD surface too steep to walk. t2l1 (1150,-2070): a patch
                // crease that is a ramp in the mesh and a wall in collision (BOTFROZEN gn 0.99 swf 0.00 swn 0.21 spd 0-6)
                // - the slide test above never saw it, and bug-2898's frozen learner wants 30u/s of speed.
                bool bFace = false;
                if (!bSteep && bWants && controlledEntity->velocity.lengthXYSquared() < Square(30.0f)) {
                    Vector fw = m_vCurrentGoal - controlledEntity->origin;
                    fw.z      = 0;
                    if (fw.lengthSquared() > 1.0f) {
                        VectorNormalize2D(fw);
                        const trace_t ft = G_Trace(
                            controlledEntity->origin, controlledEntity->mins, controlledEntity->maxs,
                            controlledEntity->origin + fw * 16.0f, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotSlideFace"
                        );
                        bFace = !ft.startsolid && ft.fraction < 1.0f && ft.ent && ft.ent->entity == world
                             && ft.plane.normal[2] > 0.05f && ft.plane.normal[2] < 0.7f;
                    }
                }
                m_iSlideRing = ((m_iSlideRing << 1) | (((bSteep || bFace) && bWants) ? 1 : 0)) & 0xff;
                m_vSlideRingPos[m_iSlideIdx & 7] = controlledEntity->origin;
                m_iSlideIdx++;
                int nSteep = 0;
                for (int b = 0; b < 8; b++) {
                    nSteep += (m_iSlideRing >> b) & 1;
                }
                if (m_iSlideIdx >= 8 && nSteep >= 5
                    && (controlledEntity->origin - m_vSlideRingPos[m_iSlideIdx & 7]).lengthXYSquared() < Square(64.0f)) {
                    Vector wish = m_vCurrentGoal - controlledEntity->origin;
                    wish.z      = 0;
                    if (wish.lengthSquared() > 1.0f) {
                        VectorNormalize2D(wish);
                        const int lr = NavLearn_MarkSteep(controlledEntity->origin, wish);
                        gi.Printf(
                            "^~^~^ BOTLEARN e=%d at=(%.0f %.0f %.0f) slide n=%d marked=%d why=%s\n", controlledEntity->entnum,
                            controlledEntity->origin.x, controlledEntity->origin.y, controlledEntity->origin.z, nSteep, lr,
                            m_szGoalWhy ? m_szGoalWhy : "none"
                        );
                        // [bug-2966 v3] standing ON the slope (not at a face): step DOWNHILL first - the ground normal's
                        // flat part points down the slope. A route from the steep poly itself went straight back up it
                        // (e1l1 (-1987,-1263): marked, then 25s more on the same slope). Held briefly so the next think
                        // does not re-route it uphill; then the normal route resumes with the slope re-costed.
                        Vector down(cl->ps.groundTrace.plane.normal[0], cl->ps.groundTrace.plane.normal[1], 0);
                        if (bSteep && down.lengthSquared() > 0.01f) {
                            VectorNormalize2D(down);
                            m_iExploreCommitTime = 0;
                            SetWhy("slidedown");
                            MoveNear(controlledEntity->origin + down * 192.0f, 96.0f);
                            if (IsMoving()) {
                                m_iExploreCommitTime = level.inttime + 1500;
                            }
                        } else if (lr == 1) {
                            SetWhy("learnrepath");
                            MoveNear(m_vTargetPos, 96.0f);
                        }
                    }
                    m_iSlideRing = 0;
                    m_iSlideIdx  = 0;
                }
            }
        } else {
            m_iSlideRing = 0;
            m_iSlideIdx  = 0;
        }
    }

    // [user 2026-09-22] BLOCKER PROBE (bot_blocklog 1). When a bot has been genuinely stuck (pushing, ~0 speed)
    // for >3s, trace forward along its move goal and log WHAT is stopping it - a scripted door/barrier/vehicle/
    // debris the baked navmesh can't see (class/model/targetname), or world=1 for a real nav gap. Turns "stuck
    // at (x,y) for 13 min" into a named culprit so the fix is targeted. Throttled ~4s/bot. Bot-only, off by default.
    {
        static cvar_t *s_blockLog = NULL;
        if (!s_blockLog) {
            s_blockLog = gi.Cvar_Get("bot_blocklog", "0", 0);
        }
        // POSITION-BASED sample every ~3s (independent of the wall-slide escape, which was resetting the old
        // timer before it could fire). If a bot that WANTS to move barely moved in 3s, it is genuinely hung:
        // log the path state (pth=0 = no navmesh route = NAV GAP) and what is directly ahead (a solid entity
        // = a scripted blocker; clear = the block is to the side / a ledge / a nav issue).
        if (s_blockLog->integer && controlledEntity
            && (m_iBlockLogTime == 0 || (level.inttime - m_iBlockLogTime) >= 3000)) {
            float moved2d = 0.0f;
            if (m_iBlockLogTime != 0) {
                moved2d = (float)sqrt(
                    Square(controlledEntity->origin.x - m_vBlockCheckPos.x)
                    + Square(controlledEntity->origin.y - m_vBlockCheckPos.y));
            }
            bool moving = IsMoving();
            if (m_iBlockLogTime != 0 && moving && moved2d < 48.0f) {
                int     nodes = m_pPath->GetNodeCount();
                Vector  wishd = m_vCurrentGoal - controlledEntity->origin;
                wishd.z = 0;
                const char *cls = "(clear)";
                const char *mdl = "", *tn = "";
                int         solid = -1, isworld = 0, hitEnt = -1; // [T3] hitEnt: which entity (partner blocks)
                int         dst = -1; // [bot_doorLeaf probe] the door's state, when it is a door
                float       dist = 56.0f;
                if (wishd.lengthSquared() > 1.0f) {
                    VectorNormalize2D(wishd);
                    Vector smins = controlledEntity->mins;
                    Vector smaxs = controlledEntity->maxs;
                    smaxs.z -= STEPSIZE;
                    Vector  sb = controlledEntity->origin + Vector(0, 0, STEPSIZE);
                    trace_t bt = G_Trace(
                        sb, smins, smaxs, sb + wishd * 56.0f, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotBlockLog"
                    );
                    if (bt.fraction < 1.0f && bt.ent && bt.ent->entity) {
                        Entity *o = bt.ent->entity;
                        if (o->IsSubclassOfDoor()) { // [bot_doorLeaf probe] 0 shut, 1 open, 2 moving; +10 locked
                            Door *dd = static_cast<Door *>(o);
                            dst      = (dd->isOpen() ? 1 : (dd->isCompletelyClosed() ? 0 : 2)) + (dd->locked ? 10 : 0);
                        }
                        isworld    = (o == world) ? 1 : 0;
                        cls        = o->getClassname();
                        mdl        = o->model.c_str();
                        tn         = o->targetname.c_str();
                        solid      = o->getSolidType();
                        hitEnt     = o->entnum;
                        dist       = bt.fraction * 56.0f;
                    }
                }
                gi.Printf(
                    "^~^~^ BOTBLOCK e=%d tm=%c at=(%.0f %.0f %.0f) moved=%.0f pth=%d nodes=%d ahead_dist=%.0f "
                    "world=%d class=%s solid=%d model=%s tn=%s ent=%d dst=%d\n",
                    controlledEntity->entnum,
                    (controlledEntity->GetTeam() == TEAM_ALLIES) ? 'a'
                                                                 : ((controlledEntity->GetTeam() == TEAM_AXIS) ? 'x' : '?'),
                    controlledEntity->origin.x, controlledEntity->origin.y, controlledEntity->origin.z,
                    moved2d, m_bPathing ? 1 : 0, nodes, dist, isworld, cls, solid, mdl, tn, hitEnt, dst
                );
                {
                    // [HZM bug-2956 probe] WHERE it is steering: the node, the corridor corners, the link ahead / under
                    // way, the ground slope - a blocker is only explained once the target it pushes toward is known
                    Vector cc[3];
                    const int nc = m_pPath ? m_pPath->GetCorners(cc, 3) : 0;
                    Vector la, lb;
                    const bool bTrav = m_pPath && m_pPath->GetTraversingLink(la, lb);
                    const bool bAppr = !bTrav && m_pPath && m_pPath->GetApproachingLink(la, lb);
                    char       cs[160];
                    int        o = 0;
                    cs[0]        = 0;
                    for (int ci = 0; ci < nc && o < 140; ci++) {
                        o += Com_sprintf(cs + o, sizeof(cs) - o, "(%.0f %.0f %.0f)", cc[ci].x, cc[ci].y, cc[ci].z);
                    }
                    gi.Printf(
                        "^~^~^ BOTBLOCK2 e=%d goal=(%.0f %.0f %.0f) corners=%s trav=%d appr=%d link=%s(%.0f %.0f %.0f)->(%.0f %.0f %.0f) "
                        "gnz=%.2f why=%s dest=(%.0f %.0f %.0f) target=(%.0f %.0f %.0f) sm=%d\n",
                        controlledEntity->entnum, m_vCurrentGoal.x, m_vCurrentGoal.y, m_vCurrentGoal.z, nc ? cs : "-",
                        m_pPath ? (int)m_pPath->GetTraversingArea() : -1, m_pPath ? (int)m_pPath->GetApproachingArea() : -1,
                        bTrav ? "trav" : (bAppr ? "appr" : "none"), (bTrav || bAppr) ? la.x : 0.0f,
                        (bTrav || bAppr) ? la.y : 0.0f, (bTrav || bAppr) ? la.z : 0.0f, (bTrav || bAppr) ? lb.x : 0.0f,
                        (bTrav || bAppr) ? lb.y : 0.0f, (bTrav || bAppr) ? lb.z : 0.0f,
                        (controlledEntity->client && controlledEntity->client->ps.groundPlane)
                            ? controlledEntity->client->ps.groundTrace.plane.normal[2]
                            : -1.0f, // [bug-3066] pmove's ground; Entity::groundplane is not kept for clients
                        m_szGoalWhy ? m_szGoalWhy : "none", GetPathDestination().x, GetPathDestination().y,
                        GetPathDestination().z, m_vTargetPos.x, m_vTargetPos.y, m_vTargetPos.z,
                        m_pPath ? m_pPath->GetSteerMode() : -1
                    );
                }
            }
            m_vBlockCheckPos = controlledEntity->origin;
            m_iBlockLogTime  = level.inttime;
        }
    }

    if (bOnElevator || bNearLift || bLadder || bNearLadder) {
        return; // [HZM bug-2862] never jump in / at the lift; [bug-2871] +JUMP drops a climber off the ladder
    }

    CheckJump(botcmd);

    if (!m_bJump) {
        CheckJumpOverEdge(botcmd);
    }
}

Vector BotMovement::AvoidPlayersAhead(const Vector& dir, float& closeness) const
{
    // [HZM bot A1] other living players in our LANE (ahead <180u, within ~44u of our line, same floor): bend the heading
    // away from the nearest, passing on the RIGHT when they are dead ahead - both bots of a head-on pair then take the
    // same side (their own right) and slide past instead of mirroring each other.
    closeness = 0.0f;
    Vector fdir(dir.x, dir.y, 0);
    if (fdir.lengthSquared() < 0.01f) {
        return dir;
    }
    VectorNormalize2D(fdir);
    const Vector rdir(fdir.y, -fdir.x, 0); // right of travel
    float        steer = 0.0f;
    for (int i = 0; i < game.maxclients; i++) {
        gentity_t *ge = &g_entities[i];
        if (!ge->inuse || !ge->entity || ge->entity == controlledEntity || !ge->entity->IsSubclassOfPlayer()) {
            continue;
        }
        Player *o = static_cast<Player *>(ge->entity);
        if (o->IsDead() || o->IsSpectator()) {
            continue;
        }
        const Vector r = o->origin - controlledEntity->origin;
        if (fabs(r.z) > 72.0f) {
            continue;
        }
        const float along = r.x * fdir.x + r.y * fdir.y;
        const float lat   = r.x * rdir.x + r.y * rdir.y;
        if (along < 16.0f || along > 180.0f || fabs(lat) > 44.0f) {
            continue;
        }
        const float w = 1.0f - along / 180.0f;
        if (w > closeness) {
            closeness = w;
            steer     = (lat > 8.0f) ? -1.0f : 1.0f; // they are on our right: go left; else keep right
        }
    }
    if (closeness <= 0.0f) {
        return dir;
    }
    Vector out = fdir + rdir * (steer * 0.9f * closeness);
    out.z      = 0;
    VectorNormalize2D(out);
    return out;
}

bool BotMovement::LowObstacleAhead(const Vector& dir) const
{
    // [HZM bot A2] something blocks us at step height 32u ahead, but the same box raised by a jump (48u) passes: a
    // hop clears it (a lip, a sandbag, a low wall). A plain wall is blocked at both heights - jumping there is jank.
    Vector d(dir.x, dir.y, 0);
    if (d.lengthSquared() < 0.01f) {
        return false;
    }
    VectorNormalize2D(d);
    Vector smins = controlledEntity->mins;
    Vector smaxs = controlledEntity->maxs;
    smaxs.z -= STEPSIZE;
    const Vector lo  = controlledEntity->origin + Vector(0, 0, STEPSIZE);
    trace_t      tlo = G_Trace(lo, smins, smaxs, lo + d * 32.0f, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotHopLo");
    if (tlo.fraction >= 1.0f || BotProbeHitWalkable(tlo)) {
        return false; // [bot_slopeWalk] clear, or only the ground rising: nothing to hop
    }
    const Vector hi  = controlledEntity->origin + Vector(0, 0, STEPSIZE + 48.0f);
    trace_t      thi = G_Trace(hi, smins, smaxs, hi + d * 32.0f, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotHopHi");
    return !thi.startsolid && thi.fraction >= 1.0f;
}

Vector BotMovement::CalculateDir(const Vector& delta) const
{
    Vector dir;

    dir    = delta;
    dir[2] = 0;
    VectorNormalize2D(dir);

    return dir;
}

Vector BotMovement::CalculateRelativeWishDirection(const Vector& dir) const
{
    Vector angles;
    Vector wishdir;

    angles = dir.toAngles() - controlledEntity->angles;
    angles.AngleVectorsLeft(&wishdir);

    return wishdir;
}

void BotMovement::CheckAttractiveNodes()
{
    for (int i = m_attractList.NumObjects(); i > 0; i--) {
        nodeAttract_t *a = m_attractList.ObjectAt(i);

        if (a->m_pNode == NULL || !a->m_pNode->CheckTeam(controlledEntity) || level.time > a->m_fRespawnTime) {
            delete a;
            m_attractList.RemoveObjectAt(i);
        }
    }
}

void BotMovement::CheckEndPos(Entity *entity)
{
    Vector  start;
    Vector  end;
    trace_t trace;

    if (!m_pPath->GetNodeCount()) {
        return;
    }

    start = m_pPath->GetDestination();
    end   = m_vTargetPos;

    trace =
        G_Trace(start, entity->mins, entity->maxs, end, entity, MASK_TARGETPATH, true, "BotController::CheckEndPos");

    if (trace.fraction < 0.95f) {
        m_vTargetPos = trace.endpos;
    }
}

void BotMovement::CheckJump(usercmd_t& botcmd)
{
    Vector  start;
    Vector  end;
    Vector  dir;
    Vector  delta;
    trace_t trace;

    if (controlledEntity->GetLadder()) {
        if (g_navigation_legacy->integer) {
            botcmd.upmove = botcmd.upmove ? 0 : 127;
        } else if (!m_pPath->GetNodeCount()) {
            // If the bot is not moving, cancel it
            botcmd.upmove = botcmd.upmove ? 0 : 127;
        }
        return;
    }

    if (!controlledEntity->groundentity && !controlledEntity->client->ps.walking) {
        // Falling
        m_bJump = false;
        return;
    }

    // [HZM bug-2879] "a lot of random jumping still" (user): this hopped whenever anything step-high was within a
    // body length ahead - 420 of 549 bot jumps on The Rail Yard, 114 of them at full run. A player only hops what
    // actually stops them: a bot running freely slides/steps over or round it; once it has slowed against it (or
    // the route is on a navmesh JUMP link) the hop below still fires, at most once per 1.5s.
    const bool bJumpLink = IsRecastJumpArea(m_pPath->GetTraversingArea()) || IsRecastJumpArea(m_pPath->GetApproachingArea());
    if (!bJumpLink && (controlledEntity->velocity.lengthXYSquared() > Square(60) || level.inttime < m_iPathJumpNext)) {
        m_bJump = false;
        return;
    }

    dir = m_vCurrentDir;

    start = controlledEntity->origin + Vector(0, 0, STEPSIZE);
    end =
        controlledEntity->origin + Vector(0, 0, STEPSIZE) + dir * (controlledEntity->maxs.y - controlledEntity->mins.y);

    if (ai_debugpath->integer) {
        G_DebugLine(start, end, 1, 0, 1, 1);
    }

    // Check if the bot needs to jump
    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        false,
        "BotController::CheckJump"
    );

    // No need to jump
    bool bLinkHop = false;
    if (!trace.startsolid && (trace.fraction > 0.5f || (!bJumpLink && BotProbeHitWalkable(trace)))) { // [bot_slopeWalk]
        // [HZM bug-2892] ...unless the route is ON a navmesh JUMP link, at its start, and the bot has all but stopped:
        // the mesh itself says "hop here" and the ground ahead can be a face too steep to walk, with nothing at step
        // height for this probe to hit (t2l4 round 10: an axis bot pushed at such a link for minutes at 5-18u/s, the
        // probe clear, never hopping). Hop along the link, at most once per 1.5s.
        Vector lf, lt;
        const bool bAtLink = IsRecastJumpArea(m_pPath->GetTraversingArea())
                          || (m_pPath->GetApproachingLink(lf, lt) && (lf - controlledEntity->origin).lengthXYSquared() < Square(48));
        if (!bJumpLink || !bAtLink || controlledEntity->velocity.lengthXYSquared() >= Square(40)
            || level.inttime < m_iPathJumpNext) {
            m_bJump = false;
            return;
        }
        bLinkHop = true;
    }

    if (!bLinkHop) {
    start = controlledEntity->origin;
    end   = controlledEntity->origin;
    end.z += STEPSIZE * 3;
    end.z += STEPSIZE / 1.5;

    if (ai_debugpath->integer) {
        G_DebugLine(start, end, 1, 0, 1, 1);
    }

    // Check if the bot can jump up
    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        true,
        "BotController::CheckJump"
    );

    start = trace.endpos;
    end   = trace.endpos + dir * (controlledEntity->maxs.y - controlledEntity->mins.y);

    if (ai_debugpath->integer) {
        G_DebugLine(start, end, 1, 0, 1, 1);
    }

    Vector bounds[2];
    bounds[0] = Vector(controlledEntity->mins[0], controlledEntity->mins[1], 0);
    bounds[1] = Vector(
        controlledEntity->maxs[0],
        controlledEntity->maxs[1],
        (controlledEntity->maxs[0] + controlledEntity->maxs[1]) * 0.5
    );

    // Check if the bot can jump at the location
    trace = G_Trace(
        start, bounds[0], bounds[1], end, controlledEntity, MASK_PLAYERSOLID, false, "BotController::CheckJump"
    );

    if (trace.plane.normal[2] <= MIN_WALK_NORMAL && trace.fraction < 1) {
        m_bJump = false;
        return;
    }
    }

    if (!m_bJump) {
        m_bJump          = true;
        m_iJumpCheckTime = level.inttime;
        m_vJumpLocation  = controlledEntity->origin;
    } else if (level.inttime > m_iJumpCheckTime + 100) {
        m_bJump = false;

        delta = m_vJumpLocation - controlledEntity->origin;
        if (delta.lengthSquared() < Square(32)) {
            m_iPathJumpNext = level.inttime + 1500;
            Jump(botcmd, bLinkHop ? "linkjump" : "pathjump");
            // [HZM bug-2913] a hop at a navmesh JUMP link: three from its take-off inside 15s without getting across and
            // the link is one a player cannot make at this pace - switch it off for every bot and route round it
            Vector la, lb;
            // (UPWARD hops only, from the low end: a drop is never a reach problem - lift4b counted a bot stuck on a
            // ledge top against every downward link round it)
            if (bJumpLink && (m_pPath->GetTraversingLink(la, lb) || m_pPath->GetApproachingLink(la, lb))
                && lb.z > la.z + STEPSIZE) {
                if ((la - m_vJumpLinkA).lengthSquared() > Square(16.0f) || level.inttime - m_iJumpLinkT > 15000) {
                    m_vJumpLinkA    = la;
                    m_vJumpLinkB    = lb;
                    m_iJumpLinkHops = 0;
                    m_iJumpLinkT    = level.inttime;
                }
                m_iJumpLinkHops++;
                if (m_iJumpLinkHops >= 3 && (controlledEntity->origin - la).lengthXYSquared() < Square(80.0f)) {
                    const int n = NavLearn_DisableJumpLink(la, lb);
                    gi.Printf(
                        "^~^~^ BOTLEARN e=%d at=(%.0f %.0f %.0f) jumplink to=(%.0f %.0f %.0f) disabled=%d why=%s\n",
                        controlledEntity->entnum, la.x, la.y, la.z, lb.x, lb.y, lb.z, n, m_szGoalWhy ? m_szGoalWhy : "none"
                    );
                    m_iJumpLinkHops = 0;
                    if (n > 0) {
                        SetWhy("learnrepath");
                        MoveNear(m_vTargetPos, 96.0f);
                    }
                }
            }
        }
    }
}

void BotMovement::CheckJumpOverEdge(usercmd_t& botcmd)
{
    Vector  start;
    Vector  end;
    Vector  dir;
    trace_t trace;

    // [HZM bug-2879] jump off a ledge only where the route crosses a real GAP (a navmesh JUMP link); anywhere else a
    // player just walks off it - this hopped at every kerb, rubble lip and stair edge
    if (!IsRecastJumpArea(m_pPath->GetTraversingArea()) && !IsRecastJumpArea(m_pPath->GetApproachingArea())) {
        return;
    }

    if (!controlledEntity->groundentity && !controlledEntity->client->ps.walking) {
        // Falling
        return;
    }

    dir = m_vCurrentDir;

    start = controlledEntity->origin + Vector(0, 0, STEPSIZE);
    end =
        controlledEntity->origin + Vector(0, 0, STEPSIZE) + dir * (controlledEntity->maxs.y - controlledEntity->mins.y);

    if (ai_debugpath->integer) {
        G_DebugLine(start, end, 1, 0, 1, 1);
    }

    // Check if the bot needs to jump
    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        false,
        "BotController::CheckJumpOverEdge"
    );

    if (trace.fraction < 1) {
        // Blocked
        return;
    }

    //
    // Check if falling
    //

    start = trace.endpos;
    end   = start - Vector(0, 0, STEPSIZE * 2);

    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        false,
        "BotController::CheckJumpOverEdge"
    );

    if (trace.fraction != 1.0) {
        // Blocked
        return;
    }

    //
    // Check if there is an edge at the end
    //

    end = start + dir * controlledEntity->GetRunSpeed() / 2.0;
    end -= Vector(0, 0, STEPSIZE * 2);

    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        false,
        "BotController::CheckJumpOverEdge"
    );

    if (trace.fraction == 1) {
        return;
    }

    if (!botcmd.upmove) {
        Jump(botcmd, "overedge");
    } else {
        botcmd.upmove = 0;
    }
}

/*
====================
AvoidPath

Avoid the specified position within the radius and start from a direction
====================
*/
void BotMovement::AvoidPath(
    Vector vAvoid, float fAvoidRadius, Vector vPreferredDir, float *vLeashHome, float fLeashRadius
)
{
    Vector      vDir;
    const char *why = m_szWhy ? m_szWhy : "internal";
    m_szWhy         = NULL;

    // [user 2026-09-21] stuck-escape commit: while the escape burst is committed, ignore the brain's re-path
    // (see MoveThink) so it can't drag a just-freed bot back into the choke.
    if (level.inttime < m_iExploreCommitTime) {
        return;
    }

    if (vPreferredDir == vec_zero) {
        vDir = controlledEntity->origin - vAvoid;
        VectorNormalizeFast(vDir);
    } else {
        vDir = vPreferredDir;
    }

    PathSearchParameter parameters;
    parameters.entity     = controlledEntity;
    parameters.fallHeight = maxFallHeight;
    parameters.leashDist  = fLeashRadius;
    if (vLeashHome) {
        parameters.leashHome = vLeashHome;
    }
    m_pPath->FindPathAway(controlledEntity->origin, vAvoid, vDir, fAvoidRadius, parameters);

    NewMove();

    if (!m_pPath->GetNodeCount()) {
        // Random movements
        m_vTargetPos = controlledEntity->origin + Vector(G_Random(256) - 128, G_Random(256) - 128, G_Random(256) - 128);
        m_vCurrentGoal = m_vTargetPos;
        LogGoal("avoidrandom", why, m_vTargetPos, false);
        return;
    }

    m_iLastMoveTime = level.inttime;
    m_vTargetPos    = m_pPath->GetDestination();
    LogGoal("avoid", why, m_vTargetPos, true);
}

/*
====================
MoveNear

Move near the specified position within the radius
====================
*/
void BotMovement::MoveNear(Vector vNear, float fRadius, float *vLeashHome, float fLeashRadius)
{
    const char *why = m_szWhy ? m_szWhy : "internal";
    m_szWhy         = NULL;
    // [user 2026-09-21] stuck-escape commit (see MoveThink). The escape itself zeroes m_iExploreCommitTime
    // before calling MoveNear, so this guard blocks only the brain's competing re-paths, not the escape.
    if (level.inttime < m_iExploreCommitTime) {
        return;
    }

    PathSearchParameter parameters;
    parameters.entity     = controlledEntity;
    parameters.fallHeight = maxFallHeight;
    parameters.leashDist  = fLeashRadius;
    if (vLeashHome) {
        parameters.leashHome = vLeashHome;
    }

    m_pPath->FindPathNear(controlledEntity->origin, vNear, fRadius, parameters);
    NewMove();

    if (!m_pPath->GetNodeCount()) {
        m_bPathing = false;
        LogGoal("near", why, vNear, false);
        return;
    }

    m_iLastMoveTime = level.inttime;
    m_vTargetPos    = m_pPath->GetDestination();
    LogGoal("near", why, vNear, true);
}

/*
====================
MoveTo

Move to the specified position
====================
*/
void BotMovement::MoveTo(Vector vPos, float *vLeashHome, float fLeashRadius)
{
    const char *why = m_szWhy ? m_szWhy : "internal";
    m_szWhy         = NULL;
    // [user 2026-09-21] stuck-escape commit: don't let the objective/combat re-path overwrite an active
    // escape goal (see MoveThink) - this is the specific overwrite that made the escape "do nothing".
    if (level.inttime < m_iExploreCommitTime) {
        return;
    }

    // [HZM bug-2881] ALREADY walking a live route to (about) this spot: keep it. The objective code re-issues its MoveTo
    // EVERY FRAME, and each one re-ran FindPath and NewMove() - which resets m_vLastCheckPos, the reference the 1s
    // "no progress" block check measures from. A bot running flat out at 150u/s therefore always looked like it had
    // moved one frame's worth: blocked -> 128u side-step / blockrepath every ~3s -> stuck escape BACKWARDS
    // (probe2 round 7: 95% of stuck escapes hit MOVING, un-crowded bots; stuckescape 6-9% of all bot time).
    // The pather re-plans by itself when the bot strays from the corridor, and MoveThink re-paths every 5s.
    if (m_bPathing && m_pPath->GetNodeCount() && (vPos - m_vTargetPos).lengthSquared() < Square(32)) {
        m_szGoalWhy = why; // same route, (maybe) a new reason for it
        return;
    }

    m_vTargetPos = vPos;

    PathSearchParameter parameters;
    parameters.entity     = controlledEntity;
    parameters.fallHeight = maxFallHeight;
    parameters.leashDist  = fLeashRadius;
    if (vLeashHome) {
        parameters.leashHome = vLeashHome;
    }

    m_pPath->FindPath(controlledEntity->origin, vPos, parameters);

    NewMove();

    if (!m_pPath->GetNodeCount()) {
        m_bPathing = false;
        LogGoal("to", why, vPos, false);
        return;
    }

    m_iLastMoveTime = level.inttime;
    CheckEndPos(controlledEntity);
    LogGoal("to", why, vPos, true);
}

/*
====================
MoveToBestAttractivePoint

Move to the nearest attractive point with a minimum priority
Returns true if no attractive point was found
====================
*/
bool BotMovement::MoveToBestAttractivePoint(int iMinPriority)
{
    Container<AttractiveNode *> list;
    AttractiveNode             *bestNode;
    float                       bestDistanceSquared;
    int                         bestPriority;

    if (m_pPrimaryAttract && AtObjective()) {
        // [HZM bug-2883] STANDING on its objective spot: stay there. Re-issuing MoveTo here every frame made a route that
        // arrived at once, was cleared, and was re-made the next frame (probe2 Desert Road: 464 arrivals + 419
        // objective routes per bot-minute), and the off-mesh recovery read that churn as a broken route and walked the
        // bots off in random directions (478 'offmesh', 61 loops). The stay timer runs as before.
        if (!m_fAttractTime) {
            m_fAttractTime = level.time + m_pPrimaryAttract->m_fMaxStayTime;
        }
        if (level.time <= m_fAttractTime) {
            return true;
        }
        m_pPrimaryAttract = NULL; // stay over: pick again below (the same node - the stock cooldown record was never kept)
    }
    if (m_pPrimaryAttract) {
        // [HZM Phase 2] path to THIS bot's scattered goal (set on acquisition below), not the shared node
        // origin - otherwise every bot re-converges to the same 16u bubble every frame and blocks each other.
        MoveTo(m_vAttractScatterGoal);

        if (!IsMoving()) {
            m_pPrimaryAttract = NULL;
        } else {
            if (MoveDone()) {
                if (!m_fAttractTime) {
                    m_fAttractTime = level.time + m_pPrimaryAttract->m_fMaxStayTime;
                }
                if (level.time > m_fAttractTime) {
                    nodeAttract_t *a  = new nodeAttract_t;
                    a->m_fRespawnTime = level.time + m_pPrimaryAttract->m_fRespawnTime;
                    a->m_pNode        = m_pPrimaryAttract;

                    m_pPrimaryAttract = NULL;
                }
            }

            return true;
        }
    }

    if (!attractiveNodes.NumObjects()) {
        return false;
    }

    bestNode            = NULL;
    // [HZM bug-2880] was 99999999 (= 10000u squared): an objective more than 10000u away could never beat the
    // sentinel, so on the big maps a bot far from its goal got NO objective and random-walked (probe2 BOTOBJ: the
    // node passed every check and was still not picked; t2l1 bots wandered 69% of the time, The Rail Yard ~25%)
    bestDistanceSquared = 1.0e18f;
    bestPriority        = iMinPriority;
    int rjResp = 0, rjPrio = 0, rjTeam = 0, rjDist = 0, rjPath = 0; // [HZM bug-2879] why no objective (BOTOBJ)

    for (int i = attractiveNodes.NumObjects(); i > 0; i--) {
        AttractiveNode *node = attractiveNodes.ObjectAt(i);
        float           distSquared;
        bool            m_bRespawning = false;

        for (int j = m_attractList.NumObjects(); j > 0; j--) {
            AttractiveNode *node2 = m_attractList.ObjectAt(j)->m_pNode;

            if (node2 == node) {
                m_bRespawning = true;
                break;
            }
        }

        if (m_bRespawning) {
            rjResp++;
            continue;
        }

        if (node->m_iPriority < bestPriority) {
            rjPrio++;
            continue;
        }

        if (!node->CheckTeam(controlledEntity)) {
            rjTeam++;
            continue;
        }

        distSquared = VectorLengthSquared(controlledEntity->origin - node->origin);

        if (node->m_fMaxDistanceSquared >= 0 && distSquared > node->m_fMaxDistanceSquared) {
            rjDist++;
            continue;
        }

        if (!CanMoveTo(node->origin)) {
            rjPath++;
            continue;
        }

        if (distSquared < bestDistanceSquared) {
            bestDistanceSquared = distSquared;
            bestNode            = node;
            bestPriority        = node->m_iPriority;
        }
    }

    if (bestNode) {
        m_pPrimaryAttract = bestNode;
        m_fAttractTime    = 0;
        // [HZM Phase 2] scatter this bot to a distinct reachable point in a ring around the objective node so
        // N bots do not stack on one origin (the reported clumping/blocking). bot_objective_spread 0 = stock.
        //
        // CRITICAL: re-roll the scattered goal ONLY when the node's origin has actually moved (>64u from the
        // point we last scattered around). The fast path at the top of this function NULLs m_pPrimaryAttract
        // the frame a bot arrives (IsMoving()==false with an empty path), so a fresh selection runs every frame
        // once the bot is standing on its goal. Rolling a new random ring point on each of those re-selections
        // made bots hop endlessly between scatter points - the "stuttering in place" bug (bug-2696). Anchoring
        // the roll to the node origin keeps each bot on ONE stable spread point while the objective is static,
        // yet still re-scatters when a mode moves the node (e.g. the Push frontline advancing).
        if (VectorLengthSquared(bestNode->origin - m_vScatterAnchor) > 4096.0f) {
            m_vScatterAnchor = bestNode->origin;
            m_iObjArriveT    = 0; // [HZM bug-2888]

            static cvar_t *bot_objective_spread = NULL;
            if (!bot_objective_spread) {
                bot_objective_spread = gi.Cvar_Get("bot_objective_spread", "260", CVAR_ARCHIVE);
            }
            m_vAttractScatterGoal = bestNode->origin;
            if (bot_objective_spread->value > 1.0f) {
                // [user 2026-09-21] PER-BOT DETERMINISTIC fan-out for ROUTE VARIETY. The old ring point was rolled
                // with G_Random, so bots clustered randomly at the goal and every one still took the SAME shortest
                // road to reach it - the "no variety, they all funnel past the tank, ignore the road that goes
                // around" report. Deriving each bot's bearing from its entnum via the golden angle (137.508 deg)
                // gives every bot a STABLE, DISTINCT direction around the objective, so different bots approach from
                // different sides and the pather routes them down different roads. Deterministic => it never
                // re-rolls, so it cannot reintroduce the stutter-in-place (bug-2696). Raise bot_objective_spread
                // (try 400-600) to push bots onto the wide/far-around roads; 0 restores stock single-road behaviour.
                float baseAng = (float)controlledEntity->entnum * 137.508f;
                for (int tries = 0; tries < 6; tries++) {
                    // walk around the ring from this bot's own bearing until a reachable lane point is found,
                    // shrinking the radius on later fallbacks so a boxed-in objective still yields a near point
                    float ang = baseAng + (float)tries * 60.0f;
                    float rad = bot_objective_spread->value * (tries < 2 ? 1.0f : 0.6f);
                    Vector cand = bestNode->origin
                                + Vector((float)cos(DEG2RAD(ang)) * rad, (float)sin(DEG2RAD(ang)) * rad, 0);
                    if (CanMoveTo(cand)) {
                        m_vAttractScatterGoal = cand;
                        break;
                    }
                }
            }
        }
        MoveTo(m_vAttractScatterGoal);
        return true;
    } else {
        // No attractive point found
        static cvar_t *s_probeA = NULL;
        if (!s_probeA) {
            s_probeA = gi.Cvar_Get("bot_probe", "0", 0);
        }
        if (s_probeA->integer && (level.inttime - m_iObjLogT > 5000 || level.inttime < m_iObjLogT)) {
            m_iObjLogT = level.inttime;
            gi.Printf(
                "^~^~^ BOTOBJ e=%d none nodes=%d minprio=%d resp=%d prio=%d team=%d dist=%d nopath=%d at=(%.0f %.0f %.0f)\n",
                controlledEntity->entnum, attractiveNodes.NumObjects(), iMinPriority, rjResp, rjPrio, rjTeam, rjDist, rjPath,
                controlledEntity->origin.x, controlledEntity->origin.y, controlledEntity->origin.z
            );
        }
        return false;
    }
}

/*
====================
NewMove

Called when there is a new move
====================
*/
void BotMovement::NewMove()
{
    // [HZM bug-2881] the 1s "no progress" check measures where the bot WAS 1-2s ago against where it is now. Re-seating
    // that reference on every new route (the brain re-routes whenever its reason changes - idle <-> curious flips
    // ~2.4x a bot-minute) made the next check read a bot running flat out as "moved <64u": blocked, and once in the
    // blocked state the per-frame re-issue escalated it to a stuck escape. Progress is position over time, whatever
    // the route: only a bot starting from a standstill takes a fresh reference.
    if (!m_bPathing) {
        m_vLastCheckPos[0] = controlledEntity->origin;
        m_vLastCheckPos[1] = controlledEntity->origin;
    }
    m_bPathing = true;
}

void BotMovement::CalculateBestFrontAvoidance(
    const Vector& targetOrg, float maxDist, const Vector& forward, const Vector& right, float& bestFrac, Vector& bestPos
)
{
    Vector  mins, maxs;
    bool    wasOnGround = true;
    Vector  start, step;
    Vector  entityStepOrg;
    trace_t trace;
    int     i;

    bestFrac = 0;
    bestPos  = vec_zero;

    mins = controlledEntity->mins;
    maxs = controlledEntity->maxs;
    maxs.z -= STEPSIZE;
    entityStepOrg = controlledEntity->origin + Vector(0, 0, STEPSIZE);

    for (i = 1; i < 5; i++) {
        start = entityStepOrg - forward + right * (32 * i);
        if (i == 1) {
            step = start;
        }

        //
        // Trace to the right
        //
        trace = G_Trace(entityStepOrg, mins, maxs, start, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta");

        if (trace.startsolid || trace.fraction <= 0) {
            break;
        }

        start   = trace.endpos;
        start.z = step.z;

        // Make sure the bot can jump after falling
        trace = G_Trace(
            start,
            mins,
            maxs,
            start - Vector(0, 0, STEPSIZE + STEPSIZE * 3),
            controlledEntity,
            MASK_PLAYERSOLID,
            qtrue,
            "GetCurrentDelta"
        );
        if (trace.fraction == 1) {
            if (!wasOnGround) {
                break;
            }

            wasOnGround = false;
            continue;
        }

        wasOnGround = true;
        step        = trace.endpos;

        //
        // Trace from the right to the node
        //
        trace = G_Trace(start, mins, maxs, targetOrg, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta");
        if (trace.fraction == 0) {
            trace = G_Trace(
                start,
                mins,
                maxs,
                start + forward * Q_min(maxDist, 64),
                controlledEntity,
                MASK_PLAYERSOLID,
                qtrue,
                "GetCurrentDelta"
            );
            trace = G_Trace(
                trace.endpos, mins, maxs, targetOrg, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta"
            );
        }

        if (trace.fraction > bestFrac) {
            bestFrac = trace.fraction;
            bestPos  = start;
        }
        if (trace.fraction >= 0.999) {
            break;
        }
    }
}

// [bug-3043] OPEN ROTATING DOOR LEAVES, at the steering level. The mesh is world-only and the lookahead ignores door leaves
// (bug-2879), so a leg that cuts past the hinge of a door standing open ("wait -1" doors stay open for the round) runs
// into the leaf. While the segment from the bot to its next corner crosses the leaf (a 20u capsule round hinge -> free
// end; a rotating door's origin is its hinge), aim at a point 44u past the free end, on the leaf's line: reached from this
// side without crossing, and from there the leg to the same corner no longer crosses. Nothing is re-planned. The door
// list is refreshed every 2s. bot_doorLeaf 2 (default; 1 = the v3 go-around, 0 = off).
static int      s_leafDoorN = 0;
static int      s_leafDoors[128];
static int      s_leafDoorT = -100000;
static str      s_leafMap;

Vector BotMovement::LeafDeflect(const Vector& delta)
{
    static cvar_t *s_leaf = NULL;
    if (!s_leaf) {
        s_leaf = gi.Cvar_Get("bot_doorLeaf", "2", 0);
    }
    if (s_leaf->integer != 2 || !controlledEntity || !m_pPath->GetNodeCount()) {
        return delta;
    }
    if (s_leafMap != level.mapname || level.inttime - s_leafDoorT > 2000 || level.inttime < s_leafDoorT) {
        s_leafMap   = level.mapname;
        s_leafDoorT = level.inttime;
        s_leafDoorN = 0;
        for (int i = 0; i < globals.num_entities && s_leafDoorN < 128; i++) {
            gentity_t *ge = &g_entities[i];
            if (ge->inuse && ge->entity && ge->entity->isSubclassOf(RotatingDoor)) {
                s_leafDoors[s_leafDoorN++] = i;
            }
        }
    }
    const Vector o = controlledEntity->origin;
    Vector       d2(delta.x, delta.y, 0);
    const float  segLen = d2.length();
    if (segLen < 8.0f) {
        return delta;
    }
    const float R = 20.0f;
    for (int k = 0; k < s_leafDoorN; k++) {
        gentity_t *ge = &g_entities[s_leafDoors[k]];
        if (!ge->inuse || !ge->entity || !ge->entity->isSubclassOf(RotatingDoor)) {
            continue;
        }
        Door *door = static_cast<Door *>(ge->entity);
        if (!door->isOpen() || o.z < door->absmin.z - 48.0f || o.z > door->absmax.z) {
            continue;
        }
        const Vector hinge(door->origin.x, door->origin.y, 0);
        Vector       c = (door->absmin + door->absmax) * 0.5f;
        c.z            = 0;
        Vector      ld   = c - hinge;
        const float half = ld.length();
        if (half < 8.0f || (Vector(o.x, o.y, 0) - hinge).lengthSquared() > Square(2.0f * half + segLen + 64.0f)) {
            continue;
        }
        ld *= 1.0f / half;
        const float  L = 2.0f * half; // hinge -> free end
        const Vector n(-ld.y, ld.x, 0);  // the leaf's normal
        const Vector a(o.x - hinge.x, o.y - hinge.y, 0);
        const Vector b = a + d2;
        const float  sa = a * n, sb = b * n;
        if (sa * sb >= 0.0f) {
            continue; // the leg does not cross the leaf's line
        }
        const float  u  = sa / (sa - sb);
        const float  tl = (a + (b - a) * u) * ld; // where along the leaf line it crosses (0 = hinge)
        // from the hinge (t=0: behind it is the door frame, which the mesh already routes round - a leg through the
        // doorway at the jamb crosses the line at t=-20 and clears the leaf; v4.0 sent those round the free end,
        // doubling the loops at m2l1's doors) to the free end + a player's half-width and the leaf's half-thickness
        if (tl < 0.0f || tl > L + 18.0f) {
            continue;
        }
        Vector wp = hinge + ld * (L + R + 24.0f);
        wp.z      = o.z;
        // walkable from here? (a leaf opened against a wall has no way round its end)
        Vector mins = controlledEntity->mins, maxs = controlledEntity->maxs;
        maxs.z -= STEPSIZE;
        const trace_t tr = G_Trace(
            o + Vector(0, 0, STEPSIZE), mins, maxs, wp + Vector(0, 0, STEPSIZE), controlledEntity, MASK_PLAYERSOLID, qtrue,
            "BotLeafDeflect"
        );
        if (tr.startsolid || tr.fraction < 0.9f) {
            continue;
        }
        static cvar_t *s_probe = NULL;
        if (!s_probe) {
            s_probe = gi.Cvar_Get("bot_probe", "0", 0);
        }
        if (s_probe->integer && level.inttime >= m_iLeafSteerLogT) {
            m_iLeafSteerLogT = level.inttime + 3000;
            gi.Printf(
                "^~^~^ BOTLEAF2 e=%d at=(%.0f %.0f %.0f) door=%d model=%s cross=%.0f/%.0f wp=(%.0f %.0f)\n",
                controlledEntity->entnum, o.x, o.y, o.z, door->entnum, door->model.c_str(), tl, L, wp.x, wp.y
            );
        }
        Vector nd = wp - o;
        nd.z      = delta.z;
        return nd;
    }
    return delta;
}

Vector BotMovement::FixDeltaFromCollision(const Vector& delta)
{
    trace_t trace;
    Vector  stepOrg;
    Vector  mins;
    Vector  maxs;
    Vector  newDelta;
    Vector  angles;
    Vector  forward, right, up;
    Vector  target;
    Vector  targetStepOrg;
    Vector  dest;
    Vector  front;
    float   dist;
    float   maxDist;

    if (controlledEntity->GetLadder()) {
        return delta;
    }

    if (level.inttime < m_iCollisionCheckTime + 250 || m_bJump) {
        if (m_bAvoidCollision) {
            newDelta = m_vTempCollisionAvoidance - controlledEntity->origin;
            if (newDelta.lengthSquared() > Square(16)) {
                // Not reached
                return newDelta;
            }

            // Path has been reached so clear the collision
            m_bAvoidCollision = false;
        }

        return delta;
    }

    m_iCollisionCheckTime = level.inttime;
    m_bAvoidCollision     = false;

    dest     = controlledEntity->origin + delta;
    newDelta = delta;
    dist     = VectorNormalize2(newDelta, forward);
    VectorToAngles(forward, angles);
    AngleVectors(angles, forward, right, up);

    mins = controlledEntity->mins;
    maxs = controlledEntity->maxs;
    maxs.z -= STEPSIZE;

    maxDist = Q_min(dist, 32);

    stepOrg       = controlledEntity->origin + Vector(0, 0, STEPSIZE);
    target        = controlledEntity->origin + forward * maxDist;
    targetStepOrg = target + Vector(0, 0, STEPSIZE);

    trace = G_Trace(stepOrg, mins, maxs, targetStepOrg, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta");
    if (trace.fraction < 1.0) {
        //
        // Try to use a flat plane instead
        //

        trace_t tmpTrace;
        Vector  forwardXY, rightXY, upXY;
        Vector  targetXY, targetStepOrgXY;

        angles.x = 0;
        AngleVectors(angles, forwardXY, rightXY, upXY);
        targetXY        = controlledEntity->origin + forwardXY * maxDist + Vector(0, 0, STEPSIZE);
        targetStepOrgXY = targetXY + Vector(0, 0, STEPSIZE);

        tmpTrace =
            G_Trace(stepOrg, mins, maxs, targetStepOrgXY, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta");

        if (tmpTrace.fraction > trace.fraction) {
            trace   = tmpTrace;
            forward = forwardXY;
            right   = rightXY;
            up      = upXY;
            target  = targetXY;
        }
    }

    if (trace.fraction < 1.0) {
        Vector start, step;
        float  bestLeftFrac = 0, bestRightFrac = 0;
        Vector bestLeftPos, bestRightPos;

        // 0 = parallel
        // -1 = perpendicular
        // If it's near parallel use the trace normal
        if (DotProduct(trace.plane.normal, forward) < -0.75) {
            VectorCopy(trace.plane.normal, forward);
            VectorNegate(forward, forward);
            VectorToAngles(forward, angles);
            AngleVectors(angles, forward, right, up);
        }

        //
        // Try to resolve following situation (schema from top):
        //
        // ┌───┐
        // ↑   │
        // p▌  t   ← Must be able to avoid the obstacle in front and move left or right, to target
        // ↓   ↑
        // └─→─┘
        //
        CalculateBestFrontAvoidance(target, 64, forward, right, bestRightFrac, bestRightPos);

        if (bestRightFrac != 1) {
            CalculateBestFrontAvoidance(target, 64, forward, -right, bestLeftFrac, bestLeftPos);
        }

        if (bestLeftFrac != 0 || bestRightFrac != 0) {
            m_bAvoidCollision = true;

            //
            // By default use the one with higher fraction
            //
            if (bestLeftFrac > bestRightFrac) {
                m_vTempCollisionAvoidance = bestLeftPos + forward * 64;
            } else if (bestLeftFrac < bestRightFrac) {
                m_vTempCollisionAvoidance = bestRightPos + forward * 64;
            } else {
                // Randomly choose direction if both are the same
                if (Vector::DistanceSquared(bestLeftPos, dest) > Vector::DistanceSquared(bestRightPos, dest)) {
                    m_vTempCollisionAvoidance = bestRightPos + forward * 64;
                } else {
                    m_vTempCollisionAvoidance = bestLeftPos + forward * 64;
                }
            }

            //
            // If falling, make sure to use the one that won't fall
            //
#if 0
            if (leftFallTrace.fraction != rightFallTrace.fraction
                && (leftFallTrace.fraction != 1 || rightFallTrace.fraction != 1)) {
                if (leftFallTrace.fraction == 1 && bestRightFrac) {
                    m_vTempCollisionAvoidance = bestRightPos + forward * 64;
                } else if (rightFallTrace.fraction == 1 && bestLeftFrac) {
                    m_vTempCollisionAvoidance = bestLeftPos + forward * 64;
                }
            }
#endif

            return m_vTempCollisionAvoidance - controlledEntity->origin;
        }
    }

    return delta;
}

/*
====================
CanMoveTo

Returns true if the bot has done moving
====================
*/
bool BotMovement::CanMoveTo(Vector vPos)
{
    PathSearchParameter parameters;
    parameters.fallHeight = maxFallHeight;
    parameters.entity     = controlledEntity;
    return m_pPath->TestPath(controlledEntity->origin, vPos, parameters);
}

/*
====================
MoveDone

Returns true if the bot has done moving
====================
*/
/*
====================
PathReaches

[HZM bug-2849] Detour answers an unreachable goal with a PARTIAL path to the closest reachable point (FindPath
closestPointOnPoly on the last corridor poly). Behind a closed-on-purpose gate that point IS the gate, so a bot
chasing a gunshot or an enemy seen through the bars walked to them and stood there pathless (m4l2 soak: 190
stationary axis samples at the track gates). True only when the current path really ends near vPos.
====================
*/
bool BotMovement::PathReaches(const Vector& vPos, float fTol)
{
    if (!m_pPath || !m_pPath->GetNodeCount()) {
        return false;
    }

    Vector delta = m_pPath->GetDestination() - vPos;
    delta.z      = 0;
    return delta.lengthSquared() <= fTol * fTol;
}

void BotMovement::CommitMove(int iMs)
{
    m_iExploreCommitTime = iMs > 0 ? level.inttime + iMs : 0;
}

Vector BotMovement::GetPathDestination() const
{
    return (m_pPath && m_pPath->GetNodeCount()) ? m_pPath->GetDestination() : controlledEntity->origin;
}

bool BotMovement::IsWaitingForDoor() const
{
    return level.inttime < m_iDoorWaitUntil;
}

bool BotMovement::GetDoorFacePoint(Vector& out) const
{
    if (level.inttime >= m_iDoorWaitUntil) {
        return false;
    }
    out = m_vDoorFace;
    return true;
}

void BotMovement::HoldFor(int iMs)
{
    m_iHoldUntil = (iMs > 0) ? level.inttime + iMs : 0;
}

bool BotMovement::IsHeld() const
{
    return level.inttime < m_iHoldUntil;
}

int BotMovement::GetPathCorners(Vector *out, int maxCorners) const
{
    if (!m_pPath || !m_bPathing) {
        return 0;
    }
    return m_pPath->GetCorners(out, maxCorners);
}

bool BotMovement::GetApproachingLadder(Vector& from, Vector& to) const
{
    // [HZM bot breach] walking up to a ladder link (start < 128u away) but not on it yet - the spot to clear the far end from
    if (!m_pPath || !m_bPathing || !controlledEntity || controlledEntity->GetLadder()
        || m_pPath->GetTraversingArea() == RECAST_AREA_LADDER || m_pPath->GetApproachingArea() != RECAST_AREA_LADDER) {
        return false;
    }
    return m_pPath->GetApproachingLink(from, to);
}

bool BotMovement::PathEntersBlast(const Vector& c, float r, float lookahead, int mask, float eyeZ) const
{
    // [HZM bot breach] does the route over the next `lookahead` units pass within r of c, at a point that can SEE c (a path
    // running along the far side of a wall is not walking into the blast)?
    if (!controlledEntity || !m_pPath || !m_bPathing) {
        return false;
    }
    Vector pts[5];
    pts[0]      = controlledEntity->origin;
    const int n = 1 + m_pPath->GetCorners(pts + 1, 4);
    if (n < 2) {
        return false;
    }
    float walked = 0.0f;
    for (int i = 0; i + 1 < n && walked < lookahead; i++) {
        const Vector a   = pts[i];
        const Vector seg = pts[i + 1] - a;
        const float  len = seg.length();
        if (len < 1.0f) {
            continue;
        }
        const Vector d = seg * (1.0f / len);
        float        t = DotProduct(c - a, d);
        t              = Q_clamp_float(t, 0.0f, Q_min(len, lookahead - walked));
        const Vector p = a + d * t;
        if ((p - c).lengthSquared() < Square(r)) {
            // (step 0: MASK_SOLID from +32 to the landing +16; bot_nadeSafe: MASK_EXPLOSION from the centroid to the
            // detonation +2 - the line RadiusDamage itself tests)
            const bool bExp = (mask == MASK_EXPLOSION);
            trace_t    tr   = G_Trace(
                p + Vector(0, 0, eyeZ), vec_zero, vec_zero, c + Vector(0, 0, bExp ? 2.0f : 16.0f), controlledEntity, mask,
                qfalse, "BotBlastPath"
            );
            if (tr.fraction >= 0.99f) {
                return true;
            }
        }
        walked += len;
    }
    return false;
}

bool BotMovement::IsOnLadder() const
{
    return m_iLadderDir != 0
        && ((m_pPath && m_pPath->GetTraversingArea() == RECAST_AREA_LADDER) || controlledEntity->GetLadder() != NULL);
}

bool BotMovement::GetLadderSteer(Vector& dir, float& pitch) const
{
    if (!controlledEntity || !IsOnLadder()) {
        return false;
    }
    if (m_pPath && m_pPath->GetTraversingArea() == RECAST_AREA_LADDER) {
        dir = m_pPath->GetCurrentDelta(); // face through the ladder toward the link's far end
    } else {
        controlledEntity->angles.AngleVectorsLeft(&dir); // finishing the climb: keep the facing we have
    }
    dir.z = 0;
    if (dir.lengthSquared() < 1.0f) {
        controlledEntity->angles.AngleVectorsLeft(&dir);
        dir.z = 0;
    }
    // LOOKING_UP "35" to mount at the bottom and "-30" to climb up; looking DOWN past 35 mounts at the top and climbs down
    pitch = (m_iLadderDir > 0) ? -50.0f : 50.0f;
    return true;
}

bool BotMovement::IsOnElevatorLink() const
{
    return m_pPath && m_pPath->GetTraversingArea() == RECAST_AREA_ELEVATOR;
}

/*
====================
[HZM bug-2912] LIFT PROTOCOL

User 2026-09-25 (The Rail Yard): "absolutely master getting the axis to understand how to get on and off the elevator
without getting stuck in the elevator gate, under it, and not moving out in time". Measured (_soak_rail_0925 at the
sv_dmspeedmult 0.6 pace): bots queued ON the door line and off-centre - the cab doorway is 96u between 12u corner
posts, single file - so the "solid mover ahead: wait" trace held them on a corner post, riders met boarders head-on,
and the gate closed on whoever was left in it. The link's own steering (cab centre, then far landing) could not tell
a shut gate from the cab, nor a crowded cab from an empty one.

For a navlinks `elevator` line that NAMES its movers (NavLift_Find), this owns the bot's movement from the moment its
route reaches the lift until it is clear on the far side, reading the real cab and gate state:
  WAIT  - queue BESIDE the door (>=64u off its centre line, 80-170u out), never in front of it or on the gate line
  ALIGN - the cab is here, the gate fully open, the riders are off (or 2s passed), fewer than 4 aboard and no lower-
          numbered team-mate waiting at this door: step onto the door's centre line 48u out
  BOARD - straight in along the centre line, then to a spot in the cab's far half (lane by entnum parity)
  RIDE  - hold that spot until the cab is at the other landing with that gate fully open
  EXIT  - out along that door's centre line to 80u past the gate plane, then re-route to the old goal
  SHAFT - on world floor under the cab (inside its footprint, below it): out through our landing's gate when it is
          open (mp_push.scr opens it for someone trapped), otherwise keep still
Riders get off first; nobody stands on a gate line when it moves; the give-up (45s) counts from the last progress
(a state change or the cab arriving with our gate open), not from joining the queue.
====================
*/
struct liftDoor_t {
    Vector c; // gate centre, z = its landing
    Vector n; // horizontal unit normal, cab -> landing
    Vector a; // horizontal unit along the door
};

static void LiftDoorOf(const Entity *gate, const Vector& landing, liftDoor_t& d)
{
    d.c = Vector((gate->absmin.x + gate->absmax.x) * 0.5f, (gate->absmin.y + gate->absmax.y) * 0.5f, landing.z);
    const float ex = gate->absmax.x - gate->absmin.x;
    const float ey = gate->absmax.y - gate->absmin.y;
    d.n            = (ex < ey) ? Vector(1, 0, 0) : Vector(0, 1, 0); // the gate's THIN axis
    Vector toLand  = landing - d.c;
    toLand.z       = 0;
    if (toLand * d.n < 0.0f) {
        d.n = d.n * -1.0f;
    }
    d.a = Vector(-d.n.y, d.n.x, 0);
}

static inline float LiftS(const liftDoor_t& d, const Vector& p) { return (p.x - d.c.x) * d.n.x + (p.y - d.c.y) * d.n.y; }
static inline float LiftT(const liftDoor_t& d, const Vector& p) { return (p.x - d.c.x) * d.a.x + (p.y - d.c.y) * d.a.y; }
static inline Vector LiftAt(const liftDoor_t& d, float s, float t) { return d.c + d.n * s + d.a * t; }

static void LiftSteer(Player *p, usercmd_t& botcmd, const Vector& to, float slowWithin)
{
    Vector d = to - p->origin;
    d.z      = 0;
    const float len = d.length();
    botcmd.upmove   = 0;
    if (len < 2.0f) {
        botcmd.forwardmove = 0;
        botcmd.rightmove   = 0;
        return;
    }
    d               = d * (1.0f / len);
    const float mag = (slowWithin > 0.0f && len < slowWithin) ? Q_max(0.35f, len / slowWithin) : 1.0f;
    Vector      f, r;
    AngleVectors(Vector(0, p->GetViewAngles().y, 0), f, r, NULL);
    botcmd.forwardmove = (signed char)((d.x * f.x + d.y * f.y) * 127.0f * mag);
    botcmd.rightmove   = (signed char)((d.x * r.x + d.y * r.y) * 127.0f * mag);
}

static inline bool LiftStill(const Entity *e) { return e->velocity.lengthSquared() < 1.0f; }

// the cab's 2x2 grid, in the BOARDING door's frame: lanes 18u either side of its centre line (36u apart - wider than a
// body, and both inside the 96u doorway, so two bots walk in or out side by side without crossing), rows 100u / 60u
// in. Slots 0/1 take the row nearest the far (exit) door, so the first aboard are the first off.
static inline float LiftSlotT(int slot) { return (slot & 1) ? 18.0f : -18.0f; }
static inline float LiftSlotS(int slot) { return (slot < 2) ? -100.0f : -60.0f; }

// the cab is standing at landing k: its box bottom sits 22u under the floor riders stand on (soak: 298 -> z 320,
// -262 -> z -240), and the navlinks landing z is that floor
static inline bool LiftCabAt(const Entity *cab, const Vector& landing)
{
    return LiftStill(cab) && fabs(cab->absmin.z + 22.0f - landing.z) < 12.0f;
}

// a gate is fully open when it has slid down into the floor: its top ~2u under the landing (closed it is ~68u above)
static inline bool LiftGateOpen(const Entity *gate, const Vector& landing)
{
    return LiftStill(gate) && gate->absmax.z < landing.z + 24.0f;
}

int BotMovement::GetLiftState(int& destEnd, const navLift_t *& liftRec) const
{
    destEnd = m_iLiftTo;
    liftRec = m_pLiftRec;
    return m_iLiftState;
}

void BotMovement::LiftAbort(const char *why)
{
    if (m_iLiftState != LIFT_NONE && controlledEntity) {
        static cvar_t *s_probe = NULL;
        if (!s_probe) {
            s_probe = gi.Cvar_Get("bot_probe", "0", 0);
        }
        if (s_probe->integer) {
            gi.Printf(
                "^~^~^ BOTLIFT e=%d end st=%d why=%s at=(%.0f %.0f %.0f)\n", controlledEntity->entnum, m_iLiftState, why,
                controlledEntity->origin.x, controlledEntity->origin.y, controlledEntity->origin.z
            );
        }
    }
    m_iLiftState  = LIFT_NONE;
    m_pLiftRec    = NULL;
    m_iLiftOpenT  = 0;
    m_iLiftQueueT = 0;
    CommitMove(0);
}

bool BotMovement::LiftThink(usercmd_t& botcmd)
{
    static cvar_t *s_liftProto = NULL, *s_probe = NULL;
    if (!s_liftProto) {
        s_liftProto = gi.Cvar_Get("bot_liftProtocol", "1", 0);
        s_probe     = gi.Cvar_Get("bot_probe", "0", 0);
    }
    if (!s_liftProto->integer || !controlledEntity || controlledEntity->IsDead() || !m_pPath) {
        if (m_iLiftState) {
            LiftAbort("off");
        }
        return false;
    }
    const Vector org = controlledEntity->origin;

    // ---- ENTER: our route reaches a lift whose movers are named
    if (m_iLiftState == LIFT_NONE) {
        Vector p, q;
        bool   have = false;
        if (m_pPath->GetTraversingArea() == RECAST_AREA_ELEVATOR && m_pPath->GetElevatorLink(p, q)
            && ((p - org).lengthXYSquared() < Square(300.0f) || (q - org).lengthXYSquared() < Square(300.0f)
                || ((p + q) * 0.5f - org).lengthXYSquared() < Square(200.0f))) {
            // (near the lift: a pather still flagged 'on the lift link' after a respawn across the map must not
            // re-admit the bot - lift3b logged 562 enter/'far' aborts from exactly that)
            have = true;
        } else if (m_pPath->GetApproachingArea() == RECAST_AREA_ELEVATOR && m_pPath->GetApproachingLink(p, q)
                   && (p - org).lengthXYSquared() < Square(300.0f) && fabs(p.z - org.z) < 96.0f) {
            have = true;
        }
        if (!have) {
            return false;
        }
        const navLift_t *rec = NavLift_Find(p, q);
        if (!rec) {
            return false;
        }
        Entity *cabE = G_FindTarget(NULL, rec->cab);
        Entity *g0   = G_FindTarget(NULL, rec->gate[0]);
        Entity *g1   = G_FindTarget(NULL, rec->gate[1]);
        if (!cabE || !g0 || !g1) {
            return false;
        }
        m_pLiftRec       = rec;
        m_pLiftEnt[0]    = cabE;
        m_pLiftEnt[1]    = g0;
        m_pLiftEnt[2]    = g1;
        m_iLiftFrom      = ((rec->end[0] - p).lengthSquared() <= (rec->end[1] - p).lengthSquared()) ? 0 : 1;
        m_iLiftTo        = 1 - m_iLiftFrom;
        m_vLiftGoal      = m_vTargetPos;
        m_iLiftSide      = (controlledEntity->entnum & 1) ? 1 : -1;
        m_iLiftOpenT     = 0;
        m_iLiftQueueT    = 0;
        m_iLiftState     = LIFT_WAIT;
        m_iLiftStateT    = level.inttime;
        m_iLiftProgressT = level.inttime;
        m_iLiftLogT      = 0;
    }

    Entity *cab     = m_pLiftEnt[0];
    Entity *gate[2] = {m_pLiftEnt[1], m_pLiftEnt[2]};
    if (!cab || !gate[0] || !gate[1] || !m_pLiftRec) {
        LiftAbort("gone");
        return false;
    }
    const Vector *L = m_pLiftRec->end;
    liftDoor_t    D[2];
    LiftDoorOf(gate[0], L[0], D[0]);
    LiftDoorOf(gate[1], L[1], D[1]);
    const int         k     = m_iLiftFrom;
    const int         j     = m_iLiftTo;
    const liftDoor_t& dk    = D[k];
    const liftDoor_t& dj    = D[j];
    const Entity     *gnd   = controlledEntity->groundentity ? controlledEntity->groundentity->entity : NULL;
    const bool        onCab = gnd == cab;
    const float       cx    = (cab->absmin.x + cab->absmax.x) * 0.5f;
    const float       cy    = (cab->absmin.y + cab->absmax.y) * 0.5f;
    const bool        inFoot = fabs(org.x - cx) < 64.0f && fabs(org.y - cy) < 64.0f;
    const bool        cabAtK = LiftCabAt(cab, L[k]);
    const bool        cabAtJ = LiftCabAt(cab, L[j]);
    const bool        openK  = LiftGateOpen(gate[k], L[k]);
    const bool        openJ  = LiftGateOpen(gate[j], L[j]);
    const int         prevState = m_iLiftState;
    auto              setState  = [&](int s) {
        m_iLiftState     = s;
        m_iLiftStateT    = level.inttime;
        m_iLiftProgressT = level.inttime;
        m_iLiftBlockT    = 0;
    };
    static cvar_t *s_liftLanes = NULL;
    if (!s_liftLanes) {
        s_liftLanes = gi.Cvar_Get("bot_liftLanes", "1", 0); // [HZM bug-2949] 0 = the bug-2912 boarding order
    }
    const bool bLanes = s_liftLanes->integer != 0;

    // ---- only AT the lift: queuing within ~400u of our landing, boarding/riding/leaving within 300u of the cab. The
    // protocol steers in straight lines, so it must never hold a bot that is anywhere else (a respawn, a knock-back,
    // a fall) - normal pathing brings it back and the entry test re-admits it
    {
        const Vector cabC(cx, cy, org.z);
        const bool   queueing = m_iLiftState == LIFT_WAIT || m_iLiftState == LIFT_ALIGN;
        if (queueing ? ((org - L[k]).lengthXYSquared() > Square(400.0f) || fabs(org.z - L[k].z) > 120.0f)
                     : (!inFoot && (org - cabC).lengthXYSquared() > Square(300.0f))) {
            LiftAbort("far");
            return false;
        }
    }

    // ---- give up only without progress: a lift that never comes, a door that never opens
    if (level.inttime - m_iLiftProgressT > 45000) {
        LiftAbort("giveup");
        SetWhy("liftgiveup");
        ClearMove();
        return false;
    }

    // ---- SHAFT: on world floor, under the cab, inside its footprint - highest priority
    if (!onCab && gnd && inFoot && org.z < cab->absmin.z - 4.0f) {
        if (m_iLiftState != LIFT_SHAFT) {
            setState(LIFT_SHAFT);
        }
        const int e = (fabs(L[0].z - org.z) < fabs(L[1].z - org.z)) ? 0 : 1;
        if (LiftGateOpen(gate[e], L[e])) {
            const Vector out = fabs(LiftT(D[e], org)) > 12.0f ? LiftAt(D[e], LiftS(D[e], org), 0) : LiftAt(D[e], 90, 0);
            LiftSteer(controlledEntity, botcmd, out, 0);
            if (controlledEntity->velocity.lengthXYSquared() < Square(20.0f) && level.inttime >= m_iPathJumpNext) {
                m_iPathJumpNext = level.inttime + 1200;
                Jump(botcmd, "liftshaft"); // the shaft floor sits below the landing
            }
        } else {
            botcmd.forwardmove = 0;
            botcmd.rightmove   = 0;
        }
        CommitMove(3000);
        return true;
    }
    if (m_iLiftState == LIFT_SHAFT) {
        setState(onCab ? LIFT_RIDE : LIFT_EXIT);
    }

    // ---- who else is at this lift
    int  aboard = 0, boarding = 0, queueAhead = 0;
    bool exiting = false, lowerMateWaiting = false;
    // [HZM bug-2949] the cab grid slots already claimed at OUR landing this stop (bots aligning / boarding / riding from
    // here), and which of them are still only lining up outside
    bool slotTaken[4] = {false, false, false, false};
    bool slotAligning[4] = {false, false, false, false};
    for (int i = 0; i < game.maxclients; i++) {
        gentity_t *ge = &g_entities[i];
        if (!ge->inuse || !ge->entity || ge->entity == controlledEntity || !ge->entity->IsSubclassOfPlayer()) {
            continue;
        }
        Player *o = static_cast<Player *>(ge->entity);
        if (o->IsDead() || o->IsSpectator()) {
            continue;
        }
        const bool oOnCab = o->groundentity && o->groundentity->entity == cab;
        int              oDest = -1;
        const navLift_t *oRec  = NULL;
        int              oSt   = LIFT_NONE;
        BotController   *bc    = botManager.getControllerManager().findController(o);
        if (bc) {
            oSt = bc->GetMovement().GetLiftState(oDest, oRec);
            if (oRec != m_pLiftRec) {
                oSt = LIFT_NONE;
            }
            if (oDest == j && (oSt == LIFT_ALIGN || oSt == LIFT_BOARD || oSt == LIFT_RIDE)) {
                const int os = bc->GetMovement().GetLiftSlot();
                if (os >= 0 && os < 4) {
                    slotTaken[os] = true;
                    slotAligning[os] = slotAligning[os] || oSt == LIFT_ALIGN;
                }
            }
        }
        if (oOnCab) {
            aboard++;
        } else if (oSt == LIFT_ALIGN || oSt == LIFT_BOARD) {
            boarding++;
        }
        // leaving through OUR door: a rider bound here, a bot on the cab with no lift route (goal changed), or a human
        // walking out through it
        const float os = LiftS(dk, o->origin);
        const bool  inDoorBox = fabs(LiftT(dk, o->origin)) < 48.0f && os > -48.0f && os < 40.0f
                             && fabs(o->origin.z - L[k].z) < 64.0f;
        if (oOnCab || inDoorBox) {
            if (bc) {
                if (((oSt == LIFT_RIDE || oSt == LIFT_EXIT) && oDest == k) || oSt == LIFT_NONE) {
                    exiting = true;
                }
            } else if (o->velocity * dk.n > 40.0f) {
                exiting = true;
            }
        }
        if (bc && oSt == LIFT_WAIT && oDest == j && o->GetTeam() == controlledEntity->GetTeam()
            && o->entnum < controlledEntity->entnum && (o->origin - L[k]).lengthXYSquared() < Square(260.0f)) {
            lowerMateWaiting = true;
        }
        // our place in the queue at this landing (any team): each waiting bot takes its own spot
        if (bc && oSt == LIFT_WAIT && oDest == j && o->entnum < controlledEntity->entnum
            && (o->origin - L[k]).lengthXYSquared() < Square(400.0f)) {
            queueAhead++;
        }
    }

    // a boarding chance at our landing: the cab here with our gate fully open (starts the let-off clock)
    if (cabAtK && openK) {
        if (!m_iLiftOpenT) {
            m_iLiftOpenT     = level.inttime;
            m_iLiftProgressT = level.inttime;
        }
    } else {
        m_iLiftOpenT = 0;
    }
    const bool gateKClosing = gate[k]->velocity.z > 1.0f;
    const bool gateJClosing = gate[j]->velocity.z > 1.0f;
    const float sK          = LiftS(dk, org);
    const float tK          = LiftT(dk, org);

    switch (m_iLiftState) {
    case LIFT_WAIT:
    {
        // the brain re-routed us away from the lift (a fight, a new goal): let normal movement have the bot back
        const bool stillRouted = m_pPath->GetApproachingArea() == RECAST_AREA_ELEVATOR
                              || m_pPath->GetTraversingArea() == RECAST_AREA_ELEVATOR;
        if (!stillRouted && !onCab) {
            LiftAbort("reroute");
            return false;
        }
        if (onCab) {
            setState(LIFT_RIDE);
            break;
        }
        const bool letOff = m_iLiftOpenT && (!exiting || level.inttime - m_iLiftOpenT > 2000);
        int freeSlot = -1;
        if (bLanes) {
            const int side    = (tK >= 0.0f) ? 1 : 0;
            const int pref[4] = {side, side + 2, 1 - side, 3 - side};
            for (int p = 0; p < 4 && freeSlot < 0; p++) {
                if (!slotTaken[pref[p]]) {
                    freeSlot = pref[p];
                }
            }
        }
        if (cabAtK && openK && letOff && aboard + boarding < 4 && !lowerMateWaiting && (!bLanes || freeSlot >= 0)) {
            if (bLanes) {
                // [HZM bug-2949] a FREE slot, on OUR side of the door: the lane on the side we queue (t -18 / +18), its
                // far row first, then the near row behind it on the same lane, then the other lane. The count-based
                // slot gave two bots of one stop the same slot / the same entry point and put lanes across each
                // other's path: m4l2 soak b8 - four axis in ALIGN shoving at two points for the whole 6s the door
                // was open, 305 stuck samples, the cab left empty
                // (no free slot: the grid is full this stop - the test above keeps our place in the queue)
                m_iLiftSlot = freeSlot;
            } else {
                m_iLiftSlot = Q_min(3, Q_max(0, aboard + boarding));
            }
            setState(LIFT_ALIGN);
            break;
        }
        // queue spot beside the door (validated: free box, ground a player can stand on). Each waiting bot takes the
        // queueAhead-th valid spot of the list (lift4b: six axis sharing two spots shoved each other for minutes)
        if (!m_iLiftQueueT || level.inttime - m_iLiftQueueT > 2000) {
            static const float cand[][2] = {
                {80, 64}, {80, -64}, {130, 64}, {130, -64}, {60, 110}, {60, -110}, {170, 0}, {120, 0},
                {200, 64}, {200, -64}, {240, 0}, {110, 110}
            };
            m_iLiftQueueT = level.inttime;
            m_vLiftQueue  = LiftAt(dk, 130, 0);
            int valid     = 0;
            for (int c = 0; c < 12; c++) {
                const Vector sp = LiftAt(dk, cand[c][0], cand[c][1] * m_iLiftSide) + Vector(0, 0, 4);
                trace_t      bt = G_Trace(
                    sp, controlledEntity->mins, controlledEntity->maxs, sp, controlledEntity, MASK_PLAYERSOLID, qtrue,
                    "LiftQueue"
                );
                if (bt.startsolid || bt.allsolid) {
                    continue;
                }
                trace_t gt = G_Trace(
                    sp, controlledEntity->mins, controlledEntity->maxs, sp - Vector(0, 0, 40), controlledEntity,
                    MASK_PLAYERSOLID, qtrue, "LiftQueueFloor"
                );
                if (gt.fraction >= 1.0f || gt.plane.normal[2] < 0.7f) {
                    continue;
                }
                m_vLiftQueue = gt.endpos; // the last valid one stands for any overflow
                if (valid++ >= queueAhead) {
                    break;
                }
            }
        }
        // there: stand (nudging at a shared spot read as stuck and shoved the neighbours)
        const float dQ2 = (m_vLiftQueue - org).lengthXYSquared();
        if (dQ2 < Square(20.0f)) {
            botcmd.forwardmove = 0;
            botcmd.rightmove   = 0;
            m_iLiftBlockT      = 0;
        } else if (bLanes && dQ2 < Square(72.0f) && controlledEntity->velocity.lengthXYSquared() < Square(20.0f)
                   && m_iLiftBlockT && level.inttime - m_iLiftBlockT > 600) {
            // [HZM bug-2949] close to the spot and pushing into whoever stands at it: this is our place in the queue -
            // stand (b8: 268 stuck samples of WAIT bots shoving each other at the bottom landing)
            botcmd.forwardmove = 0;
            botcmd.rightmove   = 0;
        } else {
            if (controlledEntity->velocity.lengthXYSquared() < Square(20.0f)) {
                if (!m_iLiftBlockT) {
                    m_iLiftBlockT = level.inttime;
                }
            } else {
                m_iLiftBlockT = 0;
            }
            LiftSteer(controlledEntity, botcmd, m_vLiftQueue, 48.0f);
        }
        break;
    }
    case LIFT_ALIGN:
    {
        // the chance went (the gate started closing, the cab left) before we got in: back to the queue
        if (!cabAtK || gateKClosing) {
            setState(LIFT_WAIT);
            break;
        }
        const float laneT = LiftSlotT(m_iLiftSlot);
        // [HZM bug-2949] each slot its own entry point: the far-row slot of a lane lines up 48u out, the near-row slot
        // of the SAME lane 40u behind it, and waits there until the one in front has started in - one file per lane,
        // two lanes, nobody crossing. (bug-2912 had one entry point per lane for both rows.)
        const int    rank   = bLanes ? (m_iLiftSlot >> 1) : 0;
        const float  entryS = 48.0f + 40.0f * rank;
        const bool   front  = !bLanes || rank == 0 || !slotAligning[m_iLiftSlot - 2];
        const Vector entry  = LiftAt(dk, entryS, laneT);
        if (fabs(tK - laneT) < 8.0f && fabs(sK - entryS) < 18.0f) {
            if (front) {
                setState(LIFT_BOARD);
                break;
            }
            botcmd.forwardmove = 0; // lined up behind the lane's first: wait for it to go in
            botcmd.rightmove   = 0;
            CommitMove(3000);
            break;
        }
        LiftSteer(controlledEntity, botcmd, entry, 24.0f);
        CommitMove(3000);
        break;
    }
    case LIFT_BOARD:
    {
        if (onCab && sK < LiftSlotS(m_iLiftSlot) + 14.0f) {
            setState(LIFT_RIDE);
            break;
        }
        // still outside the gate line when it starts to close: step back out, never stand in it
        if ((gateKClosing || !cabAtK) && sK > -12.0f) {
            setState(LIFT_WAIT);
            break;
        }
        // straight in along our lane to our slot
        const Vector spot = LiftAt(dk, LiftSlotS(m_iLiftSlot), LiftSlotT(m_iLiftSlot));
        LiftSteer(controlledEntity, botcmd, spot, 16.0f);
        CommitMove(3000);
        break;
    }
    case LIFT_RIDE:
    {
        if (!onCab && !inFoot) {
            // not on the cab after all (pushed off before it left, or it left without us): queue again
            setState(LIFT_WAIT);
            break;
        }
        if (cabAtJ && openJ) {
            setState(LIFT_EXIT);
            break;
        }
        // hold our slot in the grid
        const Vector spot = LiftAt(dk, LiftSlotS(m_iLiftSlot), LiftSlotT(m_iLiftSlot));
        if ((spot - org).lengthXYSquared() > Square(10.0f)) {
            LiftSteer(controlledEntity, botcmd, spot, 16.0f);
        } else {
            botcmd.forwardmove = 0;
            botcmd.rightmove   = 0;
        }
        CommitMove(3000);
        break;
    }
    case LIFT_EXIT:
    {
        const float sJ = LiftS(dj, org);
        const float tJ = LiftT(dj, org);
        if (sJ > 80.0f) {
            // off and clear: back to the goal we were routing to
            const Vector goal = m_vLiftGoal;
            LiftAbort("off");
            if (goal != vec_zero) {
                MoveTo(goal);
            }
            return false;
        }
        // the far gate started closing while we are still inside: stay aboard (the next stop brings us back)
        if (gateJClosing && sJ < -12.0f) {
            setState(LIFT_RIDE);
            break;
        }
        // straight out along our own lane (clamped inside the doorway) - never converge on the centre line, where the
        // riders of a full cab shoved each other sideways for the whole stop (lift1 soak: exit->ride->exit, speed 0)
        const Vector to = LiftAt(dj, 96.0f, Q_clamp_float(tJ, -22.0f, 22.0f));
        LiftSteer(controlledEntity, botcmd, to, 0);
        CommitMove(3000);
        if (level.inttime - m_iLiftStateT > 8000) {
            LiftAbort("exitslow");
            return false;
        }
        break;
    }
    default:
        LiftAbort("state");
        return false;
    }

    if (s_probe->integer && (m_iLiftState != prevState || level.inttime - m_iLiftLogT > 1000)) {
        static const char *names[] = {"none", "wait", "align", "board", "ride", "exit", "shaft"};
        m_iLiftLogT                 = level.inttime;
        gi.Printf(
            "^~^~^ BOTLIFT e=%d tm=%c st=%s from=%d cabAt=%d%d open=%d%d s=%.0f t=%.0f oncab=%d aboard=%d boarding=%d "
            "exiting=%d mate=%d\n",
            controlledEntity->entnum, controlledEntity->GetTeam() == TEAM_ALLIES ? 'a' : 'x',
            names[(m_iLiftState >= 0 && m_iLiftState <= 6) ? m_iLiftState : 0], k, cabAtK ? 1 : 0, cabAtJ ? 1 : 0, openK ? 1 : 0, openJ ? 1 : 0, sK, tK,
            onCab ? 1 : 0, aboard, boarding, exiting ? 1 : 0, lowerMateWaiting ? 1 : 0
        );
    }
    return m_iLiftState != LIFT_NONE;
}

bool BotMovement::MoveDone()
{
    if (!m_bPathing) {
        return true;
    }

    if (m_iTempAwayState != 0) {
        return false;
    }

    if (!m_pPath->GetNodeCount()) {
        return true;
    }

    Vector delta = m_pPath->GetDestination() - controlledEntity->origin;
    if (delta.lengthXYSquared() < Square(16) && (m_pPath->GetNodeCount() == 1 || delta.z < controlledEntity->maxs.z)) {
        return true;
    }

    return false;
}

/*
====================
IsMoving

Returns true if the bot has a current path
====================
*/
bool BotMovement::IsMoving(void)
{
    return m_bPathing;
}

/*
====================
ClearMove

Stop the bot from moving
====================
*/
void BotMovement::ClearMove(void)
{
    // [user 2026-09-21] stuck-escape commit: the combat think calls ClearMove() every time the weapon wants
    // the bot to stop-and-fire (playerbot.cpp), which would wipe an active escape path and re-park the bot on
    // the choke. Ignore it while committed; MoveThink's own reached-goal clears zero the commit first, so a
    // completed escape still stops normally.
    const char *why = m_szWhy ? m_szWhy : "internal";
    m_szWhy         = NULL;
    if (level.inttime < m_iExploreCommitTime) {
        return;
    }

    if (m_bPathing) {
        LogGoal("clear", why, controlledEntity ? controlledEntity->origin : vec_zero, true);
    }
    m_pPath->Clear();
    m_bPathing   = false;
    m_iNumBlocks = 0;
}

void BotMovement::LogGoal(const char *kind, const char *why, const Vector& to, bool ok)
{
    // [HZM bot probe2] one ^~^~^ BOTGOAL line per route change: who asked (why), what (to / near / avoid / clear), where,
    // whether a path was found, and how far the objective is from here and from the target (od / tod: a target
    // further from the objective than we are is a move AWAY from the mode). Repeats of the same tag to the same spot
    // within 4s are dropped (the brain re-issues its move every frame in some states).
    if (ok && Q_stricmp(kind, "clear")) {
        m_szGoalWhy = why;
    } else if (!Q_stricmp(kind, "clear")) {
        m_szGoalWhy = "none";
    }
    static cvar_t *s_probe = NULL;
    if (!s_probe) {
        s_probe = gi.Cvar_Get("bot_probe", "0", 0);
    }
    if (!s_probe->integer || !controlledEntity) {
        return;
    }
    if (m_szGoalLogWhy == why && (to - m_vGoalLogPos).lengthSquared() < Square(64) && level.inttime - m_iGoalLogT < 4000
        && level.inttime >= m_iGoalLogT) {
        return;
    }
    m_szGoalLogWhy = why;
    m_vGoalLogPos  = to;
    m_iGoalLogT    = level.inttime;
    const Vector org = controlledEntity->origin;
    Vector       obj;
    float        od = -1.0f, tod = -1.0f;
    if (GetObjective(obj)) {
        od  = (obj - org).lengthXY();
        tod = (obj - to).lengthXY();
    }
    gi.Printf(
        "^~^~^ BOTGOAL e=%d why=%s kind=%s ok=%d to=(%.0f %.0f %.0f) d=%.0f od=%.0f tod=%.0f\n", controlledEntity->entnum,
        why, kind, ok ? 1 : 0, to.x, to.y, to.z, (to - org).length(), od, tod
    );
}

bool BotMovement::AtObjective() const
{
    if (!m_pPrimaryAttract || !controlledEntity) {
        return false;
    }
    const Vector org = controlledEntity->origin;
    if ((m_vAttractScatterGoal - org).lengthXYSquared() < Square(64) && fabs(m_vAttractScatterGoal.z - org.z) < 96.0f) {
        return true;
    }
    // [HZM bug-2888] the objective spot is just OFF the walkable mesh (a ledge, against a wall): the route ends at the
    // nearest walkable point. Arrived there, near the spot, and still standing there = at the objective.
    return m_iObjArriveT && (org - m_vObjArrivePos).lengthXYSquared() < Square(64)
        && (m_vAttractScatterGoal - m_vObjArrivePos).lengthXYSquared() < Square(256);
}

bool BotMovement::GetObjective(Vector& out) const
{
    if (!m_pPrimaryAttract) {
        return false;
    }
    out = m_pPrimaryAttract->origin;
    return true;
}

void BotMovement::Jump(usercmd_t& botcmd, const char *why)
{
    // [HZM bot probe2] every bot jump, with its reason (^~^~^ BOTJUMP, bot_probe on; same reason 700ms throttled)
    botcmd.upmove = 127;
    static cvar_t *s_probe = NULL;
    if (!s_probe) {
        s_probe = gi.Cvar_Get("bot_probe", "0", 0);
    }
    if (!s_probe->integer || !controlledEntity) {
        return;
    }
    if (m_szJumpLogWhy == why && level.inttime - m_iJumpLogT < 700 && level.inttime >= m_iJumpLogT) {
        return;
    }
    m_szJumpLogWhy = why;
    m_iJumpLogT    = level.inttime;
    gi.Printf(
        "^~^~^ BOTJUMP e=%d why=%s at=(%.0f %.0f %.0f) spd=%.0f blk=%d pathwhy=%s\n", controlledEntity->entnum, why,
        controlledEntity->origin.x, controlledEntity->origin.y, controlledEntity->origin.z,
        controlledEntity->velocity.lengthXY(), m_iNumBlocks, GetGoalWhy()
    );
}

/*
====================
GetCurrentGoal

Return the current goal, usually the nearest node the player should look at
====================
*/
Vector BotMovement::GetCurrentGoal() const
{
    if (!m_pPath->GetNodeCount()) {
        return m_vCurrentGoal;
    }

    if (!m_pPath->HasReachedGoal(controlledEntity->origin) && m_pPath->GetNodeCount()) {
        const Vector delta = m_pPath->GetCurrentDelta();
        return controlledEntity->origin + Vector(delta[0], delta[1], 0);
    }

    return controlledEntity->origin;
}

Vector BotMovement::GetCurrentPathDirection() const
{
    return m_pPath->GetCurrentDirection();
}
