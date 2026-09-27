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

#pragma once

#include "g_local.h"
#include "navigation_recast_obstacle.h"
#include "doors.h" // [HZM bot A4]
#include "navigation_recast_load.h"
#include "navigation_recast_config.h"
#include "navigation_recast_helpers.h"
#include "entity.h"
#include "trigger.h"
#include "navigation_recast_load_ext.h" // [HZM bug-2893] NavEntityBlock_IsBaked

#include "DetourNavMeshQuery.h"

NavigationObstacleMap navigationObstacleMap;

/**
 * @brief Iterate through polys and set flags
 * 
 */
class NavigationObstacleQueryBase : public dtPolyQuery
{
protected:
    const dtNavMesh         *navMesh;
    NavigationObstacleTiles& tiles;

public:
    float maxRadiusSqr;

public:
    NavigationObstacleQueryBase(const dtNavMesh *inNavMesh, NavigationObstacleTiles& inTiles)
        : navMesh(inNavMesh)
        , tiles(inTiles)
    {}

    float getPolySizeSqr(const dtMeshTile *tile, const dtPoly *poly, Vector& centroid) const
    {
        Vector rcBounds[2];
        Vector rcCentroid;
        Vector size;
        int    i;

        //
        // Calculate the polys bounds
        //

        ClearBounds(rcBounds[0], rcBounds[1]);

        for (i = 0; i < poly->vertCount; i++) {
            const float *pv = &tile->verts[poly->verts[i] * 3];

            AddPointToBounds(pv, rcBounds[0], rcBounds[1]);
        }

        size       = rcBounds[1] - rcBounds[0];
        rcCentroid = (rcBounds[1] + rcBounds[0]) * 0.5;
        ConvertRecastToGameCoord(rcCentroid, centroid);

        return size.lengthSquared();
    }
};

class NavigationObstacleQueryOccupied : public NavigationObstacleQueryBase
{
private:
    int complexEntityNum;

public:
    NavigationObstacleQueryOccupied(
        const dtNavMesh *inNavMesh, NavigationObstacleTiles& inTiles, int inComplexEntityNum
    )
        : NavigationObstacleQueryBase(inNavMesh, inTiles)
        , complexEntityNum(inComplexEntityNum)
    {}

    void process(const dtMeshTile *tile, dtPoly **polys, dtPolyRef *refs, int count) override
    {
        const int tileNum = tile - navMesh->getTile(0);
        int       i;

        for (i = 0; i < count; i++) {
            const int   polyNum = polys[i] - tile->polys;
            Vector      centroid;
            const float radiusSqr = getPolySizeSqr(tile, polys[i], centroid);

            if (radiusSqr > maxRadiusSqr) {
                continue;
            }

            // Still count the poly even if it collides with an object
            // This is because we don't keep history of collision when releasing
            tiles.polysRef[tileNum][polyNum]++;

            if (complexEntityNum != -1) {
                trace_t trace;
                Vector  mins(MINS_X * 0.75, MINS_Y * 0.75, MINS_Z);
                Vector  maxs(MAXS_X * 0.75, MAXS_Y * 0.75, MAXS_Z);

                trace = G_Trace(
                    centroid, mins, maxs, centroid, NULL, CONTENTS_SOLID, qfalse, "NavigationObstacleQueryOccupied"
                );

                if (trace.entityNum != complexEntityNum && trace.entityNum != ENTITYNUM_WORLD) {
                    // No valid collision
                    continue;
                }
            }

            // Mark as busy as long as the poly size is below the allowed radius
            polys[i]->flags |= RECAST_POLYFLAG_BUSY;
        }

        tiles.tilesRef[tileNum]++;
    }
};

class NavigationObstacleQueryReleased : public NavigationObstacleQueryBase
{
public:
    NavigationObstacleQueryReleased(const dtNavMesh *inNavMesh, NavigationObstacleTiles& inTiles)
        : NavigationObstacleQueryBase(inNavMesh, inTiles)
    {}

