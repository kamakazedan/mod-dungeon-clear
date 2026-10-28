/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <cmath>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBreathSource.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPartyWideChain.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPoolExitDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRawPathDrive.h"

// Blackfathom Deeps (map 48): the pure kernels behind the party-wide chains
// (the raw-path cursor, the follower release rule, the fall volume, the breath
// planner and watchdog rule, the pooled-spawn resolution) and the shape of the
// seven authored events + the roster patch.

namespace
{
    constexpr uint32 BFD = 48;

    DungeonEvent const* Ev(uint32 id)
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(BFD, id);
        EXPECT_NE(ev, nullptr) << "map 48 event " << id << " is not registered";
        return ev;
    }

    std::vector<G3D::Vector3> Line()
    {
        return {G3D::Vector3(0, 0, 0), G3D::Vector3(10, 0, 0), G3D::Vector3(20, 0, 0),
                G3D::Vector3(30, 0, 0)};
    }
}

// --- DcRawPathDrive ---------------------------------------------------------

TEST(DcRawPathDrive, FirstTickIssuesFromTheCursorAndArmsTheWatch)
{
    DcProgressWatchdog w;
    auto const d = DcRawPathDrive::Step(Line(), 0, G3D::Vector3(-5, 0, 0), 3.0f, /*spline*/ false, 1000, w);
    EXPECT_EQ(d.verdict, DcRawPathDrive::Verdict::Issue);
    EXPECT_EQ(d.cursor, 0u);
    EXPECT_EQ(w.lastProgressMs, 1000u);
}

TEST(DcRawPathDrive, CursorAdvancesPastReachedPointsAndRearmsPerPoint)
{
    DcProgressWatchdog w;
    DcRawPathDrive::Step(Line(), 0, G3D::Vector3(-5, 0, 0), 3.0f, false, 1000, w);
    // Standing on point 1 (10,0,0): points 0 and 1 are consumed, the cursor
    // moves to 2, and the watchdog is re-armed for the new point.
    auto const d = DcRawPathDrive::Step(Line(), 0, G3D::Vector3(10.5f, 0, 0), 3.0f, true, 3000, w);
    EXPECT_EQ(d.cursor, 2u);
    EXPECT_EQ(d.verdict, DcRawPathDrive::Verdict::InFlight);
    EXPECT_EQ(w.lastProgressMs, 3000u);
}

TEST(DcRawPathDrive, LastPointIsNeverConsumedShortOfTheArriveRadius)
{
    DcProgressWatchdog w;
    // 2.9yd from the last point with pointReached 3 but arriveRadius 1.5: the
    // leg is NOT done and the cursor stays on the last point.
    auto const d = DcRawPathDrive::Step(Line(), 3, G3D::Vector3(27.1f, 0, 0), 1.5f, false, 1000, w);
    EXPECT_NE(d.verdict, DcRawPathDrive::Verdict::Arrived);
    EXPECT_EQ(d.cursor, 3u);
    auto const e = DcRawPathDrive::Step(Line(), 3, G3D::Vector3(29.0f, 0, 0), 1.5f, false, 1000, w);
    EXPECT_EQ(e.verdict, DcRawPathDrive::Verdict::Arrived);
}

TEST(DcRawPathDrive, ArrivedIsIdempotentFromAnyCursor)
{
    DcProgressWatchdog w;
    auto const d = DcRawPathDrive::Step(Line(), 0, G3D::Vector3(30, 0.5f, 0), 3.0f, false, 1000, w);
    EXPECT_EQ(d.verdict, DcRawPathDrive::Verdict::Arrived);
}

