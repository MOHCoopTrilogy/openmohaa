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
 * @file navigation_recast_load.cpp
 * @brief Modern navigation system using Recast and Detour.
 *
 * Parse BSP file, get surface triangles and rasterize them to create a navigation mesh.
 */

#include "g_local.h"
#include "navigation_recast_load.h"
#include "navigation_recast_load_ext.h"
#include "navigation_recast_obstacle.h"
#include "navigation_recast_path.h"
#include "navigation_recast_config.h"
#include "navigation_recast_config_ext.h" // [bug-2865] IsRecastWalkableArea

// [HZM bug-2887] re-mark walkable triangles steeper than bot_navSteepDeg as RECAST_AREA_STEEP (costed x4 by the
// ManualLinks extension). Recast coordinates are y-up; the normal test mirrors rcMarkWalkableTriangles.
static void MarkSteepTriangles(const float *verts, const int *tris, int nt, unsigned char *areas)
{
    static cvar_t *s_steep = NULL;
    static cvar_t *s_hard  = NULL;
    if (!s_steep) {
        s_steep = gi.Cvar_Get("bot_navSteepDeg", "33", 0);
        s_hard  = gi.Cvar_Get("bot_navSteepHardDeg", "38", 0);
    }
    const float deg = s_steep->value;
    if (deg <= 0.0f || deg >= 45.0f) {
        return; // 0 = off
    }
    const float thr     = cos(DEG2RAD(deg));
    const float hardDeg = s_hard->value;
    const float thrHard = (hardDeg > deg && hardDeg < 45.6f) ? cos(DEG2RAD(hardDeg)) : -1.0f; // -1: no last-resort tier
    for (int i = 0; i < nt; i++) {
        if (areas[i] != 63 /* RC_WALKABLE_AREA */) {
            continue;
        }
        const float *v0 = &verts[tris[i * 3 + 0] * 3];
        const float *v1 = &verts[tris[i * 3 + 1] * 3];
        const float *v2 = &verts[tris[i * 3 + 2] * 3];
        float        e0[3], e1[3], n[3];
        for (int k = 0; k < 3; k++) {
            e0[k] = v1[k] - v0[k];
            e1[k] = v2[k] - v0[k];
        }
        n[0]            = e0[1] * e1[2] - e0[2] * e1[1];
        n[1]            = e0[2] * e1[0] - e0[0] * e1[2];
        n[2]            = e0[0] * e1[1] - e0[1] * e1[0];
        const float len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0f && n[1] / len < thr) {
            areas[i] = (n[1] / len < thrHard) ? RECAST_AREA_STEEP_LAST : RECAST_AREA_STEEP;
        }
    }
}
#include "navigation_recast_helpers.h"
#include "navigation_recast_debug.h"
#include "../script/scriptexception.h"
#include "navigate.h"
#include "debuglines.h"
#include "level.h"

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourNode.h"

NavigationMap navigationMap;

/*
============
MarkHeadClearance

[HZM bug-2956] HEAD CLEARANCE. The mesh keeps agentRadius 8 off whatever it cannot walk (bug-2833/2829: more than
that closed real doorways and spawn pinches), but a player is a 15u cylinder from a step up to his head (18..94u).
Against a WALL the 7u deficit is a scrape the corner pad and pmove's slide absorb; against an OVERHANG - a bulging
rock face, a desk top, the underside of stairs - Recast only tests the clearance straight above each cell, so it lays
walkable floor right up to 8u from where the ceiling drops below head height, and there a standing body cannot be at
all (e1l1 (-2560,-3888): the route's next leg ran under a cliff overhang where only a crouched body fits - nav_fitprobe
'c' - and bots stood pinned there crouched for minutes; bug-2914's 'caught on desks'). Here, before the low-clearance
filter: a walkable floor span one of whose 8 neighbour cells has a floor at the same level (within a step) under a
CEILING at body height (more than a step and less than a standing player above OUR floor) is not walkable either; the
normal erosion then keeps the mesh 16-24u from the low ceiling, which the 15u body needs. Walls - a neighbour with no
floor at our level - are left to the 8u erosion exactly as before. bot_navHeadClear 1 = on.
DEFAULT OFF (2026-09-25): it fixed e1l1's notch (the portal went; the validated straight link took over), but the
Push connectivity sweep failed - new unreachable regions on m4l2 (48.5M sq u, 645 polys at (5475,1981,-103)) and
m5l2a (87.5M sq u at (710,2485,65)) with an 8-neighbour rule and the same with this stricter 4-neighbour/low-cell
rule: extra clearance near low ceilings closes real low/arched passages - the bug-2829 trade-off again.
============
*/
static int s_iHeadClearMarked = 0;

static void MarkHeadClearance(rcHeightfield& hf, int walkableHeight, int walkableClimb)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_navHeadClear", "0", 0);
    }
    if (!s_on->integer) {
        return;
    }
    const int w = hf.width;
    const int h = hf.height;
    int       cap = 1024, n = 0;
    rcSpan  **marks = new rcSpan *[cap];
    for (int z = 0; z < h; z++) {
        for (int x = 0; x < w; x++) {
            for (rcSpan *sp = hf.spans[x + z * w]; sp; sp = sp->next) {
                if (sp->area == RC_NULL_AREA) {
                    continue;
                }
                const int floor = (int)sp->smax;
                const int top   = sp->next ? (int)sp->next->smin : RC_SPAN_MAX_HEIGHT;
                if (top - floor < walkableHeight) {
                    continue; // filtered as too low by rcFilterWalkableLowHeightSpans anyway
                }
                bool            hit     = false;
                static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int d = 0; d < 4 && !hit; d++) {
                    const int nx = x + dirs[d][0], nz = z + dirs[d][1];
                    if (nx < 0 || nz < 0 || nx >= w || nz >= h) {
                        continue;
                    }
                    for (rcSpan *ns = hf.spans[nx + nz * w]; ns && !hit; ns = ns->next) {
                        if (abs((int)ns->smax - floor) > walkableClimb) {
                            continue;
                        }
                        // the neighbour is a LOW-CEILING cell itself (it will be filtered as unwalkable), and that
                        // ceiling is at body height over OUR floor too
                        const int ceil = ns->next ? (int)ns->next->smin : RC_SPAN_MAX_HEIGHT;
                        if (ceil - (int)ns->smax < walkableHeight && ceil - floor < walkableHeight
                            && ceil - floor > walkableClimb) {
                            hit = true;
                        }
                    }
                }
                if (hit) {
                    if (n == cap) {
                        rcSpan **bigger = new rcSpan *[cap * 2];
                        memcpy(bigger, marks, sizeof(rcSpan *) * cap);
                        delete[] marks;
                        marks = bigger;
                        cap *= 2;
                    }
                    marks[n++] = sp;
                }
            }
        }
    }
    for (int i = 0; i < n; i++) {
        marks[i]->area = RC_NULL_AREA; // after the scan, so a mark never cascades into the next cell
    }
    s_iHeadClearMarked += n;
    delete[] marks;
}

/// Recast build context.
class RecastBuildContext : public rcContext
{
protected:
    virtual void doResetLog() override {}

    virtual void doLog(const rcLogCategory category, const char *msg, const int len) override
    {
        gi.DPrintf("Recast (category %d): %s\n", (int)category, msg);
    }
};

