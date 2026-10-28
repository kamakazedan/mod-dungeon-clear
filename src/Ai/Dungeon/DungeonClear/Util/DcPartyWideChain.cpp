/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcPartyWideChain.h"

#include <cmath>
#include <optional>
#include <vector>

#include "AiObjectContext.h"
#include "Group.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRawPathDrive.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"

namespace
{
    // Default arrival radius for a chain MoveTo / Jump when the step gives none.
    constexpr float DC_CHAIN_MOVE_RADIUS = 4.0f;
    // A launched Jump that has neither landed nor is still moving after this
    // long is re-issued (the arc ended short, or the bot slid off its target).
    constexpr uint32 DC_CHAIN_JUMP_RETRY_MS = 4000;
    // Wedges per step before the step is given up (the swim stall / the fall
    // handler for a chain with a fall volume).
    constexpr uint32 DC_CHAIN_MAX_WEDGES = 2;

    DcRawPathDrive::Params PathParams(EventStep const& step)
    {
        DcRawPathDrive::Params p;
        if (!step.swim)
        {
            // A land polyline (the rim) is walked at run speed over short hops;
            // consume points tighter so a 1yd-wide rim is followed faithfully.
            p.pointReached = 2.0f;
        }
        return p;
    }

    // The run owner's state, for the fall counter (published to the record).
    DcRunState* LeaderRun(Player* bot)
    {
        Player* leader = DcLeaderSignal::FindRunOwner(bot);
        if (!leader)
            leader = DcLeaderSignal::FindLeaderTank(bot);
        PlayerbotAI* ai = leader ? GET_PLAYERBOT_AI(leader) : nullptr;
        return ai ? &DcRun::Of(ai) : nullptr;
    }

