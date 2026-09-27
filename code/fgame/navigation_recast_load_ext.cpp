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
 * @file navigation_recast_load_ext.cpp
 * @brief Extension for recast-based navigation
 * 
 */

#include "g_local.h"
#include "navigation_recast_load_ext.h"
#include "navigation_recast_config.h"
#include "navigation_recast_config_ext.h"
#include "navigation_recast_helpers.h"
#include "Recast.h"
#include "entity.h"
#include "misc.h"
#include "level.h"
#include "Entities.h"               // [HZM bug-2893] FencePost
#include "VehicleCollisionEntity.h" // [HZM bug-2893]
#include "barrels.h"                // [HZM bug-2893]
#include "crateobject.h"            // [HZM bug-2893]
#include "scriptslave.h"            // [HZM bug-2896] ScriptModel
#include "playerstart.h"            // [HZM bug-2896] PlayerStart
#include "trigger.h"                // [HZM bug-2926] TriggerHurt
#include "navigation_recast_load.h"   // [HZM bug-2898] navigationMap
#include "DetourNavMesh.h"            // [HZM bug-2898]
#include "DetourNavMeshQuery.h"       // [HZM bug-2898]

CLASS_DECLARATION(Class, INavigationMapExtension, NULL) {
    {NULL, NULL}
};

CLASS_DECLARATION(INavigationMapExtension, NavigationMapExtension_Ladders, NULL) {
    {NULL, NULL}
};

void NavigationMapExtension_Ladders::Handle(Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh)
{
    const Vector mins(MINS_X, MINS_Y, 0);
    const Vector maxs(MAXS_X, MAXS_Y, NavigationMapConfiguration::agentHeight);
    gentity_t   *edict;
    Vector       start, end;
    Vector       tstart, tend;
    Vector       top;
    trace_t      trace;

    // [HZM bug-2871] Ladder links are ON again now that the bots climb them (BotMovement ladder handling +
    // BotController::GetLadderSteer pitch). They were briefly off while the bots could not: m3l1b (Omaha Bluffs)
    // allies jump-spammed at the foot of bunker ladder *21 (link (1877,-1496,89) -> (1811,-1496,257)), and that
    // ladder is the ONLY navmesh route up the bluff - with links off the allied home could not reach the axis home.
    // bot_navLadders 0 drops every ladder link (diagnostic).
    {
        static cvar_t *s_navLadders = NULL;
        if (!s_navLadders) {
            s_navLadders = gi.Cvar_Get("bot_navLadders", "1", 0);
        }
        if (!s_navLadders->integer) {
            return;
        }
    }

    for (edict = active_edicts.next; edict != &active_edicts; edict = edict->next) {
        if (!edict->entity) {
            continue;
        }

        if (edict->entity->isSubclassOf(FuncLadder)) {
            const FuncLadder *ladder    = static_cast<FuncLadder *>(edict->entity);
            const Vector&     facingDir = ladder->getFacingDir();

            offMeshNavigationPoint point;

            start = edict->entity->origin + Vector(0, 0, edict->entity->mins.z) - facingDir * 32;
            end   = edict->entity->origin + Vector(0, 0, edict->entity->maxs.z);

            // Trace to this:
            //
            // start
            // ↓  ┌┐
            // ●──│──● ← end
            //    │
            //    │
            tstart = end - facingDir * 32 - Vector(0, 0, STEPSIZE);
            tend   = end + facingDir * 64 - Vector(0, 0, STEPSIZE);
            trace  = G_Trace(tstart, mins, maxs, tend, NULL, MASK_PLAYERSOLID, qfalse, "ConnectLadders");

            // Trace to this:
            //
            //         end
            //         ↓
            //         ●
            //         │ ┌┐
            // start → ● │
            //           │
            //           │
            tstart = trace.endpos;
            tend   = trace.endpos + Vector(0, 0, STEPSIZE * 4);
            trace  = G_Trace(tstart, mins, maxs, tend, NULL, MASK_PLAYERSOLID, qfalse, "ConnectLadders");

            // Trace to this:
            //
            //  start
            //  ↓
            //  ●───● ← end
            //   ┌┐ 
            //   │
            //   │
            //   │
            // Connect to the end position
            top    = trace.endpos;
            tstart = top;
            tend   = top + facingDir * 48;
            trace  = G_Trace(tstart, mins, maxs, tend, NULL, MASK_PLAYERSOLID, qfalse, "ConnectLadders");

            // Trace to this:
            //
            //      start
            //      ↓
            //      ●
            //   ┌┐ │
            //   │  ● ← end
            //   │
            //   │
            // Connect to the end position
            tstart = trace.endpos;
            tend   = trace.endpos - Vector(0, 0, STEPSIZE * 6);
            trace  = G_Trace(tstart, mins, maxs, tend, NULL, MASK_PLAYERSOLID, qfalse, "ConnectLadders");

            // Trace to this:
            //
            //    ┌┐
            //    │ ●──● ← start
            //    │ ↑
            //    │ └ end
            //
            tstart = trace.endpos;
            tend   = top - facingDir * 32;
            tend.z = tstart.z - STEPSIZE;
            trace  = G_Trace(tstart, mins, maxs, tend, NULL, MASK_PLAYERSOLID, qfalse, "ConnectLadders");

            point.start         = start;
            point.end           = trace.endpos;
            point.bidirectional = true;
            point.radius        = NavigationMapConfiguration::agentRadius;
            point.area          = RECAST_AREA_LADDER;
            point.flags         = RECAST_POLYFLAG_WALKABLE;

            points.AddObject(point);
        }
    }
}

Container<ExtensionArea> NavigationMapExtension_Ladders::GetSupportedAreas() const
{
    Container<ExtensionArea> list;

    // Ladders are good alternative but they shouldn't be used all the time
    list.AddObject(ExtensionArea(RECAST_AREA_LADDER, 10.0));

    return list;
}

CLASS_DECLARATION(INavigationMapExtension, NavigationMapExtension_JumpFall, NULL) {
    {NULL, NULL}
};

// [HZM bug-2896] where a link end stands must be ground a player can stand on. CanConnectJumpPoint slides the take-off
// toward the obstacle and drops it, and on a SLOPED wall that lands the point partway UP the slope: t2l3's gully got
// jumps "from" (-729 -2720 -23), 20u up a 70-deg bank - which also made a 61u climb (over the 56u limit) pass as 42u.
// Bots walked into the bank to reach the take-off and froze there at full speed (BOTFROZEN swn 0.35-0.58, World).
// A point-sized probe straight down: a surface hit steeper than a player can walk (normal z < 0.7) rejects the link.
// Nothing under it (a ledge-edge fall start whose box rests on the lip) is left alone; an end INSIDE solid is rejected.
static int s_linkSteepRejects = 0;

static bool LinkEndOnSteep(const Vector& p)
{
    const Vector mins(-2, -2, 0);
    const Vector maxs(2, 2, 2);
    trace_t      t = G_Trace(p + Vector(0, 0, 8), mins, maxs, p - Vector(0, 0, 24), NULL, MASK_PLAYERSOLID, qfalse, "LinkEndOnSteep");
    if (t.startsolid || t.allsolid) {
        // [bug-2898] the point is INSIDE geometry: a take-off slid up a bank ends with its base inside the slope (the box
        // corner rests on it). Detour then snaps the end to the floor poly below - t2l3's gully kept 61-74u "jumps" from
        // exactly such points. A link end in solid is never a place to stand.
        return true;
    }
    if (t.fraction >= 1.0f) {
        return false;
    }
    return t.plane.normal[2] < 0.7f;
}

// [HZM bug-2898] the height of the ground straight under a link end (a point probe, up to 40u down), or the end's own
// z when there is none. FixupPoint drops every vertex with a PLAYER box, which rests on the lip of a bank and leaves the
// point up to a step above the real floor: t2l3's gully "vertex" (-745 -2720) sat at z -23 over a floor at -42, so a
// 61u climb measured 43u and passed - Detour snaps the end down to the poly anyway.
static float LinkGroundZ(const Vector& p)
{
    const Vector mins(-2, -2, 0);
    const Vector maxs(2, 2, 2);
    trace_t      t = G_Trace(p + Vector(0, 0, 8), mins, maxs, p - Vector(0, 0, 40), NULL, MASK_PLAYERSOLID, qfalse, "LinkGroundZ");
    if (t.startsolid || t.allsolid || t.fraction >= 1.0f) {
        return p.z;
    }
    return t.endpos[2];
}

int NavLinks_SteepRejects(bool reset)
{
    const int n = s_linkSteepRejects;
    if (reset) {
        s_linkSteepRejects = 0;
    }
    return n;
}

/*
============
NavLearn_MarkSteep

HZM [bug-2898] LEARNED STEEP GROUND. The mesh keeps an 8u margin from walls (agentRadius 8; 12u+ closed doorways,
bug-2833) while a player is 16u wide each side, and the mesh's slope test sees triangles, not the lip of a bank - so a
route can hug, or climb into, ground a player cannot walk. A bot then runs at full speed into it and goes nowhere
(BOTFROZEN, sweep hitting World at normal z 0.35-0.58 on t2l3's gully banks). When that happens, the poly it was trying
to enter becomes RECAST_AREA_STEEP_LAST (x25) for the rest of the map: every bot's next route goes round it, and it is
never removed - where it is the only way, it is still taken. At most 64 per map; a poly wider than 256u is left alone
(re-costing a big open floor for one bank would bend routes across the whole area).
Returns 1 marked, 0 nothing to do, -1 too big.
============
*/
int NavLearn_MarkSteep(const Vector& at, const Vector& dir)
{
    static str s_map;
    static int s_n = 0;
    if (s_map != level.mapname) {
        s_map = level.mapname;
        s_n   = 0;
    }
    dtNavMesh      *nm = navigationMap.GetNavMesh();
    dtNavMeshQuery *q  = navigationMap.GetNavMeshQuery();
    if (!nm || !q || s_n >= 64) {
        return 0;
    }
    const Vector p = at + dir * 24.0f;
    float        gp[3] = {p.x, p.y, p.z};
    float        rp[3];
    ConvertGameToRecastCoord(gp, rp);
    const float   ext[3] = {12.0f, 32.0f, 12.0f};
    dtQueryFilter f;
    dtPolyRef     ref = 0;
    float         np[3];
    if (dtStatusFailed(q->findNearestPoly(rp, ext, &f, &ref, np)) || !ref) {
        return 0;
    }
    unsigned char area = 0;
    if (dtStatusFailed(nm->getPolyArea(ref, &area))) {
        return 0;
    }
    if (!(area == 63 || (area >= RECAST_AREA_COST1 && area <= RECAST_AREA_COST4) || area == RECAST_AREA_STEEP)) {
        return 0; // already last-resort, or a link / special area
    }
    const dtMeshTile *tile = NULL;
    const dtPoly     *poly = NULL;
    if (dtStatusFailed(nm->getTileAndPolyByRef(ref, &tile, &poly)) || !tile || !poly) {
        return 0;
    }
    float bmin[3] = {1e9f, 1e9f, 1e9f}, bmax[3] = {-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < poly->vertCount; i++) {
        const float *v = &tile->verts[poly->verts[i] * 3];
        for (int k = 0; k < 3; k++) {
            bmin[k] = Q_min(bmin[k], v[k]);
            bmax[k] = Q_max(bmax[k], v[k]);
        }
    }
    if (bmax[0] - bmin[0] > 256.0f || bmax[2] - bmin[2] > 256.0f) {
        return -1;
    }
    nm->setPolyArea(ref, RECAST_AREA_STEEP_LAST);
    s_n++;
    return 1;
}