TEST(DcRawPathDrive, NoProgressForStuckMsIsAWedge)
{
    DcProgressWatchdog w;
    DcRawPathDrive::Step(Line(), 0, G3D::Vector3(-5, 0, 0), 3.0f, false, 1000, w);
    EXPECT_EQ(DcRawPathDrive::Step(Line(), 0, G3D::Vector3(-5, 0, 0), 3.0f, true, 6000, w).verdict,
              DcRawPathDrive::Verdict::InFlight);
    EXPECT_EQ(DcRawPathDrive::Step(Line(), 0, G3D::Vector3(-5, 0, 0), 3.0f, true, 7100, w).verdict,
              DcRawPathDrive::Verdict::Wedged);
    // Closing again (after the caller re-issued) is progress and clears the wedge.
    w.Reset();
    EXPECT_EQ(DcRawPathDrive::Step(Line(), 0, G3D::Vector3(-2, 0, 0), 3.0f, true, 7200, w).verdict,
              DcRawPathDrive::Verdict::InFlight);
}

TEST(DcRawPathDrive, FarFromTheCurrentPointIsOffLeg)
{
    DcProgressWatchdog w;
    auto const d = DcRawPathDrive::Step(Line(), 1, G3D::Vector3(10, 80, 0), 3.0f, false, 1000, w);
    EXPECT_EQ(d.verdict, DcRawPathDrive::Verdict::OffLeg);
}

TEST(DcRawPathDrive, EmptyPathIsArrived)
{
    DcProgressWatchdog w;
    EXPECT_EQ(DcRawPathDrive::Step({}, 0, G3D::Vector3(0, 0, 0), 3.0f, false, 1, w).verdict,
              DcRawPathDrive::Verdict::Arrived);
}

// --- DcChainRelease ----------------------------------------------------------

TEST(DcChainRelease, ReleaseAfterStepIsTheFirstJumpElseTheFirstRawPath)
{
    DungeonEvent const* puzzle = Ev(6);
    ASSERT_NE(puzzle, nullptr);
    // MoveTo take-off(0), Jump 1(1), walk(2) ...: a follower starts once the bot
    // ahead is PAST step 1, i.e. has landed on the ridge and is walking it.
    EXPECT_EQ(DcChainRelease::ReleaseAfterStep(*puzzle), 1u);

    DungeonEvent const* swim = Ev(1);
    ASSERT_NE(swim, nullptr);
    EXPECT_EQ(DcChainRelease::ReleaseAfterStep(*swim), 1u);
}

TEST(DcChainRelease, ThreeFollowersReleaseOneStoneApart)
{
    using namespace DcChainRelease;
    uint32 const after = 2;
    uint32 const spacing = 8000;

    // Leader not started: nobody goes.
    Ahead leader;
    EXPECT_FALSE(MayStart(leader, after, 1000, spacing));
    leader.known = true;
    EXPECT_FALSE(MayStart(leader, after, 1000, spacing));

    // Leader on the rim (step 1) / on A's hop (step 2): follower 1 waits.
    leader.startedMs = 1000;
    leader.stepIndex = 1;
    EXPECT_FALSE(MayStart(leader, after, 2000, spacing));
    leader.stepIndex = 2;
    EXPECT_FALSE(MayStart(leader, after, 3000, spacing));
    // Leader landed A, now on B's hop (step 3): follower 1 is released.
    leader.stepIndex = 3;
    EXPECT_TRUE(MayStart(leader, after, 4000, spacing));

    // Follower 2 keys on follower 1, not on the leader: still waiting while
    // follower 1 is on the rim ...
    Ahead f1;
    f1.known = true;
    f1.startedMs = 4000;
    f1.stepIndex = 1;
    EXPECT_FALSE(MayStart(f1, after, 5000, spacing));
    // ... released once it has left stone A ...
    f1.stepIndex = 3;
    EXPECT_TRUE(MayStart(f1, after, 6000, spacing));
    // ... or when it finished, or by the time-spacing fallback.
    Ahead f2;
    f2.known = true;
    f2.startedMs = 6000;
    f2.stepIndex = 1;
    EXPECT_FALSE(MayStart(f2, after, 10000, spacing));
    EXPECT_TRUE(MayStart(f2, after, 14000, spacing));
    f2.done = true;
    EXPECT_TRUE(MayStart(f2, after, 6500, spacing));
}