/*
============
NavigationMap::GatherOffMeshPoints
============
*/
/*
============
NavExcludeSlivers

HZM [2026-09-26, bug-2988] ISOLATED SLIVERS. Recast's agent-radius erosion leaves a one-cell strip of "walkable" ground on
top of a thin obstacle when two brushes stack there - m4l2's upper-floor edge: a 10u common/clip railing (brush 2782,
z 304-356) beside a 16u nodraw block made a strip of 8u-wide ground polys at z 357-361 (8x48, 8x56, then 8u deep along
the rail) joined only to each other. The link generators then hung a web of auto JUMP links onto it from the floor below
and HIGH_FALL links off it (10-25 per poly), and routes used it as a shortcut: bots jumped onto the rail top and failed
the 308u drop over and over (T2 regression soak: 4 axis bots, 145 stuck samples in one cell). An ISLAND of ground polys
(connected by shared edges only - a tile portal means it may continue next door, so it is kept) in which every poly is
thinner than a player can stand on (under 20u across) and that is under 16384 sq u in all is taken out BEFORE the links
are generated, so no link touches it. A real ledge a player can use is wider than that; a narrow walkway that is part of
a floor joins the floor, so its island is not all thin. (v1 tested single polys with no neighbour and missed the rail:
its polys neighbour each other.) bot_navSlivers 0 = off.
============
*/
// the polygon in the ground plane (recast x / z, world units): its area, and its width = the smallest extent across it,
// over every edge direction (a convex polygon's minimum width lies across one of its edges); false = degenerate
static bool NavPolyShape(const rcPolyMesh *pm, int i, float& width, float& area, Vector& at)
{
    const int             nvp = pm->nvp;
    const float           cs  = pm->cs;
    const unsigned short *p   = &pm->polys[i * nvp * 2];
    float                 px[DT_VERTS_PER_POLYGON], pz[DT_VERTS_PER_POLYGON], py = 0;
    int                   nv = 0;
    for (int j = 0; j < nvp && j < DT_VERTS_PER_POLYGON; j++) {
        if (p[j] == RC_MESH_NULL_IDX) {
            break;
        }
        const unsigned short *v = &pm->verts[p[j] * 3];
        px[nv]                  = pm->bmin[0] + v[0] * cs;
        pz[nv]                  = pm->bmin[2] + v[2] * cs;
        py                      = pm->bmin[1] + v[1] * pm->ch;
        nv++;
    }
    if (nv < 3) {
        return false;
    }
    area = 0;
    for (int j = 0; j < nv; j++) {
        const int k = (j + 1) % nv;
        area += px[j] * pz[k] - px[k] * pz[j];
    }
    area  = fabs(area) * 0.5f;
    width = 1e9f;
    for (int j = 0; j < nv; j++) {
        const int   k  = (j + 1) % nv;
        const float ex = px[k] - px[j], ez = pz[k] - pz[j];
        const float el = sqrtf(ex * ex + ez * ez);
        if (el < 0.01f) {
            continue;
        }
        float fFar = 0;
        for (int m = 0; m < nv; m++) {
            fFar = Q_max(fFar, fabs((px[m] - px[j]) * ez - (pz[m] - pz[j]) * ex) / el);
        }
        width = Q_min(width, fFar);
    }
    at = Vector(px[0], -pz[0], py);
    return true;
}

static int NavExcludeSlivers(rcPolyMesh *pm, const char *where)
{
    static cvar_t *s_on = NULL;
    if (!s_on) {
        s_on = gi.Cvar_Get("bot_navSlivers", "1", 0);
    }
    if (!s_on->integer || !pm || pm->npolys <= 0) {
        return 0;
    }
    // [bug-2988 v2] by ISLAND: flood each connected set of ground polys over shared edges; an island with no tile portal
    // (it could continue in the next tile), every poly under 20u across and under 16384 sq u in all is a rail / wall top
    const int nvp   = pm->nvp;
    const int np    = pm->npolys;
    int      *seen  = new int[np];
    int      *queue = new int[np];
    for (int i = 0; i < np; i++) {
        seen[i] = 0;
    }
    int n = 0;
    for (int i = 0; i < np; i++) {
        if (seen[i] || !IsRecastWalkableArea(pm->areas[i])) {
            continue;
        }
        int   qn = 0, qh = 0;
        bool  bPortal = false, bThin = true;
        float total = 0, wmax = 0;
        Vector at0;
        queue[qn++] = i;
        seen[i]     = 1;
        while (qh < qn) {
            const int             c = queue[qh++];
            const unsigned short *p = &pm->polys[c * nvp * 2];
            float                 w = 0, ar = 0;
            Vector                at;
            if (!NavPolyShape(pm, c, w, ar, at)) {
                bThin = false; // degenerate: leave the island alone
            } else {
                if (qh == 1) {
                    at0 = at;
                }
                total += ar;
                wmax = Q_max(wmax, w);
                if (w >= 20.0f) {
                    bThin = false;
                }
            }
            for (int j = 0; j < nvp; j++) {
                if (p[j] == RC_MESH_NULL_IDX) {
                    break;
                }
                const unsigned short nb = p[nvp + j];
                if (nb == RC_MESH_NULL_IDX) {
                    continue;
                }
                if (nb & 0x8000) {
                    bPortal = true; // a portal to the next tile
                    continue;
                }
                if (nb < np && !seen[nb]) {
                    seen[nb]      = 1;
                    queue[qn++]   = nb;
                }
            }
        }
        if (bPortal || !bThin || total >= 16384.0f) {
            continue;
        }
        for (int k = 0; k < qn; k++) {
            pm->areas[queue[k]] = RC_NULL_AREA;
        }
        n += qn;
        gi.Printf(
            "^~^~^ NAVSLIVER %s at=(%.0f %.0f %.0f) polys=%d w=%.0f area=%.0f\n", where, at0.x, at0.y, at0.z, qn, wmax, total
        );
    }
    delete[] seen;
    delete[] queue;
    return n;
}

void NavigationMap::GatherOffMeshPoints(Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh)
{
    size_t i;

    //
    // Look for extensions and extend the navigation map
    //

    for (i = 1; i <= extensions.NumObjects(); i++) {
        INavigationMapExtension *navExtension = extensions.ObjectAt(i);
        navExtension->Handle(points, polyMesh);
    }
}

/*
============
NavigationMap::InitializeNavMesh
============
*/
void NavigationMap::InitializeNavMesh(RecastBuildContext& buildContext, const navMap_t& navMap)
{
    dtNavMeshParams params;
    dtStatus        status;

    // [bug-2831 tiled build] orig = world min corner; a tile spans tileSize CELLS = tileSize*cellSize WORLD units.
    // BuildWorldMesh addTile()s at grid indices computed from this same orig/tileWidth. maxTiles 512 (9 bits) +
    // maxPolys 8192/tile (13 bits) leaves 10 salt bits for the 32-bit dtPolyRef (DT_POLYREF64 is off).
    params.orig[0] = MIN_MAP_BOUNDS;
    params.orig[1] = MIN_MAP_BOUNDS;
    params.orig[2] = MIN_MAP_BOUNDS;
    if (NavigationMapConfiguration::useTiledBuild || m_bTiledFallback) {
        params.tileWidth  = NavigationMapConfiguration::tileSize * NavigationMapConfiguration::recastCellSize;
        params.tileHeight = NavigationMapConfiguration::tileSize * NavigationMapConfiguration::recastCellSize;
        params.maxTiles   = 512;
        params.maxPolys   = 8192;
    } else {
        // SOLO (v1.9.0): one tile over the whole map (tileWidth over-sized so everything lands in tile 0,0).
        params.tileWidth  = MAP_SIZE * NavigationMapConfiguration::recastCellSize;
        params.tileHeight = MAP_SIZE * NavigationMapConfiguration::recastCellSize;
        params.maxTiles   = 32;
        params.maxPolys   = 32768;
    }

    navMeshDt = dtAllocNavMesh();
    status    = navMeshDt->init(&params);

    if (dtStatusFailed(status)) {
        buildContext.log(RC_LOG_ERROR, "Failed to initialize the navigation mesh");
        return;
    }

    //
    // Create the nav mesh query
    //
    navMeshQuery = dtAllocNavMeshQuery();
    // [bug-2850] Own A* node pool, 16384 (was MAX_PATHNODES = 4096, the SP pathnode system's constant). A route that
    // must loop AWAY from its goal first (m4l2 with the track gates closed: west past the escape truck, round the
    // south, back east) exhausted 4096 nodes flooding the side the heuristic favours; Detour then returned a PARTIAL
    // path to the explored point nearest the goal - the closed gate - and axis bots parked at its bars (soak: 234
    // stationary samples, pth=0). Detour's cap is 65535 (16-bit node index); a truly unreachable goal now costs a
    // 16k-node search instead of 4k, still sub-frame.
    // [bug-2864] 16384 was still too few for m4l2 with the track gates CLOSED: the allies' only route to the axis end
    // is yard -> lift DOWN -> tunnel -> station -> axis platform, a loop that starts by heading away from a goal that
    // lies straight west; A* flooded the east side first, ran out, and 72% of allied soak samples idled within 1000u
    // of their home (biggest cluster pth=0). 65534 is Detour's ceiling (16-bit node index, 0xffff = null) and larger
    // than the whole m4l2 mesh (22.6k polys incl. off-mesh links), so a route that exists is always found; only a
    // truly unreachable goal pays for the full search.
    status = navMeshQuery->init(navMeshDt, 65534);

    if (dtStatusFailed(status)) {
        buildContext.log(RC_LOG_ERROR, "Could not init Detour navmesh query");
        return;
    }
}

/*
============
NavigationMap::InitializeFilter
============
*/
void NavigationMap::InitializeFilter()
{
    size_t i, j;

    queryFilter = new dtQueryFilter();
    queryFilter->setExcludeFlags(RECAST_POLYFLAG_BUSY | RECAST_POLYFLAG_PROP); // [HZM bug-2901]

    for (i = 1; i <= extensions.NumObjects(); i++) {
        INavigationMapExtension *navExtension = extensions.ObjectAt(i);

        Container<ExtensionArea> areas = navExtension->GetSupportedAreas();
        for (j = 1; j <= areas.NumObjects(); j++) {
            const ExtensionArea& area = areas.ObjectAt(j);

            queryFilter->setAreaCost(area.number, area.cost);
        }
    }
}

