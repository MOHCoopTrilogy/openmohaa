/*
===========================================================================
Copyright (C) 2025 the OpenMoHAA team

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

/**
 * @file navigation_recast_path.cpp
 * @brief Modern navigation system using Recast and Detour
 *
 * Detour is used for path-finding using a mesh that was automatically generated when parsing the BSP file.
 */

#include "navigation_recast_path.h"
#include "navigation_recast_load.h"
#include "navigation_recast_helpers.h"
#include "level.h"

#include "DetourPathCorridor.h"
#include "DetourCommon.h"
#include "navigation_recast_config_ext.h" // [HZM bug-2862] RECAST_AREA_ELEVATOR
#include "entity.h"
#include "bg_local.h"

// [HZM bot nav] kept at stock 256: raising to 512 was suspected in a ~80s dedicated-server hang under 8 bots
// (a fixed 256-sized buffer elsewhere could overflow). Reinstate only after that is proven safe.
#define MAX_NPOLYS 256

RecastPathMaster pathMaster;

// [HZM bot nav] The stock snap box was {(MAXS_X-MINS_X)/2, (MAXS_Z-MINS_Z)/2, (MAXS_Y-MINS_Y)/2} = ~{15,47,15}
// (Recast axis order: X, height, Y). A bot more than ~15u off the mesh horizontally - standing on a brush-entity
// floor/step/ledge that the world-only build never meshed - got NO nearest poly, so findNearestPoly failed and
// the path silently died: the dominant "no-path -> wander/idle" stuck mode in the Push bot study (esp. axis).
// Widen it generously so a bot re-acquires the mesh instead of giving up. Bot-nav only.
static const vec3_t DETOUR_EXTENT = {48.0f, 96.0f, 48.0f};

struct DetourData {
public:
    dtPathCorridor corridor;
    vec3_t         corners[4];
    unsigned char  cornerFlags[4];
    dtPolyRef      cornerPolys[4];
    int            ncorners;
};

RecastPather::RecastPather()
    : lastCheckTime(0)
    , moving(false)
    , traversingOffMeshLink(0)
    , traversingArea(0)
    , approachingArea(0)
    , elevatorPhase(0)
    , slideUntil(0)
    , steerMode(0)
    , gapRaw(vec_zero)
    , gapOut(vec_zero)
{
    detourData = new DetourData();
    detourData->corridor.init(MAX_NPOLYS); // [HZM bot nav] match the raised path cap
}

RecastPather::~RecastPather()
{
    if (detourData) {
        delete detourData;
        detourData = NULL;
    }
}

void RecastPather::FindPath(const Vector& start, const Vector& end, const PathSearchParameter& parameters)
{
    Vector               recastStart, recastEnd;
    dtPolyRef            startRef, endRef;
    vec3_t               startPt, endPt;
    const dtQueryFilter *filter = navigationMap.GetQueryFilter();

    lastorg = start;
    ResetPosition(start);

    ConvertGameToRecastCoord(start, recastStart);
    ConvertGameToRecastCoord(end, recastEnd);

    endRef   = 0;
    startRef = detourData->corridor.getFirstPoly();
    dtVcopy(startPt, detourData->corridor.getPos());
    navigationMap.GetNavMeshQuery()->findNearestPoly(recastEnd, DETOUR_EXTENT, filter, &endRef, endPt);

    if (!startRef || !endRef) {
        return;
    }

    dtPolyRef polys[MAX_NPOLYS];
    int       nPolys = 0;
    navigationMap.GetNavMeshQuery()->findPath(startRef, endRef, startPt, endPt, filter, polys, &nPolys, ARRAY_LEN(polys) - 1);

    if (nPolys) {
        vec3_t   closestPos;
        dtStatus status;

        if (polys[nPolys - 1] != endRef) {
            status = navigationMap.GetNavMeshQuery()->closestPointOnPoly(polys[nPolys - 1], endPt, closestPos, 0);
            if (dtStatusFailed(status)) {
                VectorCopy(endPt, closestPos);
            }
        } else {
            VectorCopy(endPt, closestPos);
        }

        moving = true;
        detourData->corridor.setCorridor(closestPos, polys, nPolys);
    }
}