// --- DcFallVolume ------------------------------------------------------------

TEST(DcFallVolume, ClassifiesPoolTakeOffIslandsAndLanding)
{
    DungeonEvent const* puzzle = Ev(6);
    ASSERT_NE(puzzle, nullptr);
    DcFallVolume const& f = puzzle->fall;
    ASSERT_TRUE(f.enabled);
    // In the pool: inside the cavern box, below the water line — including the
    // gaps between the islands and the water at the foot of the pocket's ramp.
    EXPECT_TRUE(f.Contains(-300.0f, 60.0f, -59.0f));
    EXPECT_TRUE(f.Contains(-344.0f, 46.0f, -59.0f));
    EXPECT_TRUE(f.Contains(-357.1f, 33.1f, -58.1f));
    // On the take-off / an island / the landing: above the water line.
    EXPECT_FALSE(f.Contains(-360.27f, 34.96f, -53.09f));
    EXPECT_FALSE(f.Contains(-337.57f, 43.21f, -53.12f));
    EXPECT_FALSE(f.Contains(-323.65f, 50.43f, -53.12f));
    EXPECT_FALSE(f.Contains(-314.77f, 68.71f, -53.57f));
    // Deep but outside the cavern box (the lake beyond the tunnel): not a fall.
    EXPECT_FALSE(f.Contains(-400.0f, 200.0f, -66.0f));
    // Disabled volume never matches.
    DcFallVolume off;
    EXPECT_FALSE(off.Contains(-300.0f, 60.0f, -59.0f));
}

TEST(DcFallVolume, ArmedFromTheFirstHopNotOnTheApproachSwim)
{
    DungeonEvent const* puzzle = Ev(6);
    ASSERT_NE(puzzle, nullptr);
    // Step 0 is the approach: a follower may still be swimming it, through
    // the very water the volume covers. Armed from jump 1 on.
    EXPECT_FALSE(DcPartyWideChain::FallArmed(*puzzle, 0));
    for (uint32 i = 1; i < puzzle->steps.size(); ++i)
        EXPECT_TRUE(DcPartyWideChain::FallArmed(*puzzle, i)) << "step " << i;

    // A submerged RawPath is an approach, not a hop: never arms the volume.
    DungeonEvent swim;
    EventStep move;
    move.kind = EventStepKind::MoveTo;
    EventStep sub;
    sub.kind = EventStepKind::RawPath;
    sub.swim = true;
    swim.steps = {move, sub};
    EXPECT_FALSE(DcPartyWideChain::FallArmed(swim, 1));
    swim.steps[1].swim = false;
    EXPECT_TRUE(DcPartyWideChain::FallArmed(swim, 1));
}

// --- DcBreathSource -----------------------------------------------------------

namespace
{
    DcBreathSource::Member Bot(bool aura = false, bool shaman = false, bool warlock = false)
    {
        DcBreathSource::Member m;
        m.isBot = true;
        m.alive = true;
        m.hasAura = aura;
        m.canCastShamanBreath = shaman;
        m.canCastWarlockBreath = warlock;
        return m;
    }
}

TEST(DcBreathSource, ShamanCoversEveryoneIncludingItself)
{
    std::vector<DcBreathSource::Member> party = {Bot(), Bot(false, true), Bot(), Bot(), Bot()};
    auto const plan = DcBreathSource::Plan(party);
    ASSERT_EQ(plan.size(), 5u);
    for (auto const& a : plan)
    {
        EXPECT_EQ(a.kind, DcBreathSource::Kind::Shaman);
        EXPECT_EQ(a.caster, 1);
    }
    EXPECT_FALSE(DcBreathSource::AllBotsCovered(plan));
}