    void process(const dtMeshTile *tile, dtPoly **polys, dtPolyRef *refs, int count) override
    {
        const int tileNum = tile - navMesh->getTile(0);
        int       i;

        for (i = 0; i < count; i++) {
            const int   polyNum = polys[i] - tile->polys;
            Vector      centroid;
            const float radiusSqr = getPolySizeSqr(tile, polys[i], centroid);

            if (radiusSqr > maxRadiusSqr) {
                continue;
            }

            // Last poly, release it
            if (tiles.polysRef[tileNum][polyNum] == 1) {
                polys[i]->flags &= ~RECAST_POLYFLAG_BUSY;
            }

            assert(tiles.polysRef[tileNum][polyNum] > 0);
            if (tiles.polysRef[tileNum][polyNum] > 0) {
                tiles.polysRef[tileNum][polyNum]--;
            }
        }

        assert(tiles.tilesRef[tileNum] > 0);
        if (tiles.tilesRef[tileNum] > 0) {
            tiles.tilesRef[tileNum]--;
        }
    }
};

const Vector& NavigationObstacleEntities::GetMin(unsigned int entnum) const
{
    return bounds[0][entnum];
}

const Vector& NavigationObstacleEntities::GetMax(unsigned int entnum) const
{
    return bounds[1][entnum];
}

int NavigationObstacleEntities::GetContents(unsigned int entnum) const
{
    return contents[entnum];
}

void NavigationObstacleEntities::SetContents(unsigned int entnum, int c)
{
    contents[entnum] = c;
}

solid_t NavigationObstacleEntities::GetSolidType(unsigned int entnum) const
{
    return solid[entnum];
}

void NavigationObstacleEntities::SetSolidType(unsigned int entnum, solid_t type)
{
    solid[entnum] = type;
}

void NavigationObstacleEntities::SetBounds(unsigned int entnum, const Vector& min, const Vector& max)
{
    bounds[0][entnum] = min;
    bounds[1][entnum] = max;
}

bool NavigationObstacleEntities::IsActive(unsigned int entnum) const
{
    return flag[entnum] & NAVOBS_FLAG_ACTIVE;
}

void NavigationObstacleEntities::Add(unsigned int entnum)
{
    flag[entnum] = NAVOBS_FLAG_ACTIVE;
}

void NavigationObstacleEntities::Set(unsigned int entnum, const Vector& position)
{
    flag[entnum] = NAVOBS_FLAG_ACTIVE;
}

void NavigationObstacleEntities::Remove(unsigned int entnum)
{
    bounds[0][entnum] = vec_zero;
    bounds[1][entnum] = vec_zero;
    flag[entnum]      = 0;
    contents[entnum]  = 0;
    solid[entnum]     = SOLID_NOT;
}

NavigationObstacleTiles::NavigationObstacleTiles()
{
    tilesRef = NULL;
    polysRef = NULL;
}

NavigationObstacleTiles::~NavigationObstacleTiles()
{
    Clear();
}

void NavigationObstacleTiles::Init()
{
    const dtNavMesh *navMesh = navigationMap.GetNavMesh();
    int              numTotalPolys;
    int              i;
    int              totalSize;
    byte            *start;
    byte            *buffer;

    Clear();

    numTiles      = navMesh->getMaxTiles();
    numTotalPolys = 0;

    //
    // Calculate the total number of polys from all tiles
    //

    for (i = 0; i < numTiles; i++) {
        const dtMeshTile *tile = navMesh->getTile(i);
        if (tile && tile->header) {
            numTotalPolys += tile->header->polyCount;
        }
    }

    //
    // Allocate one giant big chunk of memory to hold all that information
    //

    totalSize =
        sizeof(unsigned int) * numTiles + sizeof(unsigned int *) * numTiles + sizeof(unsigned int) * numTotalPolys;
    buffer = start = (byte *)gi.Malloc(totalSize);

    tilesRef = new (buffer) unsigned int[numTiles]();
    buffer += sizeof(unsigned int) * numTiles;

    polysRef = new (buffer) unsigned int *[numTiles];
    buffer += sizeof(unsigned int *) * numTiles;

    for (i = 0; i < numTiles; i++) {
        const dtMeshTile *tile = navMesh->getTile(i);
        if (tile && tile->header) {
            polysRef[i] = new (buffer) unsigned int[tile->header->polyCount]();
            buffer += sizeof(unsigned int) * tile->header->polyCount;
        }
    }

    assert(buffer - start <= totalSize);
}