void RecastPather::FindPathNear(
    const Vector& start, const Vector& end, float radius, const PathSearchParameter& parameters
)
{
    Vector               recastStart, recastEnd;
    dtPolyRef            startRef, endRef;
    vec3_t               startPt, endPt;
    const dtQueryFilter *filter = navigationMap.GetQueryFilter();

    lastorg = start;
    ResetPosition(start);

    ConvertGameToRecastCoord(start, recastStart);
    ConvertGameToRecastCoord(end, recastEnd);

    endRef   = 0;
    startRef = detourData->corridor.getFirstPoly();
    dtVcopy(startPt, detourData->corridor.getPos());
    navigationMap.GetNavMeshQuery()->findNearestPoly(recastEnd, DETOUR_EXTENT, filter, &endRef, endPt);

    if (!startRef || !endRef) {
        return;
    }

    // Now find a point within this radius
    if (navigationMap.GetNavMeshQuery()->findRandomPointAroundCircle(
            endRef, endPt, radius, filter, &G_Random, &endRef, endPt
        ) != DT_SUCCESS
        || !endRef) {
        return;
    }

    dtPolyRef polys[MAX_NPOLYS];
    int       nPolys = 0;
    navigationMap.GetNavMeshQuery()->findPath(startRef, endRef, startPt, endPt, filter, polys, &nPolys, ARRAY_LEN(polys) - 1);

    if (nPolys) {
        vec3_t   closestPos;
        dtStatus status;

        if (polys[nPolys - 1] != endRef) {
            status = navigationMap.GetNavMeshQuery()->closestPointOnPoly(polys[nPolys - 1], endPt, closestPos, 0);
            if (dtStatusFailed(status)) {
                VectorCopy(endPt, closestPos);
            }
        } else {
            VectorCopy(endPt, closestPos);
        }

        moving = true;
        detourData->corridor.setCorridor(closestPos, polys, nPolys);
    }
}

void RecastPather::FindPathAway(
    const Vector&              start,
    const Vector&              avoid,
    const Vector&              preferredDir,
    float                      radius,
    const PathSearchParameter& parameters
)
{
    Vector               recastStart, recastAvoid, recastEnd;
    Vector               dirNormalized;
    dtPolyRef            startRef, endRef;
    vec3_t               startPt, endPt;
    const dtQueryFilter *filter = navigationMap.GetQueryFilter();
    float                startAngle;
    float                startPitch;
    int                  i, j;

    lastorg = start;
    ResetPosition(start);

    startAngle = DEG2RAD(preferredDir.toYaw());
    startPitch = DEG2RAD(preferredDir.toPitch());

    dirNormalized = preferredDir;
    dirNormalized.normalize();

    ConvertGameToRecastCoord(start, recastStart);
    ConvertGameToRecastCoord(avoid, recastAvoid);
    ConvertGameToRecastCoord(avoid + dirNormalized * radius, recastEnd);

    startRef = detourData->corridor.getFirstPoly();
    dtVcopy(startPt, detourData->corridor.getPos());

    if (!startRef) {
        return;
    }

    for (i = 0; i < 36; i++) {
        float angle = startAngle + ((2 * M_PI) * (float)i / (float)36);
        float dx    = cos(angle) * radius;
        float dz    = sin(angle) * radius;

        for (j = 0; j < 4; j++) {
            float  pitch = startPitch + (M_PI * (float)j / 4);
            float  dy    = sin(pitch) * radius;
            Vector point(recastAvoid[0] + dx, recastAvoid[1] + dy, recastAvoid[2] + dz);

            if (navigationMap.GetNavMeshQuery()->findNearestPoly(point, DETOUR_EXTENT, filter, &endRef, endPt)
                    == DT_SUCCESS
                && endRef) {
                dtPolyRef polys[MAX_NPOLYS];
                int       nPolys = 0;
                navigationMap.GetNavMeshQuery()->findPath(
                    startRef, endRef, startPt, endPt, filter, polys, &nPolys, ARRAY_LEN(polys) - 1
                );

                if (nPolys) {
                    vec3_t   closestPos;
                    dtStatus status;

                    if (polys[nPolys - 1] != endRef) {
                        status = navigationMap.GetNavMeshQuery()->closestPointOnPoly(
                            polys[nPolys - 1], endPt, closestPos, 0
                        );
                        if (dtStatusFailed(status)) {
                            VectorCopy(endPt, closestPos);
                        }
                    } else {
                        VectorCopy(endPt, closestPos);
                    }

                    moving = true;
                    detourData->corridor.setCorridor(closestPos, polys, nPolys);
                }

                return;
            }
        }
    }
}

bool RecastPather::TestPath(const Vector& start, const Vector& end, const PathSearchParameter& parameters)
{
    Vector               recastStart, recastEnd;
    const dtQueryFilter *filter = navigationMap.GetQueryFilter();
    dtStatus             status;

    ConvertGameToRecastCoord(start, recastStart);
    ConvertGameToRecastCoord(end, recastEnd);

    dtPolyRef nearestStartRef, nearestEndRef;
    vec3_t    nearestStartPt, nearestEndPt;
    navigationMap.GetNavMeshQuery()->findNearestPoly(
        recastStart, DETOUR_EXTENT, filter, &nearestStartRef, nearestStartPt
    );
    navigationMap.GetNavMeshQuery()->findNearestPoly(recastEnd, DETOUR_EXTENT, filter, &nearestEndRef, nearestEndPt);

    dtPolyRef polys[MAX_NPOLYS];
    int       nPolys;
    status = navigationMap.GetNavMeshQuery()->findPath(
        nearestStartRef, nearestEndRef, nearestStartPt, nearestEndPt, filter, polys, &nPolys, 256
    );

    if (!(status & DT_SUCCESS)) {
        // Invalid path
        return false;
    }

    return true;
}