TEST(DcBreathSource, WarlockWithoutShamanThenElixirWithNeither)
{
    std::vector<DcBreathSource::Member> party = {Bot(), Bot(false, false, true), Bot()};
    auto const plan = DcBreathSource::Plan(party);
    for (auto const& a : plan)
    {
        EXPECT_EQ(a.kind, DcBreathSource::Kind::Warlock);
        EXPECT_EQ(a.caster, 1);
    }
    std::vector<DcBreathSource::Member> none = {Bot(), Bot(), Bot()};
    for (auto const& a : DcBreathSource::Plan(none))
    {
        EXPECT_EQ(a.kind, DcBreathSource::Kind::Elixir);
        EXPECT_EQ(a.caster, -1);
    }
}

TEST(DcBreathSource, ShamanBeatsWarlockAndCoveredHumanDeadAreLeftAlone)
{
    DcBreathSource::Member human;
    human.isBot = false;
    human.alive = true;
    DcBreathSource::Member dead = Bot();
    dead.alive = false;
    std::vector<DcBreathSource::Member> party = {Bot(true), Bot(false, false, true), Bot(false, true), human, dead};
    auto const plan = DcBreathSource::Plan(party);
    EXPECT_EQ(plan[0].kind, DcBreathSource::Kind::Covered);
    EXPECT_EQ(plan[1].kind, DcBreathSource::Kind::Shaman);
    EXPECT_EQ(plan[1].caster, 2);
    EXPECT_EQ(plan[2].kind, DcBreathSource::Kind::Shaman);
    EXPECT_EQ(plan[3].kind, DcBreathSource::Kind::Human);
    EXPECT_EQ(plan[4].kind, DcBreathSource::Kind::Dead);

    std::vector<DcBreathSource::Member> covered = {Bot(true), Bot(true), human, dead};
    EXPECT_TRUE(DcBreathSource::AllBotsCovered(DcBreathSource::Plan(covered)));
}

TEST(DcBreathSource, UnderwaterVerdicts)
{
    using namespace DcBreathSource;
    // Short dips are nothing.
    EXPECT_EQ(DecideUnderwater(5000, false, false), Verdict::None);
    // 20s under with no swim leg and no fight: stranded -> rescue.
    EXPECT_EQ(DecideUnderwater(21000, false, false), Verdict::Rescue);
    // The same 20s ON a swim leg, or in combat: leave it.
    EXPECT_EQ(DecideUnderwater(21000, true, false), Verdict::None);
    EXPECT_EQ(DecideUnderwater(21000, false, true), Verdict::None);
    // 150s of the 180s timer: rescue out of combat, log only in it.
    EXPECT_EQ(DecideUnderwater(151000, true, false), Verdict::Rescue);
    EXPECT_EQ(DecideUnderwater(151000, true, true), Verdict::LogOnly);
}

// --- pooled spawns -------------------------------------------------------------

TEST(DcPooledSpawn, ResolveLiveSpawnPicksTheOneWithALiveCreatureElseTheFirst)
{
    std::vector<DcSpawnPoint> spawns = {{-622.4f, -10.4f, -22.8f}, {-455.9f, -40.0f, -32.5f},
                                        {-474.2f, -86.9f, -39.9f}};
    auto near = [](float x, float y, float) { return std::fabs(x + 455.9f) < 1.0f && std::fabs(y + 40.0f) < 1.0f; };
    EXPECT_EQ(ResolveLiveSpawn(spawns, near), 1u);
    auto none = [](float, float, float) { return false; };
    EXPECT_EQ(ResolveLiveSpawn(spawns, none), 0u);
    std::vector<DcSpawnPoint> single;
    EXPECT_EQ(ResolveLiveSpawn(single, near), 0u);
}

// --- the authored events ------------------------------------------------------

