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

#include "navigation_path.h"

class dtNavMesh;
class dtPathCorridor;
class NavigationMap;
struct DetourData;

/**
 * @brief Detour (recastnavigation) based navigation system
 * 
 */
class RecastPather : public IPather
{
public:
    RecastPather();
    ~RecastPather();

    virtual void FindPath(const Vector& start, const Vector& end, const PathSearchParameter& parameters) override;
    virtual void
    FindPathNear(const Vector& start, const Vector& end, float radius, const PathSearchParameter& parameters) override;
    virtual void FindPathAway(
        const Vector&              start,
        const Vector&              avoid,
        const Vector&              preferredDir,
        float                      radius,
        const PathSearchParameter& parameters
    ) override;
    virtual bool TestPath(const Vector& start, const Vector& end, const PathSearchParameter& parameters) override;

    virtual void UpdatePos(const Vector& origin) override;
    virtual void Clear() override;

    virtual PathNav GetNode(unsigned int index) const override;
    virtual int     GetNodeCount() const override;
    virtual Vector  GetCurrentDelta() const override;
    virtual Vector  GetCurrentDirection() const override;
    virtual Vector  GetDestination() const override;
    virtual bool    HasReachedGoal(const Vector& origin) const override;
    virtual bool    IsQuerying() const override;
    virtual unsigned char GetTraversingArea() const override; // [HZM bug-2862]
    virtual unsigned char GetApproachingArea() const override; // [HZM bug-2862]
    virtual bool          GetCornerAfterNext(Vector& out) const override; // [HZM bot A3]
    virtual int           GetCorners(Vector *out, int maxCorners) const override;     // [HZM bot breach]
    virtual bool          GetApproachingLink(Vector& from, Vector& to) const override; // [HZM bot breach]
    virtual bool          GetElevatorEnd(Vector& out) const override;                  // [HZM bug-2886]
    virtual bool          GetElevatorLink(Vector& start, Vector& end) const override; // [HZM bug-2912]
    virtual bool          GetTraversingLink(Vector& start, Vector& end) const override; // [HZM bug-2913]
    virtual int           GetSteerMode() const override { return steerMode; }            // [HZM bug-2956]

private:
    void ResetPosition(const Vector& origin);

private:
    DetourData *detourData;
    bool        moving;
    Vector      lastorg;
    Vector      lastValidOrg;
    Vector      currentNodePos;
    int         lastCheckTime;
    int         traversingOffMeshLink;
    unsigned char traversingArea; // [HZM bug-2862] area of that link (RECAST_AREA_*), 0 when not traversing
    unsigned char approachingArea; // [HZM bug-2862] area of the off-mesh link the path reaches within 128u (0 = none)
    int           elevatorPhase;   // [HZM bug-2862] 1 = walking to the cab centre, 2 = in the cab / heading out
    Vector        elevatorMid;     // [HZM bug-2862] cab centre (link midpoint, game coords)
    Vector        elevatorEnd;     // [HZM bug-2862] far landing
    Vector        elevatorStart;   // [HZM bug-2912] near landing (the lift protocol needs both ends)
    Vector        linkStart;       // [HZM bug-2913] ends of the off-mesh link being crossed (any kind)
    Vector        linkEnd;
    Vector        padCorner;       // [HZM bug-2956] the corner the last turn pad was worked out for, and its inward side
    Vector        padInward;       //   (kept while the bot closes in on that same corner - the pad used to switch off <24u)
    Vector        slideTarget;     // [HZM bug-2956] pinned on the way to a corner: slide along the obstacle to here
    int           slideUntil;
    int           steerMode;       // [HZM bug-2956] probe (GetSteerMode)
    Vector        gapRaw;          // [bug-3146 bot_gapCentre] the raw corner the gap test was run for
    Vector        gapOut;          //   and where to steer instead (== gapRaw when it is not a narrow gap)
};

class RecastPathMaster
{
public:
    void PostLoadNavigation(const NavigationMap& map);
    void ClearNavigation();
    void Update();
};

extern RecastPathMaster pathMaster;