/*
============
NavLearn_DisableJumpLink [bug-2913]

User 2026-09-25 (The Rail Yard): axis "getting stuck in random places instead of taking obvious paths like stairs".
The auto jump-link generator accepts any hop within 128u total that clears its apex traces - it never asks whether a
player running at the real pace carries far enough horizontally while rising (m4l2 had a 120u-across / 28u-up link that
bots hopped at 96-118 times a soak, at either run speed). NavLearn_MarkSteep re-costs the GROUND poly, but a link is
its own off-mesh poly with its own cost, so the route kept taking it. When a bot hops at a JUMP link three times from
its take-off without getting across, clear that connection's flags: the default query filter (include any flag) then
never routes through it, for every bot, for the rest of the map. Only JUMP links, only ever switched off (never
re-costed ground), at most 32 per map - and reversible by the next mesh build.
============
*/
int NavLearn_DisableJumpLink(const Vector& a, const Vector& b)
{
    static str s_map;
    static int s_n = 0;
    if (s_map != level.mapname) {
        s_map = level.mapname;
        s_n   = 0;
    }
    dtNavMesh *nm = navigationMap.GetNavMesh();
    if (!nm || s_n >= 32) {
        return 0;
    }
    float ga[3] = {a.x, a.y, a.z}, gb[3] = {b.x, b.y, b.z};
    float ra[3], rb[3];
    ConvertGameToRecastCoord(ga, ra);
    ConvertGameToRecastCoord(gb, rb);
    const dtNavMesh *cnm  = nm;
    int              done = 0;
    for (int t = 0; t < cnm->getMaxTiles(); t++) {
        const dtMeshTile *tile = cnm->getTile(t);
        if (!tile || !tile->header) {
            continue;
        }
        for (int i = 0; i < tile->header->offMeshConCount; i++) {
            const dtOffMeshConnection *con = &tile->offMeshCons[i];
            const float               *p0  = &con->pos[0];
            const float               *p1  = &con->pos[3];
            auto                       d2  = [](const float *u, const float *v) {
                return (u[0] - v[0]) * (u[0] - v[0]) + (u[1] - v[1]) * (u[1] - v[1]) + (u[2] - v[2]) * (u[2] - v[2]);
            };
            const bool fwd = d2(p0, ra) < Square(20.0f) && d2(p1, rb) < Square(20.0f);
            const bool rev = d2(p0, rb) < Square(20.0f) && d2(p1, ra) < Square(20.0f);
            if (!fwd && !rev) {
                continue;
            }
            const dtPolyRef ref = cnm->getPolyRefBase(tile) | (dtPolyRef)con->poly;
            unsigned char   area = 0;
            if (dtStatusFailed(nm->getPolyArea(ref, &area)) || area != RECAST_AREA_JUMP) {
                continue;
            }
            // [bug-2913 round 5] LAST RESORT, not off: clearing the flags cut a ledge off entirely (lift4b: one bot
            // switched off all 19 links round one ledge) - the user's rule is that the only way must stay a way
            nm->setPolyArea(ref, RECAST_AREA_JUMP_HARD);
            done++;
        }
    }
    if (done) {
        s_n++;
    }
    return done;
}

bool NavigationMapExtension_JumpFall::AddPoint(
    Container<offMeshNavigationPoint>& points, const offMeshNavigationPoint& point
)
{
    if (!point.area) {
        return false;
    }

    {
        // bot_navLinkDbg "x y r": print every auto link with an end within r of (x, y), as generated (before Detour snaps
        // its ends onto the polys) - diagnostic for links that look wrong in a nav_dump
        static cvar_t *s_ldbg = NULL;
        if (!s_ldbg) {
            s_ldbg = gi.Cvar_Get("bot_navLinkDbg", "", 0);
        }
        float dx, dy, dr;
        if (s_ldbg->string[0] && sscanf(s_ldbg->string, "%f %f %f", &dx, &dy, &dr) == 3
            && (Square(point.start.x - dx) + Square(point.start.y - dy) < Square(dr)
                || Square(point.end.x - dx) + Square(point.end.y - dy) < Square(dr))) {
            gi.Printf(
                "^~^~^ NAVLINK area=%d (%.1f %.1f %.1f) -> (%.1f %.1f %.1f) steepS=%d steepE=%d\n", point.area,
                point.start.x, point.start.y, point.start.z, point.end.x, point.end.y, point.end.z,
                LinkEndOnSteep(point.start) ? 1 : 0, LinkEndOnSteep(point.end) ? 1 : 0
            );
        }
    }
    static cvar_t *s_lchk = NULL;
    if (!s_lchk) {
        s_lchk = gi.Cvar_Get("bot_navLinkCheck", "1", 0); // 0 = accept every generated link (A/B the bug-2896/2898 rules)
    }
    if (!s_lchk->integer) {
        points.AddObject(point);
        return true;
    }
    if (LinkEndOnSteep(point.start) || LinkEndOnSteep(point.end)) { // [HZM bug-2896]
        s_linkSteepRejects++;
        return false;
    }
    // [HZM bug-2898] a (two-way) JUMP link must be climbable from its FINAL ends: CanConnectJumpPoint checks the height
    // against the take-off it slid up the obstacle, then can return the original floor point - t2l3's gully kept a
    // "jump" from (-745 -2720 -42) to (-712 -2712 19), 61u, and bots hopped at it and fell back until a fight moved them
    if (point.area == RECAST_AREA_JUMP
        && fabs(LinkGroundZ(point.end) - LinkGroundZ(point.start)) > NavigationMapExtensionConfiguration::agentJumpHeight) {
        s_linkSteepRejects++;
        return false;
    }

    points.AddObject(point);
    return true;
}

bool NavigationMapExtension_JumpFall::AreVertsValid(const vec3_t pos1, const vec3_t pos2) const
{
    vec3_t delta;

    VectorSub2D(pos2, pos1, delta);
    if (VectorLength2DSquared(delta) > Square(256)) {
        return false;
    }

    const float deltaHeight = pos2[2] - pos1[2];
    const float minHeight   = STEPSIZE / 2;
    if (deltaHeight >= -minHeight && deltaHeight <= minHeight) {
        // ignore steps
        return false;
    }

    return true;
}

// HZM [bug-2949] jump-over-obstacle links refused by the clearance margin / kept, for the NAVLEDGE build line
static int s_iLedgeRejected = 0;
static int s_iLedgeKept     = 0;
static int s_iStraightKept     = 0; // [HZM bug-2964] straight links walked and kept / refused, for the NAVSTRAIGHT line
static int s_iStraightRejected = 0;

void NavigationMapExtension_JumpFall::Handle(Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh)
{
    int             i, j;
    vec3_t         *vertpos;
    Vector          pos1, pos2;
    bool           *walkableVert;
    unsigned short *vertreg;
    unsigned char  *verttype;

    walkableVert = new bool[polyMesh->nverts];
    vertpos      = new vec3_t[polyMesh->nverts];
    vertreg      = new unsigned short[polyMesh->nverts];
    verttype     = new unsigned char[polyMesh->nverts];

    for (i = 0; i < polyMesh->nverts; i++) {
        walkableVert[i] = false;
        vertreg[i]      = 0;
        verttype[i]     = 0;

        GetPolyMeshVertPosition(polyMesh, i, vertpos[i]);
        FixupPoint(vertpos[i]);
    }

    for (i = 0; i < polyMesh->npolys; i++) {
        const unsigned short *poly = &polyMesh->polys[i * polyMesh->nvp * 2];

        if (!IsRecastWalkableArea(polyMesh->areas[i])) { // [bug-2865] heat-costed ground keeps its jump/fall links
            continue;
        }

        for (j = 0; j < polyMesh->nvp; j++) {
            if (poly[j] == RC_MESH_NULL_IDX) {
                break;
            }

            assert(poly[j] < polyMesh->nverts);
            walkableVert[poly[j]] = true;
            vertreg[poly[j]]      = polyMesh->regs[i];
        }
    }

    for (i = 0; i < polyMesh->nverts; i++) {
        if (!walkableVert[i]) {
            continue;
        }

        for (j = i + 1; j < polyMesh->nverts; j++) {
            if (!walkableVert[j]) {
                continue;
            }

            if (vertreg[j] == vertreg[i]) {
                continue;
            }

            if (!AreVertsValid(vertpos[i], vertpos[j])) {
                continue;
            }
            pos1 = vertpos[i];
            pos2 = vertpos[j];

            offMeshNavigationPoint point;

            if (AddPoint(points, CanConnectStraightPoint(polyMesh, pos1, pos2))) {
                verttype[i] = verttype[j] = 1;
                continue;
            }
            if (AddPoint(points, CanConnectJumpPoint(polyMesh, pos1, pos2))) {
                verttype[i] = verttype[j] = 2;
                continue;
            }
            if (AddPoint(points, CanConnectFallPoint(polyMesh, pos1, pos2))) {
                verttype[i] = verttype[j] = 3;
                continue;
            }
        }
    }

    //
    // Add "jumping over obstacles" points.
    //  Those points are created if there are no jump points nor fall points
    //  that would allow the bot to get around the obstacle or exit an area
    //

    for (i = 0; i < polyMesh->nverts; i++) {
        if (!walkableVert[i]) {
            continue;
        }

        for (j = i + 1; j < polyMesh->nverts; j++) {
            if (!walkableVert[j]) {
                continue;
            }

            if (vertreg[j] == vertreg[i]) {
                continue;
            }

            if (verttype[i] && verttype[j]) {
                continue;
            }

            if (!AreVertsValid(vertpos[i], vertpos[j])) {
                continue;
            }
            pos1 = vertpos[i];
            pos2 = vertpos[j];

            offMeshNavigationPoint point;

            if (AddPoint(points, CanConnectJumpOverLedgePoint(polyMesh, pos1, pos2))) {
                verttype[i] = verttype[j] = 4;
                s_iLedgeKept++;
                continue;
            }
        }
    }
    gi.Printf(
        "^~^~^ NAVLEDGE %s kept=%d rejected=%d margin=%s\n", level.mapname.c_str(), s_iLedgeKept, s_iLedgeRejected,
        gi.Cvar_Get("bot_navLedgeMargin", "16", 0)->string
    );
    s_iLedgeKept     = 0;
    s_iLedgeRejected = 0;
    gi.Printf(
        "^~^~^ NAVSTRAIGHT %s kept=%d rejected=%d walk=%s\n", level.mapname.c_str(), s_iStraightKept, s_iStraightRejected,
        gi.Cvar_Get("bot_navStraightWalk", "1", 0)->string
    );
    s_iStraightKept     = 0;
    s_iStraightRejected = 0;

    delete[] verttype;
    delete[] vertreg;
    delete[] vertpos;
    delete[] walkableVert;
}