TEST(DungeonEventBlackfathom, JumpPuzzleIsAPartyWideChainWithAFallVolume)
{
    DungeonEvent const* ev = Ev(6);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->persistent);
    EXPECT_TRUE(ev->partyWide);
    EXPECT_TRUE(ev->fall.enabled);
    EXPECT_EQ(ev->fall.maxFalls, 3u);
    // MoveTo take-off, then (Jump, walk) x3 and the final Jump.
    ASSERT_EQ(ev->steps.size(), 8u);
    for (size_t i = 0; i < 8; ++i)
    {
        EventStepKind const want = i % 2 ? EventStepKind::Jump : EventStepKind::MoveTo;
        EXPECT_EQ(ev->steps[i].kind, want) << "step " << i;
        EXPECT_TRUE(ev->steps[i].partyWide) << "step " << i;
        EXPECT_LE(ev->steps[i].radius, 2.0f) << "step " << i;
    }
    // Build() stamps: only the last hop is the last movement step.
    for (size_t i = 0; i < 7; ++i)
        EXPECT_FALSE(ev->steps[i].lastMovementStep) << "step " << i;
    EXPECT_TRUE(ev->steps[7].lastMovementStep);
    // The retry point is the take-off, the final point jump 4's landing (both
    // on mesh) — the user's .gps capture.
    EXPECT_FLOAT_EQ(ev->fall.retryX, ev->steps[0].x);
    EXPECT_FLOAT_EQ(ev->fall.retryY, 34.96f);
    EXPECT_FLOAT_EQ(ev->fall.finalX, ev->steps[7].x);
    EXPECT_FLOAT_EQ(ev->fall.finalY, 68.71f);
}

TEST(DungeonEventBlackfathom, SwimsAreMoveToThenASubmergedRawPathBothWays)
{
    for (uint32 id : {1u, 2u})
    {
        DungeonEvent const* ev = Ev(id);
        ASSERT_NE(ev, nullptr);
        EXPECT_TRUE(ev->persistent) << id;
        EXPECT_TRUE(ev->partyWide) << id;
        ASSERT_EQ(ev->steps.size(), 2u) << id;
        EXPECT_EQ(ev->steps[0].kind, EventStepKind::MoveTo) << id;
        EXPECT_EQ(ev->steps[1].kind, EventStepKind::RawPath) << id;
        EXPECT_TRUE(ev->steps[1].swim) << id;
        EXPECT_TRUE(ev->steps[1].lastMovementStep) << id;
    }
    // The return legs are the forward legs reversed: event 2 ends where 1 starts.
    DungeonEvent const* there = Ev(1);
    DungeonEvent const* back = Ev(2);
    ASSERT_NE(there, nullptr);
    ASSERT_NE(back, nullptr);
    EXPECT_FLOAT_EQ(back->steps[1].path.back().x, there->steps[0].x);
    EXPECT_FLOAT_EQ(there->steps[1].path.back().x, back->steps[0].x);
}

TEST(DungeonEventBlackfathom, FiresNeverReclickAndWaitForThePortal)
{
    DungeonEvent const* ev = Ev(5);
    ASSERT_NE(ev, nullptr);
    EXPECT_TRUE(ev->persistent);
    EXPECT_FALSE(ev->partyWide);
    ASSERT_EQ(ev->steps.size(), 10u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::ClearRadius);
    uint32 const fires[] = {21118, 21119, 21120, 21121};
    uint32 const waves[] = {4825, 4977, 4823, 4978};
    for (int i = 0; i < 4; ++i)
    {
        EventStep const& use = ev->steps[1 + 2 * i];
        EventStep const& kill = ev->steps[2 + 2 * i];
        EXPECT_EQ(use.kind, EventStepKind::UseGameObject) << i;
        EXPECT_EQ(use.goEntry, fires[i]) << i;
        EXPECT_EQ(use.skipIfDataId, 1 + i) << "TYPE_FIRE" << i + 1;
        EXPECT_EQ(use.skipIfDataValue, 3u) << "DONE";
        EXPECT_GE(use.radius, 95.0f) << "a kill step can drag the tank ~88yd onto the static snapjaws";
        EXPECT_EQ(kill.kind, EventStepKind::KillCreature) << i;
        EXPECT_TRUE(kill.engage) << i;
        EXPECT_EQ(kill.creatureEntry, waves[i]) << i;
        EXPECT_GE(kill.radius, 86.0f) << "both spawn ends sit 43yd out";
    }
    EXPECT_EQ(ev->steps[9].kind, EventStepKind::WaitForGameObjectState);
    EXPECT_EQ(ev->steps[9].goEntry, 21117u);
    EXPECT_EQ(ev->steps[9].wantState, 0u) << "GO_STATE_ACTIVE";
    EXPECT_TRUE(DcEventDoorRegistry::IsScriptOnly(21117));
}