    // The bot is in the event's fall volume: count it, log it, and either send
    // it back to the retry point (chain rewound) or, after maxFalls, on to the
    // final landing (chain finished). Returns true if a fall was handled.
    bool HandleFall(Player* bot, DungeonEvent const& ev, DcChainState& st, uint32 stepIndex, uint32 now,
                    bool force = false)
    {
        if (!ev.fall.enabled)
            return false;
        if (!force && (!DcPartyWideChain::FallArmed(ev, stepIndex) ||
                       !ev.fall.Contains(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ())))
            return false;

        ++st.falls;
        if (DcRunState* run = LeaderRun(bot))
        {
            ++run->chainFalls;
            run->SetTestExtra("chainFalls", std::to_string(run->chainFalls), /*numeric*/ true);
        }
        bool const finalise = st.falls >= ev.fall.maxFalls;
        float const tx = finalise ? ev.fall.finalX : ev.fall.retryX;
        float const ty = finalise ? ev.fall.finalY : ev.fall.retryY;
        float const tz = finalise ? ev.fall.finalZ : ev.fall.retryZ;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] chain '{}': {} at ({:.1f},{:.1f},{:.1f}) on step {} "
                 "(fall {} of {}) -> {}",
                 bot->GetName(), ev.name,
                 force ? "wedged on the crossing (treated as a fall)" : "fell into the water",
                 bot->GetPositionX(), bot->GetPositionY(),
                 bot->GetPositionZ(), stepIndex, st.falls, ev.fall.maxFalls,
                 finalise ? "teleporting to the far landing and finishing the chain"
                          : "teleporting back to the start to re-attempt the crossing");
        bot->GetMotionMaster()->Clear();
        bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR |
                                    MOVEMENTFLAG_SWIMMING);
        bot->NearTeleportTo(tx, ty, tz, bot->GetOrientation(), /*casting*/ false,
                            /*vehicle*/ false, /*withPet*/ true);
        if (finalise)
            st.done = true;
        else
        {
            st.stepIndex = 0;
            st.BeginStep(now);
        }
        return true;
    }

    bool Within(Player* bot, float x, float y, float z, float radius)
    {
        return bot->GetExactDist(x, y, z) <= radius;
    }

    DcPartyWideChain::StepOutcome DriveMoveTo(Player* bot, EventStep const& step)
    {
        float const radius = step.radius > 0.0f ? step.radius : DC_CHAIN_MOVE_RADIUS;
        if (Within(bot, step.x, step.y, step.z, radius))
            return DcPartyWideChain::StepOutcome::Done;
        if (!bot->isMoving())
        {
            DcMovement::ClearMovementWait(bot);
            bot->GetMotionMaster()->MovePoint(0, step.x, step.y, step.z, FORCED_MOVEMENT_NONE,
                                              0.0f, 0.0f, /*generatePath*/ true, false);
        }
        return DcPartyWideChain::StepOutcome::Running;
    }

    DcPartyWideChain::StepOutcome DriveRawPath(Player* bot, PlayerbotAI* botAI,
                                               DungeonEvent const& ev, EventStep const& step,
                                               DcChainState& st, uint32 now)
    {
        using namespace DcRawPathDrive;
        float const radius = step.radius > 0.0f ? step.radius : DC_CHAIN_MOVE_RADIUS;
        G3D::Vector3 const pos(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        MotionMaster* mm = bot->GetMotionMaster();
        bool const splineRunning =
            mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE && bot->isMoving();

        Decision const d = Step(step.path, st.cursor, pos, radius, splineRunning, now, st.watch,
                                PathParams(step));
        st.cursor = d.cursor;
        switch (d.verdict)
        {
            case Verdict::Arrived:
                DcMovement::ResolveEscortConflict(bot);
                return DcPartyWideChain::StepOutcome::Done;
            case Verdict::InFlight:
                return DcPartyWideChain::StepOutcome::Running;
            case Verdict::OffLeg:
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] chain '{}': {:.0f}yd off the authored polyline at point {} -> abandoning",
                         bot->GetName(), ev.name, (pos - step.path[st.cursor]).length(), st.cursor);
                return DcPartyWideChain::StepOutcome::Stalled;
            case Verdict::Wedged:
                ++st.wedges;
                st.watch.Reset();
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] chain '{}': no 3D progress toward point {} for 6s (wedge {} of {}) -> {}",
                         bot->GetName(), ev.name, st.cursor, st.wedges, DC_CHAIN_MAX_WEDGES,
                         st.wedges >= DC_CHAIN_MAX_WEDGES ? "giving up" : "re-issuing the spline");
                if (st.wedges >= DC_CHAIN_MAX_WEDGES)
                    return DcPartyWideChain::StepOutcome::Stalled;
                DcMovement::ResolveEscortConflict(bot);
                [[fallthrough]];
            case Verdict::Issue:
                break;
        }

        // Spline window from the cursor: [live pos, remaining points...] with the
        // authored Z verbatim (no UpdateAllowedPositionZ, no navmesh).
        Movement::PointsArray pts;
        pts.push_back(pos);
        for (size_t i = st.cursor;
             i < step.path.size() && pts.size() < DungeonPathFollower::MAX_SPLINE_WINDOW_POINTS; ++i)
            pts.push_back(step.path[i]);
        if (!DcMovement::SplinePath(botAI, pts))
            return DcPartyWideChain::StepOutcome::Stalled;
        return DcPartyWideChain::StepOutcome::Running;
    }

    DcPartyWideChain::StepOutcome DriveJump(Player* bot, DungeonEvent const& ev, EventStep const& step,
                                            DcChainState& st, uint32 now)
    {
        float const radius = step.radius > 0.0f ? step.radius : DC_CHAIN_MOVE_RADIUS;
        if (Within(bot, step.x, step.y, step.z, radius))
            return DcPartyWideChain::StepOutcome::Done;
        // While the arc is in flight the bot reads as moving — never re-issue
        // (MoveJump would restart the arc and never land).
        if (bot->isMoving())
            return DcPartyWideChain::StepOutcome::Running;
        if (st.jumpIssuedMs && now - st.jumpIssuedMs < DC_CHAIN_JUMP_RETRY_MS)
            return DcPartyWideChain::StepOutcome::Running;  // settling after the landing
        if (st.jumpIssuedMs)
        {
            ++st.wedges;
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] chain '{}': jump to ({:.1f},{:.1f},{:.1f}) ended {:.1f}yd short "
                     "(wedge {} of {})",
                     bot->GetName(), ev.name, step.x, step.y, step.z,
                     bot->GetExactDist(step.x, step.y, step.z), st.wedges, DC_CHAIN_MAX_WEDGES);
            if (st.wedges >= DC_CHAIN_MAX_WEDGES)
                return DcPartyWideChain::StepOutcome::Stalled;
        }
        float const speed = bot->GetSpeed(MOVE_RUN);
        float const speedZ = step.jumpSpeedZ > 0.0f ? step.jumpSpeedZ : speed;
        MotionMaster* mm = bot->GetMotionMaster();
        mm->Clear();
        DcMovement::ClearMovementWait(bot);
        mm->MoveJump(step.x, step.y, step.z, speed, speedZ, 1);
        st.jumpIssuedMs = now;
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] chain '{}': jump -> ({:.1f},{:.1f},{:.1f}) speedZ {:.1f}",
                  bot->GetName(), ev.name, step.x, step.y, step.z, speedZ);
        return DcPartyWideChain::StepOutcome::Running;
    }

    // The bots of `leader`'s group on its map, in group order, leader excluded —
    // the release order of the chain.
    std::vector<Player*> FollowerBots(Player* leader)
    {
        std::vector<Player*> out;
        Group* group = leader->GetGroup();
        if (!group)
            return out;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || m == leader || !m->IsInWorld() || !GET_PLAYERBOT_AI(m))
                continue;
            if (m->GetMapId() != leader->GetMapId())
                continue;
            out.push_back(m);
        }
        return out;
    }

    // The leader's own view of the chain, as the release rule sees it.
    DcChainRelease::Ahead LeaderAhead(Player* leader, DungeonEvent const& ev)
    {
        DcChainRelease::Ahead a;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(leader);
        if (!ai)
            return a;
        AiObjectContext* ctx = ai->GetAiObjectContext();
        DungeonEventProgress const& prog =
            ctx->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
        DcChainState const& st = ctx->GetValue<DcChainState&>(DcKey::ChainState)->Get();
        if (st.eventId != ev.id || st.mapId != ev.mapId)
            return a;
        a.known = true;
        a.startedMs = st.startedMs;
        // The leader is "done" with the chain for release purposes once it has
        // moved past the event (objective latched) — its progress no longer
        // points at it — or finished the last movement step.
        a.done = prog.eventId != ev.id || st.done || st.arrivedMs != 0;
        a.stepIndex = prog.eventId == ev.id ? prog.stepIndex : static_cast<uint32>(ev.steps.size());
        return a;
    }

    DcChainRelease::Ahead FollowerAhead(Player* follower, DungeonEvent const& ev)
    {
        DcChainRelease::Ahead a;
        DcChainState const* st = DcPartyWideChain::StateOf(follower);
        if (!st || st->eventId != ev.id || st->mapId != ev.mapId)
            return a;
        a.known = true;
        a.startedMs = st->startedMs;
        a.done = st->done;
        a.stepIndex = st->stepIndex;
        return a;
    }
}