/*
============
NavigationMap::InitializeExtensions
============
*/
void NavigationMap::InitializeExtensions()
{
    for (const ClassDef *c = ClassDef::classlist; c; c = c->next) {
        if (c != INavigationMapExtension::classinfostatic()
            && checkInheritance(INavigationMapExtension::classinfostatic(), c)) {
            extensions.AddObject(static_cast<INavigationMapExtension *>(c->newInstance()));
        }
    }
}

/*
============
NavigationMap::ClearExtensions
============
*/
void NavigationMap::ClearExtensions()
{
    size_t i;

    for (i = 1; i <= extensions.NumObjects(); i++) {
        delete extensions.ObjectAt(i);
    }

    extensions.FreeObjectList();
}

/*
============
NavigationMap::BuildDetourData
============
*/
bool NavigationMap::BuildDetourData(
    RecastBuildContext&                      buildContext,
    rcPolyMesh                              *polyMesh,
    rcPolyMeshDetail                        *polyMeshDetail,
    int                                      tileX,
    int                                      tileY,
    int                                      layer,
    const Container<offMeshNavigationPoint>& points
)
{
    unsigned char *navData     = NULL;
    int            navDataSize = 0;
    dtStatus       status;

    if (polyMesh->npolys < 2) {
        // Useless mesh
        return true;
    }

    dtNavMeshCreateParams dtParams {0};

    dtParams.verts            = polyMesh->verts;
    dtParams.vertCount        = polyMesh->nverts;
    dtParams.polys            = polyMesh->polys;
    dtParams.polyAreas        = polyMesh->areas;
    dtParams.polyFlags        = polyMesh->flags;
    dtParams.polyCount        = polyMesh->npolys;
    dtParams.nvp              = polyMesh->nvp;
    dtParams.detailMeshes     = polyMeshDetail->meshes;
    dtParams.detailVerts      = polyMeshDetail->verts;
    dtParams.detailVertsCount = polyMeshDetail->nverts;
    dtParams.detailTris       = polyMeshDetail->tris;
    dtParams.detailTriCount   = polyMeshDetail->ntris;
    dtParams.walkableHeight   = NavigationMapConfiguration::agentHeight;
    dtParams.walkableRadius   = NavigationMapConfiguration::agentRadius;
    dtParams.walkableClimb    = NavigationMapConfiguration::agentMaxClimb;
    rcVcopy(dtParams.bmin, polyMesh->bmin);
    rcVcopy(dtParams.bmax, polyMesh->bmax);
    dtParams.cs          = NavigationMapConfiguration::recastCellSize;
    dtParams.ch          = NavigationMapConfiguration::recastCellHeight;
    dtParams.tileX       = tileX;
    dtParams.tileY       = tileY;
    dtParams.tileLayer   = layer;
    dtParams.buildBvTree = true;

    if (points.NumObjects()) {
        dtParams.offMeshConCount = offMeshConCount = points.NumObjects();

        offMeshConVerts  = new float[6 * dtParams.offMeshConCount];
        offMeshConRad    = new float[dtParams.offMeshConCount];
        offMeshConFlags  = new unsigned short[dtParams.offMeshConCount];
        offMeshConAreas  = new unsigned char[dtParams.offMeshConCount];
        offMeshConDir    = new unsigned char[dtParams.offMeshConCount];
        offMeshConUserID = new unsigned int[dtParams.offMeshConCount];

        for (int i = 0; i < dtParams.offMeshConCount; i++) {
            const offMeshNavigationPoint& point = points.ObjectAt(i + 1);

            ConvertGameToRecastCoord(point.start, &offMeshConVerts[i * 6]);
            ConvertGameToRecastCoord(point.end, &offMeshConVerts[i * 6 + 3]);

            offMeshConRad[i]    = point.radius;
            offMeshConFlags[i]  = point.flags;
            offMeshConAreas[i]  = point.area;
            offMeshConDir[i]    = point.bidirectional ? DT_OFFMESH_CON_BIDIR : 0;
            offMeshConUserID[i] = i;
        }

        dtParams.offMeshConVerts  = offMeshConVerts;
        dtParams.offMeshConRad    = offMeshConRad;
        dtParams.offMeshConFlags  = offMeshConFlags;
        dtParams.offMeshConAreas  = offMeshConAreas;
        dtParams.offMeshConDir    = offMeshConDir;
        dtParams.offMeshConUserID = offMeshConUserID;
    }

    const bool created = dtCreateNavMeshData(&dtParams, &navData, &navDataSize);

    if (points.NumObjects()) {
        delete[] offMeshConVerts;
        delete[] offMeshConRad;
        delete[] offMeshConFlags;
        delete[] offMeshConAreas;
        delete[] offMeshConDir;
        delete[] offMeshConUserID;
    }

    if (!created || !navData) {
        // [bug-2861] dtCreateNavMeshData refuses a tile over its 16-bit vertex/poly limits (e3l2 at cellSize 8: one
        // solo tile over the whole map). Report it so the caller can fall back to the tiled build.
        buildContext.log(RC_LOG_ERROR, "Failed to create navmesh data (%d verts, %d polys)", polyMesh->nverts, polyMesh->npolys);
        return false;
    }
    status = navMeshDt->addTile(navData, navDataSize, DT_TILE_FREE_DATA, 0, NULL);
    if (!dtStatusSucceed(status)) {
        buildContext.log(RC_LOG_ERROR, "Failed to create tile for navigation mesh");
        dtFree(navData);
        return false;
    }
    return true;
}

/*
============
NavigationMap::GeneratePolyMesh
============
*/
void NavigationMap::GeneratePolyMesh(
    RecastBuildContext& buildContext,
    float              *vertsBuffer,
    int                 numVertices,
    int                *indexesBuffer,
    int                 numIndexes,
    rcPolyMesh       *&       outPolyMesh,
    rcPolyMeshDetail *& outPolyMeshDetail
)
{
    Vector             minBounds, maxBounds;
    int                gridSizeX, gridSizeZ;
    const unsigned int walkableHeight =
        (int)ceilf(NavigationMapConfiguration::agentHeight / NavigationMapConfiguration::recastCellHeight);
    const unsigned int walkableClimb =
        (int)floorf(NavigationMapConfiguration::agentMaxClimb / NavigationMapConfiguration::recastCellHeight);
    const unsigned int walkableRadius =
        (int)ceilf(NavigationMapConfiguration::agentRadius / NavigationMapConfiguration::recastCellSize);
    const unsigned int maxEdgeLen =
        (int)(NavigationMapConfiguration::edgeMaxLen / NavigationMapConfiguration::recastCellSize);
    const unsigned int numTris = numIndexes / 3;

    //
    // Calculate the grid size
    //
    rcCalcBounds(vertsBuffer, numVertices, minBounds, maxBounds);
    rcCalcGridSize(minBounds, maxBounds, NavigationMapConfiguration::recastCellSize, &gridSizeX, &gridSizeZ);

    rcHeightfield *heightfield = rcAllocHeightfield();

    //
    // Rasterize polygons
    //

    rcCreateHeightfield(
        &buildContext,
        *heightfield,
        gridSizeX,
        gridSizeZ,
        minBounds,
        maxBounds,
        NavigationMapConfiguration::recastCellSize,
        NavigationMapConfiguration::recastCellHeight
    );

    unsigned char *triAreas = new unsigned char[numTris];
    memset(triAreas, 0, sizeof(unsigned char) * numTris);

    rcMarkWalkableTriangles(
        &buildContext,
        NavigationMapConfiguration::agentMaxSlope,
        vertsBuffer,
        numVertices,
        indexesBuffer,
        numTris,
        triAreas
    );
    MarkSteepTriangles(vertsBuffer, indexesBuffer, numTris, triAreas); // [HZM bug-2887]
    rcRasterizeTriangles(
        &buildContext, vertsBuffer, numVertices, indexesBuffer, triAreas, numTris, *heightfield, walkableClimb
    );

    delete[] triAreas;

    //
    // Filter walkable surfaces
    //

    rcFilterLowHangingWalkableObstacles(&buildContext, walkableClimb, *heightfield);
    MarkHeadClearance(*heightfield, walkableHeight, walkableClimb); // [HZM bug-2956]

    //rcFilterLedgeSpans(&buildContext, walkableHeight, walkableClimb, *heightfield);
    rcFilterWalkableLowHeightSpans(&buildContext, walkableHeight, *heightfield);

    // Partition walkable surfaces

    rcCompactHeightfield *compactedHeightfield = rcAllocCompactHeightfield();

    rcBuildCompactHeightfield(&buildContext, walkableHeight, walkableClimb, *heightfield, *compactedHeightfield);

    // The heightfield is not needed anymore
    rcFreeHeightField(heightfield);

    rcErodeWalkableArea(&buildContext, walkableRadius, *compactedHeightfield);
    ManualBlocks_Apply(&buildContext, *compactedHeightfield); // [bug-2842] cut the mesh at closed-on-purpose gates

    rcBuildDistanceField(&buildContext, *compactedHeightfield);
    // [bug-2831 2026-09-23] Watershed kept. Monotone was A/B'd here to test whether the intermittent m4l2
    // allies-spawn "disconnection" was watershed non-determinism - it was NOT: monotone (a fully deterministic
    // sweep) STILL stranded bots on the elevated allies ledge in 2/8 boots. Root cause is spawn placement, not
    // partitioning: m4l2's allies spawns sit on a z~330-416 ledge that only marginally connects down to the z~0
    // ground, so bots assigned there sometimes cannot path down (pth=0, stuck at pz=328). Fix is in push_spawns
    // (relocate the elevated spawns to ground), not the mesh. Monotone gave no benefit + coarser polys -> reverted.
    rcBuildRegions(
        &buildContext,
        *compactedHeightfield,
        0,
        Square(NavigationMapConfiguration::regionMinSize),
        Square(NavigationMapConfiguration::regionMergeSize)
    );

    //
    // Simplify region contours
    //

    rcContourSet *contourSet = rcAllocContourSet();

    rcBuildContours(
        &buildContext, *compactedHeightfield, NavigationMapConfiguration::edgeMaxError, maxEdgeLen, *contourSet
    );

    //
    // Build polygon mesh from contours
    //

    rcPolyMesh *polyMesh = rcAllocPolyMesh();

    rcBuildPolyMesh(&buildContext, *contourSet, NavigationMapConfiguration::vertsPerPoly, *polyMesh);

    //
    // Create detail mesh to access approximate height for each polygon
    //

    rcPolyMeshDetail *polyMeshDetail = rcAllocPolyMeshDetail();

    rcBuildPolyMeshDetail(
        &buildContext,
        *polyMesh,
        *compactedHeightfield,
        NavigationMapConfiguration::recastCellSize * NavigationMapConfiguration::detailSampleDist,
        NavigationMapConfiguration::detailSampleMaxError,
        *polyMeshDetail
    );

    rcFreeCompactHeightfield(compactedHeightfield);
    rcFreeContourSet(contourSet);

    outPolyMesh       = polyMesh;
    outPolyMeshDetail = polyMeshDetail;
}