void NavigationMapExtension_JumpFall::FixupPoint(vec3_t pos)
{
    trace_t trace;
    int     i;
    Vector  start, end;
    float   bestFrac = 9999;
    Vector  bestPos  = pos;

    const Vector mins(MINS_X, MINS_Y, 0);
    const Vector maxs(MAXS_X, MAXS_Y, STEPSIZE);

    trace = G_Trace(pos, mins, maxs, pos, NULL, MASK_PLAYERSOLID, qfalse, "FixupPoint");
    if (trace.startsolid) {
        //
        // Find the best fraction
        //

        for (i = 0; i < 4; i++) {
            float angle = ((2 * M_PI) * (float)i / (float)4);
            float dx    = cos(angle) * maxs.x;
            float dy    = sin(angle) * maxs.y;
            float dz    = STEPSIZE;

            start = Vector(pos[0] + dx, pos[1] + dy, pos[2] + dz);
            end   = pos;

            trace = G_Trace(start, mins, maxs, end, NULL, MASK_PLAYERSOLID, qfalse, "FixupPoint");
            if (trace.fraction < bestFrac && !trace.startsolid) {
                bestFrac = trace.fraction;
                bestPos  = trace.endpos;
            }
        }
    }

    //
    // Drop to floor
    //
    trace = G_Trace(
        bestPos + Vector(0, 0, STEPSIZE),
        mins,
        maxs,
        bestPos - Vector(0, 0, STEPSIZE * 2),
        NULL,
        MASK_PLAYERSOLID,
        qfalse,
        "CanConnectJumpPoint"
    );

    VectorCopy(trace.endpos, pos);
}

