/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include <string>
#include <vector>

#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPoolExitDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRawPathDrive.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"

// --- Blackfathom Deeps (map 48) ----------------------------------------------
//
// Seven encounters in DungeonEncounter order: Ghamoo-ra 4887 (bit 0), Lady
// Sarevess 4831 (1), Gelihast 6243 (2), Lorgus Jett 12902 (3), Old Serra'kis
// 4830 (5), Twilight Lord Kelris 4832 (6), Aku'mai 4829 (7) — bit 4 is unused
// on map 48 (DungeonEncounter.dbc). The order is kept natural (user decision,
// 2026-10-21): Kelris and Aku'mai last.
//
// OLD SERRA'KIS IS SKIPPED (user decision, 2026-10-21). He patrols the floor
// of the Forgotten Pool, 20-25yd under water in a cave with no air pocket;
// the authored dive / underwater fight / surface legs did not hold up and left
// the party stranded on the pool floor. He is removed from the roster, so the
// run never targets him and the all-cleared contract does not wait on him.
//
// The navmesh (190 components; tools/bfd_islands in the research notes) makes
// Ghamoo-ra's lake -> Gelihast -> Lorgus -> Kelris -> Portal -> Aku'mai ONE
// connected walk (island #0). Everything else this file does is about the
// three places the mesh does not carry the party, in dungeon order:
//
//   1. THE JUMP PUZZLE (event 6). Detour's route from the entrance corridor
//      to Ghamoo-ra swims 88yd of the first pool to the SW corner — the
//      shortcut, which the user calls a BUG. Players swim along the south wall
//      to a dry pocket against the west wall, walk up its ramp out of the
//      water, and jump east across three rock islands (a ridge, a small
//      island, the big island) to the SW corner. Coordinates are the user's
//      live .gps capture (2026-10-21). The swim is ordinary navmesh: the
//      objective anchor IS the take-off, and Detour's only route there is the
//      water route up the ramp, so boss-nav swims the party in. The islands
//      are meshed but unlinked (#18 / #19 / #22), so the chain is four Jumps
//      with on-mesh walks between them, PARTY-WIDE: every bot jumps itself,
//      leader first, followers released one hop apart. A bot that misses a
//      jump is put back on the take-off and re-runs the chain (three falls ->
//      straight to the SW corner).
//
//   2. THE PASSAGE TO LADY SAREVESS (events 1 and 2). Her chamber (island #6)
//      is cut off from the lake by solid rock between x -390..-370; the only
//      link is a submerged passage under the chamber's south lip around
//      (-365..-345, 295..315), ~30yd long and ~10yd deep. The surface sheets
//      (#0/#4) and the pool floor (#2) are meshed but UNLINKED, so a navmesh
//      bot can never descend. An authored 3D polyline is swum verbatim, both
//      ways, party-wide; stock boss-nav takes over on the far island (her 29-
//      point patrol under mod-individual-progression is an ordinary patrol).
//
//   3. THE FIRES OF AKU'MAI AND THE PORTAL (event 5). instance_blackfathom_deeps
//      opens the Portal of Aku'mai (21117) ONLY when all four fires (21118-
//      21121) read DONE and every wave summon they produced is dead. Each fire
//      is a plain button whose SmartAI summons a wave on Gossip Hello with no
//      once-only guard — a SECOND click summons a SECOND wave — so each click
//      is guarded on the script's own TYPE_FIREn == DONE (SkipIfInstanceData).
//      The portal is IsScriptOnly in DcEventDoorRegistry so boss-nav toward
//      Aku'mai never tries to open it (lock 85 would otherwise read as
//      clickable and auto-pause the run at a locked door).
//
//   4. THE FORGOTTEN POOL (event 8). Kelris is reached along a WALKWAY (#0)
//      over Serra'kis's pool — you cannot swim to him (user, 2026-10-22). A bot
//      that falls off lands on the pool floor (#1) or its surface sheet (#7),
//      neither linked to #0, so every route rebuilt from the water dead-ends
//      ~12yd UNDER Kelris; the tank reads as "within engage range" there and
//      the approach loops on rebuild-and-yield until the 600s watchdog — every
//      BFD timeout of sk-20261022-002111 (and the dive pulls Serra'kis). The way
//      out is the ramp at the pool's north end (user .gps); the floor route to
//      it is long and winding, so it is Detour's floor corridor, swum 1.5yd off
//      the floor, then up the ramp. Conditional + Repeatable: it fires whenever
//      the tank is in the pool, before or after Kelris. Followers need nothing:
//      the breath watchdog teleports a drowning follower to the tank, which is
//      on land once this has run.
//
// Lorgus Jett needs nothing here: under mod-individual-progression he is a
// 1-of-3 spawn pool (guids 26173 / 695095 / 695096, all on the #0 corridor),
// and DungeonBossesValue re-anchors a pooled boss on the spawn the live
// creature stands at (DungeonBossInfo::spawns). Gelihast and his oracles stand
// in sub-yard water on the #0 sheet: plain boss-nav.
//
// Coordinates: every anchor and boss position is from the live acore_world DB;
// the jump puzzle is a live .gps capture; the swim polylines are OFFLINE
// estimates from the map-48 mmtiles and the VMAP collision survey (1yd grid)
// and are marked CAPTURE LIVE in the plan
// (deployment-files/docs/mod-dungeon-clear_blackfathom-deeps_plan.md §4). A
// wrong jump coordinate shows up as a fall, which the chain handler logs,
// counts (`chainFalls`) and retries; it cannot wedge a run.