/*
============
NavigationMap::GeneratePolyMeshTile
[bug-2831 tiled build] Build ONE navmesh tile. tileBmin/tileBmax are the LOGICAL tile bounds (Recast coords).
The heightfield is expanded by borderSize cells on each horizontal (X/Z) side, and borderSize is passed to
rcBuildRegions, so the tile's polygons are built with neighbouring context but trimmed exactly to the logical
bounds - adjacent tiles then share identical edge vertices in world space and Detour links them. Returns false
(and leaves the outputs NULL) for an empty tile. Per-tile heightfield stays small, so finer cells never hit the
solo-build size limit that crashed m3l3 at cellSize 6.
============
*/
bool NavigationMap::GeneratePolyMeshTile(
    RecastBuildContext& buildContext,
    float              *vertsBuffer,
    int                 numVertices,
    int                *indexesBuffer,
    int                 numIndexes,
    const float         tileBmin[3],
    const float         tileBmax[3],
    int                 borderSize,
    rcPolyMesh       *&       outPolyMesh,
    rcPolyMeshDetail *& outPolyMeshDetail
)
{
    const float        cs             = NavigationMapConfiguration::recastCellSize;
    const float        ch             = NavigationMapConfiguration::recastCellHeight;
    const unsigned int walkableHeight = (int)ceilf(NavigationMapConfiguration::agentHeight / ch);
    const unsigned int walkableClimb  = (int)floorf(NavigationMapConfiguration::agentMaxClimb / ch);
    const unsigned int walkableRadius = (int)ceilf(NavigationMapConfiguration::agentRadius / cs);
    const unsigned int maxEdgeLen     = (int)(NavigationMapConfiguration::edgeMaxLen / cs);
    const unsigned int numTris        = numIndexes / 3;

    outPolyMesh       = NULL;
    outPolyMeshDetail = NULL;

    // Expand the logical tile bounds by the border margin (horizontal X/Z only; Y is the full map height).
    float bmin[3], bmax[3];
    rcVcopy(bmin, tileBmin);
    rcVcopy(bmax, tileBmax);
    bmin[0] -= borderSize * cs;
    bmin[2] -= borderSize * cs;
    bmax[0] += borderSize * cs;
    bmax[2] += borderSize * cs;

    int gridSizeX, gridSizeZ;
    rcCalcGridSize(bmin, bmax, cs, &gridSizeX, &gridSizeZ);

    rcHeightfield *heightfield = rcAllocHeightfield();
    if (!rcCreateHeightfield(&buildContext, *heightfield, gridSizeX, gridSizeZ, bmin, bmax, cs, ch)) {
        rcFreeHeightField(heightfield);
        return false;
    }

    unsigned char *triAreas = new unsigned char[numTris];
    memset(triAreas, 0, sizeof(unsigned char) * numTris);
    rcMarkWalkableTriangles(
        &buildContext, NavigationMapConfiguration::agentMaxSlope, vertsBuffer, numVertices, indexesBuffer, numTris, triAreas
    );
    MarkSteepTriangles(vertsBuffer, indexesBuffer, numTris, triAreas); // [HZM bug-2887]
    // Recast clips each triangle to the heightfield bounds, so passing the whole world triangle set is correct -
    // triangles outside this tile+border simply produce no spans.
    rcRasterizeTriangles(
        &buildContext, vertsBuffer, numVertices, indexesBuffer, triAreas, numTris, *heightfield, walkableClimb
    );
    delete[] triAreas;

    rcFilterLowHangingWalkableObstacles(&buildContext, walkableClimb, *heightfield);
    MarkHeadClearance(*heightfield, walkableHeight, walkableClimb); // [HZM bug-2956]
    rcFilterWalkableLowHeightSpans(&buildContext, walkableHeight, *heightfield);

    rcCompactHeightfield *compactedHeightfield = rcAllocCompactHeightfield();
    rcBuildCompactHeightfield(&buildContext, walkableHeight, walkableClimb, *heightfield, *compactedHeightfield);
    rcFreeHeightField(heightfield);

    rcErodeWalkableArea(&buildContext, walkableRadius, *compactedHeightfield);
    ManualBlocks_Apply(&buildContext, *compactedHeightfield); // [bug-2842]
    rcBuildDistanceField(&buildContext, *compactedHeightfield);
    // borderSize (not 0) trims each tile's polys to its logical bounds so neighbours align -> Detour links them.
    rcBuildRegions(
        &buildContext,
        *compactedHeightfield,
        borderSize,
        Square(NavigationMapConfiguration::regionMinSize),
        Square(NavigationMapConfiguration::regionMergeSize)
    );

    rcContourSet *contourSet = rcAllocContourSet();
    rcBuildContours(
        &buildContext, *compactedHeightfield, NavigationMapConfiguration::edgeMaxError, maxEdgeLen, *contourSet
    );

    rcPolyMesh *polyMesh = rcAllocPolyMesh();
    rcBuildPolyMesh(&buildContext, *contourSet, NavigationMapConfiguration::vertsPerPoly, *polyMesh);

    rcPolyMeshDetail *polyMeshDetail = rcAllocPolyMeshDetail();
    rcBuildPolyMeshDetail(
        &buildContext,
        *polyMesh,
        *compactedHeightfield,
        cs * NavigationMapConfiguration::detailSampleDist,
        NavigationMapConfiguration::detailSampleMaxError,
        *polyMeshDetail
    );

    rcFreeCompactHeightfield(compactedHeightfield);
    rcFreeContourSet(contourSet);

    if (polyMesh->npolys < 1) {
        // Empty tile - no walkable geometry here.
        rcFreePolyMesh(polyMesh);
        rcFreePolyMeshDetail(polyMeshDetail);
        return false;
    }

    NavExcludeSlivers(polyMesh, "tile"); // [HZM bug-2988] before the flags and the link generators
    // Update poly flags from areas.
    for (int i = 0; i < polyMesh->npolys; ++i) {
        if (IsRecastWalkableArea(polyMesh->areas[i])) { // [bug-2865] + heat cost tiers
            polyMesh->flags[i] = RECAST_POLYFLAG_WALKABLE;
        } else if (polyMesh->areas[i] == RECAST_AREA_PROP) { // [HZM bug-2901] a prop's footprint: excluded until it goes
            polyMesh->flags[i] = RECAST_POLYFLAG_WALKABLE | RECAST_POLYFLAG_PROP;
        }
    }

    outPolyMesh       = polyMesh;
    outPolyMeshDetail = polyMeshDetail;
    return true;
}

