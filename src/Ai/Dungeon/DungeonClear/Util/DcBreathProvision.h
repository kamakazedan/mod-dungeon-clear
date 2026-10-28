/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCBREATHPROVISION_H
#define _PLAYERBOT_DCBREATHPROVISION_H

#include "Common.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"

class Player;

// The IMPURE half of water breathing for an underwater leg (no event uses it
// since Blackfathom Deeps' Old Serra'kis dive was dropped). Two entry points:
//
//   * Tick — the EnsureWaterBreathing event step, run by the LEADER: reads the
//     party into DcBreathSource::Members, plans, and acts one cast per caster
//     per tick (through PlayerbotAI::CastSpell, so GCD / range / cost handling
//     is the stock one and the cast shows in the normal logs; Water Breathing's
//     Shiny Fish Scales reagent is waived by playerbots' triggered cast). The
//     elixir route grants item 5996 with Player::AddItem — the Deadmines
//     gunpowder / Old Hillsbrad bomb / Mechanar key pattern — and has the
//     member use it. Druids found in Aquatic Form are shifted out first.
//     Done once every bot carries the aura; after
//     DC_BREATH_PROVISION_TIMEOUT_MS the uncovered members are logged at INFO
//     and the step completes anyway (the watchdog below remains).
//
//   * WatchdogTick — the per-tick backstop, run by the LEADER from the
//     stranded-recovery trigger site: stamps "under water since" per member on
//     the run state, applies DcBreathSource::DecideUnderwater, teleports a
//     rescued bot to the tank (the stranded-recovery fan), logs a LogOnly.
//     Humans are stamped (for the diag's uwSec) but never teleported.
namespace DcBreathProvision
{
    // The only tunable, a constant per the no-new-configs rule for dungeon events.
    constexpr uint32 DC_BREATH_PROVISION_TIMEOUT_MS = 20000;

    StepResult Tick(Player* leader, DungeonEventProgress& prog, uint32 nowMs);

    void WatchdogTick(Player* bot);

    // Seconds `member` has been under water per the run's stamps (0 when it is
    // not, or no run tracks it). For the DcDiag member row.
    uint32 UnderwaterSeconds(Player* member, uint32 nowMs);

    // True when ANY bot of the party lacks the aura — the top-up Conditional's
    // predicate half.
    bool AnyBotLacksBreath(Player* bot);
}

#endif  // _PLAYERBOT_DCBREATHPROVISION_H