void NavigationObstacleTiles::Clear()
{
    // Single block of memory
    if (tilesRef) {
        gi.Free(tilesRef);
        tilesRef = NULL;
    }
}

NavigationObstacleEntities::NavigationObstacleEntities()
{
    int i;

    for (i = 0; i < ARRAY_LEN(flag); i++) {
        flag[i] = 0;
    }
}

NavigationObstacleMap::NavigationObstacleMap()
{
    ents = NULL;
}

NavigationObstacleMap::~NavigationObstacleMap()
{
    Clear();
}

void NavigationObstacleMap::Clear()
{
    if (ents) {
        delete ents;
        ents = NULL;
    }

    tiles.Clear();
}

void NavigationObstacleMap::Init()
{
    Clear();

    ents = new NavigationObstacleEntities();
    tiles.Init();
}

void NavigationObstacleMap::Update()
{
    gentity_t *ent;
    int        i;

    if (!ents) {
        return;
    }

    NavEntityBlocks_Update(); // [HZM bug-2901]

    for (i = 0; i < MAX_GENTITIES; i++) {
        ent = &g_entities[i];

        if (i == ENTITYNUM_WORLD) {
            continue;
        }

        if (IsValidEntity(ent)) {
            if (!ents->IsActive(i)) {
                EntityAdded(ent);
            } else if (HasChanged(ent)) {
                EntityChanged(ent);
            }
        } else if (ents->IsActive(i)) {
            EntityRemoved(ent);
        }
    }
}

bool NavigationObstacleMap::IsValidEntity(gentity_t *ent) const
{
    if (!ent->inuse || !ent->entity) {
        return false;
    }

    // [HZM bug-2893] already cut out of the mesh at build time and not moved since: carving it again would only
    // exclude the neighbouring polys too (a gap beside a parked tank closing for no reason)
    if (NavEntityBlock_IsBaked(ent - g_entities, ent->r.absmin)) {
        return false;
    }
    // [bug-3026] a brush slab lower than a step is floor (NavEntity_IsSlab): carving it would shut the floor it forms
    if (NavEntity_IsSlab(ent)) {
        return false;
    }

    if (!IsSpecialEntity(ent)) {
        if (ent->s.solid == SOLID_NOT || ent->s.solid == SOLID_TRIGGER) {
            return false;
        }

        // Same contents as player's clipmask
        if (!(ent->r.contents & MASK_PLAYERSOLID)) {
            return false;
        }
    }

    // Ignore other sentients (moving AI / players are not static navmesh obstacles)...
    // [user 2026-09-21] ...BUT NOT VEHICLES. A Vehicle is a Sentient (VehicleTank : DrivableVehicle : ... :
    // Sentient), so this line was silently excluding the parked scenic tanks/turrets on the MP arena maps
    // from the obstacle map - they were never carved from the bot navmesh, so bots pathed their SHORTEST
    // route straight into the solid tank and jammed (the user's "stuck on the tank", still happening after
    // respawn because a fresh bot re-picks that shortest route). Carving the vehicle punches a hole the
    // pathfinder routes around. This Recast mesh only builds for MP (sv_maxbots>0); coop uses the legacy
    // navigate.cpp A*, so coop pathing is untouched. A driven vehicle simply moves its hole (the obstacle
    // system already re-tiles changed entities).
    if (ent->entity->IsSubclassOfSentient() && !ent->entity->IsSubclassOfVehicle()
        && !ent->entity->IsSubclassOfVehicleTurretGun()) {
        return false;
    }

    // Ignore doors as they can be interacted
    if (ent->entity->IsSubclassOfDoor()) {
        // [HZM bot A4] ...except a SHUT door no player can open by USE - locked, jammed (health>0), or trigger/auto-only
        // (spawnflags 64/128, doors.cpp DOOR_AUTO_OPEN/DOOR_TARGETED): for routing that is a wall. Carving it makes A*
        // go round instead of sending bots to push on it forever. The moment it opens (or unlocks) the carve is released.
        Door *door = static_cast<Door *>(ent->entity);
        const bool unusable = !door->CanBeOpenedBy(ent->entity) || door->health > 0 || (door->spawnflags & (64 | 128));
        if (!unusable || !door->isCompletelyClosed()) {
            return false;
        }
    }

    return true;
}