bool DcPartyWideChain::HandleAsFall(Player* bot, DungeonEvent const& ev, DcChainState& st, uint32 stepIndex)
{
    return bot && HandleFall(bot, ev, st, stepIndex, getMSTime(), /*force*/ true);
}

DcChainState* DcPartyWideChain::StateOf(Player* bot)
{
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!ai)
        return nullptr;
    return &ai->GetAiObjectContext()->GetValue<DcChainState&>(DcKey::ChainState)->Get();
}

DcPartyWideChain::StepOutcome DcPartyWideChain::DriveStep(Player* bot, PlayerbotAI* botAI,
                                                          AiObjectContext* /*context*/,
                                                          DungeonEvent const& ev,
                                                          EventStep const& step, DcChainState& st)
{
    if (!bot || !botAI)
        return StepOutcome::Stalled;
    uint32 const now = getMSTime();
    if (HandleFall(bot, ev, st, st.stepIndex, now))
        return StepOutcome::Fell;
    switch (step.kind)
    {
        case EventStepKind::MoveTo:
            return DriveMoveTo(bot, step);
        case EventStepKind::RawPath:
            return DriveRawPath(bot, botAI, ev, step, st, now);
        case EventStepKind::Jump:
            return DriveJump(bot, ev, step, st, now);
        default:
            return StepOutcome::Done;
    }
}