namespace
{
    // Instance script slots (blackfathom_deeps.h) and the encounter state value
    // the fire guard keys on. Authored from the header, not the DBC bit index.
    constexpr uint32 BFD_TYPE_FIRE1 = 1;
    constexpr uint32 BFD_TYPE_FIRE2 = 2;
    constexpr uint32 BFD_TYPE_FIRE3 = 3;
    constexpr uint32 BFD_TYPE_FIRE4 = 4;
    constexpr uint32 BFD_STATE_DONE = 3;  // EncounterState::DONE

    constexpr uint32 BFD_GO_FIRE_1 = 21118;   // -> 4x Aku'mai Snapjaw 4825
    constexpr uint32 BFD_GO_FIRE_2 = 21119;   // -> 4x Murkshallow Softshell 4977
    constexpr uint32 BFD_GO_FIRE_3 = 21120;   // -> 4x Barbed Crustacean 4823
    constexpr uint32 BFD_GO_FIRE_4 = 21121;   // -> 2x Aku'mai Servant 4978
    constexpr uint32 BFD_GO_PORTAL = 21117;   // Portal of Aku'mai
    constexpr uint32 BFD_NPC_SNAPJAW = 4825;
    constexpr uint32 BFD_NPC_SOFTSHELL = 4977;
    constexpr uint32 BFD_NPC_CRUSTACEAN = 4823;
    constexpr uint32 BFD_NPC_SERVANT = 4978;
    constexpr uint32 BFD_NPC_SERRAKIS = 4830;

    // --- 1. the jump puzzle (user .gps capture, 2026-10-21) -----------------
    // The take-off at the top of the pocket's ramp, on mesh (#0): the objective
    // anchor and the chain's start / fall-retry point.
    constexpr float BFD_TAKEOFF_X = -360.27f, BFD_TAKEOFF_Y = 34.96f, BFD_TAKEOFF_Z = -53.09f;
    // Jump 4's landing at the SW corner, on mesh (#0): the chain's end / fall-
    // finalise point. From here Detour walks dry to the lake tunnel and fords
    // 43yd of knee-deep water to Ghamoo-ra's island.
    constexpr float BFD_LANDING_X = -314.77f, BFD_LANDING_Y = 68.71f, BFD_LANDING_Z = -53.57f;
    // The first pool's volume: anything below the -58.3 water line inside the
    // cavern (with a yard of slack) is "in the pool". Armed only from the first
    // jump (DcPartyWideChain::FallArmed) — the approach swims through it.
    constexpr float BFD_POOL_MIN_X = -366.0f, BFD_POOL_MAX_X = -265.0f;
    constexpr float BFD_POOL_MIN_Y = 0.0f, BFD_POOL_MAX_Y = 120.0f;
    constexpr float BFD_POOL_WATER_Z = -57.0f;

