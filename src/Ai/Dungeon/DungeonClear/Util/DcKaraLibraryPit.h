/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCKARALIBRARYPIT_H
#define _PLAYERBOT_DCKARALIBRARYPIT_H

// Karazhan (map 532): the void under the Guardian's Library. Pure — no game
// state — so the gtests (t/TestKarazhan, DcKarazhanTest) pin the numbers.
//
// The library's lower floor (z 136) and its stair up to the z 152 landing sit
// on navmesh island #1, the castle interior that also holds Illhoof, Aran and
// Netherspite. Directly beneath it, ~25yd down, is the castle shell: island #0,
// meshed but with NO link to #1. A tank that ends up there reads every route to
// the next boss as `complete=false`, ending ~52yd short, and the run dies on the
// 120s stall. tr-20261022-194409-1044 (sk-20261022-002111) is the case: dragging
// an Arcane Protector pull back up the stair, the tank was carried back down it
// and through the floor at its foot — 136.0 at (-11244.2,-1879.0), 107.4 at
// (-11244.8,-1879.7) ten seconds later — then wandered the shell to
// (-11203.2,-1729.7,119.1) under Illhoof, and stranded-recovery teleported a
// member down after it.
//
// The VMAP floor is SOLID there (136.0 with the shell at ~105-111 below), so this
// is not a hole to plug — a server-side spline does not collide. Whatever carries
// a member down, the way back is the same: a teleport onto island #1.
//
// The box was checked offline against the dev mmaps (bfd_islands/points on a 5yd
// grid, z 95-125): every island-#1 poly under z 128 in x -11280..-11170 /
// y -1920..-1670 lies at x >= -11175 (a z~92 corridor east of the box); the
// rest is island #0 shell. No DC route runs in it.
namespace DcKaraLibraryPit
{
    constexpr unsigned MAP_KARAZHAN = 532;
    // The rescue event (KarazhanEvents.cpp). Karazhan's events are 1-3; hook ids
    // are one flat space across every dungeon and 64 is Stratholme's.
    constexpr unsigned EVENT_PIT_RESCUE = 4;
    constexpr unsigned HOOK_PIT_RESCUE = 65;

    constexpr float PIT_MIN_X = -11280.0f, PIT_MAX_X = -11180.0f;
    constexpr float PIT_MIN_Y = -1920.0f, PIT_MAX_Y = -1670.0f;
    constexpr float PIT_MAX_Z = 128.0f;  // the library's lower floor is 136

    // Where the rescue puts the party: the top of the library stair, the camp the
    // 1044 party was dragging to (island #1, poly z 152.5), clear of the Arcane
    // Protector that stands 6yd off the stair foot's route point.
    constexpr float LANDING_X = -11210.0f, LANDING_Y = -1885.4f, LANDING_Z = 152.5f;

    inline bool InPit(float x, float y, float z)
    {
        return x >= PIT_MIN_X && x <= PIT_MAX_X && y >= PIT_MIN_Y && y <= PIT_MAX_Y && z < PIT_MAX_Z;
    }
}

#endif