/*
============
NavigationMap::BuildRecastMesh
============
*/
void NavigationMap::BuildRecastMesh(
    RecastBuildContext& buildContext,
    const navModel_t&   model,
    const Vector&       origin,
    const Vector&       angles,
    rcPolyMesh       *&       outPolyMesh,
    rcPolyMeshDetail *& outPolyMeshDetail
)
{
    int numIndexes  = 0;
    int numVertices = 0;
    int baseVertice, baseIndice;
    int i, j;

    for (i = 1; i <= model.surfaces.NumObjects(); i++) {
        const navSurface_t& surface = model.surfaces.ObjectAt(i);

        numIndexes += surface.indices.NumObjects();
        numVertices += surface.vertices.NumObjects();
    }

    //
    // Recreate the vertice buffer so it's compatible
    // with Recast's right-handed Y-up coordinate system
    float *vertsBuffer = new float[numVertices * 3];

    baseIndice  = 0;
    baseVertice = 0;

    if (angles != vec_zero) {
        float axis[3][3];

        AnglesToAxis(angles, axis);
        for (i = 1; i <= model.surfaces.NumObjects(); i++) {
            const navSurface_t& surface = model.surfaces.ObjectAt(i);

            for (j = 0; j < surface.vertices.NumObjects(); j++) {
                const navVertice_t& inVertice = surface.vertices.ObjectAt(j + 1);
                Vector              offset;

                MatrixTransformVector(inVertice.xyz, axis, offset);
                offset += origin;

                ConvertGameToRecastCoord(offset, &vertsBuffer[baseVertice * 3 + j * 3]);
            }

            baseVertice += surface.vertices.NumObjects();
        }
    } else {
        for (i = 1; i <= model.surfaces.NumObjects(); i++) {
            const navSurface_t& surface = model.surfaces.ObjectAt(i);

            for (j = 0; j < surface.vertices.NumObjects(); j++) {
                const navVertice_t& inVertice = surface.vertices.ObjectAt(j + 1);
                const Vector        offset    = inVertice.xyz + origin;

                ConvertGameToRecastCoord(offset, &vertsBuffer[baseVertice * 3 + j * 3]);
            }

            baseVertice += surface.vertices.NumObjects();
        }
    }

    baseIndice  = 0;
    baseVertice = 0;

    int *indexesBuffer = new int[numIndexes];
    for (i = 1; i <= model.surfaces.NumObjects(); i++) {
        const navSurface_t& surface = model.surfaces.ObjectAt(i);

        const int numTris = surface.indices.NumObjects() / 3;

        for (j = 0; j < numTris; j++) {
            indexesBuffer[baseIndice + j * 3 + 0] = baseVertice + surface.indices[j * 3 + 2].indice;
            indexesBuffer[baseIndice + j * 3 + 1] = baseVertice + surface.indices[j * 3 + 1].indice;
            indexesBuffer[baseIndice + j * 3 + 2] = baseVertice + surface.indices[j * 3 + 0].indice;
        }

        baseIndice += surface.indices.NumObjects();
        baseVertice += surface.vertices.NumObjects();
    }

    //
    // Generate the mesh
    //

    rcPolyMesh       *polyMesh;
    rcPolyMeshDetail *polyMeshDetail;

    GeneratePolyMesh(buildContext, vertsBuffer, numVertices, indexesBuffer, numIndexes, polyMesh, polyMeshDetail);

    NavExcludeSlivers(polyMesh, "solo"); // [HZM bug-2988] before the flags and the link generators
    // Update poly flags from areas.
    for (int i = 0; i < polyMesh->npolys; ++i) {
        if (IsRecastWalkableArea(polyMesh->areas[i])) { // [bug-2865] + heat cost tiers
            polyMesh->flags[i] = RECAST_POLYFLAG_WALKABLE;
        } else if (polyMesh->areas[i] == RECAST_AREA_PROP) { // [HZM bug-2901] a prop's footprint: excluded until it goes
            polyMesh->flags[i] = RECAST_POLYFLAG_WALKABLE | RECAST_POLYFLAG_PROP;
        }
    }

    delete[] indexesBuffer;
    delete[] vertsBuffer;

    outPolyMesh       = polyMesh;
    outPolyMeshDetail = polyMeshDetail;
}

/*
============
G_Navigation_Frame
============
*/
void G_Navigation_Frame()
{
    pathMaster.Update();
    navigationMap.Update();
    navigationObstacleMap.Update();
    // [HZM bug-2901] nav_dump: a prop released at runtime changes the mesh the bots actually use - rewrite the dump
    // (at most once per 5s) so the offline sweeps (nav_sinks.py) see the mesh after the MP scripts cleared the debris
    {
        static int s_redumpNext = 0;
        static int s_pending    = 0;
        s_pending += NavEntityBlocks_TakeReleased();
        if (s_pending && level.inttime >= s_redumpNext) {
            static cvar_t *s_navDump = NULL;
            if (!s_navDump) {
                s_navDump = gi.Cvar_Get("nav_dump", "0", 0);
            }
            if (s_navDump->integer && navigationMap.IsValid()) {
                navigationMap.DumpNavMeshNow();
            }
            s_pending    = 0;
            s_redumpNext = level.inttime + 5000;
        }
    }
    G_Navigation_DebugDraw();
}

/*
============
NavigationMap::NavigationMap
============
*/
NavigationMap::NavigationMap()
    : navMeshDt(NULL)
    , navMeshQuery(NULL)
    , queryFilter(NULL)
    , m_bTiledFallback(false)
    , m_bSoloFailed(false)
{
    validNavigation = false;
}

/*
============
NavigationMap::~NavigationMap
============
*/
NavigationMap::~NavigationMap()
{
    ClearNavigation();
}

/*
============
NavigationMap::GetNavMesh
============
*/
dtNavMesh *NavigationMap::GetNavMesh() const
{
    return navMeshDt;
}

/*
============
NavigationMap::GetNavMeshQuery
============
*/
dtNavMeshQuery *NavigationMap::GetNavMeshQuery() const
{
    return navMeshQuery;
}

/*
============
NavigationMap::GetQueryFilter
============
*/
const dtQueryFilter *NavigationMap::GetQueryFilter() const
{
    return queryFilter;
}

/*
============
NavigationMap::GetNavigationData
============
*/
const navMap_t& NavigationMap::GetNavigationData() const
{
    return navigationData.navMap;
}

/*
============
NavigationMap::Update
============
*/
void NavigationMap::Update() {}

/*
============
NavigationMap::ClearNavigation
============
*/
void NavigationMap::ClearNavigation()
{
    size_t i;

    validNavigation = false;

    pathMaster.ClearNavigation();
    navigationObstacleMap.Clear();
    NavLift_Clear(); // [bug-2912] lift records live and die with the mesh their links were built into

    if (navMeshQuery) {
        dtFreeNavMeshQuery(navMeshQuery);
        navMeshQuery = NULL;
    }

    if (navMeshDt) {
        dtFreeNavMesh(navMeshDt);
        navMeshDt = NULL;
    }

    if (queryFilter) {
        delete queryFilter;
        queryFilter = NULL;
    }

    ClearExtensions();
}

bool NavigationMap::IsValid() const
{
    return navMeshDt != NULL;
}