    // --- 2. the Sarevess passage (S0..S4; capture live) ---------------------
    G3D::Vector3 const BFD_S0(-378.0f, 292.0f, -56.0f);   // lake surface, #0
    G3D::Vector3 const BFD_S1(-370.0f, 296.0f, -62.0f);   // under the lip
    G3D::Vector3 const BFD_S2(-362.0f, 300.0f, -66.0f);   // the floor, #2
    G3D::Vector3 const BFD_S3(-352.0f, 310.0f, -63.0f);   // floor, chamber side
    G3D::Vector3 const BFD_S4(-348.0f, 320.0f, -55.5f);   // chamber surface, #6

    // --- 3. the fires -------------------------------------------------------
    constexpr float BFD_FIRES_X = -818.7f, BFD_FIRES_Y = -164.5f, BFD_FIRES_Z = -24.5f;
    constexpr float BFD_FIRE_SEARCH = 100.0f;

    // --- 4. the Forgotten Pool exit -----------------------------------------
    // The box is the pool floor island's (#1) bbox; Aku'mai's lake (y < -250)
    // is outside it. The surface is the #7 sheet (-31..-30). The walkway's
    // north end is ankle-deep water AT the surface (-31.3), which the 3yd depth
    // gate keeps out; elsewhere the walkway is above the surface.
    constexpr float BFD_FPOOL_MIN_X = -872.0f, BFD_FPOOL_MAX_X = -679.0f;
    constexpr float BFD_FPOOL_MIN_Y = -194.0f, BFD_FPOOL_MAX_Y = 5.0f;
    constexpr float BFD_FPOOL_SURFACE_Z = -30.9f;
    constexpr float BFD_FPOOL_MIN_DEPTH = 3.0f;
    // Where Detour's floor corridor ends at the foot of the ramp (#1, offline
    // pathq from three fall spots), and the top of the ramp (#0, user .gps).
    G3D::Vector3 const BFD_FPOOL_RAMP_FOOT(-746.7f, -10.9f, -35.2f);
    G3D::Vector3 const BFD_FPOOL_RAMP_TOP(-740.39f, -2.38f, -31.28f);
    // Wedges before the climb gives up and stalls the run.
    constexpr uint32 BFD_FPOOL_MAX_WEDGES = 3;

    // The floor under a bot: the first collision BELOW it (VMAP ignores liquid).
    float FloorUnder(Player* bot)
    {
        return bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() + 1.0f);
    }

    bool InForgottenPool(Player* bot)
    {
        DcPoolExit::Pool const& pool = DcBlackfathomDeeps::ForgottenPool();
        // The box first: this runs on every tick of every BFD leader.
        if (!DcPoolExit::InBox(pool, bot->GetPositionX(), bot->GetPositionY()))
            return false;
        float const floorZ = FloorUnder(bot);
        if (floorZ <= INVALID_HEIGHT)
            return false;
        return DcPoolExit::InPool(pool, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), floorZ);
    }

    bool ForgottenPoolExitDue(Player* bot, AiObjectContext* /*context*/)
    {
        return bot && bot->GetMapId() == 48 && bot->IsAlive() && InForgottenPool(bot);
    }

    // Detour's corridor from the floor under the bot to the ramp foot. The start
    // is the FLOOR, not the bot: a swimmer's own z snaps to the #7 surface
    // sheet, whose corridor stops at (-783.7,-133.6), short of the ramp.
    std::vector<G3D::Vector3> FloorCorridor(Player* bot, float floorZ)
    {
        Map* map = bot->GetMap();
        dtNavMesh const* mesh = map ? map->GetMapCollisionData().GetMMapData().GetNavMesh() : nullptr;
        if (!mesh)
            return {};
        LongRangePathfinder::RawResult const raw = LongRangePathfinder::BuildCoreFromMesh(
            mesh, 48, bot->GetPositionX(), bot->GetPositionY(), floorZ,
            BFD_FPOOL_RAMP_FOOT.x, BFD_FPOOL_RAMP_FOOT.y, BFD_FPOOL_RAMP_FOOT.z);
        if (!raw.reachable)
            return {};
        return raw.rawPts;
    }
}