bool DcPartyWideChain::DriveFollower(Player* bot, PlayerbotAI* botAI, AiObjectContext* context)
{
    if (!bot || !botAI || !context || !bot->IsAlive())
        return false;

    DcChainState& st = context->GetValue<DcChainState&>(DcKey::ChainState)->Get();
    uint32 const now = getMSTime();

    // Adopt the leader's chain the moment it starts one; keep our own copy of
    // the event until our chain is done (see the header).
    Player* leader = DcLeaderSignal::FindLeaderTank(bot);
    DungeonEvent const* live = leader && leader != bot ? DcLeaderSignal::GetLeaderPartyWideChain(bot) : nullptr;
    if (live && (st.ev != live || st.instanceId != bot->GetInstanceId()))
    {
        st.Reset();
        st.ev = live;
        st.eventId = live->id;
        st.mapId = live->mapId;
        st.instanceId = bot->GetInstanceId();
    }
    if (!st.ev)
        return false;
    if (!live && (st.done || st.startedMs == 0))
    {
        // The leader has moved on and we never started (or already finished):
        // nothing left to drive. Keep the record so a late signal cannot
        // re-adopt a finished chain in the same instance.
        return false;
    }
    if (st.done)
        return false;
    if (!leader || leader->GetMapId() != bot->GetMapId())
        return false;

    DungeonEvent const& ev = *st.ev;

    // Not yet released: hold where we stand. Follow-tank would otherwise walk us
    // after the leader onto the off-mesh rim via MoveFollow — the clip.
    if (st.startedMs == 0)
    {
        std::vector<Player*> const bots = FollowerBots(leader);
        size_t idx = bots.size();
        for (size_t i = 0; i < bots.size(); ++i)
            if (bots[i] == bot)
            {
                idx = i;
                break;
            }
        DcChainRelease::Ahead const ahead =
            idx == 0 || idx >= bots.size() ? LeaderAhead(leader, ev) : FollowerAhead(bots[idx - 1], ev);
        if (!DcChainRelease::MayStart(ahead, DcChainRelease::ReleaseAfterStep(ev), now,
                                      DC_CHAIN_SPACING_MS))
        {
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            return true;
        }
        st.startedMs = now;
        st.stepIndex = 0;
        st.BeginStep(now);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] chain '{}': released (follower {} of {}) — running the chain from "
                 "({:.1f},{:.1f},{:.1f})",
                 bot->GetName(), ev.name, idx + 1, bots.size(), bot->GetPositionX(),
                 bot->GetPositionY(), bot->GetPositionZ());
    }

    // Skip the leader-only steps.
    while (st.stepIndex < ev.steps.size() && !IsMovementStepKind(ev.steps[st.stepIndex].kind))
        ++st.stepIndex;
    if (st.stepIndex >= ev.steps.size())
    {
        st.done = true;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] chain '{}': complete after {}s ({} fall(s))",
                 bot->GetName(), ev.name, (now - st.startedMs) / 1000, st.falls);
        return false;
    }

    EventStep const& step = ev.steps[st.stepIndex];
    switch (DriveStep(bot, botAI, context, ev, step, st))
    {
        case StepOutcome::Running:
        case StepOutcome::Fell:
            return true;
        case StepOutcome::Done:
            ++st.stepIndex;
            st.BeginStep(now);
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] chain '{}': step {} done -> {}",
                      bot->GetName(), ev.name, st.stepIndex - 1, st.stepIndex);
            return true;
        case StepOutcome::Stalled:
            // A chain with a fall volume escalates through the fall ladder
            // (retry from the start, finalise after maxFalls) rather than
            // sitting on a rock. Otherwise hand ourselves to the straggler net:
            // the leader pulls us across once its own chain lands.
            if (HandleAsFall(bot, ev, st, st.stepIndex))
                return true;
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] chain '{}': step {} stalled -> leaving the chain to the straggler net",
                     bot->GetName(), ev.name, st.stepIndex);
            st.done = true;
            return false;
    }
    return false;
}