/*
============
FlattenNavModelToRecast
[bug-2831 tiled build] Flatten a nav model's surfaces into a Recast-coordinate vertex buffer + triangle index
buffer (caller frees both). Mirrors the flatten in BuildRecastMesh; kept separate so the tiled world build can
rasterise the same triangle set into many tiles.
============
*/
static void FlattenNavModelToRecast(
    const navModel_t& model,
    const Vector&     origin,
    const Vector&     angles,
    float           *& outVerts,
    int&              outNumVerts,
    int             *& outIndices,
    int&              outNumIndexes
)
{
    int numIndexes = 0, numVertices = 0, baseIndice, baseVertice, i, j;

    for (i = 1; i <= model.surfaces.NumObjects(); i++) {
        const navSurface_t& surface = model.surfaces.ObjectAt(i);
        numIndexes += surface.indices.NumObjects();
        numVertices += surface.vertices.NumObjects();
    }

    float *vertsBuffer = new float[numVertices * 3];
    baseVertice        = 0;
    if (angles != vec_zero) {
        float axis[3][3];
        AnglesToAxis(angles, axis);
        for (i = 1; i <= model.surfaces.NumObjects(); i++) {
            const navSurface_t& surface = model.surfaces.ObjectAt(i);
            for (j = 0; j < surface.vertices.NumObjects(); j++) {
                const navVertice_t& inVertice = surface.vertices.ObjectAt(j + 1);
                Vector              offset;
                MatrixTransformVector(inVertice.xyz, axis, offset);
                offset += origin;
                ConvertGameToRecastCoord(offset, &vertsBuffer[baseVertice * 3 + j * 3]);
            }
            baseVertice += surface.vertices.NumObjects();
        }
    } else {
        for (i = 1; i <= model.surfaces.NumObjects(); i++) {
            const navSurface_t& surface = model.surfaces.ObjectAt(i);
            for (j = 0; j < surface.vertices.NumObjects(); j++) {
                const navVertice_t& inVertice = surface.vertices.ObjectAt(j + 1);
                const Vector        offset    = inVertice.xyz + origin;
                ConvertGameToRecastCoord(offset, &vertsBuffer[baseVertice * 3 + j * 3]);
            }
            baseVertice += surface.vertices.NumObjects();
        }
    }

    int *indexesBuffer = new int[numIndexes];
    baseIndice         = 0;
    baseVertice        = 0;
    for (i = 1; i <= model.surfaces.NumObjects(); i++) {
        const navSurface_t& surface = model.surfaces.ObjectAt(i);
        const int           numTris = surface.indices.NumObjects() / 3;
        for (j = 0; j < numTris; j++) {
            indexesBuffer[baseIndice + j * 3 + 0] = baseVertice + surface.indices[j * 3 + 2].indice;
            indexesBuffer[baseIndice + j * 3 + 1] = baseVertice + surface.indices[j * 3 + 1].indice;
            indexesBuffer[baseIndice + j * 3 + 2] = baseVertice + surface.indices[j * 3 + 0].indice;
        }
        baseIndice += surface.indices.NumObjects();
        baseVertice += surface.vertices.NumObjects();
    }

    outVerts      = vertsBuffer;
    outNumVerts   = numVertices;
    outIndices    = indexesBuffer;
    outNumIndexes = numIndexes;
}

/*
============
NavigationMap::BuildWorldMesh
[bug-2831 tiled build] Split the world into a grid of tileSize-cell tiles and build each into its own bounded
heightfield, so per-tile cell count stays small and finer cells never hit the solo-build size limit that crashed
m3l3 at cellSize 6. Each tile is added to the Detour navmesh at its grid (tileX,tileY); Detour links adjacent
tiles along their shared, border-trimmed edges. Off-mesh links are gathered per tile.
============
*/
void NavigationMap::BuildWorldMesh(RecastBuildContext& buildContext, const navMap_t& navigationMap)
{
    if (!NavigationMapConfiguration::useTiledBuild && !m_bTiledFallback) {
        // SOLO build - exactly the v1.9.0 path: one heightfield / one Detour tile for the whole world.
        rcPolyMesh       *polyMesh;
        rcPolyMeshDetail *polyMeshDetail;

        BuildRecastMesh(buildContext, navigationMap.GetWorldMap(), vec_origin, vec_zero, polyMesh, polyMeshDetail);

        Container<offMeshNavigationPoint> points;
        GatherOffMeshPoints(points, polyMesh);

        if (!BuildDetourData(buildContext, polyMesh, polyMeshDetail, 0, 0, 0, points)) {
            m_bSoloFailed = true; // [bug-2861] LoadWorldMap re-initialises and rebuilds this map tiled
        }

        rcFreePolyMeshDetail(polyMeshDetail);
        rcFreePolyMesh(polyMesh);
        return;
    }

    float *vertsBuffer   = NULL;
    int   *indexesBuffer = NULL;
    int    numVertices = 0, numIndexes = 0;

    FlattenNavModelToRecast(
        navigationMap.GetWorldMap(), vec_origin, vec_zero, vertsBuffer, numVertices, indexesBuffer, numIndexes
    );

    if (numVertices < 3 || numIndexes < 3) {
        delete[] vertsBuffer;
        delete[] indexesBuffer;
        return;
    }

    // Whole-mesh bounds (Recast coords).
    float meshBmin[3], meshBmax[3];
    rcCalcBounds(vertsBuffer, numVertices, meshBmin, meshBmax);

    const float cs         = NavigationMapConfiguration::recastCellSize;
    const int   ts         = NavigationMapConfiguration::tileSize; // tile interior, in cells
    const float tcs        = ts * cs;                              // tile world size
    const int   borderSize = (int)ceilf(NavigationMapConfiguration::agentRadius / cs) + 3;
    const float orig       = MIN_MAP_BOUNDS;                       // must match InitializeNavMesh params.orig

    // Tile grid indices spanning the geometry footprint (relative to the Detour origin).
    const int tx0 = (int)floorf((meshBmin[0] - orig) / tcs);
    const int tx1 = (int)floorf((meshBmax[0] - orig) / tcs);
    const int ty0 = (int)floorf((meshBmin[2] - orig) / tcs);
    const int ty1 = (int)floorf((meshBmax[2] - orig) / tcs);

    int built = 0;
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            float tileBmin[3], tileBmax[3];
            tileBmin[0] = orig + tx * tcs;
            tileBmin[1] = meshBmin[1];
            tileBmin[2] = orig + ty * tcs;
            tileBmax[0] = orig + (tx + 1) * tcs;
            tileBmax[1] = meshBmax[1];
            tileBmax[2] = orig + (ty + 1) * tcs;

            rcPolyMesh       *polyMesh       = NULL;
            rcPolyMeshDetail *polyMeshDetail = NULL;
            if (!GeneratePolyMeshTile(
                    buildContext, vertsBuffer, numVertices, indexesBuffer, numIndexes, tileBmin, tileBmax, borderSize,
                    polyMesh, polyMeshDetail
                )) {
                continue;
            }

            Container<offMeshNavigationPoint> points;
            GatherOffMeshPoints(points, polyMesh);

            BuildDetourData(buildContext, polyMesh, polyMeshDetail, tx, ty, 0, points);
            built++;

            rcFreePolyMeshDetail(polyMeshDetail);
            rcFreePolyMesh(polyMesh);
        }
    }

    gi.Printf(
        "  Tiled navmesh: %d tiles built (%dx%d grid, tileSize %d cells @ cs %.1f)\n",
        built,
        (tx1 - tx0 + 1),
        (ty1 - ty0 + 1),
        ts,
        cs
    );

    //
    // [bug-2831] Cross-tile link audit. Every polygon edge rcBuildPolyMesh marked as a tile-border portal
    // (neis = DT_EXT_LINK|side) should get a Detour external link when a neighbour tile exists on that side.
    // An unconnected portal facing a real neighbour tile is a seam bots cannot path across.
    //
    {
        int portalsFacingTile = 0, portalsLinked = 0, portalsFacingEmpty = 0;
        int unlinkedNoCounterpart = 0, unlinkedHeight = 0, unlinkedOther = 0, unlinkedLogged = 0;
        int unlinkedInsetNear = 0, unlinkedInsetFar = 0, unlinkedInsetNone = 0, maxTilePolys = 0;
        const dtNavMesh *nm = navMeshDt;
        for (int ti = 0; ti < nm->getMaxTiles(); ti++) {
            const dtMeshTile *tile = nm->getTile(ti);
            if (!tile || !tile->header) {
                continue;
            }
            maxTilePolys = rcMax(maxTilePolys, tile->header->polyCount);
            for (int pi = 0; pi < tile->header->polyCount; pi++) {
                const dtPoly *poly = &tile->polys[pi];
                if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
                    continue;
                }
                for (int e = 0; e < poly->vertCount; e++) {
                    if (!(poly->neis[e] & DT_EXT_LINK)) {
                        continue;
                    }
                    const int side = poly->neis[e] & 0xff;
                    int       nx = tile->header->x, ny = tile->header->y;
                    if (side == 0) {
                        nx++;
                    } else if (side == 2) {
                        ny++;
                    } else if (side == 4) {
                        nx--;
                    } else if (side == 6) {
                        ny--;
                    }
                    if (!nm->getTileAt(nx, ny, 0)) {
                        portalsFacingEmpty++;
                        continue;
                    }
                    portalsFacingTile++;
                    bool linked = false;
                    for (unsigned int k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next) {
                        if (tile->links[k].edge == e && tile->links[k].side != 0xff) {
                            linked = true;
                            break;
                        }
                    }
                    if (linked) {
                        portalsLinked++;
                        continue;
                    }

                    // Unlinked: classify against the neighbour tile's opposite-side portals.
                    // u = coordinate ALONG the border (z for x-sides, x for z-sides); y = height.
                    const float *va  = &tile->verts[poly->verts[e] * 3];
                    const float *vb  = &tile->verts[poly->verts[(e + 1) % poly->vertCount] * 3];
                    const int    ui  = (side == 0 || side == 4) ? 2 : 0;
                    const float  au0 = rcMin(va[ui], vb[ui]), au1 = rcMax(va[ui], vb[ui]);
                    const float  ay  = (va[1] + vb[1]) * 0.5f;
                    const int    opp = (side + 4) & 7;
                    float        bestDy  = 1e9f;
                    bool         overlap = false;
                    const dtMeshTile *nt = nm->getTileAt(nx, ny, 0);
                    for (int qi = 0; qi < nt->header->polyCount; qi++) {
                        const dtPoly *q = &nt->polys[qi];
                        if (q->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
                            continue;
                        }
                        for (int f = 0; f < q->vertCount; f++) {
                            if (q->neis[f] != (DT_EXT_LINK | opp)) {
                                continue;
                            }
                            const float *wa  = &nt->verts[q->verts[f] * 3];
                            const float *wb  = &nt->verts[q->verts[(f + 1) % q->vertCount] * 3];
                            const float  bu0 = rcMin(wa[ui], wb[ui]), bu1 = rcMax(wa[ui], wb[ui]);
                            if (bu1 <= au0 + 0.01f || bu0 >= au1 - 0.01f) {
                                continue;
                            }
                            overlap          = true;
                            const float dy   = fabsf(ay - (wa[1] + wb[1]) * 0.5f);
                            bestDy           = rcMin(bestDy, dy);
                        }
                    }
                    if (!overlap) {
                        unlinkedNoCounterpart++;
                    } else if (bestDy > NavigationMapConfiguration::agentMaxClimb) {
                        unlinkedHeight++;
                    } else {
                        unlinkedOther++;
                    }

                    // Inset: how far from the border line does the neighbour's nearest same-height floor vertex
                    // sit, within this seam's u-range? 1-2 cells => neighbour floor stops just short of the border
                    // (one-sided seam from processing asymmetry); large/none => genuinely different geometry.
                    const int   bi    = (side == 0 || side == 4) ? 0 : 2; // axis perpendicular to the border
                    const float bline = (side == 0 || side == 2) ? rcMax(va[bi], vb[bi]) : rcMin(va[bi], vb[bi]);
                    float       inset = 1e9f;
                    for (int qi = 0; qi < nt->header->polyCount; qi++) {
                        const dtPoly *q = &nt->polys[qi];
                        if (q->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
                            continue;
                        }
                        for (int f = 0; f < q->vertCount; f++) {
                            const float *w = &nt->verts[q->verts[f] * 3];
                            if (w[ui] < au0 - 1.0f || w[ui] > au1 + 1.0f) {
                                continue;
                            }
                            if (fabsf(w[1] - ay) > NavigationMapConfiguration::agentMaxClimb) {
                                continue;
                            }
                            inset = rcMin(inset, fabsf(w[bi] - bline));
                        }
                    }
                    const float cs2 = NavigationMapConfiguration::recastCellSize;
                    if (inset < 1e8f) {
                        const int cells = (int)(inset / cs2 + 0.5f);
                        if (cells <= 3) {
                            unlinkedInsetNear++;
                        } else {
                            unlinkedInsetFar++;
                        }
                    } else {
                        unlinkedInsetNone++;
                    }

                    const float len = au1 - au0;
                    if (len >= 12.0f && unlinkedLogged < 300) {
                        // Recast (x,y,z) -> game (x, -z, y)
                        gi.Printf(
                            "    unlinked seam: game(%.0f %.0f %.0f) len %.0f side %d %s dy %.0f inset %s%.0f\n",
                            (va[0] + vb[0]) * 0.5f,
                            -(va[2] + vb[2]) * 0.5f,
                            ay,
                            len,
                            side,
                            overlap ? "counterpart" : "NO-counterpart",
                            overlap ? bestDy : 0.0f,
                            inset < 1e8f ? "" : "none/",
                            inset < 1e8f ? inset : 0.0f
                        );
                        unlinkedLogged++;
                    }
                }
            }
        }
        gi.Printf(
            "  Tiled navmesh links: %d/%d border portals linked (%.1f%%), %d face empty tiles | unlinked: %d no-counterpart, "
            "%d height>climb, %d other | neighbour floor inset: %d near(<=3 cells) %d far %d none | max polys/tile %d\n",
            portalsLinked,
            portalsFacingTile,
            portalsFacingTile ? 100.0f * portalsLinked / portalsFacingTile : 100.0f,
            portalsFacingEmpty,
            unlinkedNoCounterpart,
            unlinkedHeight,
            unlinkedOther,
            unlinkedInsetNear,
            unlinkedInsetFar,
            unlinkedInsetNone,
            maxTilePolys
        );
    }

    delete[] vertsBuffer;
    delete[] indexesBuffer;
}

