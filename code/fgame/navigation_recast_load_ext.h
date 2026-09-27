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
 * @brief Extension for recast-based navigation
 * 
 */

#pragma once

#include "../qcommon/class.h"
#include "../qcommon/vector.h"
#include "../qcommon/container.h"

struct rcPolyMesh;
struct rcCompactHeightfield;
class rcContext;

// HZM [bug-2842] "block" volumes from navlinks/<map>[_<variant>].txt -> RC_NULL_AREA (see the .cpp)
void ManualBlocks_Apply(rcContext *ctx, rcCompactHeightfield& chf);
// HZM [bug-2893] a static prop the mesh build already cut out, still where it was then (the runtime carve skips it)
bool NavEntityBlock_IsBaked(int entnum, const Vector& absmin);
bool NavEntity_IsSlab(const gentity_t *ge); // [bug-3026] a brush entity lower than a step: floor, not an obstacle
// HZM [bug-2901] release the cut of a prop that was removed / made non-solid / moved (every frame, obstacle map)
void NavEntityBlocks_Update();
int  NavEntityBlocks_TakeReleased(); // releases since last asked (nav_dump rewrites the dump after them)
// HZM [bug-2896] auto links dropped because an end stood on unwalkable slope (reset=true: read and clear, per map)
int NavLinks_SteepRejects(bool reset);
// HZM [bug-2898] a bot froze running into unwalkable slope: re-cost the poly ahead of it to last-resort (see the .cpp)
int NavLearn_MarkSteep(const Vector& at, const Vector& dir);
// HZM [bug-2913] a bot hopped at a navmesh JUMP link 3 times and never got across: switch that link off for everyone
int NavLearn_DisableJumpLink(const Vector& a, const Vector& b);

// HZM [bug-2912] a navlinks `elevator` line that names its movers - read by the bot lift protocol (LiftThink)
#define NAVLIFT_MAX 4
struct navLift_t {
    Vector end[2];        // the two landings, as written in the navlinks line
    char   cab[64];       // targetname of the cab mover
    char   gate[2][64];   // targetname of the gate at end[0] / end[1]
};
void             NavLift_Parse(const char *line, const Vector& a, const Vector& b);
const navLift_t *NavLift_Find(const Vector& p, const Vector& q); // either order, 16u tolerance
void             NavLift_Clear();

/**
 * @brief An offmesh point that the navigation system will use to find path.
 *
 */
struct offMeshNavigationPoint {
    Vector         start;
    Vector         end;
    float          radius;
    unsigned short flags;
    unsigned char  area;
    bool           bidirectional;
    int            id;

    offMeshNavigationPoint()
        : radius(0)
        , flags(0)
        , area(0)
        , bidirectional(true)
        , id(0)
    {}

    bool operator==(const offMeshNavigationPoint& other) const { return start == other.start && end == other.end; }

    bool operator!=(const offMeshNavigationPoint& other) const { return !(*this == other); }
};

struct ExtensionArea {
    float         cost;
    unsigned char number;

    ExtensionArea(unsigned char number, float cost)
    {
        this->number = number;
        this->cost   = cost;
    }
};

class INavigationMapExtension : public Class
{
private:
    CLASS_PROTOTYPE(INavigationMapExtension);

public:
    virtual void Handle(Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh) {}

    virtual Container<ExtensionArea> GetSupportedAreas() const { return Container<ExtensionArea>(); }
};

class NavigationMapExtension_Ladders : public INavigationMapExtension
{
private:
    CLASS_PROTOTYPE(NavigationMapExtension_Ladders);

public:
    void                     Handle(Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh) override;
    Container<ExtensionArea> GetSupportedAreas() const override;
};

class NavigationMapExtension_JumpFall : public INavigationMapExtension
{
private:
    CLASS_PROTOTYPE(NavigationMapExtension_JumpFall);

public:
    void                     Handle(Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh) override;
    Container<ExtensionArea> GetSupportedAreas() const override;

private:
    void                   FixupPoint(vec3_t pos);
    bool                   AddPoint(Container<offMeshNavigationPoint>& points, const offMeshNavigationPoint& point);
    bool                   AreVertsValid(const vec3_t pos1, const vec3_t pos2) const;
    offMeshNavigationPoint CanConnectFallPoint(const rcPolyMesh *polyMesh, const Vector& pos1, const Vector& pos2);
    offMeshNavigationPoint CanConnectJumpPoint(const rcPolyMesh *polyMesh, const Vector& pos1, const Vector& pos2);
    offMeshNavigationPoint CanConnectJumpOverLedgePoint(const rcPolyMesh *polyMesh, const Vector& pos1, const Vector& pos2);
    offMeshNavigationPoint CanConnectStraightPoint(const rcPolyMesh *polyMesh, const Vector& pos1, const Vector& pos2);
};

/**
 * @brief HZM [bug-2837] Hand-placed off-mesh links for gaps the automatic JumpFall generator cannot bridge (its caps:
 * 256u horizontal / 128u jump / 600u fall). Read from navlinks/<map>.txt and navlinks/<map>_<variant>.txt.
 */
class NavigationMapExtension_ManualLinks : public INavigationMapExtension
{
private:
    CLASS_PROTOTYPE(NavigationMapExtension_ManualLinks);

public:
    void                     Handle(Container<offMeshNavigationPoint>& points, const rcPolyMesh *polyMesh) override;
    Container<ExtensionArea> GetSupportedAreas() const override; // [bug-2862] RECAST_AREA_ELEVATOR cost
};
