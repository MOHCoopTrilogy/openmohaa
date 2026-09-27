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
 * @brief Configuration
 * 
 */

#pragma once

#include "bg_public.h"

namespace NavigationMapConfiguration
{
    // [bug-2833 2026-09-23, user-approved] 8.0 - the v1.9.0 value. History: 10.25 original (coarse, bots pathed into
    // bare geometry) -> 8.0 (bug-2829; 6.0 SOLO hard-crashed m3l3) -> 6.0 TILED (crash-safe, but its erosion
    // walkableRadius=ceil(8/6)=2 cells=12u narrowed doors/passages: NEW no-path on t2l1 + e3l2 allies) -> back to
    // 8.0 (walkableRadius=ceil(8/8)=1 cell=8u). The reason for 6.0 (m5l1a/m5l3 "chokes") dissolved: m5l1a is axis
    // spawns behind a map-end gate, m5l3 was pulled from Push. Only built for MP push (coop sv_maxbots 0 never
    // builds this). Going finer again needs useTiledBuild (below) AND an erosion answer (agentRadius or rounding).
    static const float recastCellSize   = 8.0;
    static const float recastCellHeight = 1.0;
    static const float agentHeight      = MAXS_Z;
    static const float agentMaxClimb    = STEPSIZE;

    // normal of { 0.714142799, 0, 0.700000048 }, or an angle of -44.4270058
    static const float agentMaxSlope = 45.5729942f;

    // [bug-2787] was 1.0 (one unit) - path centrelines hugged walls and obstacle edges, so MP bots scraped
    // and stuck on geometry (the tank/rubble the user reported). Eroding the walkable surface by a real
    // clearance keeps bot paths off walls so they route AROUND obstacles. 8 is ~half the bot half-width and
    // ~0.8 of a recast cell (10.25) - meaningful clearance without closing the game's normal-width passages.
    // Affects only the Recast MP-bot navmesh; coop actors use the separate legacy navigate.cpp A*, and coop
    // maps (sv_maxbots 0) never build this mesh, so it cannot change coop pathing.
    static const float agentRadius          = 8.0;
    // [bug-2829 nav experiment 2026-09-22] was 5 - pruned small ledges/platforms out of the mesh so bots there
    // had NO path (part of m5l1a's no-path, which dropped 16%->3% with 2). 2 keeps more small walkable islands
    // connected (regionMergeSize still folds true noise into neighbours). Note this is CELLS^2, so at cellSize 8
    // the min kept area is 2^2*8^2 = 256 sq units. Unrelated to the m3l3 crash (that was cell COUNT, see above).
    static const int   regionMinSize        = 2;
    static const int   regionMergeSize      = 20;
    static const float edgeMaxLen           = 100.0;
    static const float edgeMaxError         = 1.3f;
    static const int   vertsPerPoly         = 6;
    static const float detailSampleDist     = 12.0;
    static const float detailSampleMaxError = 1.3f;
    // [bug-2831/2833] TILED Recast build (navigation_recast_load.cpp::BuildWorldMesh): the world split into
    // tileSize x tileSize CELL tiles, each its own bounded heightfield with a borderSize margin so adjacent tiles'
    // edge polys align and Detour links them. Caps per-tile cells (256^2~65k, ~100x under the solo crash), so finer
    // cells are safe on any map; nav-gen 1-3s. OFF by default: validated crash-free on 24 maps, but 3-13% of tile-
    // border portals stay one-sided/unlinked (nav-gen log "Tiled navmesh links" classifies them) and the asymmetry
    // cause is not yet found. false = the SOLO build (one heightfield for the whole map) exactly as shipped in v1.9.0.
    static const bool  useTiledBuild        = false;
    static const int   tileSize             = 256;
} // namespace NavigationMapConfiguration

// Polyflags
static constexpr unsigned int RECAST_POLYFLAG_WALKABLE = (1 << 0);
static constexpr unsigned int RECAST_POLYFLAG_BUSY     = (1 << 1);