static bool overOffmeshConnection(
    const vec3_t pos, const unsigned char *cornerFlags, const vec3_t cornerVerts, const float radius, const int ncorners
)
{
    if (!ncorners) {
        return false;
    }

    const bool offMeshConnection = (cornerFlags[ncorners - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) ? true : false;
    if (offMeshConnection) {
        const float distSq = dtVdist2DSqr(&cornerVerts[(ncorners - 1) * 3], pos);
        if (distSq < radius * radius) {
            return true;
        }
    }

    return false;
}

void RecastPather::UpdatePos(const Vector& origin)
{
    const dtQueryFilter *filter = navigationMap.GetQueryFilter();
    const Vector velocity = (origin - lastorg) * (1.0 / level.frametime);
    const float distSqr = Q_min(Square(64), velocity.lengthSquared());

    lastorg = origin;
    Vector recastOrigin;

    ConvertGameToRecastCoord(origin, recastOrigin);

    if (traversingOffMeshLink && traversingArea == RECAST_AREA_ELEVATOR && elevatorPhase == 1) {
        // [bug-2862] boarding: walk to the cab centre; once there (in the cab), aim for the far landing and let
        // the lift carry us - the far-landing XY is straight through the opposite door
        Vector d = Vector(elevatorMid.x, elevatorMid.y, origin.z) - origin;
        // [HZM bug-2912] INSIDE THE CAB, not at its dead centre: only one bot fits within 24u of the centre, so with
        // two or more riders the rest stayed in phase 1 and kept steering to the centre when the cab reached the far
        // end - they never headed out and rode back (user: "they kinda walk in the wrong direction" at the top).
        // 40u each way = inside the 128u cab clear of its 12u corner posts.
        if (fabs(d.x) < 40.0f && fabs(d.y) < 40.0f) {
            elevatorPhase  = 2;
            currentNodePos = elevatorEnd;
        }
    }

    if (traversingOffMeshLink) {
        Vector agentPos;
        Vector delta;

        ConvertRecastToGameCoord(detourData->corridor.getPos(), agentPos);
        delta = agentPos - origin;
        if (delta.lengthSquared() < Square(24) + distSqr) {
            // traversed
            traversingOffMeshLink = false;
            traversingArea        = 0;
        }
    } else if (level.inttime >= lastCheckTime + 2000) {
        vec3_t delta;
        VectorSubtract(recastOrigin, detourData->corridor.getPos(), delta);

        if (VectorLengthSquared(delta) > Square(64)) {
            dtPolyRef startRef, endRef;
            vec3_t    startPt, endPt;

            //
            // Get the target position
            //
            endRef = detourData->corridor.getLastPoly();
            VectorCopy(detourData->corridor.getTarget(), endPt);

            ResetPosition(origin);

            startRef = 0;
            navigationMap.GetNavMeshQuery()->findNearestPoly(recastOrigin, DETOUR_EXTENT, filter, &startRef, startPt);

            if (startRef) {
                dtPolyRef polys[MAX_NPOLYS];
                int       nPolys = 0;
                navigationMap.GetNavMeshQuery()->findPath(
                    startRef, endRef, startPt, endPt, filter, polys, &nPolys, ARRAY_LEN(polys) - 1
                );

                if (nPolys) {
                    moving = true;
                    detourData->corridor.setCorridor(endPt, polys, nPolys);
                }
            }
        }

        lastCheckTime = level.inttime;
    } else {
        detourData->corridor.movePosition(recastOrigin, navigationMap.GetNavMeshQuery(), filter);
        ConvertRecastToGameCoord(detourData->corridor.getPos(), lastValidOrg);

        detourData->ncorners = detourData->corridor.findCorners(
            (float *)detourData->corners,
            detourData->cornerFlags,
            detourData->cornerPolys,
            4,
            navigationMap.GetNavMeshQuery(),
            filter
        );
        approachingArea = 0;
        if (detourData->ncorners) {
            dtPolyRef refs[2];
            vec3_t    startOffPos, endOffPos;
            float     triggerRadius = 16;

            // [HZM bug-2862] which kind of link is the path about to take? An ELEVATOR link starts at a landing where
            // the bot may be jostled/strafed while it waits; 16u (2D) was never hit - widen it to 48u, and let
            // BotMovement hold its stuck recovery off while approaching (GetApproachingArea).
            if (detourData->cornerFlags[detourData->ncorners - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) {
                const dtMeshTile *atile = NULL;
                const dtPoly     *apoly = NULL;
                if (navigationMap.GetNavMesh()->getTileAndPolyByRef(
                        detourData->cornerPolys[detourData->ncorners - 1], &atile, &apoly
                    )
                    == DT_SUCCESS) {
                    const float d2 =
                        dtVdist2DSqr(detourData->corners[detourData->ncorners - 1], (const float *)recastOrigin);
                    if (d2 < Square(128)) {
                        approachingArea = apoly->getArea();
                    }
                    if (apoly->getArea() == RECAST_AREA_ELEVATOR) {
                        // the whole landing: bots queueing for the lift stalled 50-57u out (soak ELEVDBG)
                        triggerRadius = 96;
                    } else if (apoly->getArea() == RECAST_AREA_LADDER) {
                        // [HZM bug-2871] the link start is 32u out from the ladder face; at 16u a bot bumped the rungs
                        // before the climb began and the stock stuck logic jump-spammed at the foot
                        triggerRadius = 32;
                    }
                }
            }

            if (overOffmeshConnection(
                    recastOrigin, detourData->cornerFlags, (const vec_t *)detourData->corners, triggerRadius, detourData->ncorners
                )
                && detourData->corridor.moveOverOffmeshConnection(
                    detourData->cornerPolys[detourData->ncorners - 1],
                    refs,
                    startOffPos,
                    endOffPos,
                    navigationMap.GetNavMeshQuery()
                )) {
                ConvertRecastToGameCoord(detourData->corridor.getPos(), currentNodePos);

                traversingOffMeshLink = true;
                // [HZM bug-2862] remember WHICH kind of link (the elevator ride needs different bot handling)
                {
                    const dtMeshTile *otile = NULL;
                    const dtPoly     *opoly = NULL;
                    traversingArea          = 0;
                    if (navigationMap.GetNavMesh()->getTileAndPolyByRef(
                            detourData->cornerPolys[detourData->ncorners - 1], &otile, &opoly
                        )
                        == DT_SUCCESS) {
                        traversingArea = opoly->getArea();
                    }
                    // [HZM bug-2913] the ends of every link crossed (the failed-jump learner needs them)
                    ConvertRecastToGameCoord(startOffPos, linkStart);
                    ConvertRecastToGameCoord(endOffPos, linkEnd);
                    if (traversingArea == RECAST_AREA_ELEVATOR) {
                        // [bug-2862] steer to the CAB CENTRE (link midpoint) first, then out to the far landing -
                        // heading straight for the far landing pushed off-centre bots into the cab's door frame
                        Vector s, e;
                        ConvertRecastToGameCoord(startOffPos, s);
                        ConvertRecastToGameCoord(endOffPos, e);
                        elevatorMid   = (s + e) * 0.5f;
                        elevatorEnd   = e;
                        elevatorStart = s; // [HZM bug-2912]
                        elevatorPhase = 1;
                        currentNodePos = Vector(elevatorMid.x, elevatorMid.y, s.z);
                    }
                }
            } else {
                ConvertRecastToGameCoord(detourData->corners[0], currentNodePos);
                // [HZM bug-2914] CORNER ROUNDING. The mesh is eroded by agentRadius 8 (bug-2833: more closed real doors)
                // but a player is 15u to its edge, so a route turning round a desk / crate / door post put the corner
                // 7u too tight and the bot's box caught it (user 2026-09-25: "they get caught on dumb objects like
                // desks still when they could just move around"). At a real TURN, steer to a point pushed OUT from
                // the inside of the turn by the missing clearance - only if a player box reaches it unobstructed from
                // here, else the plain corner as before. bot_cornerPad 0 = off.
                static cvar_t *s_cornerPad = NULL;
                if (!s_cornerPad) {
                    s_cornerPad = gi.Cvar_Get("bot_cornerPad", "8", 0);
                }
                static cvar_t *s_cornerFix = NULL;
                if (!s_cornerFix) {
                    s_cornerFix = gi.Cvar_Get("bot_cornerFix", "0", 0);
                }
                if (s_cornerFix->integer && s_cornerPad->value > 0.0f
                    && !(detourData->cornerFlags[0] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)) {
                    // [HZM bug-2956] the pad above never ran: MASK_SOLID INCLUDES CONTENTS_BODY, and a trace with no
                    // entity to skip starts inside the bot's own box, so every pad trace was startsolid (e1l1: a bot
                    // pushed for 4 minutes into the overhang of a cliff face beside a corner). Now: the world + clips
                    // only (bodies are the bot-avoidance layer's business); the pad worked out for a corner is KEPT as
                    // the bot closes in on that same corner (it switched off inside 24u, exactly where the box meets
                    // the inside of the turn); the full AABB clearance first (16u: a 15u box needs 13u more than the
                    // 8u erosion diagonally at a square corner), then the old 8u; and a bot already pinned on the way to
                    // its corner slides along what pins it, toward the next leg, for up to 0.5s.
                    // bot_cornerFix 1 = on. DEFAULT OFF (2026-09-25): three e1l1 A/B pairs showed no gain (overall
                    // stuck 3.35% vs 3.47%, the cliff spot 60% vs 65%) - that spot is not a corner problem: its next leg
                    // runs under a head-height rock overhang (nav_fitprobe: crouch-only) on 45-60 deg ground.
                    const int    padMask = MASK_PLAYERSOLID & ~CONTENTS_BODY;
                    // the body from a step up to its real top: the old box also stopped 18u short of the head, so it
                    // passed under the cliff overhang that the bot's own box (BOTBLOCK 'ahead World at 0') was pinned on
                    const Vector mins(MINS_X, MINS_Y, STEPSIZE);
                    const Vector maxs(MAXS_X, MAXS_Y, MAXS_Z);
                    Vector       c1       = currentNodePos;
                    const bool   bHaveNext = detourData->ncorners >= 2;
                    if (bHaveNext) {
                        ConvertRecastToGameCoord(detourData->corners[1], c1);
                    }
                    Vector din  = currentNodePos - origin;
                    Vector dout = c1 - currentNodePos;
                    din.z       = 0;
                    dout.z      = 0;
                    const float lin     = din.length();
                    const float lout    = dout.length();
                    Vector      inward  = vec_zero;
                    bool        bInward = false;
                    // [bug-3146] bot_cornerFix 2: pad AND slide only at a real corner - the next leg 24u+. Mode 1 (A/B:
                    // m2l1 -55%, its doorways *21/*27 81 vs 372 samples) swung bots 24u sideways and back on t1l3's
                    // zig-zag of 16-24u corners at (-1180,-2064) (+56% there); pad-only (tried) lost the m2l1 gain.
                    const bool  bMicroGuard = s_cornerFix->integer >= 2;
                    const float minOut      = bMicroGuard ? 24.0f : 8.0f;
                    if (bHaveNext && lin > 24.0f && lout > minOut) {
                        const Vector di = din * (1.0f / lin);
                        const Vector dd = dout * (1.0f / lout);
                        if (di * dd < 0.94f) { // a turn of ~20 degrees or more
                            inward   = dd - di;
                            inward.z = 0;
                            if (inward.lengthSquared() > 0.0001f) {
                                inward.normalize();
                                bInward   = true;
                                padCorner = currentNodePos;
                                padInward = inward;
                            }
                        }
                    } else if (bHaveNext && lout > minOut && (padCorner - currentNodePos).lengthSquared() < 1.0f
                               && padInward.lengthSquared() > 0.5f) {
                        inward  = padInward; // closing in on the corner the pad was worked out for: keep it
                        bInward = true;
                    }
                    const Vector corner  = currentNodePos;
                    bool         bPadded = false;
                    steerMode            = 0;
                    if (bInward) {
                        const float pads[2] = {Q_max(16.0f, s_cornerPad->value), s_cornerPad->value};
                        for (int pi = 0; pi < 2 && !bPadded; pi++) {
                            const Vector cand = corner - inward * pads[pi];
                            trace_t tr = G_Trace(origin, mins, maxs, cand, NULL, padMask, qtrue, "BotCornerPad");
                            if (!tr.startsolid && !tr.allsolid && tr.fraction >= 1.0f) {
                                currentNodePos = cand;
                                bPadded        = true;
                                steerMode      = (pi == 0 ? 1 : 2) + ((lin > 24.0f) ? 0 : 8);
                            }
                        }
                    }
                    if (!bPadded && (!bMicroGuard || !bHaveNext || lout > minOut)) {
                        if (level.inttime < slideUntil && (slideTarget - origin).lengthXYSquared() > Square(6.0f)) {
                            currentNodePos = slideTarget; // committed slide still under way
                            steerMode      = 3;
                        } else if (lin > 1.0f && lin < 48.0f) {
                            // (only the wedge at the INSIDE of a turn, next to its corner: pinned half-way along a leg is
                            // an obstacle the mesh ignores, for the stuck recovery - a slide there only swayed the bot)
                            trace_t tr = G_Trace(origin, mins, maxs, corner, NULL, padMask, qtrue, "BotCornerReach");
                            if (!tr.startsolid && !tr.allsolid && tr.fraction < 1.0f && tr.fraction * lin < 4.0f) {
                                // pinned: slide along the blocking face, the way that leads on to the next leg
                                Vector n = tr.plane.normal;
                                n.z      = 0;
                                if (n.lengthSquared() > 0.01f) {
                                    n.normalize();
                                    Vector t(-n.y, n.x, 0.0f);
                                    Vector want = (bHaveNext ? c1 : corner) - origin;
                                    want.z      = 0;
                                    if (t * want < 0.0f) {
                                        t = t * -1.0f;
                                    }
                                    const float lens[2] = {24.0f, 12.0f};
                                    for (int si = 0; si < 2; si++) {
                                        const Vector cand = origin + t * lens[si];
                                        trace_t      ts   = G_Trace(origin, mins, maxs, cand, NULL, padMask, qtrue, "BotCornerSlide");
                                        if (!ts.startsolid && !ts.allsolid && ts.fraction >= 1.0f) {
                                            slideTarget    = cand;
                                            slideUntil     = level.inttime + 500;
                                            currentNodePos = cand;
                                            steerMode      = 3;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (s_cornerPad->value > 0.0f && detourData->ncorners >= 2
                    && !(detourData->cornerFlags[0] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)) {
                    Vector c1;
                    ConvertRecastToGameCoord(detourData->corners[1], c1);
                    Vector din  = currentNodePos - origin;
                    Vector dout = c1 - currentNodePos;
                    din.z       = 0;
                    dout.z      = 0;
                    const float lin  = din.length();
                    const float lout = dout.length();
                    if (lin > 24.0f && lout > 8.0f) {
                        din  = din * (1.0f / lin);
                        dout = dout * (1.0f / lout);
                        if (din * dout < 0.94f) { // a turn of ~20 degrees or more
                            Vector inward = dout - din;
                            inward.z      = 0;
                            if (inward.lengthSquared() > 0.0001f) {
                                inward.normalize();
                                const Vector cand = currentNodePos - inward * s_cornerPad->value;
                                // MASK_SOLID, not PLAYERSOLID: the pather has no entity to skip, and its own
                                // bot's box (CONTENTS_BODY) would make every trace start solid
                                const Vector mins(MINS_X, MINS_Y, STEPSIZE);
                                const Vector maxs(MAXS_X, MAXS_Y, MAXS_Z - STEPSIZE);
                                trace_t      tr = G_Trace(
                                    origin, mins, maxs, cand, NULL, MASK_SOLID, qfalse, "BotCornerPad"
                                );
                                if (!tr.startsolid && !tr.allsolid && tr.fraction >= 1.0f) {
                                    currentNodePos = cand;
                                }
                            }
                        }
                    }
                }
                // [bug-3146] GAP CENTRING (bot_gapCentre): a corner inside a narrow gap - a doorway - goes to the gap's
                // middle. Probed across the NEXT leg (the way through), at body height, from the raw corner (or from
                // the nearest free spot to it: in a doorway the raw corner itself is often inside the jamb).
                static cvar_t *s_gapCentre = NULL;
                if (!s_gapCentre) {
                    s_gapCentre = gi.Cvar_Get("bot_gapCentre", "1", 0); // default 1 after the A/B (m2l1 -40%, e3l2 -43%)
                }
                if (s_gapCentre->integer && !(detourData->cornerFlags[0] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)) {
                    Vector raw;
                    ConvertRecastToGameCoord(detourData->corners[0], raw);
                    if ((raw - gapRaw).lengthSquared() > 1.0f) {
                        gapRaw = raw;
                        gapOut = raw;
                        Vector dir;
                        if (detourData->ncorners >= 2) {
                            Vector c1;
                            ConvertRecastToGameCoord(detourData->corners[1], c1);
                            dir = c1 - raw;
                        } else {
                            dir = raw - origin;
                        }
                        dir.z = 0;
                        if (dir.lengthSquared() < Square(4.0f)) {
                            dir   = raw - origin;
                            dir.z = 0;
                        }
                        if (dir.lengthSquared() > 1.0f) {
                            dir.normalize();
                            const Vector side(-dir.y, dir.x, 0.0f);
                            const int    gapMask = MASK_PLAYERSOLID & ~CONTENTS_BODY;
                            const Vector gmins(MINS_X, MINS_Y, STEPSIZE);
                            const Vector gmaxs(MAXS_X, MAXS_Y, MAXS_Z);
                            // a free spot to measure from: the corner, else the nearest of +-4..16u across the gap
                            Vector       st    = raw;
                            bool         bFree = false;
                            for (int k = 0; k <= 16 && !bFree; k += 4) {
                                for (int sgn = 1; sgn >= -1 && !bFree; sgn -= 2) {
                                    const Vector p = raw + side * (float)(k * sgn);
                                    trace_t      t = G_Trace(p, gmins, gmaxs, p, NULL, gapMask, qtrue, "BotGapFree");
                                    if (!t.startsolid && !t.allsolid) {
                                        st    = p;
                                        bFree = true;
                                    }
                                    if (!k) {
                                        break;
                                    }
                                }
                            }
                            if (bFree) {
                                trace_t tl = G_Trace(st, gmins, gmaxs, st - side * 48.0f, NULL, gapMask, qtrue, "BotGapL");
                                trace_t tr = G_Trace(st, gmins, gmaxs, st + side * 48.0f, NULL, gapMask, qtrue, "BotGapR");
                                const float l = tl.fraction * 48.0f;
                                const float r = tr.fraction * 48.0f;
                                if (tl.fraction < 1.0f && tr.fraction < 1.0f && l + r < 48.0f) {
                                    gapOut = st + side * ((r - l) * 0.5f);
                                }
                            }
                        }
                    }
                    if ((gapOut - gapRaw).lengthSquared() > 1.0f) {
                        currentNodePos = gapOut;
                        steerMode      = 16;
                    } else if (steerMode == 16) {
                        steerMode = 0; // (probe only: the cornerFix-0 path never resets it)
                    }
                }
            }
        } else {
            ConvertRecastToGameCoord(detourData->corridor.getPos(), currentNodePos);
        }
    }
}

void RecastPather::Clear()
{
    ResetPosition(lastorg);
}

PathNav RecastPather::GetNode(unsigned int index) const
{
    PathNav               nav;
    Vector                target;
    Vector                delta;
    Vector                agentPos;
    const dtPathCorridor *inCorridor;
    const dtPolyRef      *path;
    int                   npath;

    inCorridor = &detourData->corridor;

    path  = inCorridor->getPath();
    npath = inCorridor->getPathCount();
    if (npath <= 0) {
        return {};
    }

    if (index + 1 == npath) {
        // Just return the target pos
        ConvertRecastToGameCoord(inCorridor->getTarget(), nav.origin);
        //ConvertRecastToGameCoord(agent->npos, agentPos);
        agentPos = lastorg;

        nav.dir[0] = nav.origin[0] - agentPos[0];
        nav.dir[1] = nav.origin[1] - agentPos[1];
        nav.dist   = VectorLength2D(nav.dir);
        VectorNormalize2D(nav.dir);

        return nav;
    }

    const dtMeshTile *tile;
    const dtPoly     *poly;
    if (navigationMap.GetNavMesh()->getTileAndPolyByRef(path[index], &tile, &poly) != DT_SUCCESS) {
        return {};
    }

    const unsigned int tileId = (unsigned int)(poly - tile->polys);

    if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
        dtOffMeshConnection *con = &tile->offMeshCons[tileId - tile->header->offMeshBase];
        Vector               start, end;

        ConvertRecastToGameCoord(&con->pos[0], start);
        ConvertRecastToGameCoord(&con->pos[3], end);

        nav.origin = start;
        nav.dir[0] = end[0] - start[0];
        nav.dir[1] = end[1] - start[1];
        VectorNormalize2D(nav.dir);
        nav.dist = delta.length();
    } else {
        const dtPolyDetail *dm = &tile->detailMeshes[tileId];
        Vector              middle[1];

        for (int i = 0; i < 1; i++) {
            for (int j = 0; j < dm->triCount; ++j) {
                const unsigned char *t = &tile->detailTris[(dm->triBase + j) * 4];
                for (int k = 0; k < 3; ++k) {
                    if (t[k] < poly->vertCount) {
                        middle[i] += &tile->verts[poly->verts[t[k]] * 3];
                    } else {
                        middle[i] += &tile->detailVerts[(dm->vertBase + t[k] - poly->vertCount) * 3];
                    }
                }
            }

            middle[i] /= dm->triCount * 3;
        }

        ConvertRecastToGameCoord(middle[0], nav.origin);
        ConvertRecastToGameCoord(middle[1], target);

        delta      = target - nav.origin;
        nav.dir[0] = delta[0];
        nav.dir[1] = delta[1];
        VectorNormalize2D(nav.dir);
        nav.dist = delta.length();
    }

    return nav;
}

int RecastPather::GetNodeCount() const
{
    if (!moving) {
        return 0;
    }

    return detourData->corridor.getPathCount();
}

bool RecastPather::GetCornerAfterNext(Vector& out) const
{
    // [HZM bot A3] corners[0] is being steered to (currentNodePos); corners[1] is the turn after it
    if (!moving || traversingOffMeshLink || detourData->ncorners < 2) {
        return false;
    }
    ConvertRecastToGameCoord(detourData->corners[1], out);
    return true;
}

int RecastPather::GetCorners(Vector *out, int maxCorners) const
{
    // [HZM bot breach] the straight-path corners ahead (findCorners stops at an off-mesh link start)
    if (!moving || traversingOffMeshLink) {
        return 0;
    }
    const int n = Q_min(detourData->ncorners, maxCorners);
    for (int i = 0; i < n; i++) {
        ConvertRecastToGameCoord(detourData->corners[i], out[i]);
    }
    return n;
}

bool RecastPather::GetElevatorEnd(Vector& out) const
{
    if (!moving || !traversingOffMeshLink || traversingArea != RECAST_AREA_ELEVATOR) {
        return false;
    }
    out = elevatorEnd;
    return true;
}

bool RecastPather::GetTraversingLink(Vector& start, Vector& end) const
{
    if (!moving || !traversingOffMeshLink) {
        return false;
    }
    start = linkStart;
    end   = linkEnd;
    return true;
}

bool RecastPather::GetElevatorLink(Vector& start, Vector& end) const
{
    // [HZM bug-2912] both ends of the elevator link being ridden, oriented by travel direction
    if (!moving || !traversingOffMeshLink || traversingArea != RECAST_AREA_ELEVATOR) {
        return false;
    }
    start = elevatorStart;
    end   = elevatorEnd;
    return true;
}

bool RecastPather::GetApproachingLink(Vector& from, Vector& to) const
{
    // [HZM bot breach] the off-mesh link the path is about to take: its connection's two ends, ordered by which one the
    // path reaches first (links are bidirectional, so pos[0..2] is not necessarily our side)
    if (!moving || traversingOffMeshLink || !approachingArea || !detourData->ncorners) {
        return false;
    }
    const int last = detourData->ncorners - 1;
    if (!(detourData->cornerFlags[last] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)) {
        return false;
    }
    const dtOffMeshConnection *con = navigationMap.GetNavMesh()->getOffMeshConnectionByRef(detourData->cornerPolys[last]);
    if (!con) {
        return false;
    }
    Vector p0, p1;
    ConvertRecastToGameCoord(&con->pos[0], p0);
    ConvertRecastToGameCoord(&con->pos[3], p1);
    Vector c;
    ConvertRecastToGameCoord(detourData->corners[last], c);
    if ((p0 - c).lengthSquared() <= (p1 - c).lengthSquared()) {
        from = p0;
        to   = p1;
    } else {
        from = p1;
        to   = p0;
    }
    return true;
}

Vector RecastPather::GetCurrentDelta() const
{
    Vector delta;

    delta = currentNodePos - lastorg;

    return delta;
}

Vector RecastPather::GetCurrentDirection() const
{
    Vector delta;

    delta = currentNodePos - lastorg;
    delta.normalize();

    return delta;
}

Vector RecastPather::GetDestination() const
{
    Vector dest;

    ConvertRecastToGameCoord(detourData->corridor.getTarget(), dest);

    return dest;
}

bool RecastPather::HasReachedGoal(const Vector& origin) const
{
    Vector target;

    const dtPolyRef lastPoly = detourData->corridor.getLastPoly();
    ConvertRecastToGameCoord(detourData->corridor.getTarget(), target);

    if (fabs(origin[0] - target[0]) < 16.0f && fabs(origin[1] - target[1]) < 16.0f) {
        return true;
    }

    return false;
}

bool RecastPather::IsQuerying() const
{
    return false;
}

unsigned char RecastPather::GetTraversingArea() const
{
    return traversingOffMeshLink ? traversingArea : 0;
}

unsigned char RecastPather::GetApproachingArea() const
{
    return traversingOffMeshLink ? 0 : approachingArea;
}

void RecastPather::ResetPosition(const Vector& origin)
{
    const dtQueryFilter *filter = navigationMap.GetQueryFilter();
    vec3_t               agentPos;

    traversingOffMeshLink = false;
    traversingArea        = 0;
    lastCheckTime         = level.inttime;

    moving = false;

    ConvertGameToRecastCoord(origin, agentPos);

    // Find nearest position on navmesh and place the agent there.
    dtPolyRef ref = 0;
    float     nearest[3];
    dtStatus  status;

    status = navigationMap.GetNavMeshQuery()->findNearestPoly(agentPos, DETOUR_EXTENT, filter, &ref, nearest);

    if (dtStatusFailed(status) || !ref) {
        // Use the last valid position instead
        ConvertGameToRecastCoord(lastValidOrg, agentPos);
        status = navigationMap.GetNavMeshQuery()->findNearestPoly(agentPos, DETOUR_EXTENT, filter, &ref, nearest);
    }

    if (dtStatusSucceed(status) && ref) {
        detourData->corridor.reset(ref, nearest);
        ConvertRecastToGameCoord(agentPos, lastValidOrg);
    } else {
        detourData->corridor.reset(0, agentPos);
    }
}

void RecastPathMaster::PostLoadNavigation(const NavigationMap& map) {}

void RecastPathMaster::ClearNavigation() {}

void RecastPathMaster::Update() {}