TEST(DungeonEventBlackfathom, RosterSkipsSerrakisAndInsertsFourObjectivesInNaturalOrder)
{
    ASSERT_TRUE(BossRosterRegistry::HasPatch(BFD));
    // A stand-in derived roster with the seven real bosses on their DBC bits
    // (DungeonEncounter.dbc, map 48: bit 4 is unused).
    std::vector<DungeonBossInfo> base;
    struct Row { uint32 entry; uint32 bit; char const* name; };
    Row const rows[] = {{4887, 0, "Ghamoo-ra"}, {4831, 1, "Lady Sarevess"}, {6243, 2, "Gelihast"},
                        {12902, 3, "Lorgus Jett"}, {4830, 5, "Old Serra'kis"},
                        {4832, 6, "Twilight Lord Kelris"}, {4829, 7, "Aku'mai"}};
    for (Row const& r : rows)
    {
        DungeonBossInfo b;
        b.entry = r.entry;
        b.encounterIndex = r.bit;
        b.name = r.name;
        b.mapId = BFD;
        base.push_back(b);
    }
    std::vector<DungeonBossInfo> roster = BossRosterRegistry::Apply(BFD, DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_NORMAL), base);
    // Seven bosses less Old Serra'kis, plus four objectives.
    ASSERT_EQ(roster.size(), 10u);
    uint32 objectives = 0;
    for (DungeonBossInfo const& b : roster)
    {
        if (b.kind != DungeonAnchorKind::Objective)
            continue;
        ++objectives;
        EXPECT_NE(b.eventId, 0u) << b.name;
        DungeonEvent const* ev = DungeonEventRegistry::Find(BFD, b.eventId);
        ASSERT_NE(ev, nullptr) << b.name;
        EXPECT_EQ(ev->activation, EventActivation::Anchored) << b.name;
        EXPECT_EQ(ev->orderIndex, BossOrderKey(b)) << b.name;
    }
    EXPECT_EQ(objectives, 4u);
    // Kelris before the fires (they stand 9yd from him), the fires right
    // before Aku'mai: roster order is Kelris -> fires objective -> Aku'mai.
    auto indexOf = [&roster](auto pred) -> int
    {
        for (size_t i = 0; i < roster.size(); ++i)
            if (pred(roster[i]))
                return static_cast<int>(i);
        return -1;
    };
    int const kelris = indexOf([](DungeonBossInfo const& b) { return b.entry == 4832; });
    int const akumai = indexOf([](DungeonBossInfo const& b) { return b.entry == 4829; });
    int const fires = indexOf([](DungeonBossInfo const& b) { return b.eventId == 5; });
    ASSERT_GE(kelris, 0);
    ASSERT_GE(fires, 0);
    EXPECT_LT(kelris, fires);
    EXPECT_EQ(fires + 1, akumai);
    // Old Serra'kis is removed (the underwater fight is skipped); no other boss
    // was removed or reordered: every other bit survives on its entry.
    for (Row const& r : rows)
    {
        bool found = false;
        for (DungeonBossInfo const& b : roster)
            if (b.entry == r.entry && b.encounterIndex == r.bit && b.orderOverride < 0)
                found = true;
        EXPECT_EQ(found, r.entry != 4830) << r.name;
    }
    // The dive, surface and breath top-up events went with him.
    for (uint32 id : {3u, 4u, 7u})
        EXPECT_EQ(DungeonEventRegistry::Find(BFD, id), nullptr) << id;
}

