/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCRAWPATHDRIVE_H
#define _PLAYERBOT_DCRAWPATHDRIVE_H

#include <cstdint>
#include <vector>

#include "G3D/Vector3.h"

#include "Ai/Dungeon/DungeonClear/Util/DcProgressWatchdog.h"

// The PURE core of driving a bot along a raw 3D polyline — the cursor advance,
// the arrival test, the off-leg test and the progress watchdog — shared by the
// auto-detect swim (DriveActiveSwim) and the authored RawPath event step
// (DcPartyWideChain::DriveStep). One tick = one Step() call: the caller feeds
// the live position and whether its spline is still in flight, and acts on the
// verdict (issue a spline window from `cursor`, leave it alone, re-issue after
// a wedge, or hand back). No game state in here, so every rule is unit-tested
// with a fake position feed (t/TestBlackfathomDeeps.cpp).
namespace DcRawPathDrive
{
    enum class Verdict : std::uint8_t
    {
        Arrived,   // within arriveRadius (3D) of the LAST point — the leg is done
        InFlight,  // a spline is running and progress is healthy — leave it alone
        Issue,     // nothing running — issue a spline window from `cursor`
        Wedged,    // no 3D progress for stuckMs — re-issue from `cursor` (the
                   // caller counts wedges and escalates on the second)
        OffLeg,    // implausibly far from the current point (teleport, knockback,
                   // stale leg) — abandon
    };

    struct Params
    {
        float pointReached = 3.0f;      // a point is consumed within this (3D)
        float offLegMax = 50.0f;        // further than this from the current point => OffLeg
        float minClose = 0.5f;          // TickClosing's progress epsilon
        std::uint32_t stuckMs = 6000;   // no progress for this long => Wedged
    };

    struct Decision
    {
        Verdict verdict = Verdict::Issue;
        std::uint32_t cursor = 0;       // the (possibly advanced) cursor to drive from
    };

    // `cursor` indexes the next point to reach; `watch` is the leg's closing-
    // distance watchdog (re-armed here whenever the cursor advances, so each
    // point gets its own budget). The last point is never consumed by
    // pointReached alone: it stays the target until within arriveRadius, so a
    // generous pointReached cannot end the leg short.
    inline Decision Step(std::vector<G3D::Vector3> const& pts, std::uint32_t cursor,
                         G3D::Vector3 const& pos, float arriveRadius, bool splineRunning,
                         std::uint32_t nowMs, DcProgressWatchdog& watch, Params const& p = Params())
    {
        Decision d;
        if (pts.empty())
        {
            d.verdict = Verdict::Arrived;
            return d;
        }
        std::uint32_t const last = static_cast<std::uint32_t>(pts.size()) - 1;
        if ((pos - pts[last]).length() <= arriveRadius)
        {
            d.verdict = Verdict::Arrived;
            d.cursor = last;
            return d;
        }
        // Consume up to the FURTHEST point within reach, not only a run of them
        // from the cursor: a tick can sample the bot after it has already
        // passed a point without ever being within pointReached of it, and a
        // cursor left on that point behind it would read as no progress and be
        // re-issued backwards.
        std::uint32_t const before = cursor;
        if (cursor > last)
            cursor = last;
        for (std::uint32_t k = last; k-- > cursor;)
            if ((pos - pts[k]).length() <= p.pointReached)
            {
                cursor = k + 1;
                break;
            }
        d.cursor = cursor;
        if (cursor != before)
            watch.Reset();  // a fresh point, a fresh budget

        float const distToPoint = (pos - pts[cursor]).length();
        if (distToPoint > p.offLegMax)
        {
            d.verdict = Verdict::OffLeg;
            return d;
        }

        bool const progressed = watch.TickClosing(distToPoint, p.minClose, nowMs);
        if (!progressed && nowMs - watch.lastProgressMs > p.stuckMs)
        {
            d.verdict = Verdict::Wedged;
            return d;
        }
        d.verdict = splineRunning ? Verdict::InFlight : Verdict::Issue;
        return d;
    }
}

#endif  // _PLAYERBOT_DCRAWPATHDRIVE_H
