/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_BOT_RUN_H
#define _DC_BOT_RUN_H

// A clear for a party with nobody playing in it, started and watched for
// another module: mod-playerbots-plus's bot dungeons, where random bots get
// together through the dungeon finder on their own. There is nobody in the
// group to say `dc on`, to resume a pause, or to call a stuck run off, so:
//
//   * Start() says `dc on` on the elected tank itself: IsAuthorized lets
//     exactly that call through (StartingFor). It latches the wing the
//     dungeon finder made the group for, waits for the boss roster, and
//     switches WaitAtBoss off for the run, since nobody would ever resume it.
//
//   * The run is watched the way `.dc test` watches its runs: the reason the
//     disable funnel gives, the whole party dead past a grace, a pause or a
//     stall that outlasts its grace, nothing moving for too long (no kill, no
//     anchor, no change in the fight, not getting closer to the next target),
//     and an overall cap. The verdict comes from DcTestRunVerdict, the same
//     kernel the harness uses, with the caller's limits.
//
//   * A failed run is switched off. What happens to the party after that
//     (leaving, going home) is the caller's: this only reports.
//
//   * A real player turning up in the group ends the watch at once and leaves
//     the clear alone: it is that player's run now (HandedOver).
//
// The clear itself is the one a player starts: the same leader election,
// strategies, pulls, rests, rez recovery and final-loot hold.
//
// World thread only, except OnRunDisabled, which the disable funnel calls on
// whatever thread the leader's AI runs on.
//
// A module that uses this without depending on this module:
//
//     #if __has_include("DcBotRun.h")
//     #include "DcBotRun.h"
//     #endif
//     #if defined(DC_BOT_RUN_API) && DC_BOT_RUN_API >= 1
//     ...

#include <cstdint>
#include <string>

#include "ObjectGuid.h"

class Player;

#define DC_BOT_RUN_API 1

namespace DcBotRun
{
    // How long a run may go wrong before it is called. 0 switches a check off,
    // except overallS, which always applies.
    struct Limits
    {
        uint32_t noProgressS = 600;   // nothing moving forward
        uint32_t overallS    = 7200;  // the whole run, from `dc on`
        uint32_t stallGraceS = 120;   // stuck on something the run has reported
        uint32_t pauseGraceS = 60;    // paused (a door, a held run): nobody will resume it
        uint32_t wipeGraceS  = 15;    // everyone dead, with nobody to rez
        uint32_t startS      = 120;   // to get going: the party in, the roster read, `dc on` taken
    };

    enum class Phase : uint8_t
    {
        None,        // not a run this watches (or no longer)
        Starting,    // waiting for the party, the roster, `dc on`
        Running,
        Cleared,     // every boss of the run down: the run's own all-cleared
        Failed,      // `why` says why
        HandedOver   // a real player joined: the clear, if any, is theirs now
    };

    struct Status
    {
        Phase       phase = Phase::None;
        std::string why;            // Failed: what ended it ("nothing moved for 600s", ...)
        ObjectGuid  tank;           // the run's leader
        uint32_t    lfgDungeon = 0;
        uint32_t    elapsedS = 0;   // since `dc on` (Starting: since Start)
        uint32_t    sinceProgressS = 0;
        uint32_t    bossesDown = 0; // of the run's roster (its wing's, on a split map)
        uint32_t    bossesTotal = 0;
        std::string wing;           // the wing's name on a split map, else ""
    };

    // Takes on the clear of `member`'s group: a party (not a raid) in a
    // dungeon, nobody playing in it, a tank bot alive on the map, no clear
    // running there yet, and not one already watched. lfgDungeon is the
    // dungeon finder's dungeon the group was made for, which picks the wing
    // on a split map (Blackrock Spire, Blackrock Depths, Maraudon); 0 if none.
    // The clear starts on a later tick, once the roster is there. False, with
    // `why`, when it cannot be taken on.
    bool Start(Player* member, uint32_t lfgDungeon, Limits const& limits, std::string& why);

    // How the run of group `group` stands. Phase::None for a group this does
    // not watch. A finished run (Cleared, Failed, HandedOver) is kept until
    // Stop, or for an hour.
    Status Get(ObjectGuid group);

    // Stops watching group `group`. A clear still running is switched off
    // first, with `why` as its reason -- unless a real player is in the group.
    void Stop(ObjectGuid group, std::string const& why);

    // Runs watched now: starting or running.
    uint32_t Active();

    // The wing of a split dungeon (Scarlet Monastery, Dire Maul, Maraudon,
    // Blackrock Spire, Blackrock Depths) that creature `entry` is a boss of:
    // its name ("Scarlet Monastery (Library)") and the dungeon finder's
    // dungeon for it when the wing names one; 0 where the wing is simply
    // wherever the party lands (Scarlet Monastery, Dire Maul). False when
    // `entry` is no wing's boss on `mapId`, or the map is not split.
    bool WingOf(uint32_t mapId, uint32_t entry, std::string& wingName, uint32_t& lfgDungeon);

    // True while Start's own `dc on` is going to `bot`: IsAuthorized's one
    // exception for a party with no player in it. World thread.
    bool StartingFor(Player* bot);

    // From the disable funnel (DcActionShared::DisableDungeonClear), on the
    // leader's AI thread. Notes the reason; the world tick reads it.
    void OnRunDisabled(Player* leader, std::string const& reason);

    // World tick (DungeonClearReaperScript), with the module's latched
    // enable: with it off, every watched run fails.
    void Tick(uint32_t diff, bool moduleEnabled);
}

#endif  // _DC_BOT_RUN_H