DcPoolExit::Pool const& DcBlackfathomDeeps::ForgottenPool()
{
    static DcPoolExit::Pool const pool{BFD_FPOOL_MIN_X, BFD_FPOOL_MAX_X, BFD_FPOOL_MIN_Y, BFD_FPOOL_MAX_Y,
                                       BFD_FPOOL_SURFACE_Z, BFD_FPOOL_MIN_DEPTH,
                                       BFD_FPOOL_RAMP_FOOT, BFD_FPOOL_RAMP_TOP,
                                       /*doneRadius*/ 3.0f, /*floorLift*/ 1.5f};
    return pool;
}

namespace
{
    // The climb out, one tick at a time: build the leg on the first tick, then
    // drive it with the shared raw-path cursor (the authored RawPath's rules).
    // Done the moment the bot is out of the pool — the ramp top, a teleport, a
    // rez — and Blocked (a stall naming the problem) after repeated wedges.
    ObjectiveArriveResult DriveForgottenPoolExit(Player* bot, AiObjectContext* context,
                                                 DungeonBossInfo const& /*info*/)
    {
        using namespace DcRawPathDrive;
        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!botAI || !context || bot->GetMapId() != 48)
            return ObjectiveArriveResult::Done;

        DcRunState& run = DcRun::Of(context);
        DcPoolExit::Pool const& pool = DcBlackfathomDeeps::ForgottenPool();
        G3D::Vector3 const pos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

        if (!InForgottenPool(bot))
        {
            if (!run.poolExitLeg.empty())
            {
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] forgotten pool: out of the water at ({:.1f},{:.1f},{:.1f}) -> back to the clear",
                         bot->GetName(), pos.x, pos.y, pos.z);
                DcMovement::ResolveEscortConflict(bot);
            }
            run.ClearPoolExit();
            return ObjectiveArriveResult::Done;
        }

        uint32 const now = getMSTime();
        if (run.poolExitLeg.empty())
        {
            std::vector<G3D::Vector3> const corridor = FloorCorridor(bot, FloorUnder(bot));
            run.poolExitLeg = DcPoolExit::BuildLeg(pool, corridor);
            run.poolExitCursor = 0;
            run.poolExitWatch.Reset();
            ++run.poolExits;
            run.SetTestExtra("poolExits", std::to_string(run.poolExits), /*numeric*/ true);
            DcMovement::ResolveEscortConflict(bot);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] forgotten pool: fell in at ({:.1f},{:.1f},{:.1f}) -> swimming {} pts "
                     "({}) to the ramp (exit {} this run)",
                     bot->GetName(), pos.x, pos.y, pos.z, run.poolExitLeg.size(),
                     corridor.empty() ? "no floor corridor, straight" : "floor corridor", run.poolExits);
        }

        MotionMaster* mm = bot->GetMotionMaster();
        bool const splineRunning =
            mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE && bot->isMoving();
        Decision const d = Step(run.poolExitLeg, run.poolExitCursor, pos, pool.doneRadius, splineRunning, now,
                                run.poolExitWatch);
        run.poolExitCursor = d.cursor;
        switch (d.verdict)
        {
            case Verdict::Arrived:
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] forgotten pool: up the ramp -> back to the clear",
                         bot->GetName());
                DcMovement::ResolveEscortConflict(bot);
                run.ClearPoolExit();
                return ObjectiveArriveResult::Done;
            case Verdict::InFlight:
                return ObjectiveArriveResult::Running;
            case Verdict::OffLeg:
                // Knocked or dragged off the leg (a fight in the water): rebuild
                // from where the bot is now.
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] forgotten pool: {:.0f}yd off the leg -> rebuilding it",
                         bot->GetName(), (pos - run.poolExitLeg[run.poolExitCursor]).length());
                run.poolExitLeg.clear();
                return ObjectiveArriveResult::Running;
            case Verdict::Wedged:
                ++run.poolExitWedges;
                run.poolExitWatch.Reset();
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] forgotten pool: no progress toward point {} (wedge {} of {})",
                         bot->GetName(), run.poolExitCursor, run.poolExitWedges, BFD_FPOOL_MAX_WEDGES);
                if (run.poolExitWedges >= BFD_FPOOL_MAX_WEDGES)
                {
                    run.ClearPoolExit();
                    return ObjectiveArriveResult::Blocked;
                }
                DcMovement::ResolveEscortConflict(bot);
                [[fallthrough]];
            case Verdict::Issue:
                break;
        }

        // Spline window from the cursor, Z verbatim (submerged; no navmesh snap).
        Movement::PointsArray pts;
        pts.push_back(pos);
        for (size_t i = run.poolExitCursor;
             i < run.poolExitLeg.size() && pts.size() < DungeonPathFollower::MAX_SPLINE_WINDOW_POINTS; ++i)
            pts.push_back(run.poolExitLeg[i]);
        DcMovement::SplinePath(botAI, pts);
        return ObjectiveArriveResult::Running;
    }
}