bool NavigationObstacleMap::HasChanged(gentity_t *ent) const
{
    const int entnum = ent - g_entities;

    if (ents->GetMin(entnum) != ent->r.absmin || ents->GetMax(entnum) != ent->r.absmax) {
        return true;
    }

    if (ents->GetSolidType(entnum) != ent->solid) {
        return true;
    }

    if (ents->GetContents(entnum) != ent->r.contents) {
        return true;
    }

    return false;
}

bool NavigationObstacleMap::IsSpecialEntity(gentity_t *ent) const
{
    const Trigger *trig;

    //
    // FIXME: Use an alternative solution other than hardcoding
    //

    if (!ent->entity->isSubclassOf(Trigger)) {
        return false;
    }

    trig = static_cast<Trigger *>(ent->entity);

    if (str::icmp(ent->entity->targetname, "minefield")) {
        return false;
    }

    return true;
}

void NavigationObstacleMap::EntityAdded(gentity_t *ent)
{
    int entnum = ent - g_entities;

    ents->Add(entnum);
    ents->SetBounds(entnum, ent->r.absmin, ent->r.absmax);
    ents->SetSolidType(entnum, ent->solid);
    ents->SetContents(entnum, ent->r.contents);

    // Mark the poly as occupied
    EngagePolysAt(ent, ent->r.absmin, ent->r.absmax);

    // [user 2026-09-21] CARVE DIAGNOSTIC: bot_navObstDebug 1 logs every entity carved out of the bot navmesh
    // (class + entnum + solidity + bounds), so a playtest reveals whether the parked tank AND its separate
    // collision entity are actually being avoided. Fires only on the add edge and only when the cvar is on.
    {
        static cvar_t *s_navDbg = NULL;
        if (!s_navDbg) {
            s_navDbg = gi.Cvar_Get("bot_navObstDebug", "0", 0);
        }
        if (s_navDbg->integer && ent->entity) {
            gi.Printf("^~^~^ NAVOBST carve ent=%d class=%s solid=%d min=(%.0f %.0f %.0f) max=(%.0f %.0f %.0f)\n",
                      entnum, ent->entity->getClassID(), ent->solid, ent->r.absmin[0], ent->r.absmin[1],
                      ent->r.absmin[2], ent->r.absmax[0], ent->r.absmax[1], ent->r.absmax[2]);
        }
    }
}

void NavigationObstacleMap::EntityRemoved(gentity_t *ent)
{
    int entnum = ent - g_entities;

    // Release the poly
    ReleasePolysAt(ent, ents->GetMin(entnum), ents->GetMax(entnum));

    ents->Remove(entnum);
}

void NavigationObstacleMap::EntityChanged(gentity_t *ent)
{
    int entnum = ent - g_entities;

    // Release polys at the old location
    ReleasePolysAt(ent, ents->GetMin(entnum), ents->GetMax(entnum));

    // Mark polys as occupied at the new location
    EngagePolysAt(ent, ent->r.absmin, ent->r.absmax);
    ents->SetBounds(entnum, ent->r.absmin, ent->r.absmax);
    ents->SetSolidType(entnum, ent->solid);
    ents->SetContents(entnum, ent->r.contents);
}

