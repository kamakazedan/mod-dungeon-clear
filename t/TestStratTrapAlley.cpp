/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <cmath>

#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStratTrapAlley.h"

// Stratholme's Scarlet gate-trap alley: the hole strip the critter despawn
// uses, the pit the rescue event fires in, and the rescue event's shape. The
// fixtures are real coordinates from sk-20261022-002111.

using namespace DcStratTrapAlley;

// --- the hole strip --------------------------------------------------------

TEST(DcStratTrapAlley, CrittersSittingOverTheHoleAreInTheStrip)
{
    // The rats the tanks chased in (tr-...-940 Low 715, tr-...-453 Low 742).
    EXPECT_TRUE(InHoleStrip(3615.7f, -3345.7f, 126.0f));
    EXPECT_TRUE(InHoleStrip(3615.4f, -3344.5f, 126.0f));
    // The extremes of the session's fallen-critter despawns, at their summon z.
    EXPECT_TRUE(InHoleStrip(3610.1f, -3344.0f, 126.0f));
    EXPECT_TRUE(InHoleStrip(3621.2f, -3345.9f, 126.0f));
}

TEST(DcStratTrapAlley, WhereThePartyWalksIsNotInTheStrip)
{
    // Tank positions on the alley route, and the southernmost street-level bot
    // position of the whole session.
    EXPECT_FALSE(InHoleStrip(3614.9f, -3338.4f, 125.0f));
    EXPECT_FALSE(InHoleStrip(3618.0f, -3337.0f, 123.7f));
    EXPECT_FALSE(InHoleStrip(3609.1f, -3338.9f, 124.9f));
    EXPECT_FALSE(InHoleStrip(3612.0f, -3340.9f, 125.0f));
    // The Supply Crate east of the east portcullis stands on real street.
    EXPECT_FALSE(InHoleStrip(3636.5f, -3347.5f, 125.6f));
}

TEST(DcStratTrapAlley, TheStripIsStreetLevelOnly)
{
    // A critter that already dropped to the pit floor is the fall test's.
    EXPECT_FALSE(InHoleStrip(3615.7f, -3345.7f, 59.0f));
}

// --- the pit ---------------------------------------------------------------

TEST(DcStratTrapAlley, EveryTankLandingIsInThePit)
{
    // First pit position of each of the seven Forresten stalls, plus the end
    // positions of the no_progress run and of the deepest dead-end walk.
    float const landings[][3] = {
        {3613.3f, -3347.4f, 59.5f}, {3613.0f, -3345.5f, 59.4f}, {3618.7f, -3345.1f, 58.5f},
        {3613.7f, -3345.7f, 59.3f}, {3599.6f, -3341.6f, 61.5f}, {3616.0f, -3348.4f, 59.0f},
        {3614.6f, -3348.4f, 59.3f}, {3621.2f, -3345.0f, 58.1f}, {3574.9f, -3491.2f, 61.4f},
        {3584.9f, -3475.5f, 88.0f}, {3581.1f, -3468.6f, 94.5f},
    };
    for (auto const& p : landings)
        EXPECT_TRUE(InPit(p[0], p[1], p[2])) << p[0] << "," << p[1] << "," << p[2];
}

TEST(DcStratTrapAlley, TheStreetsAboveThePitAreNotInIt)
{
    EXPECT_FALSE(InPit(3614.9f, -3338.4f, 125.0f));               // the alley
    EXPECT_FALSE(InPit(3595.8f, -3509.9f, 137.5f));               // Forresten's spawn
    EXPECT_FALSE(InPit(3653.4f, -3162.2f, 130.6f));               // King's Square, off the box
}

TEST(DcStratTrapAlley, TheLandingIsOnTheStreetAndClearOfTheTrap)
{
    EXPECT_FALSE(InPit(LANDING_X, LANDING_Y, LANDING_Z));
    EXPECT_FALSE(InHoleStrip(LANDING_X, LANDING_Y, LANDING_Z));
    // The trap fires within 5.5yd (2D) of its centre.
    float const dx = LANDING_X - 3612.29f, dy = LANDING_Y - (-3335.39f);
    EXPECT_GT(std::sqrt(dx * dx + dy * dy), 5.5f + 5.0f);
}

// --- the rescue event --------------------------------------------------------

TEST(DcStratTrapAlley, PitRescueIsARepeatableConditionalCustomHook)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_STRATHOLME, EVENT_PIT_RESCUE);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(ev->condition != nullptr);
    EXPECT_TRUE(ev->repeatable) << "a second fall must re-arm it";
    EXPECT_TRUE(ev->stepsOwnMovement) << "the hook teleports";
    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_PIT_RESCUE);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_PIT_RESCUE));
    // PanelBeforeBoss on a Repeatable event is a permanent pull hold.
    EXPECT_EQ(ev->panelGatesBossEntry, 0u);
    EXPECT_EQ(ev->panelSortAfterBossEntry, NPC_THE_UNFORGIVEN);
}