/*
============
NavigationMap::BuildMeshesForEntities
============
*/
void NavigationMap::BuildMeshesForEntities(RecastBuildContext& buildContext, const navMap_t& navigationMap)
{
    gentity_t        *edict;
    rcPolyMesh       *polyMesh;
    rcPolyMeshDetail *polyMeshDetail;

    for (edict = active_edicts.next; edict != &active_edicts; edict = edict->next) {
        if (!edict->entity || edict->entity == world) {
            continue;
        }

        switch (edict->solid) {
        case SOLID_TRIGGER:
        default:
            continue;
        case SOLID_BSP:
            break;
        }

        if (edict->s.modelindex < 1 || edict->s.modelindex > navigationMap.GetNumSubmodels()) {
            continue;
        }

        const navModel_t& submodel = navigationMap.GetSubmodel(edict->s.modelindex - 1);
        if (!submodel.surfaces.NumObjects()) {
            // Could be a trigger
            continue;
        }

        BuildRecastMesh(buildContext, submodel, edict->entity->origin, edict->entity->angles, polyMesh, polyMeshDetail);

        // (disabled path - kept compiling) entity submodel as its own layer at tile (0,0).
        BuildDetourData(buildContext, polyMesh, polyMeshDetail, 0, 0, edict->s.modelindex, {});

        rcFreePolyMeshDetail(polyMeshDetail);
        rcFreePolyMesh(polyMesh);
    }
}

