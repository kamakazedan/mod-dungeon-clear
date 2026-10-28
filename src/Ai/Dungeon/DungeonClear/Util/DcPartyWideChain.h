/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPARTYWIDECHAIN_H
#define _PLAYERBOT_DCPARTYWIDECHAIN_H

#include <cstdint>

#include "Common.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcProgressWatchdog.h"

class Player;
class PlayerbotAI;
class AiObjectContext;

// A PARTY-WIDE event (DungeonEvent::partyWide) is a chain of movement steps
// every bot runs itself — the Blackfathom jump puzzle, the authored swims.
// Each bot keeps its own progress record here ("dungeon clear chain state"):
// the leader's step pointer stays the event progress (DungeonEventProgress,
// driven by DcObjectiveArriveAction), and this record carries the per-step
// cursor, watchdog and fall count beside it; a follower's record is the whole
// of its chain (step pointer included), driven from the follow-tank rung.
//
// Why a copy of the event pointer: the leader's objective latches the moment
// its own chain completes, and NextDungeonBoss moves on to the boss. A follower
// mid-chain must keep driving to the far landing regardless, or follow-tank
// would MoveFollow it off a stone top toward a tank it cannot path to. Registry
// rows are static-lifetime, so the pointer is safe for the life of the process.
struct DcChainState
{
    uint32 eventId{0};
    uint32 mapId{0};
    uint32 instanceId{0};
    DungeonEvent const* ev{nullptr};

    // Follower step pointer (the leader's is DungeonEventProgress::stepIndex).
    uint32 stepIndex{0};
    uint32 startedMs{0};      // 0 => released but not yet started / not released
    uint32 stepStartMs{0};
    bool   done{false};

    // Active-step scratch: the RawPath cursor + watchdog, the Jump launch stamp.
    uint32 cursor{0};
    DcProgressWatchdog watch;
    uint32 wedges{0};
    uint32 jumpIssuedMs{0};

    // Fall handling + the leader's straggler clock.
    uint32 falls{0};
    uint32 arrivedMs{0};      // leader: when it reached the last movement step's end

    void BeginStep(uint32 now)
    {
        stepStartMs = now;
        cursor = 0;
        watch.Reset();
        wedges = 0;
        jumpIssuedMs = 0;
    }

    void Reset()
    {
        *this = DcChainState{};
    }
};

// PURE release rule for the followers of a PartyWide chain: the leader goes
// first; follower k starts when the bot AHEAD of it (the leader for k = 0,
// follower k-1 otherwise) has moved past `releaseAfterStep` — the first
// off-mesh hop, so two bots never share a stone top — or has finished, or has
// been in the chain for `spacingMs` (the time-spacing fallback, so a bot
// wedged ahead cannot hold everyone behind it forever; the straggler net
// covers the wedged one).
namespace DcChainRelease
{
    struct Ahead
    {
        bool   known = false;      // the bot ahead has adopted this chain at all
        bool   done = false;
        uint32 stepIndex = 0;
        uint32 startedMs = 0;      // 0 => not started
    };

    // The step index the bot ahead must be PAST before the next one starts: the
    // first Jump of the event (a follower starts the rim once the bot ahead has
    // landed stone A and is working on B), else the first RawPath, else 0.
    inline uint32 ReleaseAfterStep(DungeonEvent const& ev)
    {
        for (size_t i = 0; i < ev.steps.size(); ++i)
            if (ev.steps[i].kind == EventStepKind::Jump)
                return static_cast<uint32>(i);
        for (size_t i = 0; i < ev.steps.size(); ++i)
            if (ev.steps[i].kind == EventStepKind::RawPath)
                return static_cast<uint32>(i);
        return 0;
    }

    inline bool MayStart(Ahead const& ahead, uint32 releaseAfterStep, uint32 nowMs, uint32 spacingMs)
    {
        if (!ahead.known || ahead.startedMs == 0)
            return false;
        if (ahead.done)
            return true;
        if (ahead.stepIndex > releaseAfterStep)
            return true;
        return nowMs - ahead.startedMs >= spacingMs;
    }
}

