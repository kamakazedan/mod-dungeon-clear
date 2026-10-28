/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCSTRATTRAPALLEY_H
#define _PLAYERBOT_DCSTRATTRAPALLEY_H

// Stratholme (map 329) Scarlet gate-trap alley geometry. Pure — no game state —
// so the gtests (t/TestStratTrapAlley) pin the numbers.
//
// The alley runs east-west at y~-3335 between portcullises 175351 (x 3603) and
// 175350 (x 3621.5); the trap fires within 5.5yd of (3612.29,-3335.39). Its
// SOUTH EDGE has a hole: past y~-3341 there is no floor on the street level, and
// whatever steps there drops ~65yd onto a floor at z~59 that no route leaves
// (every path to Hearthsinger Forresten dead-ends 79yd short of him). Every
// "Can't reach Hearthsinger Forresten" stall of sk-20261022-002111 (7 runs, plus
// one no_progress) is a tank that went through it.
//
// The way in is the trap's own critters. DoSpawnPlaguedCritters summons them
// within 8yd of the player at the player's z+1 with no ground check, so some
// land over the hole and sit there, at their summon z and attackable, for
// 10-25s before they drop. The tank follows one in: a gap-closer (Feral Charge
// 16979, Charge 11578) or plain stock reach-melee once leader-assist hands the
// fight to the combat engine.
//
// Measured from the session's logs: no bot position on the street level ever
// lies south of y -3340.9 anywhere in x 3590-3650, every fallen-critter despawn
// sits at x 3610-3621 / y -3344..-3346, and every tank landed at x 3599-3621 /
// y -3341..-3348 / z 58-62. The pit's footprint below z 105 holds no creature or
// gameobject spawn (all 62 spawns over it sit at z >= 123.6).
namespace DcStratTrapAlley
{
    constexpr unsigned MAP_STRATHOLME = 329;
    // The rescue event (StratholmeEvents.cpp). Stratholme's events are 1-6; hook
    // ids are one flat space across every dungeon and 63 is Blackfathom's.
    constexpr unsigned EVENT_PIT_RESCUE = 7;
    constexpr unsigned HOOK_PIT_RESCUE = 64;
    constexpr unsigned NPC_THE_UNFORGIVEN = 10516;  // the boss before Forresten

    // The hole strip on the street level: a trap critter inside it is despawned
    // the moment it is seen there, before anything can chase it in.
    constexpr float HOLE_MIN_X = 3602.0f, HOLE_MAX_X = 3624.0f;
    constexpr float HOLE_MIN_Y = -3352.0f, HOLE_MAX_Y = -3342.0f;
    constexpr float STREET_MIN_Z = 110.0f;  // above it = street level; the pit is ~59

    // The pit under it, out to where its dead-end routes wander (under Forresten).
    constexpr float PIT_MIN_X = 3540.0f, PIT_MAX_X = 3660.0f;
    constexpr float PIT_MIN_Y = -3520.0f, PIT_MAX_Y = -3320.0f;
    constexpr float PIT_MAX_Z = 105.0f;

    // Where the rescue puts the party: the route point just west of the west
    // portcullis, on the way to Forresten and 15.8yd from the trap trigger (it
    // fires at 5.5yd and then sits out a 30-minute cooldown anyway).
    constexpr float LANDING_X = 3597.1f, LANDING_Y = -3339.8f, LANDING_Z = 126.7f;

    inline bool InHoleStrip(float x, float y, float z)
    {
        return x >= HOLE_MIN_X && x <= HOLE_MAX_X && y >= HOLE_MIN_Y && y <= HOLE_MAX_Y && z > STREET_MIN_Z;
    }

    inline bool InPit(float x, float y, float z)
    {
        return x >= PIT_MIN_X && x <= PIT_MAX_X && y >= PIT_MIN_Y && y <= PIT_MAX_Y && z < PIT_MAX_Z;
    }
}

#endif