/*
============
NavigationMap::LoadWorldMap
============
*/
void NavigationMap::LoadWorldMap(const char *mapname)
{
    RecastBuildContext buildContext;
    int                start, end;

    gi.Printf("---- Recast Navigation ----\n");

    //
    // Free up existing navigation if there is one
    //

    if (currentMap != mapname) {
        currentMap = mapname;
        ClearNavigation();
    }

    if (!sv_maxbots->integer) {
        gi.Printf("No bots, skipping navigation\n");
        return;
    }

    if (validNavigation) {
        return;
    }

    //
    // Parse the BSP file into triangles
    //

    try {
        start = gi.Milliseconds();

        navigationData.ProcessBSPForNavigation(mapname);
    } catch (const ScriptException& e) {
        gi.Printf("Failed to load BSP for navigation: %s\n", e.string.c_str());
        ClearNavigation();
        return;
    }

    end = gi.Milliseconds();

    gi.Printf("BSP file loaded and parsed in %.03f seconds\n", (float)((end - start) / 1000.0));

    //
    // Build and create the navigation mesh
    //

    InitializeExtensions();

    m_bTiledFallback = false; // [bug-2861] every map starts on the solo build
    m_bSoloFailed    = false;
    InitializeNavMesh(buildContext, navigationData.navMap);
    InitializeFilter();

    gi.Printf("Building the navigation mesh...\n");

    try {
        start = gi.Milliseconds();

        gi.Printf("  Building the world mesh...\n");
        BuildWorldMesh(buildContext, navigationData.navMap);

        if (m_bSoloFailed) {
            // [bug-2861] The one-tile build is over Detour's per-tile limits for this map (e3l2 "Monte Cassino" at
            // cellSize 8 produced a navmesh with ZERO polys - every bot on it was pathless). Rebuild TILED: small
            // per-tile heightfields, Detour links the tiles. Only maps that fail take this path; the rest keep the
            // v1.9.0 solo mesh unchanged.
            gi.Printf("  Solo navmesh build failed - rebuilding this map TILED\n");
            if (navMeshQuery) {
                dtFreeNavMeshQuery(navMeshQuery);
                navMeshQuery = NULL;
            }
            if (navMeshDt) {
                dtFreeNavMesh(navMeshDt);
                navMeshDt = NULL;
            }
            m_bTiledFallback = true;
            InitializeNavMesh(buildContext, navigationData.navMap);
            BuildWorldMesh(buildContext, navigationData.navMap);
        }

        gi.Printf("  Building meshes for entities...\n");
        // FIXME: TODO
        //  Split everything into chunks and use a tile-based approach
        //BuildMeshesForEntities(buildContext, navigationData.navMap);

    } catch (const ScriptException& e) {
        gi.Printf("Couldn't build recast navigation mesh: %s\n", e.string.c_str());
        ClearNavigation();
        return;
    }

    pathMaster.PostLoadNavigation(*this);
    navigationObstacleMap.Init();

    // Finished processing the map with extensions
    ClearExtensions();

    end = gi.Milliseconds();

    gi.Printf("Recast navigation mesh(es) generated in %.03f seconds\n", (float)((end - start) / 1000.0));
    gi.Printf(
        "^~^~^ NAVHEAD %s marked=%d headclear=%s\n", mapname, s_iHeadClearMarked,
        gi.Cvar_Get("bot_navHeadClear", "0", 0)->string
    ); // [HZM bug-2956]
    s_iHeadClearMarked = 0;
    gi.Printf(
        "  Navmesh: %d auto jump/fall link(s) rejected - take-off or landing on unwalkable slope\n",
        NavLinks_SteepRejects(true)
    ); // [HZM bug-2896]

    validNavigation = true;

    static cvar_t *s_navDump = NULL;
    if (!s_navDump) {
        s_navDump = gi.Cvar_Get("nav_dump", "0", 0);
    }
    if (s_navDump->integer) {
        DumpNavMesh(mapname);
    }

    // [HZM bug-2956 probe] nav_fitprobe "x y z [r]": the free space a PLAYER body has around a point, as the collision
    // sees it (terrain included - the offline BSP tools only read brushes). One row per 4u of y, one char per 4u of x:
    // '.' a standing body (cylinder r15, a step up to the head, z+18..94) fits, 'c' only a crouched one (z+18..49),
    // '#' neither, ' ' no ground within 48u. Plus the ground height row. Dev diagnostic, default off.
    {
        cvar_t     *fp  = gi.Cvar_Get("nav_fitprobe", "", 0);
        const char *cur = fp->string; // several probes: "x y z r;x y z r"
        while (cur && *cur) {
        float     px = 0, py = 0, pz = 0, pr = 32;
        const int got = sscanf(cur, "%f %f %f %f", &px, &py, &pz, &pr);
        cur           = strchr(cur, ';');
        if (cur) {
            cur++;
        }
        if (got >= 3) {
            const int    n = (int)(pr / 4.0f);
            const Vector smin(MINS_X, MINS_Y, STEPSIZE), smax(MAXS_X, MAXS_Y, MAXS_Z);
            const Vector cmax(MAXS_X, MAXS_Y, CROUCH_MAXS_Z);
            const Vector gmin(MINS_X, MINS_Y, 0), gmax(MAXS_X, MAXS_Y, 1);
            gi.Printf("^~^~^ NAVFIT %s at=(%.0f %.0f %.0f) r=%.0f x from %.0f step 4\n", mapname, px, py, pz, pr, px - n * 4);
            int hitEnt[16], hitN[16], nHit = 0; // what blocks the body: entity numbers (ENTITYNUM_WORLD = world/terrain)
            for (int iy = n; iy >= -n; iy--) {
                char row[256], zrow[1024];
                int  zl = 0;
                for (int ix = -n; ix <= n && ix + n < 250; ix++) {
                    const Vector p(px + ix * 4, py + iy * 4, pz);
                    trace_t      g = G_Trace(p + Vector(0, 0, 48), gmin, gmax, p - Vector(0, 0, 48), NULL, MASK_PLAYERSOLID, qtrue, "NavFitG");
                    char         ch = ' ';
                    if (!g.startsolid && g.fraction < 1.0f) {
                        const Vector gp = g.endpos;
                        trace_t      s  = G_Trace(gp, smin, smax, gp, NULL, MASK_PLAYERSOLID, qtrue, "NavFitS");
                        trace_t      c  = G_Trace(gp, smin, cmax, gp, NULL, MASK_PLAYERSOLID, qtrue, "NavFitC");
                        ch              = (!s.startsolid && !s.allsolid) ? '.' : ((!c.startsolid && !c.allsolid) ? 'c' : '#');
                        if (ch != '.') {
                            const int en = s.entityNum;
                            int       k  = 0;
                            while (k < nHit && hitEnt[k] != en) {
                                k++;
                            }
                            if (k == nHit && nHit < 16) {
                                hitEnt[nHit] = en;
                                hitN[nHit++] = 0;
                            }
                            if (k < nHit) {
                                hitN[k]++;
                            }
                        }
                        zl += Com_sprintf(zrow + zl, sizeof(zrow) - zl, "%d,", (int)(gp.z - pz));
                    } else {
                        zl += Com_sprintf(zrow + zl, sizeof(zrow) - zl, "_,");
                    }
                    row[ix + n]     = ch;
                    row[ix + n + 1] = 0;
                }
                gi.Printf("^~^~^ NAVFIT y=%.0f |%s| z=%s\n", py + iy * 4, row, zrow);
            }
            for (int k = 0; k < nHit; k++) {
                gentity_t *he = (hitEnt[k] >= 0 && hitEnt[k] < MAX_GENTITIES) ? &g_entities[hitEnt[k]] : NULL;
                Entity    *e  = (he && he->entity) ? he->entity : NULL;
                gi.Printf(
                    "^~^~^ NAVFIT blocker ent=%d cells=%d class=%s model=%s tn=%s origin=(%.0f %.0f %.0f) abs=(%.0f %.0f %.0f)-(%.0f %.0f %.0f) contents=0x%x\n",
                    hitEnt[k], hitN[k], e ? e->getClassname() : "-", e ? e->model.c_str() : "", e ? e->targetname.c_str() : "",
                    e ? e->origin.x : 0.0f, e ? e->origin.y : 0.0f, e ? e->origin.z : 0.0f, e ? e->absmin.x : 0.0f,
                    e ? e->absmin.y : 0.0f, e ? e->absmin.z : 0.0f, e ? e->absmax.x : 0.0f, e ? e->absmax.y : 0.0f,
                    e ? e->absmax.z : 0.0f, he ? he->r.contents : 0
                );
            }
        }
        }
    }
}

/*
============
NavigationMap::DumpNavMesh

[HZM bug-2860] Write the built Detour mesh as text so its CONNECTIVITY can be analysed offline (components, the
break on a route, which side of a gate a region hangs off). Soak data only shows where bots already go; this shows
where the mesh lets them go. navdump/<map>[_<variant>].txt in the homepath game dir:
    P <ref> <area> <flags> <type> <nverts> x y z ...   one per poly (game coords; type 1 = off-mesh link, 2 verts)
    L <ref> <neighbourRef>                           one per Detour link (tile-internal, portal and off-mesh)
============
*/
void NavigationMap::DumpNavMesh(const char *mapname)
{
    const dtNavMesh *nm = navMeshDt;
    cvar_t          *variant = gi.Cvar_Get("cm_variant", "", 0);
    char             filename[MAX_QPATH];
    char             line[1024];
    fileHandle_t     f;
    int              polys = 0, links = 0;

    if (!nm) {
        return;
    }

    if (variant && variant->string[0]) {
        Com_sprintf(filename, sizeof(filename), "navdump/%s_%s.txt", mapname, variant->string);
    } else {
        Com_sprintf(filename, sizeof(filename), "navdump/%s.txt", mapname);
    }
    f = gi.FS_FOpenFileWrite(filename);
    if (!f) {
        gi.Printf("nav_dump: could not open %s\n", filename);
        return;
    }

    for (int i = 0; i < nm->getMaxTiles(); i++) {
        const dtMeshTile *tile = nm->getTile(i);
        if (!tile || !tile->header) {
            continue;
        }
        const dtPolyRef base = nm->getPolyRefBase(tile);
        for (int j = 0; j < tile->header->polyCount; j++) {
            const dtPoly   *p   = &tile->polys[j];
            const dtPolyRef ref = base | (dtPolyRef)j;
            int             n   = Com_sprintf(
                line, sizeof(line), "P %u %d %d %d %d", (unsigned int)ref, p->getArea(), p->flags, p->getType(), p->vertCount
            );
            for (int k = 0; k < p->vertCount && n < (int)sizeof(line) - 40; k++) {
                float g[3];
                ConvertRecastToGameCoord(&tile->verts[p->verts[k] * 3], g);
                n += Com_sprintf(line + n, sizeof(line) - n, " %.0f %.0f %.0f", g[0], g[1], g[2]);
            }
            n += Com_sprintf(line + n, sizeof(line) - n, "\n");
            gi.FS_Write(line, n, f);
            polys++;
            for (unsigned int l = p->firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
                n = Com_sprintf(line, sizeof(line), "L %u %u\n", (unsigned int)ref, (unsigned int)tile->links[l].ref);
                gi.FS_Write(line, n, f);
                links++;
            }
        }
    }
    gi.FS_FCloseFile(f);
    gi.Printf("nav_dump: %d polys, %d links -> %s\n", polys, links, filename);
}

void NavigationMap::CleanUp(qboolean samemap)
{
    if (!samemap) {
        ClearNavigation();
    }
}