namespace DcPartyWideChain
{
    // How long after the LEADER reaches the last movement step's end the
    // followers still short of it are pulled across (the straggler net).
    constexpr uint32 DC_CHAIN_STRAGGLER_MS = 20000;
    // The time-spacing fallback of the release rule.
    constexpr uint32 DC_CHAIN_SPACING_MS = 8000;

    // PURE: is the event's fall volume ARMED on `stepIndex`? Only from the
    // chain's first off-mesh hop (a Jump, or a land RawPath) onward. The steps
    // before it are the approach, and an approach may legitimately run through
    // the very water the volume covers — Blackfathom's jump puzzle starts with
    // a swim across the first pool to the take-off, exactly as players do it.
    // A forced fall (a wedge escalation) ignores this.
    inline bool FallArmed(DungeonEvent const& ev, uint32 stepIndex)
    {
        for (size_t i = 0; i < ev.steps.size() && i <= stepIndex; ++i)
        {
            EventStep const& s = ev.steps[i];
            if (s.kind == EventStepKind::Jump || (s.kind == EventStepKind::RawPath && !s.swim))
                return true;
        }
        return false;
    }

    enum class StepOutcome : uint8
    {
        Running,   // owned the tick, keep driving
        Done,      // this step is complete — advance
        Fell,      // the bot is in the event's fall volume (already handled:
                   // teleported + rewound, or finalised) — keep driving
        Stalled,   // wedged twice / off-leg — the caller decides (leader stalls
                   // the run, a follower hands itself to the straggler net)
    };

    // IMPURE: drive ONE movement step (MoveTo / RawPath / Jump) of `ev` for
    // `bot` from its own position, using `st` for the cursor/watchdog scratch.
    // The fall volume is checked first on every tick. Any other step kind
    // reports Done immediately (followers skip the leader-only steps).
    StepOutcome DriveStep(Player* bot, PlayerbotAI* botAI, AiObjectContext* context,
                          DungeonEvent const& ev, EventStep const& step, DcChainState& st);

    // IMPURE, FOLLOWER rung: adopt the leader's PartyWide chain, wait for the
    // release, run the movement steps, mark done. Returns true while it owns
    // the follower's tick (holding for the release or driving a step); false
    // when there is nothing to do and follow-tank should carry on.
    bool DriveFollower(Player* bot, PlayerbotAI* botAI, AiObjectContext* context);

    // IMPURE: the leader's straggler gate for its LAST movement step. True once
    // every alive bot follower on the map has finished its chain, or
    // DC_CHAIN_STRAGGLER_MS has passed since the leader arrived (stamps
    // `st.arrivedMs` on first call) — the caller then pulls the stragglers.
    bool StragglersResolved(Player* leader, DungeonEvent const& ev, DcChainState& st, uint32 nowMs);

    // Mark every alive bot follower's chain for `ev` done (after a straggler
    // teleport, so a pulled bot does not keep driving a chain it was lifted off).
    void FinishFollowerChains(Player* leader, DungeonEvent const& ev);

    // True while `bot` is driving a SUBMERGED RawPath step of its own chain (a
    // follower), or — for the leader — its active event step is one. The breath
    // watchdog's "driving a swim leg" and the druid aquatic-form veto read this.
    bool IsOnSwimLeg(Player* bot);

    // IsOnSwimLeg, widened to the LEG: true while this bot's leader is inside an
    // event that contains a submerged step or a water-breathing gate (a dive
    // with breath provisioning and a fight at the bottom), or the bot itself
    // is on a swim step. The druid aquatic-form veto reads it.
    bool IsWaterLegActive(Player* bot);

    // The chain record of `bot` (any bot with a DC context). nullptr for a human.
    DcChainState* StateOf(Player* bot);

    // Treat a double wedge on a chain WITH a fall volume as a fall: the bot is
    // stuck on (or beside) a stone it cannot leave, and the fall handler's
    // retry / finalise ladder is the right escalation — a stall would park the
    // whole run on a rock. No-op (false) when the event has no fall volume.
    bool HandleAsFall(Player* bot, DungeonEvent const& ev, DcChainState& st, uint32 stepIndex);
}

#endif  // _PLAYERBOT_DCPARTYWIDECHAIN_H