/*
============
NavigationMap::CanConnectFallPoint
============
*/
offMeshNavigationPoint
NavigationMapExtension_JumpFall::CanConnectFallPoint(const rcPolyMesh *polyMesh, const Vector& pos1, const Vector& pos2)
{
    const Vector           mins(MINS_X, MINS_Y, 0);
    const Vector           maxs(MAXS_X, MAXS_Y, NavigationMapConfiguration::agentHeight);
    const float            maxDistEdge = maxs.x * 3;
    Vector                 start, end;
    Vector                 tstart, tend;
    Vector                 delta;
    Vector                 dir;
    float                  fallheight;
    float                  dist;
    trace_t                trace;
    offMeshNavigationPoint point;

    if (pos1.z > pos2.z) {
        start = pos1;
        end   = pos2;
    } else {
        start = pos2;
        end   = pos1;
    }

    delta      = end - start;
    fallheight = -delta.z;

    if (fallheight > 600) {
        // This would cause too much damage
        return {};
    }

    dist = delta.lengthXY();
    if (dist > fallheight) {
        // Too far from the start
        return {};
    }

    dir   = delta;
    dir.z = 0;
    dir.normalize();

    // s---x <-- Check if the path from s to x is reachable
    //     |
    //     |
    tstart = start;
    tend   = tstart + dir * Q_min(dist, maxDistEdge);

    trace = G_Trace(tstart, mins, maxs, tend, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectFallPoint");
    if (trace.allsolid || trace.startsolid) {
        return {};
    }
    /*
    else if (trace.startsolid) {
        tend = tstart;
        tstart.z += STEPSIZE;

        trace = G_Trace(tstart, mins, maxs, tend, NULL, MASK_PLAYERSOLID, qtrue, "CanConnectFallPoint");
        if (trace.allsolid || trace.startsolid) {
            // Still in a solid
            return {};
        }

        // Apply the new position
        tstart = start = trace.endpos;
        tend           = tstart + dir * Q_min(dist, maxDistEdge);

        trace = G_Trace(tstart, mins, maxs, tend, NULL, MASK_PLAYERSOLID, qtrue, "CanConnectFallPoint");
        if (trace.allsolid || trace.startsolid) {
            return {};
        }
    }
    */

    // ----s
    //     |
    //     |
    //     x <-- Check if the path from s to x is reachable
    tstart = trace.endpos;
    tend   = end;

    if (!G_SightTrace(
            tstart, mins, maxs, tend, (Entity *)NULL, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectFallPoint"
        )) {
        return {};
    }

    point.start         = start;
    point.end           = tend;
    point.bidirectional = false;
    point.radius        = NavigationMapConfiguration::agentRadius;
    if (fallheight <= NavigationMapExtensionConfiguration::smallFallHeight) {
        point.area = RECAST_AREA_FALL;
    } else if (fallheight <= NavigationMapExtensionConfiguration::mediumFallHeight) {
        point.area = RECAST_AREA_MEDIUM_FALL;
    } else {
        point.area = RECAST_AREA_HIGH_FALL;
    }

    point.flags = RECAST_POLYFLAG_WALKABLE;

    return point;
}

/*
============
NavigationMap::CanConnectJumpPoint
============
*/
offMeshNavigationPoint
NavigationMapExtension_JumpFall::CanConnectJumpPoint(const rcPolyMesh *polyMesh, const Vector& pos1, const Vector& pos2)
{
    const Vector           mins(MINS_X, MINS_Y, 0);
    const Vector           maxs(MAXS_X, MAXS_Y, NavigationMapConfiguration::agentHeight);
    Vector                 start, end;
    Vector                 tend;
    Vector                 delta;
    Vector                 fwdDir;
    float                  jumpheight;
    float                  length;
    Vector                 dir, right;
    Vector                 destGroundDir, destGroundUp;
    Vector                 planeNormal, wallForward;
    trace_t                trace;
    int                    i;
    offMeshNavigationPoint point;

    if (pos1.z > pos2.z) {
        start = pos2;
        end   = pos1;
    } else {
        start = pos1;
        end   = pos2;
    }

    jumpheight = end.z - start.z;
    if (jumpheight > NavigationMapExtensionConfiguration::agentJumpHeight) {
        return {};
    }

    //planeNormal = trace.plane.normal;
    //planeNormal.toAngles().AngleVectors(&destGroundUp, NULL, &destGroundDir);

    delta      = end - start;
    jumpheight = delta.z;
    if (jumpheight > NavigationMapExtensionConfiguration::agentJumpHeight) {
        return {};
    }

    if (delta.lengthSquared() > Square(128)) {
        return {};
    }

    dir    = delta;
    dir.z  = 0;
    length = dir.normalize();

    fwdDir = dir * 32;

    // Check for this situation:
    //
    // straight path
    //       |
    //       ↓
    // x───────────e
    trace = G_Trace(start, mins, maxs, end, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectJumpPoint");
    if (!trace.allsolid && trace.fraction >= 0.999) {
        // Straight path
        return {};
    }

    // Calculate the right so multiple tries can be done
    planeNormal = trace.plane.normal;
    planeNormal.toAngles().AngleVectors(&wallForward, &right);
    right   = -right;
    right.z = 0;

    // Drop to floor
    //
    //        ┌ Go to e and drop to d
    //        ↓
    // s─────┐e┌─x
    //       └d┘
    trace = G_Trace(start, mins, maxs, start + dir * length, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectJumpPoint");

    //
    //        ┌ Drop here
    //        ↓
    // ──────┐s┌─x
    //       └e┘
    trace = G_Trace(
        trace.endpos,
        mins,
        maxs,
        trace.endpos - Vector(0, 0, NavigationMapExtensionConfiguration::agentJumpHeight),
        NULL,
        MASK_PLAYERSOLID,
        qfalse,
        "CanConnectJumpPoint"
    );

    if (!trace.allsolid && trace.fraction > 0 && trace.fraction < 1) {
        start      = trace.endpos;
        delta      = end - start;
        jumpheight = delta.z;
        if (jumpheight > NavigationMapExtensionConfiguration::agentJumpHeight) {
            return {};
        }

        if (delta.lengthSquared() > Square(128)) {
            return {};
        }
    }

    for (i = 0; i < 4; i++) {
        Vector target;
        Vector highestPoint;

        //          ┌ get the highest point
        //          ↓
        //          h
        //          │
        // ──────┐ ┌x
        //       └─┘
        //
        highestPoint   = end;
        highestPoint.z = start.z + NavigationMapExtensionConfiguration::agentJumpHeight;

        trace = G_Trace(end, mins, maxs, highestPoint, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectJumpPoint");

        target = start;
        target += wallForward * (i * 4);
        target += right * (i * 4);
        target.z = trace.endpos[2];

        //  Make sure the player can jump at this point
        //
        //        ┌ must be able to jump
        //        ↓
        //        e
        // ──────┐│┌x
        //       └s┘
        //
        trace = G_Trace(start, mins, maxs, target, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectJumpPoint");
        if (trace.allsolid || trace.fraction == 0) {
            continue;
        }

        tend = trace.endpos;

        //   Check if the point is reachable while jumping
        //
        //           ┌ must be able to reach this point
        //        s  ↓
        // ──────┐│┌─x
        //       └┴┘
        //

        if (G_SightTrace(tend, mins, maxs, end, (Entity *)NULL, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectJumpPoint")
            && G_SightTrace(
                end, mins, maxs, tend, (Entity *)NULL, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectJumpPoint"
            )) {
            delta      = end - start;
            jumpheight = delta.z;
            if (jumpheight > NavigationMapExtensionConfiguration::agentJumpHeight) {
                return {};
            }

            if (delta.lengthSquared() > Square(128)) {
                return {};
            }
            break;
        }
    }

    if (i >= 4) {
        return {};
    }

    point.start         = start;
    point.end           = end;
    point.bidirectional = true;
    point.radius        = NavigationMapConfiguration::agentRadius;
    point.area          = RECAST_AREA_JUMP;
    point.flags         = RECAST_POLYFLAG_WALKABLE;

    return point;
}

/*
============
NavigationMap::CanConnectJumpOverLedgePoint

The following action can be performed:

    Jump over that ledge
    ↓
   ┌─┐
 s─┘ └─e
============
*/
offMeshNavigationPoint NavigationMapExtension_JumpFall::CanConnectJumpOverLedgePoint(
    const rcPolyMesh *polyMesh, const Vector& pos1, const Vector& pos2
)
{
    const Vector           mins(MINS_X, MINS_Y, 0);
    const Vector           maxs(MAXS_X, MAXS_Y, NavigationMapConfiguration::agentHeight);
    Vector                 start, end;
    Vector                 startStep;
    Vector                 startInAir;
    Vector                 startToEnd;
    Vector                 delta;
    Vector                 dir;
    float                  length, dist;
    float                  fallheight;
    float                  heightDiff;
    float                  jumpHeight;
    int                    i;
    trace_t                trace;
    offMeshNavigationPoint point;

    if (pos1.z > pos2.z) {
        start = pos1;
        end   = pos2;
    } else {
        start = pos2;
        end   = pos1;
    }

    heightDiff = fabs(start.z - end.z);
    if (heightDiff > 600) {
        return {};
    }

    delta = end - start;

    dir    = delta;
    dir.z  = 0;
    length = dir.normalize();

    // Check for this situation:
    //
    // straight path
    //       |
    //       ↓
    // x───────────e
    if (G_SightTrace(
            start, mins, maxs, end, (Entity *)NULL, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectJumpOverLedgePoint"
        )) {
        // Straight path
        return {};
    }

    // Go here
    // ↓
    // e
    // │  ┌─┐
    // s──┘ └─t
    trace = G_Trace(
        start,
        mins,
        maxs,
        start + Vector(0, 0, NavigationMapExtensionConfiguration::agentJumpHeight),
        NULL,
        MASK_PLAYERSOLID,
        qfalse,
        "CanConnectJumpOverLedgePoint"
    );

    if (trace.allsolid) {
        return {};
    }

    jumpHeight = trace.endpos[2] - start[2];
    startInAir = trace.endpos;

    for (i = 0; i < STEPSIZE; i += STEPSIZE / 2.0) {
        startStep = start + Vector(0, 0, i);

        //
        //   Go here
        //   ↓
        //    ┌─┐
        // s─e┘ └─t
        trace = G_Trace(
            startStep,
            mins,
            maxs,
            startStep + dir * length,
            NULL,
            MASK_PLAYERSOLID,
            qfalse,
            "CanConnectJumpOverLedgePoint"
        );
        if (trace.fraction >= 0.999) {
            return {};
        }
    }

    delta = trace.endpos - startStep;
    if (delta.lengthXYSquared() > Square(32)) {
        // Too much distance
        return {};
    }

    // HZM [bug-2949] CLEAR IT WITH ROOM TO SPARE. The in-air test below accepts the obstacle when the box clears it at the
    // very APEX of a jump (56u, the legs .st "jump 56"). A player crosses an obstacle only while the box is ABOVE it for
    // long enough to carry its own width (30u) plus the obstacle's thickness at the running pace; at the apex that window is
    // nothing. m4l2's upper floor ends in a 52u playerclip railing (brush 2783) over a 192u drop - the link over it was
    // accepted with 4u to spare, classed a FALL (so a bot walks, never jumps, at it), and Axis bots pressed into the
    // invisible rail for up to 50s at a time (bug-2949: 14-57 BOTBLOCK a run). Require the whole leg clear bot_navLedgeMargin
    // (16) below the jump height - a 40u obstacle leaves a ~0.5s airborne window, ~65u at the 0.6 pace. 0 = the old test.
    {
        static cvar_t *s_ledgeMargin = NULL;
        if (!s_ledgeMargin) {
            s_ledgeMargin = gi.Cvar_Get("bot_navLedgeMargin", "16", 0);
        }
        if (s_ledgeMargin->value > 0.0f) {
            const float h = Q_min(jumpHeight, NavigationMapExtensionConfiguration::agentJumpHeight) - s_ledgeMargin->value;
            if (h <= STEPSIZE) {
                s_iLedgeRejected++;
                return {};
            }
            trace = G_Trace(
                start + Vector(0, 0, h), mins, maxs, start + Vector(0, 0, h) + dir * length, NULL, MASK_PLAYERSOLID,
                qfalse, "CanConnectJumpOverLedgeMargin"
            );
            if (trace.startsolid || trace.allsolid || trace.fraction < 0.999) {
                s_iLedgeRejected++;
                return {};
            }
        }
    }

    for (i = 0; i < NavigationMapExtensionConfiguration::agentJumpHeight && i < jumpHeight; i += 10) {
        //        Go here
        //        ↓
        // s──────e
        //    ┌─┐
        // ───┘ └─t
        trace = G_Trace(
            startInAir - Vector(0, 0, i),
            mins,
            maxs,
            startInAir + dir * length - Vector(0, 0, i),
            NULL,
            MASK_PLAYERSOLID,
            qfalse,
            "CanConnectJumpOverLedgePoint"
        );

        if (trace.fraction >= 0.999) {
            break;
        }
    }

    if (i >= NavigationMapExtensionConfiguration::agentJumpHeight) {
        return {};
    }

    startToEnd = trace.endpos;
    fallheight = startInAir.z - end.z;

    if (fallheight > 600) {
        // This would cause too much damage
        return {};
    }

    delta = startInAir - end;
    dist  = delta.lengthXY();
    if (dist > fallheight) {
        // Too far from the start
        return {};
    }

    //        Go here
    //        ↓
    //        s
    //    ┌─┐ │
    // ───┘ └─e
    if (!G_SightTrace(
            startToEnd, mins, maxs, end, (Entity *)NULL, NULL, MASK_PLAYERSOLID, qfalse, "CanConnectJumpOverLedgePoint"
        )) {
        return {};
    }

    point.start = start;
    point.end   = end;
    if (fallheight > NavigationMapExtensionConfiguration::agentJumpHeight) {
        point.bidirectional = false;
    } else {
        point.bidirectional = true;
    }

    point.radius = NavigationMapConfiguration::agentRadius;
    if (fallheight <= NavigationMapExtensionConfiguration::smallFallHeight) {
        point.area = RECAST_AREA_FALL;
    } else if (fallheight <= NavigationMapExtensionConfiguration::mediumFallHeight) {
        point.area = RECAST_AREA_MEDIUM_FALL;
    } else {
        point.area = RECAST_AREA_HIGH_FALL;
    }
    point.flags = RECAST_POLYFLAG_WALKABLE;

    return point;
}

/*
============
NavigationMap::CanConnectStraightPoint
============
*/
offMeshNavigationPoint NavigationMapExtension_JumpFall::CanConnectStraightPoint(
    const rcPolyMesh *polyMesh, const Vector& pos1, const Vector& pos2
)
{
    const Vector           mins(MINS_X, MINS_Y, 0);
    const Vector           maxs(MAXS_X, MAXS_Y, NavigationMapConfiguration::agentHeight);
    Vector                 start, end;
    Vector                 delta;
    Vector                 fwdDir;
    float                  jumpheight;
    Vector                 dir;
    int                    i;
    static const float     offsets[2] = {-16, 16};
    offMeshNavigationPoint point;

    if (pos1.z > pos2.z) {
        start = pos2;
        end   = pos1;
    } else {
        start = pos1;
        end   = pos2;
    }

    delta      = end - start;
    jumpheight = delta.z;
    if (jumpheight > STEPSIZE) {
        return {};
    }

    if (!G_SightTrace(
            start, mins, maxs, end, NULL, (Entity *)NULL, MASK_PLAYERSOLID, qfalse, "CanConnectStraightPoint"
        )) {
        // Not straight path
        return {};
    }

    if (!G_SightTrace(
            end, mins, maxs, start, NULL, (Entity *)NULL, MASK_PLAYERSOLID, qfalse, "CanConnectStraightPoint"
        )) {
        return {};
    }

    dir = delta;
    dir.normalize();

    for (i = 0; i < ARRAY_LEN(offsets); i++) {
        if (!G_SightTrace(
                start + dir * offsets[i],
                vec_zero,
                vec_zero,
                end,
                NULL,
                (Entity *)NULL,
                MASK_PLAYERSOLID,
                qfalse,
                "CanConnectStraightPoint"
            )) {
            return {};
        }

        if (!G_SightTrace(
                end + dir * offsets[i],
                vec_zero,
                vec_zero,
                start,
                NULL,
                (Entity *)NULL,
                MASK_PLAYERSOLID,
                qfalse,
                "CanConnectStraightPoint"
            )) {
            return {};
        }
    }

    // [HZM bug-2964] WALK the line. The sweeps above only move a box at the ENDS' height - they prove the air along the
    // line is clear, not that there is ground a player can walk on under it. On uneven ground a straight link skims over
    // what is really there: e1l1's cliff foot (-2560,-3888) got links from the east floor to the west one straight across
    // a 60-degree bank beside a crouch-only notch, and bots that took them slid off the bank into the notch and stayed
    // (BOTBLOCK2 trav=33, 'World ahead at 13'). Now every 8u along it: ground within a step above / 128u below the line,
    // no more than a step from the last sample, steep ground (normal z < 0.7) only in runs of 24u or less and never more
    // than a step off the last walkable ground, and a standing player (cylinder) fits on it.
    // bot_navStraightWalk 0 = off. Validated 2026-09-26 (v4): 0 of 259 real link crossings in 75 min of soak were straight
    // links (they cost x100, last resort), 37,745 spawn/zone routes on 23 maps keep their exact cost, 0 new sinks.
    {
        static cvar_t *s_walk = NULL;
        if (!s_walk) {
            s_walk = gi.Cvar_Get("bot_navStraightWalk", "1", 0);
        }
        if (s_walk->integer) {
            const Vector gmins(MINS_X, MINS_Y, 0), gmaxs(MAXS_X, MAXS_Y, 1);
            const Vector bmins(MINS_X, MINS_Y, STEPSIZE), bmaxs(MAXS_X, MAXS_Y, MAXS_Z);
            const Vector d   = end - start;
            const int    n   = Q_max(1, (int)(sqrt(d.x * d.x + d.y * d.y) / 8.0f));
            float        lz  = 0;
            bool         bad = false;
            const char  *why = "";
            Vector       at;
            float        an = 0, adz = 0;
            float        wz       = 0; // ground height at the last walkable (normal z >= 0.7) sample
            bool         haveWz   = false;
            int          steepRun = 0;    // consecutive steep samples (8u each)
            // nav_straightTrace "x y r": print every sample of the straight links with an end within r of (x,y) (dev)
            static cvar_t *s_tr = NULL;
            if (!s_tr) {
                s_tr = gi.Cvar_Get("nav_straightTrace", "", 0);
            }
            bool tron = false;
            {
                float tx = 0, ty = 0, trr = 0;
                if (s_tr->string[0] && sscanf(s_tr->string, "%f %f %f", &tx, &ty, &trr) == 3) {
                    tron = (Square(start.x - tx) + Square(start.y - ty) < trr * trr)
                        || (Square(end.x - tx) + Square(end.y - ty) < trr * trr);
                }
            }
            for (int k = 0; k <= n && !bad; k++) {
                const Vector pt = start + d * ((float)k / (float)n);
                // down to 128u under the chord: gentle terrain sags well below a long straight line between two ends
                // (a 250u link over a 40u-deep dip is a walkable valley) - a real hole shows up as a jump > a step
                trace_t      g  = G_Trace(
                    pt + Vector(0, 0, STEPSIZE), gmins, gmaxs, pt - Vector(0, 0, 128), NULL, MASK_PLAYERSOLID, qtrue,
                    "CanConnectStraightWalkG"
                );
                at = pt;
                if (g.startsolid || g.allsolid) {
                    bad = true, why = "solid";
                    break;
                }
                if (g.fraction >= 1.0f) {
                    bad = true, why = "noground";
                    break;
                }
                at = g.endpos;
                an = g.plane.normal[2];
                if (g.plane.normal[2] < 0.7f) {
                    // steep under the line. A short run of it within a step of the last walkable ground is a lip, a
                    // terrain seam or a sloped kerb that pmove steps over (18u); more than 24u of it, or ground more than
                    // a step off the last walkable ground, is a slope a player slides on - including a line that hugs
                    // a steep face end to end (e1l1: the ground under the chord WAS the chord, normal 0.22, all along)
                    steepRun++;
                    if (steepRun > 3 || (haveWz && fabs(g.endpos[2] - wz) > STEPSIZE)) {
                        adz = haveWz ? g.endpos[2] - wz : 0.0f;
                        bad = true, why = "steep";
                        break;
                    }
                } else {
                    steepRun = 0;
                    wz       = g.endpos[2];
                    haveWz   = true;
                }
                adz = k ? g.endpos[2] - lz : 0.0f;
                if (k && fabs(g.endpos[2] - lz) > STEPSIZE) {
                    bad = true, why = "step";
                    break;
                }
                lz        = g.endpos[2];
                trace_t b = G_Trace(g.endpos, bmins, bmaxs, g.endpos, NULL, MASK_PLAYERSOLID, qtrue, "CanConnectStraightWalkB");
                if (b.startsolid || b.allsolid) {
                    bad = true, why = "body";
                }
                if (tron) {
                    gi.Printf(
                        "^~^~^ NAVWALK %d/%d pt=(%.0f %.0f %.0f) ground=%.1f n=%.2f wz=%.1f body=%d\n", k, n, pt.x, pt.y, pt.z,
                        g.endpos[2], g.plane.normal[2], wz, (b.startsolid || b.allsolid) ? 0 : 1
                    );
                }
            }
            if (tron) {
                gi.Printf(
                    "^~^~^ NAVWALK link (%.0f %.0f %.0f)->(%.0f %.0f %.0f) %s %s at=(%.0f %.0f %.0f)\n", start.x, start.y, start.z,
                    end.x, end.y, end.z, bad ? "REJECT" : "KEEP", why, at.x, at.y, at.z
                );
            }
            if (bad) {
                s_iStraightRejected++;
                static cvar_t *s_dbg = NULL;
                if (!s_dbg) {
                    s_dbg = gi.Cvar_Get("nav_straightDebug", "0", 0);
                }
                if (s_dbg->integer) {
                    gi.Printf(
                        "^~^~^ NAVSTRAIGHTREJ %s from=(%.0f %.0f %.0f) to=(%.0f %.0f %.0f) why=%s at=(%.0f %.0f %.0f) n=%.2f dz=%.0f\n",
                        level.mapname.c_str(), start.x, start.y, start.z, end.x, end.y, end.z, why, at.x, at.y, at.z, an, adz
                    );
                }
                return {};
            }
        }
        s_iStraightKept++;
    }

    point.start         = start;
    point.end           = end;
    point.bidirectional = true;
    point.radius        = NavigationMapConfiguration::agentRadius;
    point.area          = RECAST_AREA_STRAIGHT;
    point.flags         = RECAST_POLYFLAG_WALKABLE;

    return point;
}

Container<ExtensionArea> NavigationMapExtension_JumpFall::GetSupportedAreas() const
{
    Container<ExtensionArea> list;

    // Only jump when necessary
    list.AddObject(ExtensionArea(RECAST_AREA_JUMP, 10.0));
    list.AddObject(ExtensionArea(RECAST_AREA_JUMP_HARD, 60.0)); // [bug-2913] learned: last resort

    // Small falls can be used as a shortcut
    list.AddObject(ExtensionArea(RECAST_AREA_FALL, 5.0));
    list.AddObject(ExtensionArea(RECAST_AREA_MEDIUM_FALL, 10.0));
    // Take high fall as a last resort, when no alternative is available
    list.AddObject(ExtensionArea(RECAST_AREA_HIGH_FALL, 20.0));

    // [HZM bot nav] reverted 2.0 -> stock 100.0 while isolating a ~80s dedicated-server hang. Making straight
    // off-mesh links cheap may have let the pather loop on a bad link; reinstate only once proven safe.
    list.AddObject(ExtensionArea(RECAST_AREA_STRAIGHT, 100.0));

    return list;
}

/*
============
NavigationMapExtension_ManualLinks

HZM [bug-2837] HAND-PLACED off-mesh links, for gaps the automatic JumpFall generator cannot bridge (its caps are
256u horizontal / 128u jump / 600u fall, AreVertsValid + CanConnect*Point above). First use: m4l2 "The Rail Yard" -
a sandbag on the allies' route through the tank depot that bots stalled at; user: bots must ALWAYS jump it. Also
the tool for ledge-traps (a spot bots climb onto with no mesh path back down).

Files: navlinks/<map>.txt (always) and navlinks/<map>_<variant>.txt (when cm_variant is set, e.g. "mp" for a
campaign map played as Push/Arena). One link per line, game coordinates of a STANDING spot on each side:
    jump x1 y1 z1  x2 y2 z2    bidirectional jump - RECAST_AREA_JUMP, exactly what auto jump links use, so the
                               bot's existing off-mesh traversal presses jump across it
    fall x1 y1 z1  x2 y2 z2    one-way drop from 1 to 2 - RECAST_AREA_FALL
'#' starts a comment. Links are gated to the polymesh whose XZ bounds hold their start, because Detour attaches an
off-mesh connection to the tile containing its start (a no-op for the solo build, correct for the tiled one).
============
*/
CLASS_DECLARATION(INavigationMapExtension, NavigationMapExtension_ManualLinks, NULL) {
    {NULL, NULL}
};

static void ManualLinks_Load(const char *filename, Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh)
{
    char       *buf   = NULL;
    const char *line;
    int         added = 0;

    if (gi.FS_ReadFile(filename, (void **)&buf, qtrue) <= 0 || !buf) {
        return;
    }

    line = buf;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t      len = eol ? (size_t)(eol - line) : strlen(line);
        char        tmp[256];
        char        kind[16];
        float       a[3], b[3];
        char       *hash;

        if (len >= sizeof(tmp)) {
            len = sizeof(tmp) - 1;
        }
        memcpy(tmp, line, len);
        tmp[len] = 0;
        hash     = strchr(tmp, '#');
        if (hash) {
            *hash = 0;
        }

        if (sscanf(tmp, "%15s %f %f %f %f %f %f", kind, &a[0], &a[1], &a[2], &b[0], &b[1], &b[2]) == 7) {
            offMeshNavigationPoint point;
            float                  rs[3];

            point.start  = Vector(a[0], a[1], a[2]);
            point.end    = Vector(b[0], b[1], b[2]);
            point.radius = NavigationMapConfiguration::agentRadius;
            point.flags  = RECAST_POLYFLAG_WALKABLE;
            if (!Q_stricmp(kind, "fall")) {
                point.area          = RECAST_AREA_FALL;
                point.bidirectional = false;
            } else if (!Q_stricmp(kind, "jump")) {
                point.area          = RECAST_AREA_JUMP;
                point.bidirectional = true;
            } else if (!Q_stricmp(kind, "elevator")) {
                // [bug-2862] landing -> landing through a working lift; the bot waits for the cab (see
                // BotMovement::MoveThink) instead of jumping
                point.area          = RECAST_AREA_ELEVATOR;
                point.bidirectional = true;
                // [bug-2912] optional `cab= gate1= gate2=` entity names after the 7 numbers: the lift protocol
                // (BotMovement::LiftThink) reads the real cab/gate state instead of guessing from traces
                NavLift_Parse(tmp, point.start, point.end);
            } else {
                // not a link ("block" is ManualBlocks_Apply's; anything else is ignored, never guessed as a jump)
                if (!eol) {
                    break;
                }
                line = eol + 1;
                continue;
            }

            ConvertGameToRecastCoord(point.start, rs);
            if (rs[0] >= polyMesh->bmin[0] && rs[0] <= polyMesh->bmax[0] && rs[2] >= polyMesh->bmin[2]
                && rs[2] <= polyMesh->bmax[2]) {
                points.AddObject(point);
                added++;
            }
        }

        if (!eol) {
            break;
        }
        line = eol + 1;
    }

    gi.FS_FreeFile(buf);
    if (added) {
        gi.Printf("  Navmesh: %s -> %d manual off-mesh link(s)\n", filename, added);
    }
}

/*
============
NavLift table [bug-2912]

One entry per navlinks `elevator` line that names its movers (m4l2: `cab=elevator_cab gate1=elevator_gate_1
gate2=elevator_gate_2`, gate1 at end 1 = the first landing written). ManualLinks_Load runs once per tile and again
on a solo->tiled fallback, so entries are keyed by their endpoints and re-adds update in place. Cleared with the
navmesh (NavigationMap::ClearNavigation); a same-map reload that keeps the mesh keeps the table too.
============
*/
static navLift_t s_navLifts[NAVLIFT_MAX];
static int       s_numNavLifts = 0;

void NavLift_Parse(const char *line, const Vector& a, const Vector& b)
{
    char        cab[64] = "", g1[64] = "", g2[64] = "";
    const char *p              = line;
    int         field          = 0;

    // skip the kind + 6 numbers, then read key=value tokens
    while (*p && field < 7) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        field++;
    }
    while (*p) {
        char tok[MAX_QPATH];
        int  n = 0;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        while (*p && !isspace((unsigned char)*p) && n < MAX_QPATH - 1) {
            tok[n++] = *p++;
        }
        tok[n] = 0;
        if (!n) {
            break;
        }
        if (!Q_stricmpn(tok, "cab=", 4)) {
            Q_strncpyz(cab, tok + 4, sizeof(cab));
        } else if (!Q_stricmpn(tok, "gate1=", 6)) {
            Q_strncpyz(g1, tok + 6, sizeof(g1));
        } else if (!Q_stricmpn(tok, "gate2=", 6)) {
            Q_strncpyz(g2, tok + 6, sizeof(g2));
        }
    }
    if (!cab[0] || !g1[0] || !g2[0]) {
        return;
    }

    navLift_t *lift = NULL;
    for (int i = 0; i < s_numNavLifts; i++) {
        if ((s_navLifts[i].end[0] - a).lengthSquared() < 1.0f && (s_navLifts[i].end[1] - b).lengthSquared() < 1.0f) {
            lift = &s_navLifts[i];
            break;
        }
    }
    if (!lift) {
        if (s_numNavLifts >= NAVLIFT_MAX) {
            return;
        }
        lift = &s_navLifts[s_numNavLifts++];
    }
    lift->end[0] = a;
    lift->end[1] = b;
    Q_strncpyz(lift->cab, cab, sizeof(lift->cab));
    Q_strncpyz(lift->gate[0], g1, sizeof(lift->gate[0]));
    Q_strncpyz(lift->gate[1], g2, sizeof(lift->gate[1]));
}

const navLift_t *NavLift_Find(const Vector& p, const Vector& q)
{
    // either order: Detour keeps the ends as written, but a path may cross the link in either direction
    for (int i = 0; i < s_numNavLifts; i++) {
        const navLift_t *l = &s_navLifts[i];
        if (((l->end[0] - p).lengthSquared() < Square(16) && (l->end[1] - q).lengthSquared() < Square(16))
            || ((l->end[0] - q).lengthSquared() < Square(16) && (l->end[1] - p).lengthSquared() < Square(16))) {
            return l;
        }
    }
    return NULL;
}

void NavLift_Clear()
{
    s_numNavLifts = 0;
}

// navlinks/<map>.txt and, when cm_variant is set, navlinks/<map>_<variant>.txt. Returns how many names were written.
static int ManualNav_FileNames(char out[3][MAX_QPATH])
{
    char        base[MAX_QPATH];
    const char *slash;
    cvar_t     *variant = gi.Cvar_Get("cm_variant", "", 0);
    int         blen;
    int         n = 0;

    // level.mapname is normally the bare name; strip any path / extension / _sml defensively (same as cmpatch).
    slash = strrchr(level.mapname.c_str(), '/');
    Q_strncpyz(base, slash ? slash + 1 : level.mapname.c_str(), sizeof(base));
    COM_StripExtension(base, base, sizeof(base));
    blen = strlen(base);
    if (blen > 4 && !Q_stricmp(base + blen - 4, "_sml")) {
        base[blen - 4] = 0;
    }

    Com_sprintf(out[n++], MAX_QPATH, "navlinks/%s.txt", base);
    if (variant && variant->string[0]) {
        Com_sprintf(out[n++], MAX_QPATH, "navlinks/%s_%s.txt", base, variant->string);
        // [bug-2865] GENERATED heat-map costs (docs/tools/gen_navheat.py) - kept apart from the hand-written file
        Com_sprintf(out[n++], MAX_QPATH, "navlinks/%s_%s_heat.txt", base, variant->string);
    }
    return n;
}

Container<ExtensionArea> NavigationMapExtension_ManualLinks::GetSupportedAreas() const
{
    Container<ExtensionArea> areas;
    // [bug-2862] a lift ride costs the wait (~2 x 13s cycle legs) - only worth taking when it is the real route
    areas.AddObject(ExtensionArea(RECAST_AREA_ELEVATOR, 12.0f));
    // [bug-2865] heat tiers: x2 (danger), x4 / x8 / x16 (stuck severity)
    areas.AddObject(ExtensionArea(RECAST_AREA_COST1, 2.0f));
    areas.AddObject(ExtensionArea(RECAST_AREA_COST1 + 1, 4.0f));
    areas.AddObject(ExtensionArea(RECAST_AREA_COST1 + 2, 8.0f));
    areas.AddObject(ExtensionArea(RECAST_AREA_COST4, 16.0f));
    // [bug-2887] steep-but-walkable ground (see RECAST_AREA_STEEP)
    areas.AddObject(ExtensionArea(RECAST_AREA_STEEP, 4.0f));
    areas.AddObject(ExtensionArea(RECAST_AREA_STEEP_LAST, 25.0f)); // only-way ground
    areas.AddObject(ExtensionArea(RECAST_AREA_HURT, 40.0f));       // [HZM bug-2926] inside a trigger_hurt
    return areas;
}

void NavigationMapExtension_ManualLinks::Handle(Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh)
{
    char files[3][MAX_QPATH];
    int  n = ManualNav_FileNames(files);

    for (int i = 0; i < n; i++) {
        ManualLinks_Load(files[i], points, polyMesh);
    }
}

/*
============
ManualBlocks_Apply

HZM [bug-2842] HAND-PLACED nav BLOCK volumes, from the same navlinks files:
    block minx miny minz  maxx maxy maxz     (game coordinates)
Every walkable span inside the box is marked RC_NULL_AREA on the compact heightfield (after erosion, before
regions), so the navmesh is CUT there. Why: the navmesh is built from the WORLD only - entity brush models (doors,
gates, script_objects) are not in it (BuildMeshesForEntities is a disabled path) - so a gate that stays CLOSED on
purpose is invisible to pathing and bots route straight into its bars. First use: m5l1a's map-end gate (user
2026-09-23: axis "try to get through the gate"; a soak bot's next path corner sat 521u behind it for minutes).
Called for the solo build and for every tile of the tiled build.
============
*/
// [HZM bot B3] the heat map's tier-1 cells are DEATH cells (docs/tools/gen_navheat.py) - ground where bots kept dying.
// Besides pricing them for A*, keep them as a list the bots read at runtime: walk that ground, eyes up.
static float s_navDanger[256][6];
static int   s_navDangerN = 0;
static str   s_navDangerMap;

bool NavDanger_IsInside(const float *p)
{
    for (int i = 0; i < s_navDangerN; i++) {
        const float *b = s_navDanger[i];
        if (p[0] >= b[0] && p[0] <= b[3] && p[1] >= b[1] && p[1] <= b[4] && p[2] >= b[2] && p[2] <= b[5]) {
            return true;
        }
    }
    return false;
}

/*
============
EntityBlocks_Apply

HZM [bug-2893] STATIC SOLID PROPS cut the mesh at build time, on every map. The mesh is built from the world only, so
a breakable fence, a parked vehicle (and its separate collision box), a fixed turret, a crate or a barrel is invisible
to routing; the runtime obstacle carve only excludes whole polys whose CENTRE the prop covers, which a thin fence line
or a prop on the edge of a large poly never does. Round 10: t1l1's paddock fence (14 func_fencepost rails along one
line) held five allied bots for ~4 minutes each - 236 BOTBLOCK samples naming it - and e3l2 / m5l2a / t3l1 bots piled
on a vehicle collision box, a turret and parked tanks. Each such entity present when the mesh is built nulls the
walkable spans under its box padded by a player half-width (s_propPad, 16u), floors from 80u below its underside to
its top - as a prop-area (bug-2901), not a deletion, so it can be released. A prop that later MOVES falls back to the runtime carve
(NavEntityBlock_IsBaked stops the two stacking while it has not moved). bot_navEntityBlocks 0 disables.
============
*/
static int     s_entBlockN = 0;
static int     s_entBlockNum[512];
static Vector  s_entBlockMin[512];
static Vector  s_entBlockMax[512];   // [HZM bug-2901] the box, for releasing it
static Entity *s_entBlockEnt[512];   // [HZM bug-2901] who it was (an entnum can be reused)
static bool    s_entBlockLive[512];  // [HZM bug-2901] still cut (false once released)
static str     s_entBlockMap;
// [bug-2902] prop pad = a player's half-width (16u). 8u let t1l1 bots scrape along the paddock fence (round 14: 117
// BOTBLOCK FencePost, stuck 3.3% -> 14.3%). The 16u pad was blamed for splitting t1l3 in round 13, but that split was
// the exploder props the MP script removes later - which the reversible cut now releases.
static const float s_propPad = 16.0f;

// [bug-3026] a BRUSH entity lower than a player's step is FLOOR: pmove steps onto brush models like world brushes. t1l3's
// comm-centre room floor is two pairs of script_object slabs (commfloorbefore / commfloorafter, 11-16u tall - the SP
// script swaps them when the wall blows); the named-ScriptSlave cut (bug-2902) excluded the whole room and its doorway
// threshold - the map's only crossing - and Push split in two (37-46% stuck, homes unreachable). Used by the build-time
// cut here AND the runtime obstacle carve (NavigationObstacleMap::IsValidEntity), which otherwise marks such a floor's
// polys BUSY (a 0.75-player box at the poly centre overlaps the slab). Only SOLID_BSP: a low BOX (t2l3's mp44
// script_model, 5u) is still a blocker pmove will not step onto. bot_navPropSlab 0 = treat them as blockers again.
bool NavEntity_IsSlab(const gentity_t *ge)
{
    static cvar_t *s_slab = NULL;
    if (!s_slab) {
        s_slab = gi.Cvar_Get("bot_navPropSlab", "1", 0);
    }
    return s_slab->integer && ge && ge->solid == SOLID_BSP && ge->r.absmax[2] - ge->r.absmin[2] < STEPSIZE
        && ge->r.absmax[0] - ge->r.absmin[0] >= 1.0f;
}

static bool EntityBlock_Kind(Entity *e)
{
    // (+ a solid script_model: t2l3's weapons/mp44.tik lying in a crater - a box 5u tall that pmove will not step onto - held
    // bots at full speed for minutes. Its targetname is the SP script's, which Push never runs; the navmesh exists only
    // for MP bots, so a placed script_model is static here. One that an MP script does move falls back to the carve.)
    // (+ an UN-NAMED script_object / brush ScriptSlave that is not a door: no script can address it, so nothing moves it -
    // t1l3's *177 at (2848 3198) held axis bots 123 BOTBLOCK samples in round 12. Named ones are lift cabs, gates and
    // doors (m4l2's elevator_cab / elevator_gate_*) and are left to the runtime carve.)
    // [bug-2902] ...and a NAMED one too, now that a cut is released the moment its entity moves (bug-2901): t1l3's
    // $panzer_map1 (*2, a brush tank) became round 14's worst trap (652 of 801 samples stuck against it). Except the
    // things whose REST position must stay routable - a lift cab / gate / door / platform the route rides or opens
    // (m4l2's elevator_cab + elevator_gate_*): left to the runtime carve.
    if (e->isSubclassOf(ScriptSlave) && !e->IsSubclassOfDoor() && !e->isSubclassOf(ScriptModel) && e->targetname.length()) {
        static const char *movers[] = {"elev", "lift", "cab", "gate", "door", "platform", "bridge", "train", "boat", "hatch"};
        for (size_t i = 0; i < sizeof(movers) / sizeof(movers[0]); i++) {
            if (Q_stristr(e->targetname.c_str(), movers[i])) {
                return false;
            }
        }
        return true;
    }
    return e->isSubclassOf(FencePost) || e->IsSubclassOfVehicle() || e->isSubclassOf(VehicleCollisionEntity)
        || e->isSubclassOf(BarrelObject) || e->isSubclassOf(CrateObject) || e->isSubclassOf(ScriptModel)
        || (e->isSubclassOf(ScriptSlave) && !e->IsSubclassOfDoor() && !e->targetname.length());
}

// a player spawn inside the prop's (padded) box: the prop evidently does not block there in play - e1l1's axis home
// spawn stands in a parked jeep's box and bots spawn and walk out of it - and cutting the floor would strand the spawn
static bool EntityBlock_CoversSpawn(const Vector& mn, const Vector& mx, float pad)
{
    for (int i = 1; i <= level.m_SimpleArchivedEntities.NumObjects(); i++) {
        SimpleArchivedEntity *se = level.m_SimpleArchivedEntities.ObjectAt(i);
        if (!se || !se->isSubclassOf(PlayerStart)) {
            continue;
        }
        const Vector& o = se->origin;
        if (o.x >= mn.x - pad && o.x <= mx.x + pad && o.y >= mn.y - pad && o.y <= mx.y + pad && o.z >= mn.z - 80.0f
            && o.z <= mx.z + 16.0f) {
            return true;
        }
    }
    return false;
}

bool NavEntityBlock_IsBaked(int entnum, const Vector& absmin)
{
    for (int i = 0; i < s_entBlockN; i++) {
        if (s_entBlockNum[i] == entnum && s_entBlockLive[i]) {
            return (s_entBlockMin[i] - absmin).lengthSquared() < 1.0f;
        }
    }
    return false;
}

class NavPropRelease : public dtPolyQuery
{
public:
    Container<dtPolyRef> refs;
    void                 process(const dtMeshTile *tile, dtPoly **polys, dtPolyRef *prefs, int count) override
    {
        for (int i = 0; i < count; i++) {
            if (polys[i]->getArea() == RECAST_AREA_PROP) {
                refs.AddObject(prefs[i]);
            }
        }
    }
};

/*
============
NavEntityBlocks_Update

HZM [bug-2901] release a cut prop the moment it stops being one - removed, made non-solid, or moved. t1l3 round 13: the
comm-centre wall's exploder props (radio, phone, switches, chair) were cut at build, then mp_push_blockers removed them
~1-15s in to open the blown wall - the cut stayed and split the map in two (one death in the round). Its polys go back to
plain walkable ground; a polygon still under another live prop's box stays cut. Called every frame from the obstacle map.
============
*/
static int s_propReleased = 0; // [HZM bug-2901] releases since the last nav_dump rewrite

int NavEntityBlocks_TakeReleased()
{
    const int n    = s_propReleased;
    s_propReleased = 0;
    return n;
}

void NavEntityBlocks_Update()
{
    if (s_entBlockMap != level.mapname) {
        return;
    }
    dtNavMesh      *nm = navigationMap.GetNavMesh();
    dtNavMeshQuery *q  = navigationMap.GetNavMeshQuery();
    if (!nm || !q) {
        return;
    }
    const float pad = s_propPad;
    for (int i = 0; i < s_entBlockN; i++) {
        if (!s_entBlockLive[i]) {
            continue;
        }
        gentity_t *ge   = &g_entities[s_entBlockNum[i]];
        const bool gone = !ge->inuse || ge->entity != s_entBlockEnt[i]
                       || (ge->solid != SOLID_BBOX && ge->solid != SOLID_BSP) || !(ge->r.contents & MASK_PLAYERSOLID)
                       || (Vector(ge->r.absmin) - s_entBlockMin[i]).lengthSquared() > 1.0f;
        if (!gone) {
            continue;
        }
        s_entBlockLive[i] = false;

        const Vector mn = s_entBlockMin[i] - Vector(pad, pad, 80.0f);
        const Vector mx = s_entBlockMax[i] + Vector(pad, pad, 8.0f);
        float        gc[3] = {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
        float        rc[3], rh[3];
        ConvertGameToRecastCoord(gc, rc);
        rh[0] = (mx.x - mn.x) * 0.5f;
        rh[1] = (mx.z - mn.z) * 0.5f;
        rh[2] = (mx.y - mn.y) * 0.5f;
        NavPropRelease rq;
        dtQueryFilter  all;
        all.setIncludeFlags(0xffff);
        all.setExcludeFlags(0);
        q->queryPolygons(rc, rh, &all, &rq);

        int freed = 0;
        for (int k = 1; k <= rq.refs.NumObjects(); k++) {
            const dtPolyRef   ref  = rq.refs.ObjectAt(k);
            const dtMeshTile *tile = NULL;
            const dtPoly     *poly = NULL;
            if (dtStatusFailed(nm->getTileAndPolyByRef(ref, &tile, &poly))) {
                continue;
            }
            // centroid (game coords) - still under another live prop? then it stays cut
            float c[3] = {0, 0, 0};
            for (int v = 0; v < poly->vertCount; v++) {
                const float *pv = &tile->verts[poly->verts[v] * 3];
                c[0] += pv[0];
                c[1] += pv[1];
                c[2] += pv[2];
            }
            for (int a = 0; a < 3; a++) {
                c[a] /= poly->vertCount;
            }
            float gpos[3];
            ConvertRecastToGameCoord(c, gpos);
            bool held = false;
            for (int j = 0; j < s_entBlockN && !held; j++) {
                if (j == i || !s_entBlockLive[j]) {
                    continue;
                }
                held = gpos[0] >= s_entBlockMin[j].x - pad && gpos[0] <= s_entBlockMax[j].x + pad
                    && gpos[1] >= s_entBlockMin[j].y - pad && gpos[1] <= s_entBlockMax[j].y + pad
                    && gpos[2] >= s_entBlockMin[j].z - 80.0f && gpos[2] <= s_entBlockMax[j].z + 8.0f;
            }
            if (held) {
                continue;
            }
            unsigned short fl = 0;
            nm->getPolyFlags(ref, &fl);
            nm->setPolyFlags(ref, (unsigned short)(fl & ~RECAST_POLYFLAG_PROP));
            nm->setPolyArea(ref, 63);
            freed++;
        }
        s_propReleased++;
        gi.Printf(
            "^~^~^ NAVPROP release ent=%d at=(%.0f %.0f %.0f) polys=%d\n", s_entBlockNum[i],
            (s_entBlockMin[i].x + s_entBlockMax[i].x) * 0.5f, (s_entBlockMin[i].y + s_entBlockMax[i].y) * 0.5f,
            s_entBlockMin[i].z, freed
        );
    }
}

/*
============
HurtBlocks_Apply

HZM [bug-2926] HURT VOLUMES. A trigger_hurt (e1l1's burning wrecks: 10 damage a tick) is ordinary floor to the navmesh,
so bots routed straight through it - 142 hurt ticks in one 12-min e1l1 soak, some of them dying of it. Every TRIGGERABLE,
damaging trigger_hurt present when the mesh is built re-areas the walkable spans in its box (padded by a player
half-width, from 40u below its underside) as RECAST_AREA_HURT, costed x40: A* goes round it while any other way exists,
and never loses it - no cut, so no connectivity change (a fire across the only street is still walked). Box volumes over
16000u across (whole-map kill triggers) are left alone. bot_navHurt 0 = off.
v2: a trigger_hurt is a BRUSH model and its box can be far bigger than its brushes - e1l1 *84 is a hillside wedge whose
box (1430 x 4650u, z 254-1666) reaches down over the allied spawn valley, and v1 re-costed all of it x40 (bots spent 9-12%
of their time inside that box and were never hurt there). Now only the spans where a standing player's box would TOUCH
the trigger's brushes (gi.PointBrushnum on its inline model, at the player box's centre and corners) are re-areaed.
============
*/
static int HurtBlocks_Apply(rcContext *ctx, rcCompactHeightfield& chf)
{
    static cvar_t *s_on = NULL;
    static str     s_logMap;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_navHurt", "1", 0);
    }
    if (!s_on->integer) {
        return 0;
    }
    const bool bLog = s_logMap != level.mapname; // once per map, not once per tile
    s_logMap        = level.mapname;
    int n           = 0;
    for (int i = 0; i < globals.num_entities && i < ENTITYNUM_WORLD; i++) {
        gentity_t *ge = &g_entities[i];
        if (!ge->inuse || !ge->entity || ge->client || !ge->entity->isSubclassOf(TriggerHurt)) {
            continue;
        }
        TriggerHurt *th = static_cast<TriggerHurt *>(ge->entity);
        if (!th->HZM_IsTriggerable() || th->HZM_GetDamage() <= 0.0f) {
            continue;
        }
        const Vector mn = ge->r.absmin;
        const Vector mx = ge->r.absmax;
        if (mx.x - mn.x < 1.0f || mx.y - mn.y < 1.0f || mx.x - mn.x > 16000.0f || mx.y - mn.y > 16000.0f) {
            continue; // not linked yet, or a whole-map volume
        }
        const float pad = 16.0f;
        float       rmin[3], rmax[3];
        // game (x, y, z) -> recast (x, z, -y): the y extent flips, so min/max swap on that axis
        rmin[0] = mn.x - pad;
        rmin[1] = mn.z - 40.0f;
        rmin[2] = -(mx.y + pad);
        rmax[0] = mx.x + pad;
        rmax[1] = mx.z;
        rmax[2] = -(mn.y - pad);
        // [bug-2926 v2] the spans in that box from which a standing player would touch the trigger's BRUSHES (not the whole
        // box). Triggers have r.contents 0, so an entity clip (gi.ClipToEntity) rejects them outright: test the inline
        // model's own brushes instead (gi.PointBrushnum, model space = world - origin) at the player box's centre and
        // corners, low and high. A trigger with no brush model (a scripted bbox trigger) is its box: mark the box.
        const int mi = ge->s.modelindex;
        if (!ge->r.bmodel || mi <= 0) {
            rcMarkBoxArea(ctx, rmin, rmax, RECAST_AREA_HURT, chf);
        } else {
            const Vector org = ge->s.origin;
            const int    x0  = Q_max(0, (int)floorf((rmin[0] - chf.bmin[0]) / chf.cs));
            const int    x1  = Q_min(chf.width - 1, (int)floorf((rmax[0] - chf.bmin[0]) / chf.cs));
            const int    z0  = Q_max(0, (int)floorf((rmin[2] - chf.bmin[2]) / chf.cs));
            const int    z1  = Q_min(chf.height - 1, (int)floorf((rmax[2] - chf.bmin[2]) / chf.cs));
            static const float off[5][2] = {
                {0, 0},
                {-15, -15},
                {15, -15},
                {-15, 15},
                {15, 15}
            };
            static const float hgt[3] = {4.0f, 48.0f, 88.0f};
            for (int cz = z0; cz <= z1; cz++) {
                for (int cx = x0; cx <= x1; cx++) {
                    const rcCompactCell& cell = chf.cells[cx + cz * chf.width];
                    for (int si = (int)cell.index, se = (int)(cell.index + cell.count); si < se; si++) {
                        if (chf.areas[si] == RC_NULL_AREA) {
                            continue;
                        }
                        const float fz = chf.bmin[1] + chf.spans[si].y * chf.ch; // the floor (recast y = game z)
                        if (fz < mn.z - 92.0f || fz > mx.z) {
                            continue;
                        }
                        const float wx = chf.bmin[0] + (cx + 0.5f) * chf.cs;
                        const float wy = -(chf.bmin[2] + (cz + 0.5f) * chf.cs);
                        bool        hit = false;
                        for (int k = 0; k < 5 && !hit; k++) {
                            for (int h = 0; h < 3 && !hit; h++) {
                                const vec3_t lp = {wx + off[k][0] - org.x, wy + off[k][1] - org.y, fz + hgt[h] - org.z};
                                hit             = gi.PointBrushnum(lp, mi) >= 0;
                            }
                        }
                        if (hit) {
                            chf.areas[si] = RECAST_AREA_HURT;
                        }
                    }
                }
            }
        }
        n++;
        if (bLog) {
            gi.Printf(
                "^~^~^ NAVHURT ent=%d tn=%s dmg=%.0f min=(%.0f %.0f %.0f) max=(%.0f %.0f %.0f)\n", i,
                ge->entity->targetname.c_str(), th->HZM_GetDamage(), mn.x, mn.y, mn.z, mx.x, mx.y, mx.z
            );
        }
    }
    return n;
}

static int EntityBlocks_Apply(rcContext *ctx, rcCompactHeightfield& chf)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_navEntityBlocks", "1", 0);
    }
    if (s_entBlockMap != level.mapname) { // a new map: a fresh list (tiled builds call this per tile)
        s_entBlockMap = level.mapname;
        s_entBlockN   = 0;
    }
    if (!s_on->integer) {
        return 0;
    }

    static str     s_dbgMap;
    static cvar_t *s_dbg = NULL;
    if (!s_dbg) {
        s_dbg = gi.Cvar_Get("bot_navEntityBlocksDebug", "0", 0);
    }
    int n = 0;
    for (int i = 0; i < globals.num_entities && i < ENTITYNUM_WORLD; i++) {
        gentity_t *ge = &g_entities[i];
        if (!ge->inuse || !ge->entity || ge->entity == world || ge->client) {
            continue;
        }
        if (s_dbg->integer && s_dbgMap != level.mapname && ge->entity->isSubclassOf(ScriptModel)) {
            gi.Printf(
                "^~^~^ NAVPROP cand ent=%d class=%s model=%s tn=%s solid=%d contents=%x min=(%.0f %.0f %.0f) max=(%.0f %.0f %.0f)\n",
                i, ge->entity->getClassname(), ge->entity->model.c_str(), ge->entity->targetname.c_str(), ge->solid,
                ge->r.contents, ge->r.absmin[0], ge->r.absmin[1], ge->r.absmin[2], ge->r.absmax[0], ge->r.absmax[1],
                ge->r.absmax[2]
            );
        }
        if ((ge->solid != SOLID_BBOX && ge->solid != SOLID_BSP) || !(ge->r.contents & MASK_PLAYERSOLID)) {
            continue;
        }
        if (!EntityBlock_Kind(ge->entity)) {
            continue;
        }
        const Vector mn = ge->r.absmin;
        const Vector mx = ge->r.absmax;
        if (mx.x - mn.x > 1024.0f || mx.y - mn.y > 1024.0f || mx.x - mn.x < 1.0f || mx.y - mn.y < 1.0f
            || mx.z - mn.z > 512.0f) {
            continue; // not a prop (or not linked yet) - t1l1's searchlight models report boxes 16000u tall
        }
        if (NavEntity_IsSlab(ge)) { // [bug-3026] floor, not a blocker (see NavEntity_IsSlab)
            if (s_dbg->integer && s_dbgMap != level.mapname) {
                gi.Printf(
                    "^~^~^ NAVPROP skip-slab ent=%d class=%s tn=%s h=%.0f\n", i, ge->entity->getClassname(),
                    ge->entity->targetname.c_str(), mx.z - mn.z
                );
            }
            continue;
        }
        const float pad = s_propPad; // [bug-2902] player half-width: a route along a prop keeps a PLAYER clear of it
        if (EntityBlock_CoversSpawn(mn, mx, pad)) {
            if (s_dbg->integer && s_dbgMap != level.mapname) {
                gi.Printf(
                    "^~^~^ NAVPROP skip-spawn ent=%d class=%s tn=%s\n", i, ge->entity->getClassname(),
                    ge->entity->targetname.c_str()
                );
            }
            continue;
        }
        float       rmin[3], rmax[3];
        // game (x, y, z) -> recast (x, z, -y): the y extent flips, so min/max swap on that axis
        rmin[0] = mn.x - pad;
        rmin[1] = mn.z - 80.0f;
        rmin[2] = -(mx.y + pad);
        rmax[0] = mx.x + pad;
        rmax[1] = mx.z;
        rmax[2] = -(mn.y - pad);
        rcMarkBoxArea(ctx, rmin, rmax, RECAST_AREA_PROP, chf); // [bug-2901] its own polys, excluded - not deleted
        n++;

        bool known = false;
        for (int k = 0; k < s_entBlockN && !known; k++) {
            known = s_entBlockNum[k] == i;
        }
        if (!known && s_dbg->integer) {
            gi.Printf(
                "^~^~^ NAVPROP block ent=%d class=%s model=%s tn=%s min=(%.0f %.0f %.0f) max=(%.0f %.0f %.0f)\n", i,
                ge->entity->getClassname(), ge->entity->model.c_str(), ge->entity->targetname.c_str(), mn.x, mn.y, mn.z,
                mx.x, mx.y, mx.z
            );
        }
        if (!known && s_entBlockN < 512) {
            s_entBlockNum[s_entBlockN]  = i;
            s_entBlockMin[s_entBlockN]  = mn;
            s_entBlockMax[s_entBlockN]  = mx;
            s_entBlockEnt[s_entBlockN]  = ge->entity;
            s_entBlockLive[s_entBlockN] = true;
            s_entBlockN++;
        }
    }
    s_dbgMap = level.mapname;
    return n;
}

void ManualBlocks_Apply(rcContext *ctx, rcCompactHeightfield& chf)
{
    static str s_lastLogged;
    char       files[3][MAX_QPATH];
    int        n       = ManualNav_FileNames(files);
    int        applied = 0;
    int        costed  = 0; // [bug-2865]

    if (s_navDangerMap != level.mapname) { // [HZM bot B3] a new map: a fresh danger list (tiled builds call this per tile)
        s_navDangerMap = level.mapname;
        s_navDangerN   = 0;
    }

    for (int f = 0; f < n; f++) {
        char       *buf = NULL;
        const char *line;

        if (gi.FS_ReadFile(files[f], (void **)&buf, qtrue) <= 0 || !buf) {
            continue;
        }
        line = buf;
        while (*line) {
            const char *eol = strchr(line, '\n');
            size_t      len = eol ? (size_t)(eol - line) : strlen(line);
            char        tmp[256];
            char        kind[16];
            float       a[3], b[3];
            char       *hash;

            if (len >= sizeof(tmp)) {
                len = sizeof(tmp) - 1;
            }
            memcpy(tmp, line, len);
            tmp[len] = 0;
            hash     = strchr(tmp, '#');
            if (hash) {
                *hash = 0;
            }

            float v[7];
            int   nv = sscanf(tmp, "%15s %f %f %f %f %f %f %f", kind, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
            if (nv == 8 && !Q_stricmp(kind, "blockline")) {
                // [bug-2848] blockline x1 y1  x2 y2  halfwidth  zmin zmax: an ORIENTED strip along a (diagonal) gate
                // or fence, extended by halfwidth past both ends so it seals the posts. An axis-aligned box around a
                // diagonal gate is too fat and leaves corners bots wedge into (m4l2 track gates).
                float dx = v[2] - v[0], dy = v[3] - v[1];
                float len = sqrt(dx * dx + dy * dy);
                float hw  = v[4];
                if (len > 1.0f && hw > 0.0f) {
                    float ux = dx / len, uy = dy / len; // along the line
                    float px = -uy, py = ux;            // perpendicular
                    float g[4][2] = {
                        {v[0] - ux * hw + px * hw, v[1] - uy * hw + py * hw},
                        {v[2] + ux * hw + px * hw, v[3] + uy * hw + py * hw},
                        {v[2] + ux * hw - px * hw, v[3] + uy * hw - py * hw},
                        {v[0] - ux * hw - px * hw, v[1] - uy * hw - py * hw},
                    };
                    float verts[12];
                    for (int k = 0; k < 4; k++) {
                        verts[k * 3 + 0] = g[k][0];  // recast x = game x
                        verts[k * 3 + 1] = v[5];     // recast y = height (unused by the xz test)
                        verts[k * 3 + 2] = -g[k][1]; // recast z = -game y
                    }
                    rcMarkConvexPolyArea(ctx, verts, 4, Q_min(v[5], v[6]), Q_max(v[5], v[6]), RC_NULL_AREA, chf);
                    applied++;
                }
            } else if (nv == 8 && !Q_stricmp(kind, "cost")) {
                // [bug-2865] cost minx miny minz maxx maxy maxz tier(1-4): re-area the box's walkable spans to a
                // heat tier (NULL spans stay NULL - a block always wins)
                const int tier = (int)v[6];
                if (tier >= 1 && tier <= 4) {
                    float rmin[3], rmax[3];
                    rmin[0] = Q_min(v[0], v[3]);
                    rmin[1] = Q_min(v[2], v[5]);
                    rmin[2] = -Q_max(v[1], v[4]);
                    rmax[0] = Q_max(v[0], v[3]);
                    rmax[1] = Q_max(v[2], v[5]);
                    rmax[2] = -Q_min(v[1], v[4]);
                    rcMarkBoxArea(ctx, rmin, rmax, (unsigned char)(RECAST_AREA_COST1 + tier - 1), chf);
                    costed++;
                    if (tier == 1 && s_navDangerN < 256) { // [HZM bot B3] remember the death cell (once per map)
                        float b[6] = {Q_min(v[0], v[3]), Q_min(v[1], v[4]), Q_min(v[2], v[5]),
                                      Q_max(v[0], v[3]), Q_max(v[1], v[4]), Q_max(v[2], v[5])};
                        bool  dup  = false;
                        for (int d = 0; d < s_navDangerN && !dup; d++) {
                            dup = !memcmp(s_navDanger[d], b, sizeof(b));
                        }
                        if (!dup) {
                            memcpy(s_navDanger[s_navDangerN++], b, sizeof(b));
                        }
                    }
                }
            } else if (nv >= 7 && !Q_stricmp(kind, "block")) {
                a[0] = v[0]; a[1] = v[1]; a[2] = v[2];
                b[0] = v[3]; b[1] = v[4]; b[2] = v[5];
                float gmin[3], gmax[3], rmin[3], rmax[3];
                for (int k = 0; k < 3; k++) {
                    gmin[k] = Q_min(a[k], b[k]);
                    gmax[k] = Q_max(a[k], b[k]);
                }
                // game (x, y, z) -> recast (x, z, -y): the y extent flips, so min/max swap on that axis
                rmin[0] = gmin[0];
                rmin[1] = gmin[2];
                rmin[2] = -gmax[1];
                rmax[0] = gmax[0];
                rmax[1] = gmax[2];
                rmax[2] = -gmin[1];
                rcMarkBoxArea(ctx, rmin, rmax, RC_NULL_AREA, chf);
                applied++;
            }

            if (!eol) {
                break;
            }
            line = eol + 1;
        }
        gi.FS_FreeFile(buf);
    }

    // [HZM bug-2926] hurt volumes after the heat costs (a fire outranks a heat tier), before the props
    HurtBlocks_Apply(ctx, chf);
    // [HZM bug-2903] props LAST: a heat 'cost' box re-areas every non-null span in it, so applied after the prop cut it
    // turned t1l1's fence footprint back into plain costed ground (the fence line is heat-costed from the rounds when
    // bots stuck on it) - round 15 bots pushed into the rails again (171 BOTBLOCK FencePost, stuck 13.5%)
    EntityBlocks_Apply(ctx, chf); // [HZM bug-2893]

    // once per map, not once per tile
    if (s_lastLogged != level.mapname) {
        s_lastLogged = level.mapname;
        gi.Printf(
            "  Navmesh: %d manual nav block volume(s), %d heat cost volume(s), %d static prop block(s) applied\n", applied,
            costed, s_entBlockN
        );
    }
}