TEST(DungeonEventBlackfathom, BuilderStampsPartyWideAndLastMovementOntoSteps)
{
    DungeonEvent ev = EventBuilder(1, 1, "x")
                          .Anchored(0)
                          .Persistent()
                          .PartyWide()
                          .EnsureWaterBreathing()
                          .MoveTo(0, 0, 0, 2)
                          .RawPath({G3D::Vector3(1, 0, 0), G3D::Vector3(2, 0, 0)}, 1.5f)
                          .Swim()
                          .Jump(3, 0, 0, 1.5f)
                          .JumpSpeedZ(5.0f)
                          .UseGO(7, 10)
                          .SkipIfInstanceData(4, 3)
                          .Build();
    ASSERT_EQ(ev.steps.size(), 5u);
    EXPECT_TRUE(ev.partyWide);
    for (EventStep const& s : ev.steps)
        EXPECT_TRUE(s.partyWide);
    EXPECT_FALSE(ev.steps[1].lastMovementStep);
    EXPECT_FALSE(ev.steps[2].lastMovementStep);
    EXPECT_TRUE(ev.steps[3].lastMovementStep);
    EXPECT_FALSE(ev.steps[4].lastMovementStep);
    EXPECT_TRUE(ev.steps[2].swim);
    EXPECT_FLOAT_EQ(ev.steps[2].x, 2.0f) << "RawPath mirrors its last point into x/y/z";
    EXPECT_FLOAT_EQ(ev.steps[3].jumpSpeedZ, 5.0f);
    EXPECT_EQ(ev.steps[4].skipIfDataId, 4);
    EXPECT_EQ(ev.steps[4].skipIfDataValue, 3u);
    EXPECT_TRUE(IsMovementStepKind(EventStepKind::RawPath));
    EXPECT_TRUE(IsOffMeshStepKind(EventStepKind::Jump));
    EXPECT_FALSE(IsOffMeshStepKind(EventStepKind::MoveTo));
    EXPECT_FALSE(IsMovementStepKind(EventStepKind::EnsureWaterBreathing));
}

// --- the Forgotten Pool exit --------------------------------------------------
// Positions are the sk-20261022-002111 timeouts' own (tank parked under Kelris,
// swimming the pool) and the walkway they fell from; floors are the offline
// navmesh/VMAP reads. A bot is "in the pool" only over deep water inside it.

TEST(DcPoolExit, InPoolOnlyOverDeepWaterInsideThePool)
{
    DcPoolExit::Pool const& pool = DcBlackfathomDeeps::ForgottenPool();
    // tr-...-1026's final tank position, on the pool floor under Kelris.
    EXPECT_TRUE(DcPoolExit::InPool(pool, -808.3f, -153.0f, -36.2f, -38.4f));
    // tr-...-690's, bobbing at the surface under the walkway overhang.
    EXPECT_TRUE(DcPoolExit::InPool(pool, -818.8f, -154.5f, -29.5f, -40.0f));
    // Swimming mid-pool where it fell in.
    EXPECT_TRUE(DcPoolExit::InPool(pool, -689.9f, -55.9f, -36.6f, -37.2f));

    // The walkway's north end: ankle-deep water at the surface level.
    EXPECT_FALSE(DcPoolExit::InPool(pool, -770.1f, -14.5f, -31.3f, -31.3f));
    // Kelris's walkway: above the surface.
    EXPECT_FALSE(DcPoolExit::InPool(pool, -818.8f, -155.6f, -25.9f, -25.9f));
    // The ramp top itself (the leg's end) is out, whatever is under it.
    EXPECT_FALSE(DcPoolExit::InPool(pool, -740.39f, -2.38f, -31.28f, -40.0f));
    // Outside the box: Aku'mai's lake, and the corridor in from Lorgus.
    EXPECT_FALSE(DcPoolExit::InPool(pool, -848.4f, -453.9f, -33.9f, -50.0f));
    EXPECT_FALSE(DcPoolExit::InPool(pool, -709.7f, 28.4f, -29.8f, -40.0f));
}