void RegisterBlackfathomDeepsEvents(std::vector<DungeonEvent>& out)
{
    // --- 1. The jump puzzle (OBJ(0), key 0, before Ghamoo-ra) ---------------
    // Boss-nav swims the party to the take-off (the anchor); followers trail
    // the tank in and are held until released. The chain settles each bot on
    // the take-off, then jump / walk / jump across the three islands to the SW
    // corner. Walks are plain MoveTo: each island is its own meshed component,
    // so Detour paths along it. The roof is 10yd+ over every hop. PERSISTENT:
    // the chain is one-way (a bot on the big island cannot path back to the
    // pocket) and any combat gap would otherwise rewind it. Fall volume: the
    // pool below the -58.3 line, armed from jump 1.
    out.push_back(
        EventBuilder(48, 6, "Cross the jump puzzle")
            .Anchored(/*encounterIndex*/ 0)
            .Persistent()
            .PartyWide()
            .FallVolume(BFD_POOL_MIN_X, BFD_POOL_MAX_X, BFD_POOL_MIN_Y, BFD_POOL_MAX_Y, BFD_POOL_WATER_Z,
                        BFD_TAKEOFF_X, BFD_TAKEOFF_Y, BFD_TAKEOFF_Z,
                        BFD_LANDING_X, BFD_LANDING_Y, BFD_LANDING_Z, /*maxFalls*/ 3)
            // 0. up the pocket's ramp onto the take-off (a follower still in
            //    the water swims the last yards here)
            .MoveTo(BFD_TAKEOFF_X, BFD_TAKEOFF_Y, BFD_TAKEOFF_Z, /*radius*/ 1.5f)
            // 1. jump 1: pocket -> the ridge (#18), then walk it 19yd
            .Jump(-354.35f, 35.09f, -52.12f, /*radius*/ 1.5f)
            .MoveTo(-337.57f, 43.21f, -53.12f, /*radius*/ 1.5f)
            // 3. jump 2: ridge -> the small island (#19), then walk it 5yd
            .Jump(-334.89f, 48.72f, -53.12f, /*radius*/ 1.5f)
            .MoveTo(-330.01f, 50.59f, -53.12f, /*radius*/ 1.5f)
            // 5. jump 3: small island -> the big island (#22), then walk it 14yd
            .Jump(-323.65f, 50.43f, -53.12f, /*radius*/ 1.5f)
            .MoveTo(-315.25f, 61.75f, -53.12f, /*radius*/ 1.5f)
            // 7. jump 4: big island -> the SW corner, on mesh -> Done (the leader
            //    waits for the followers' chains here and nets any straggler)
            .Jump(BFD_LANDING_X, BFD_LANDING_Y, BFD_LANDING_Z, /*radius*/ 2.0f)
            .Build());

    // --- 2. The Sarevess passage, both ways (OBJ(1) key 1, OBJ(2) key 2) -----
    // The Wailing Caverns Serpentis pattern with a swim in place of the jump:
    // boss-nav delivers the tank to S0 like any boss, the event swims S0->S4,
    // stock boss-nav takes it the rest of the way on island #6 (trash +
    // Sarevess, patrol-aware). After the kill the second objective brings the
    // party back. PERSISTENT for the one-way reason; the RawPath is idempotent
    // (Done within `arrive` of the last point), the second guard.
    out.push_back(
        EventBuilder(48, 1, "Swim the passage to Lady Sarevess")
            .Anchored(/*encounterIndex*/ 1)
            .Persistent()
            .PartyWide()
            .MoveTo(BFD_S0.x, BFD_S0.y, BFD_S0.z, /*radius*/ 3.0f)
            .RawPath({BFD_S1, BFD_S2, BFD_S3, BFD_S4}, /*arriveRadius*/ 4.0f).Swim()
            .Build());

    out.push_back(
        EventBuilder(48, 2, "Swim back to the Pool of Ask'ar")
            .Anchored(/*encounterIndex*/ 2)
            .Persistent()
            .PartyWide()
            .MoveTo(BFD_S4.x, BFD_S4.y, BFD_S4.z, /*radius*/ 3.0f)
            .RawPath({BFD_S3, BFD_S2, BFD_S1, BFD_S0}, /*arriveRadius*/ 4.0f).Swim()
            .Build());

    // --- 3. Light the Fires of Aku'mai (OBJ(3) key 7, before Aku'mai) --------
    // Light one fire, kill its wave, repeat; the script opens the portal on the
    // last wave's death. KillCreatureEngage rather than the passive gate: the
    // adds zone-aggro and run in, but one wedged at its spawn point 43yd away
    // would otherwise hang the gate; 90yd from the fires covers both spawn ends
    // (+-43yd) with margin. NEVER re-click a lit fire (a second wave): the
    // click is skipped once the script's TYPE_FIREn reads DONE, and the event
    // is Persistent so a combat gap cannot rewind to it. The fire clicks search
    // 100yd: the kill steps engage anything of the wave's entry within 90yd,
    // and four STATIC Aku'mai Snapjaws stand 50-88yd south past the portal —
    // chasing them parks the tank 80+yd from the next fire, which a 20yd search
    // never found again (tp-20261021-232500-1: 3/5 runs stalled on fire 2).
    // Each fire is its own entry, so the wide search cannot pick a wrong one,
    // and the step walks the bot back to a fire found out of use range.
    // Kelris-first is by roster order (bit 6 before key 7); if a human skipped
    // him, lighting fire 21118 pulls him too (5.8yd) — acceptable. A worldserver restart mid-event
    // leaves the door shut forever (_requiredDeaths is not saved; core
    // limitation, not fixable module-side).
    out.push_back(
        EventBuilder(48, 5, "Light the Fires of Aku'mai")
            .Anchored(/*encounterIndex*/ 7)
            .Persistent()
            .ClearRadius(BFD_FIRES_X, BFD_FIRES_Y, BFD_FIRES_Z, /*radius*/ 40.0f)
            .UseGO(BFD_GO_FIRE_1, /*search*/ BFD_FIRE_SEARCH).SkipIfInstanceData(BFD_TYPE_FIRE1, BFD_STATE_DONE)
            .KillCreatureEngage(BFD_NPC_SNAPJAW, 1, /*search*/ 90.0f)
            .UseGO(BFD_GO_FIRE_2, /*search*/ BFD_FIRE_SEARCH).SkipIfInstanceData(BFD_TYPE_FIRE2, BFD_STATE_DONE)
            .KillCreatureEngage(BFD_NPC_SOFTSHELL, 1, /*search*/ 90.0f)
            .UseGO(BFD_GO_FIRE_3, /*search*/ BFD_FIRE_SEARCH).SkipIfInstanceData(BFD_TYPE_FIRE3, BFD_STATE_DONE)
            .KillCreatureEngage(BFD_NPC_CRUSTACEAN, 1, /*search*/ 90.0f)
            .UseGO(BFD_GO_FIRE_4, /*search*/ BFD_FIRE_SEARCH).SkipIfInstanceData(BFD_TYPE_FIRE4, BFD_STATE_DONE)
            .KillCreatureEngage(BFD_NPC_SERVANT, 1, /*search*/ 90.0f)
            .WaitForGOState(BFD_GO_PORTAL, GO_STATE_ACTIVE, /*timeout*/ 60000, /*search*/ 60.0f)
            .Build());

    // --- 4. Climb out of the Forgotten Pool (conditional, event 8) -----------
    // Fires whenever the tank is in the pool; the condition going false (up the
    // ramp) is the only "done", so a second fall re-arms it. Steps own movement
    // (the hook issues its own spline). PanelAfterBoss, never PanelBeforeBoss:
    // a Repeatable event never latches, so a BeforeBoss gate would hold the
    // pull at Kelris forever ([[dc-panelbeforeboss-repeatable-permanent-hold]]).
    out.push_back(
        EventBuilder(48, DcBlackfathomDeeps::EVENT_POOL_EXIT, "Climb out of the Forgotten Pool")
            .Conditional(&ForgottenPoolExitDue)
            .Repeatable()
            .StepsOwnMovement()
            .PanelAfterBoss(DcBlackfathomDeeps::NPC_LORGUS_JETT)
            .Custom(DcBlackfathomDeeps::HOOK_POOL_EXIT)
                .Timeout(DcBlackfathomDeeps::POOL_EXIT_TIMEOUT_MS)
            .Build());
}

void RegisterBlackfathomDeepsHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, DcBlackfathomDeeps::HOOK_POOL_EXIT, &DriveForgottenPoolExit);
}

// --- roster patch ----------------------------------------------------------
// Four travel OBJECTIVES, each sharing its ordering key with the boss it
// precedes (the objective-before-boss tie-break runs it first); every kept
// boss keeps its DungeonEncounter bit and position. Old Serra'kis (bit 5) is
// removed — see the header. Final order:
//   0 jump puzzle -> Ghamoo-ra | 1 dive -> Sarevess | 2 swim back -> Gelihast |
//   3 Lorgus (live spawn) | 6 Kelris | 7 fires -> Aku'mai.
void RegisterBlackfathomDeepsRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = 48;
    p.remove = { BFD_NPC_SERRAKIS };
    p.add = {
        MakeObjective(OBJ(0), /*encounterIndex*/ 0, 48, "Cross the jump puzzle",
                      BFD_TAKEOFF_X, BFD_TAKEOFF_Y, BFD_TAKEOFF_Z, /*arriveRadius*/ 4.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 6),
        MakeObjective(OBJ(1), /*encounterIndex*/ 1, 48, "Dive into the passage to Lady Sarevess",
                      BFD_S0.x, BFD_S0.y, BFD_S0.z, /*arriveRadius*/ 6.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
        MakeObjective(OBJ(2), /*encounterIndex*/ 2, 48, "Swim back to the Pool of Ask'ar",
                      BFD_S4.x, BFD_S4.y, BFD_S4.z, /*arriveRadius*/ 6.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 2),
        MakeObjective(OBJ(3), /*encounterIndex*/ 7, 48, "Light the Fires of Aku'mai",
                      BFD_FIRES_X, BFD_FIRES_Y, BFD_FIRES_Z, /*arriveRadius*/ 8.0f,
                      /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 5),
    };
    t.push_back(std::move(p));
}
