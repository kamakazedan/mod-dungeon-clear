/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPOOLEXITDECISION_H
#define _PLAYERBOT_DCPOOLEXITDECISION_H

#include <algorithm>
#include <cmath>
#include <vector>

#include "G3D/Vector3.h"

// The PURE core of "a bot fell off a walkway into a pool the navmesh cannot
// climb out of — swim it back to the ramp". No game state in here; the BFD
// driver (Overrides/BlackfathomDeepsDriver.cpp) feeds it live positions, floor
// heights and the Detour corridor, so every rule is unit-tested
// (t/TestBlackfathomDeeps.cpp).
//
// Why it exists: Blackfathom Deeps' Kelris is reached along a WALKWAY (navmesh
// island #0) above the Forgotten Pool. A bot that falls off lands on the pool
// floor (#1) or surface sheet (#7), neither linked to #0, so every route
// rebuilt from the water dead-ends ~12yd under Kelris. The tank reads as
// "within engage range" there (2D), so the approach takes the benign
// rebuild-and-yield branch forever instead of swimming or stalling — every
// timeout of sk-20261022-002111 (3/3 runs at Kelris). The only way out is the
// ramp at the pool's north end; the route to it along the floor is long and
// not straight, so it is Detour's floor corridor, not an authored line.
namespace DcPoolExit
{
    struct Pool
    {
        // Horizontal extent of the pool (the floor island's bbox).
        float minX, maxX, minY, maxY;
        // The water surface. Above it a bot is on land (the walkway).
        float surfaceZ;
        // Water at least this deep (surface - floor) under the bot is "in the
        // pool". The walkway's north end is ankle-deep water at the surface
        // level (-31.3 vs -30.9), which this keeps out.
        float minDepth;
        // The foot of the ramp ON the floor island — the Detour target.
        G3D::Vector3 rampFoot;
        // The top of the ramp ON the walkway island (user .gps) — the leg's end.
        G3D::Vector3 rampTop;
        // Within this (3D) of rampTop the bot is out.
        float doneRadius;
        // Swim this far above the floor corridor (Detour's Z is the floor).
        float floorLift;
    };

    inline bool InBox(Pool const& p, float x, float y)
    {
        return x >= p.minX && x <= p.maxX && y >= p.minY && y <= p.maxY;
    }

    // Is a bot at (x,y,z), with floor height `floorZ` under it, in the pool?
    // In the box, at or below the surface (two yards of slack for a bot bobbing
    // on it — tr-20261022-131010-690's tank parked at -29.5), over deep water,
    // and not already standing at the ramp top.
    inline bool InPool(Pool const& p, float x, float y, float z, float floorZ)
    {
        if (!InBox(p, x, y))
            return false;
        if (z > p.surfaceZ + 2.0f)
            return false;
        if (p.surfaceZ - floorZ < p.minDepth)
            return false;
        return (G3D::Vector3(x, y, z) - p.rampTop).length() > p.doneRadius;
    }

    // The swim leg from Detour's floor corridor: drop the leading start point,
    // lift each point `floorLift` off the floor (never above the surface), make
    // sure the leg reaches the ramp foot, and end on the ramp top. A missing or
    // partial corridor degrades to [rampFoot, rampTop] — a straight swim the
    // driver's wedge watchdog bounds.
    inline std::vector<G3D::Vector3> BuildLeg(Pool const& p, std::vector<G3D::Vector3> const& corridor)
    {
        std::vector<G3D::Vector3> leg;
        float const ceiling = p.surfaceZ - 0.5f;
        for (size_t i = 1; i < corridor.size(); ++i)
        {
            G3D::Vector3 q = corridor[i];
            q.z = std::min(q.z + p.floorLift, ceiling);
            leg.push_back(q);
        }
        G3D::Vector3 foot = p.rampFoot;
        foot.z = std::min(foot.z + p.floorLift, ceiling);
        if (leg.empty() || (leg.back() - foot).length() > 3.0f)
            leg.push_back(foot);
        leg.push_back(p.rampTop);
        return leg;
    }
}

#endif  // _PLAYERBOT_DCPOOLEXITDECISION_H