TEST(DcPoolExit, LegFollowsTheFloorCorridorOffTheFloorThenUpTheRamp)
{
    DcPoolExit::Pool const& pool = DcBlackfathomDeeps::ForgottenPool();
    // pathq's floor corridor from the -1026 spot (start point first).
    std::vector<G3D::Vector3> const corridor = {
        {-808.3f, -153.0f, -38.4f}, {-801.6f, -110.7f, -37.8f}, {-768.0f, -64.0f, -44.2f},
        {-760.8f, -49.1f, -40.5f},  {-746.7f, -10.9f, -35.2f}};
    std::vector<G3D::Vector3> const leg = DcPoolExit::BuildLeg(pool, corridor);
    // Start dropped, the four corridor points, then the ramp top (the foot is
    // already the corridor's end, so it is not doubled).
    ASSERT_EQ(leg.size(), 5u);
    EXPECT_FLOAT_EQ(leg[0].x, -801.6f);
    EXPECT_FLOAT_EQ(leg[1].z, -44.2f + pool.floorLift) << "swum off the floor";
    for (G3D::Vector3 const& q : leg)
        EXPECT_LE(q.z, pool.surfaceZ) << "never above the water";
    EXPECT_EQ(leg.back(), pool.rampTop);
    // It reaches the walkway (#0) at the user's .gps.
    EXPECT_FLOAT_EQ(leg.back().x, -740.39f);
    EXPECT_FLOAT_EQ(leg.back().y, -2.38f);
}

TEST(DcPoolExit, NoCorridorStillSwimsToTheFootAndUpTheRamp)
{
    DcPoolExit::Pool const& pool = DcBlackfathomDeeps::ForgottenPool();
    std::vector<G3D::Vector3> const leg = DcPoolExit::BuildLeg(pool, {});
    ASSERT_EQ(leg.size(), 2u);
    EXPECT_FLOAT_EQ(leg[0].x, pool.rampFoot.x);
    EXPECT_EQ(leg[1], pool.rampTop);
    // A partial corridor that stops short gets the foot appended.
    std::vector<G3D::Vector3> const partial = DcPoolExit::BuildLeg(
        pool, {{-808.3f, -153.0f, -38.4f}, {-783.7f, -133.6f, -31.4f}});
    ASSERT_EQ(partial.size(), 3u);
    EXPECT_FLOAT_EQ(partial[1].x, pool.rampFoot.x);
}

TEST(DungeonEventBlackfathom, PoolExitIsARepeatableConditionalCustomHook)
{
    using namespace DcBlackfathomDeeps;
    DungeonEvent const* ev = Ev(EVENT_POOL_EXIT);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(ev->condition != nullptr);
    EXPECT_TRUE(ev->repeatable) << "a second fall must re-arm it";
    EXPECT_TRUE(ev->stepsOwnMovement) << "the hook issues its own spline";
    EXPECT_FALSE(ev->drivesInCombat) << "a fight in the water is the combat engine's";
    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_POOL_EXIT);
    EXPECT_EQ(ev->steps[0].timeoutMs, POOL_EXIT_TIMEOUT_MS);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_POOL_EXIT));
    // PanelBeforeBoss on a Repeatable event is a permanent pull hold.
    EXPECT_EQ(ev->panelGatesBossEntry, 0u);
    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_LORGUS_JETT);
}
