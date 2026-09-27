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

namespace NavigationMapExtensionConfiguration
{
    static const float agentJumpHeight = 56;

    static const float smallFallHeight = 100;
    static const float mediumFallHeight = 250;
    static const float maxFallHeight = 600;
}

// Areas
static constexpr unsigned char RECAST_AREA_LADDER = 32;
static constexpr unsigned char RECAST_AREA_STRAIGHT = 33;
static constexpr unsigned char RECAST_AREA_JUMP = 34;
static constexpr unsigned char RECAST_AREA_FALL = 35;
static constexpr unsigned char RECAST_AREA_MEDIUM_FALL = 36;
static constexpr unsigned char RECAST_AREA_HIGH_FALL = 37;
// [HZM bug-2862] hand-placed ELEVATOR ride (navlinks "elevator" line): a bot waits at the gate / in the cab until the
// Push script's auto-elevator carries it to the other landing (playerbot_movement.cpp suppresses stuck recovery).
static constexpr unsigned char RECAST_AREA_ELEVATOR = 38;
// [HZM bug-2913] a JUMP link bots proved they cannot make (3 hops, never across): costed x60 - LAST RESORT, never
// removed (user: "If it's the absolute only way, the bot needs to know that"). Moves like a JUMP link.
static constexpr unsigned char RECAST_AREA_JUMP_HARD = 39;
static inline bool IsRecastJumpArea(unsigned char a) { return a == RECAST_AREA_JUMP || a == RECAST_AREA_JUMP_HARD; }
// [HZM bug-2865] HEAT-MAP cost tiers (navlinks "cost" lines, generated per map from soak data by gen_navheat.py):
// walkable ground that bots demonstrably get stuck on / die in, costed x2 / x4 / x8 / x16 so A* routes round it when
// any sane alternative exists. Still WALKABLE (flags + jump/fall links) - a tier never disconnects the mesh.
static constexpr unsigned char RECAST_AREA_COST1 = 48;
static constexpr unsigned char RECAST_AREA_COST4 = 51;
// [HZM bug-2887] STEEP but walkable ground (steeper than bot_navSteepDeg, default 33, up to the 45.6 deg walk limit):
// players slow, slide and have to hop on it, so it costs x4 - routes take the road / the gentle ramp and climb a steep
// face only when it is truly the only way (user: "allies still jumping trying to get up the hill ... vs taking the
// obvious road"). Walkable (flags + jump/fall links), never a disconnect.
static constexpr unsigned char RECAST_AREA_STEEP = 52;
// [HZM bug-2887] ...and a LAST-RESORT tier above bot_navSteepHardDeg (default 38): ground a player cannot walk up
// without hopping and sliding. Costed x25 - never chosen while ANY other way exists, but never removed either: where it
// is the only way between two places the route still finds it (user: "if it's the absolute only way, the bot needs to
// know that") - no per-map exceptions, and no way to cut a map in two.
static constexpr unsigned char RECAST_AREA_STEEP_LAST = 53;
// [HZM bug-2901] the footprint of a static solid PROP (EntityBlocks_Apply): its own polys, carrying RECAST_POLYFLAG_PROP,
// which the query filter excludes - so a route goes round it exactly as round a wall, but REVERSIBLY: the moment the prop
// is removed, made non-solid or moved (an MP script clearing SP debris, ~1-15s into the round), NavEntityBlocks_Update
// turns its polys back into plain walkable ground. Not in IsRecastWalkableArea (no jump/fall links start on a prop).
static constexpr unsigned char RECAST_AREA_PROP        = 54;
static constexpr unsigned short RECAST_POLYFLAG_PROP   = (1 << 2);
// [HZM bug-2926] ground inside a TRIGGER_HURT (a burning wreck, a fire): costed x40 - routes go round it whenever any other
// way exists; walkable (flags + jump/fall links), never a disconnect - a fire across the only street is still walked.
static constexpr unsigned char RECAST_AREA_HURT = 55;
static inline bool IsRecastWalkableArea(unsigned char area)
{
    return area == 63 /* RC_WALKABLE_AREA */ || (area >= RECAST_AREA_COST1 && area <= RECAST_AREA_COST4)
        || area == RECAST_AREA_STEEP || area == RECAST_AREA_STEEP_LAST || area == RECAST_AREA_HURT;
}