bool DcPartyWideChain::StragglersResolved(Player* leader, DungeonEvent const& ev, DcChainState& st,
                                          uint32 nowMs)
{
    if (!st.arrivedMs)
    {
        st.arrivedMs = nowMs;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] chain '{}': leader across — waiting up to {}s for the followers' chains",
                 leader->GetName(), ev.name, DC_CHAIN_STRAGGLER_MS / 1000);
    }
    bool allDone = true;
    for (Player* f : FollowerBots(leader))
    {
        if (!f->IsAlive())
            continue;
        DcChainState const* fs = StateOf(f);
        if (!fs || fs->eventId != ev.id || !fs->done)
        {
            allDone = false;
            break;
        }
    }
    if (allDone)
        return true;
    return nowMs - st.arrivedMs >= DC_CHAIN_STRAGGLER_MS;
}

void DcPartyWideChain::FinishFollowerChains(Player* leader, DungeonEvent const& ev)
{
    for (Player* f : FollowerBots(leader))
    {
        DcChainState* fs = StateOf(f);
        if (!fs)
            continue;
        if (fs->eventId != ev.id)
        {
            fs->Reset();
            fs->ev = &ev;
            fs->eventId = ev.id;
            fs->mapId = ev.mapId;
            fs->instanceId = f->GetInstanceId();
        }
        if (!fs->done)
        {
            fs->startedMs = fs->startedMs ? fs->startedMs : getMSTime();
            fs->done = true;
        }
    }
}

bool DcPartyWideChain::IsOnSwimLeg(Player* bot)
{
    PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!ai)
        return false;
    AiObjectContext* ctx = ai->GetAiObjectContext();

    // A follower mid-chain on a submerged step.
    DcChainState const& st = ctx->GetValue<DcChainState&>(DcKey::ChainState)->Get();
    if (st.ev && !st.done && st.startedMs && st.stepIndex < st.ev->steps.size() &&
        st.ev->steps[st.stepIndex].kind == EventStepKind::RawPath && st.ev->steps[st.stepIndex].swim)
        return true;

    // The leader on a submerged step of its active anchored event.
    DungeonEventProgress const& prog = ctx->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
    if (!prog.eventId)
        return false;
    std::optional<DungeonBossInfo> const next =
        ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
    if (!next.has_value() || next->kind != DungeonAnchorKind::Objective || !next->eventId)
        return false;
    DungeonEvent const* ev = DungeonEventRegistry::Find(next->mapId, next->eventId);
    if (!ev || prog.eventId != ev->id || prog.stepIndex >= ev->steps.size())
        return false;
    EventStep const& step = ev->steps[prog.stepIndex];
    return step.kind == EventStepKind::RawPath && step.swim;
}

bool DcPartyWideChain::IsWaterLegActive(Player* bot)
{
    if (!bot)
        return false;
    if (IsOnSwimLeg(bot))
        return true;

    Player* leader = DcLeaderSignal::FindLeaderTank(bot);
    PlayerbotAI* leaderAI = leader ? GET_PLAYERBOT_AI(leader) : nullptr;
    if (!leaderAI)
        return false;
    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    if (!DcRun::Of(ctx).enabled)
        return false;
    DungeonEventProgress const& prog = ctx->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
    if (!prog.eventId)
        return false;
    std::optional<DungeonBossInfo> const next =
        ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
    if (!next.has_value() || next->kind != DungeonAnchorKind::Objective || !next->eventId)
        return false;
    DungeonEvent const* ev = DungeonEventRegistry::Find(next->mapId, next->eventId);
    if (!ev || prog.eventId != ev->id)
        return false;
    for (EventStep const& s : ev->steps)
        if (s.kind == EventStepKind::EnsureWaterBreathing || (s.kind == EventStepKind::RawPath && s.swim))
            return true;
    return false;
}