void NavigationObstacleMap::EngagePolysAt(gentity_t *ent, const Vector& min, const Vector& max)
{
    dtNavMeshQuery *navMeshQuery = navigationMap.GetNavMeshQuery();
    // Use complex collision for BSP brushes
    NavigationObstacleQueryOccupied query(
        navigationMap.GetNavMesh(), tiles, ent->solid == SOLID_BSP ? ent - g_entities : -1
    );

    Vector center;
    Vector halfExtents;
    Vector size;
    float  radiusSqr;
    center      = (max + min) * 0.5;
    size        = max - min;
    halfExtents = size * 0.5;

    radiusSqr = halfExtents.lengthXYSquared();
    // [bug-2792] Minimum obstacle size to carve from the bot navmesh, host-tunable (default 55, was hardcoded
    // 100). At 100 a tank/vehicle WRECK (~60-70u XY half-extent) fell UNDER the gate and was never carved, so
    // bots had no path around it and piled against it (the user's report). 55 carves medium props like the
    // tank while still ignoring small barrels/crates a bot walks around. Raise it to ignore more.
    static cvar_t *s_botObsMin = NULL;
    if (!s_botObsMin) {
        s_botObsMin = gi.Cvar_Get("bot_obstacleMinRadius", "55", CVAR_ARCHIVE);
    }
    float fMin = s_botObsMin->value;
    if (fMin < 16.0f) {
        fMin = 16.0f;
    }
    if (radiusSqr < (fMin * fMin)) {
        return;
    }

    // Allow the object to cover polygons up to twice its size
    query.maxRadiusSqr = radiusSqr * Square(1.5);

    float rcCenter[3];
    float rcHalfExtents[3];
    ConvertGameToRecastCoord(center, rcCenter);
    ConvertGameToRecastExtents(halfExtents, rcHalfExtents);

    dtQueryFilter filter;
    navMeshQuery->queryPolygons(rcCenter, rcHalfExtents, &filter, &query);
}

void NavigationObstacleMap::ReleasePolysAt(gentity_t *ent, const Vector& min, const Vector& max)
{
    dtNavMeshQuery                 *navMeshQuery = navigationMap.GetNavMeshQuery();
    NavigationObstacleQueryReleased query(navigationMap.GetNavMesh(), tiles);

    Vector center;
    Vector halfExtents;
    Vector size;
    float  radiusSqr;
    center      = (max + min) * 0.5;
    size        = max - min;
    halfExtents = size * 0.5;

    radiusSqr = halfExtents.lengthXYSquared();
    // [HZM bug-2894] the SAME gate as EngagePolysAt (bot_obstacleMinRadius, 55 since bug-2792): this one was left at the
    // stock 100, so a prop of radius 55-100 that moved or was removed (a driven vehicle, an opened door, a destroyed
    // wreck) was carved and never released - its old polys stayed excluded for the rest of the map
    static cvar_t *s_botObsMinR = NULL;
    if (!s_botObsMinR) {
        s_botObsMinR = gi.Cvar_Get("bot_obstacleMinRadius", "55", CVAR_ARCHIVE);
    }
    const float fMinR = Q_max(16.0f, s_botObsMinR->value);
    if (radiusSqr < Square(fMinR)) {
        return;
    }

    query.maxRadiusSqr = radiusSqr * Square(1.5);

    float rcCenter[3];
    float rcHalfExtents[3];
    ConvertGameToRecastCoord(center, rcCenter);
    ConvertGameToRecastExtents(halfExtents, rcHalfExtents);

    dtQueryFilter filter;
    navMeshQuery->queryPolygons(rcCenter, rcHalfExtents, &filter, &query);
}
