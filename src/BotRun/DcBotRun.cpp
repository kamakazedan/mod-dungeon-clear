/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BotRun/DcBotRun.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Group.h"
#include "GroupMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Timer.h"
#include "DcModuleEnable.h"
#include "DcStrategyGate.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonWingRegistry.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPlayerbotCompat.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRunWing.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "TestRun/DcDiagSnapshot.h"
#include "TestRun/DcTestRunVerdict.h"
#include "TestRun/DcWipeContext.h"

namespace
{
    // A watched run is looked at this often. The `.dc test` monitor looks every
    // tick; half a second is plenty for verdicts measured in seconds and
    // minutes, and keeps a handful of runs off the tick.
    constexpr uint32 kStepMs = 500;

    // A verdict nobody came for is let go after this.
    constexpr uint32 kKeepFinishedMs = HOUR * IN_MILLISECONDS;

    // Getting this much closer to the run's next target counts as progress
    // (the `.dc test` monitor's DC_TESTRUN_PROGRESS_EPSILON_YD).
    constexpr float kCloserYd = 1.0f;

    struct Watch
    {
        ObjectGuid group;
        ObjectGuid tank;             // who `dc on` goes (went) to; the run's owner after
        uint32 lfgDungeon = 0;
        DcBotRun::Limits limits;
        DcBotRun::Phase phase = DcBotRun::Phase::Starting;
        std::string why;
        uint32 mapId = 0;
        uint32 instanceId = 0;

        uint32 wingMask = 0;         // the wing's encounter bits; 0 = the whole map
        std::string wing;
        uint32 bossMask = 0;         // encounter bits of the roster's bosses (not objectives)
        uint32 bossesTotal = 0;
        uint32 bossesDown = 0;

        uint32 accMs = 0;
        uint32 stageMs = 0;          // Starting: since Start
        uint32 totalMs = 0;          // Running: since `dc on`
        uint32 finishedAt = 0;       // getMSTime() it was decided
        bool   waitAtBossOff = false;
        ObjectGuid overrideOwner;    // whose DcSettings overrides were set

        // The `.dc test` monitor's progress picture.
        DcTestRun::ProgressClock clock;
        uint32 lastMask = 0;
        std::size_t lastAnchors = 0;
        bool lastInCombat = false;
        ObjectGuid lastVictim;
        uint32 lastVictimHp = 0;
        uint32 bestDistEntry = 0;
        float bestDist = -1.0f;
        uint32 pausedForMs = 0;
        uint32 stalledForMs = 0;
        uint32 wipedForMs = 0;
        bool   stallLogged = false;  // this spell of no progress has had its snapshot
    };

    // World thread only.
    std::unordered_map<uint64, Watch> g_watches;   // by group guid
    ObjectGuid g_startingFor;

    // The disable funnel's notes, from the leaders' AI threads. Only for the
    // members of watched groups (g_members, published by the world thread), so
    // nothing piles up from the clears players run.
    std::mutex g_noteMutex;
    std::unordered_set<uint64> g_members;
    std::unordered_map<uint64, std::string> g_notes;   // member guid -> first reason

    bool Finished(DcBotRun::Phase phase)
    {
        return phase == DcBotRun::Phase::Cleared || phase == DcBotRun::Phase::Failed ||
               phase == DcBotRun::Phase::HandedOver;
    }

    Group* GroupOf(ObjectGuid guid)
    {
        return guid ? sGroupMgr->GetGroupByGUID(guid.GetCounter()) : nullptr;
    }

    // Somebody in the group is playing: a person, or a person's own character
    // on autopilot (a self-bot).
    bool SomeonePlaying(Group* group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (DcPlayerbotCompat::IsRealPlayer(member) || DcPlayerbotCompat::IsSelfBot(member))
                    return true;
        return false;
    }

    std::vector<Player*> MembersOnMap(Group* group, uint32 mapId, uint32 instanceId)
    {
        std::vector<Player*> out;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member->IsInWorld() && member->GetMapId() == mapId &&
                member->GetInstanceId() == instanceId)
                out.push_back(member);
        }
        return out;
    }

    // The run's owner: the member whose run state is enabled, alive or dead.
    Player* RunOwner(std::vector<Player*> const& members)
    {
        for (Player* member : members)
            if (Player* owner = DcLeaderSignal::FindRunOwner(member))
                return owner;
        return nullptr;
    }

    // The first reason the disable funnel gave for any of these members.
    bool TakeNote(Group* group, std::string& reason)
    {
        std::lock_guard<std::mutex> lock(g_noteMutex);
        if (g_notes.empty())
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;
            auto it = g_notes.find(member->GetGUID().GetRawValue());
            if (it == g_notes.end())
                continue;
            reason = it->second;
            g_notes.erase(it);
            return true;
        }
        return false;
    }

    // Who OnRunDisabled notes, from now on: every member of a run being watched.
    void PublishMembers()
    {
        std::unordered_set<uint64> members;
        for (auto const& [key, w] : g_watches)
        {
            if (w.phase != DcBotRun::Phase::Running && w.phase != DcBotRun::Phase::Starting)
                continue;
            if (Group* group = GroupOf(w.group))
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource())
                        members.insert(member->GetGUID().GetRawValue());
        }
        std::lock_guard<std::mutex> lock(g_noteMutex);
        g_members.swap(members);
        for (auto it = g_notes.begin(); it != g_notes.end();)
            it = g_members.count(it->first) ? std::next(it) : g_notes.erase(it);
    }

    void ClearOverrides(Watch& w)
    {
        if (w.overrideOwner)
            DcSettings::ClearRun(w.overrideOwner);
        w.overrideOwner.Clear();
    }

    void Finish(Watch& w, DcBotRun::Phase phase, std::string const& why)
    {
        w.phase = phase;
        w.why = why;
        w.finishedAt = getMSTime();
        ClearOverrides(w);
        char const* what = phase == DcBotRun::Phase::Cleared  ? "cleared"
                         : phase == DcBotRun::Phase::Failed   ? "failed"
                                                              : "handed over";
        LOG_INFO("playerbots.dungeonclear", "BOTRUN group {} on map {} {} after {}s{}{}",
                 w.group.GetCounter(), w.mapId, what, (w.totalMs ? w.totalMs : w.stageMs) / IN_MILLISECONDS,
                 why.empty() ? "" : ": ", why);
    }

    // How the run stands, in the log: the snapshot `.dc test` records at a
    // failed run's teardown (DcDiag) -- where everyone is, the target and the
    // route to it, the pull, the stall reason, who holds the party in combat.
    // A bot run has nobody to ask afterwards, and the party is gone a moment
    // later, so this is all there is to go on. Read-only; before anything is
    // switched off. With `json`, the whole record as well.
    void LogDiag(Watch const& w, Player* tank, char const* when, bool json)
    {
        if (!tank)
            return;
        DcDiag::Snapshot const snap = DcDiag::Capture(tank, when);
        LOG_INFO("playerbots.dungeonclear", "BOTRUN group {} on map {} {}: {}", w.group.GetCounter(), w.mapId, when,
                 DcDiag::Summarize(snap));
        if (!snap.valid)
            return;
        LOG_INFO("playerbots.dungeonclear", "BOTRUN group {} combat: {}", w.group.GetCounter(),
                 DcDiag::SummarizeCombat(snap));
        if (json)
        {
            std::ostringstream out;
            DcDiag::AppendJson(out, snap);
            LOG_INFO("playerbots.dungeonclear", "BOTRUN group {} diag {}", w.group.GetCounter(), out.str());
        }
    }

    // A failed run that is still switched on is switched off, with the reason.
    void SwitchOff(std::vector<Player*> const& members, std::string const& why)
    {
        if (Player* owner = RunOwner(members))
            if (PlayerbotAI* ownerAI = GET_PLAYERBOT_AI(owner))
                DcActionShared::DisableDungeonClear(ownerAI, "Dungeon clear stopped: " + why + ".");
    }

    uint32 Limit(uint32 seconds)
    {
        return seconds ? seconds * IN_MILLISECONDS : std::numeric_limits<uint32>::max();
    }

    std::string SecondsText(uint32 ms)
    {
        return std::to_string(ms / IN_MILLISECONDS) + "s";
    }

    // What a failed verdict says, for the log and the caller.
    std::string Describe(DcTestRun::Verdict verdict, Watch const& w, std::string const& disabledWhy)
    {
        switch (verdict)
        {
            case DcTestRun::Verdict::FailDisabled:
                return "the clear stopped: " + DcTestRun::StripResumeHint(disabledWhy);
            case DcTestRun::Verdict::FailPartyWiped:
                return "the whole party died";
            case DcTestRun::Verdict::FailPausedTimeout:
                return "paused for " + SecondsText(w.pausedForMs);
            case DcTestRun::Verdict::FailStalledTimeout:
                return "stuck for " + SecondsText(w.stalledForMs);
            case DcTestRun::Verdict::FailNoProgress:
                return "nothing moved forward for " + SecondsText(w.clock.sinceMs);
            case DcTestRun::Verdict::FailOverallTimeout:
                return "still going after " + SecondsText(w.totalMs);
            case DcTestRun::Verdict::FailAborted:
                return "the tank left the dungeon";
            default:
                break;
        }
        return DcTestRun::VerdictName(verdict);
    }

    // Waiting for the clear to take: a tank alive in the dungeon, the wing
    // latched, the roster read, then `dc on`.
    void StepStarting(Watch& w, std::vector<Player*> const& members, uint32 dt)
    {
        w.stageMs += dt;
        bool const late = w.stageMs >= Limit(w.limits.startS);

        // The tank `dc on` goes to: the one chosen at Start, or whoever the
        // election names now if that one is gone or dead.
        Player* tank = ObjectAccessor::FindPlayer(w.tank);
        if (!tank || std::find(members.begin(), members.end(), tank) == members.end() || !tank->IsAlive())
        {
            tank = nullptr;
            for (Player* member : members)
                if ((tank = DcLeaderSignal::FindLeaderTank(member)))
                    break;
            if (tank)
                w.tank = tank->GetGUID();
        }
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!tankAI)
        {
            if (late)
                Finish(w, DcBotRun::Phase::Failed, "no tank alive in the dungeon");
            return;
        }

        // Freshly teleported bots may not have the clear's strategies yet.
        for (Player* member : members)
            DcStrategyGate::Reconcile(member);

        // The wing the dungeon finder made the group for (split maps only),
        // latched before `dc on` so the run clears that wing.
        if (w.lfgDungeon && w.wing.empty())
            if (DungeonWing const* wing = DungeonWingRegistry::WingForLfgDungeon(w.mapId, w.lfgDungeon))
            {
                DcRunWing::Set(tank, *wing, DcRunWing::Source::Lfg);
                w.wing = wing->name;
                w.wingMask = wing->encounterMask;
            }

        AiObjectContext* ctx = tankAI->GetAiObjectContext();
        std::vector<DungeonBossInfo> const bosses =
            ctx->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses)->Get();
        if (bosses.empty())
        {
            // The roster takes a tick or two after the teleport.
            if (late)
                Finish(w, DcBotRun::Phase::Failed, "no boss list for this dungeon");
            return;
        }

        // Nobody will resume a pre-pull hold.
        if (!w.waitAtBossOff)
        {
            DcSettings::SetOverride(tank->GetGUID(), "WaitAtBoss", 0.0);
            w.overrideOwner = tank->GetGUID();
            w.waitAtBossOff = true;
        }

        g_startingFor = tank->GetGUID();
        tankAI->DoSpecificAction("dc on", Event("dc", "", tank), true);
        g_startingFor.Clear();

        if (!DcRun::Of(ctx).enabled)
        {
            if (late)
                Finish(w, DcBotRun::Phase::Failed, "the clear would not start");
            return;
        }

        // Running. The roster's bosses, for the count; what is already done,
        // so it is not counted as progress.
        w.phase = DcBotRun::Phase::Running;
        w.bossMask = 0;
        w.bossesTotal = 0;
        for (DungeonBossInfo const& boss : bosses)
        {
            if (boss.kind != DungeonAnchorKind::Boss || boss.skipByDesign)
                continue;
            ++w.bossesTotal;
            if (boss.encounterIndex < 32)
                w.bossMask |= 1u << boss.encounterIndex;
        }
        if (w.wingMask)
            w.bossMask &= w.wingMask;
        if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
            w.lastMask = inst->GetCompletedEncounterMask();
        w.lastAnchors = ctx->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get().size();
        w.clock = DcTestRun::ProgressClock{};
        w.totalMs = 0;
        LOG_INFO("playerbots.dungeonclear", "BOTRUN group {} on map {}: {} leads the clear{}{} ({} bosses)",
                 w.group.GetCounter(), w.mapId, tank->GetName(), w.wing.empty() ? "" : " of ", w.wing,
                 w.bossesTotal);
    }

    // The `.dc test` monitor, run for a party nobody is playing in.
    void StepRunning(Watch& w, Group* group, std::vector<Player*> const& members, uint32 dt)
    {
        w.totalMs += dt;

        DcTestRun::Observation obs;
        obs.elapsedMs = w.totalMs;

        std::string disabledWhy;
        obs.disableFired = TakeNote(group, disabledWhy);

        // The run's owner, alive or a corpse; else the tank it started on.
        Player* tank = RunOwner(members);
        if (!tank)
        {
            tank = ObjectAccessor::FindPlayer(w.tank);
            if (tank && std::find(members.begin(), members.end(), tank) == members.end())
                tank = nullptr;
        }
        else
            w.tank = tank->GetGUID();
        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;

        if (!tankAI)
            obs.leaderMissing = true;
        else
        {
            AiObjectContext* ctx = tankAI->GetAiObjectContext();
            DcRunState const& rs = DcRun::Of(ctx);

            // The funnel tells OnRunDisabled; this catches a run switched off
            // any other way.
            if (!obs.disableFired && !rs.enabled)
            {
                obs.disableFired = true;
                disabledWhy = "it was switched off";
            }

            uint32 mask = w.lastMask;
            if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
                mask = inst->GetCompletedEncounterMask();
            std::size_t const anchors =
                ctx->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get().size();

            // A kill (the wing's own, on a split map) or an anchor cleared.
            bool progressed = (mask & ~w.lastMask & (w.wingMask ? w.wingMask : ~0u)) != 0;
            w.lastMask = mask;
            if (anchors > w.lastAnchors)
            {
                w.lastAnchors = anchors;
                progressed = true;
            }
            w.bossesDown = std::min<uint32>(static_cast<uint32>(std::popcount(mask & w.bossMask)), w.bossesTotal);

            // The fight changing: in or out of combat, a new victim, its
            // health moving. Something CHANGING, never a state holding: a
            // phantom combat that never ends must not read as progress.
            bool const inCombat = tank->IsInCombat();
            Unit const* victim = tank->GetVictim();
            ObjectGuid const victimGuid = victim ? victim->GetGUID() : ObjectGuid::Empty;
            uint32 const victimHp = victim ? static_cast<uint32>(victim->GetHealthPct()) : 0u;
            if (inCombat != w.lastInCombat || victimGuid != w.lastVictim || victimHp != w.lastVictimHp)
                progressed = true;
            w.lastInCombat = inCombat;
            w.lastVictim = victimGuid;
            w.lastVictimHp = victimHp;

            // Getting closer to the next target.
            std::optional<DungeonBossInfo> const next =
                ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
            if (next.has_value() && next->mapId == tank->GetMapId())
            {
                if (next->entry != w.bestDistEntry)
                {
                    w.bestDistEntry = next->entry;
                    w.bestDist = -1.0f;
                }
                float const dist = tank->GetDistance(next->x, next->y, next->z);
                if (w.bestDist < 0.0f || dist < w.bestDist - kCloserYd)
                {
                    w.bestDist = dist;
                    progressed = true;
                }
            }

            w.clock.Step(dt, progressed, rs.eventProgressSeq);
            obs.sinceProgressMs = w.clock.sinceMs;

            // Half the time a run may go without moving forward: how it stands
            // as it starts to stall, once a spell (the end of it is logged as
            // it fails, if it does).
            uint32 const halfway = w.limits.noProgressS ? w.limits.noProgressS * IN_MILLISECONDS / 2 : 0;
            if (halfway && w.clock.sinceMs >= halfway && !w.stallLogged)
            {
                w.stallLogged = true;
                LogDiag(w, tank, "stalling", false);
            }
            else if (w.clock.sinceMs < halfway)
                w.stallLogged = false;

            if (rs.paused)
                w.pausedForMs += dt;
            else
                w.pausedForMs = 0;
            obs.paused = rs.paused;
            obs.pausedForMs = w.pausedForMs;

            // Dead or a released ghost, everyone on the map.
            bool anyAlive = false;
            for (Player* member : members)
                if (member->IsAlive() && !member->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                {
                    anyAlive = true;
                    break;
                }
            w.wipedForMs = anyAlive ? 0 : w.wipedForMs + dt;
            obs.partyWiped = !anyAlive;
            obs.wipedForMs = w.wipedForMs;

            bool const stalled = !ctx->GetValue<std::string&>(DcKey::StallReason)->Get().empty();
            w.stalledForMs = stalled ? w.stalledForMs + dt : 0;
            obs.stalled = stalled;
            obs.stalledForMs = w.stalledForMs;
        }
        obs.disableAllCleared = obs.disableFired && disabledWhy == DcActionShared::kReasonAllCleared;

        DcTestRun::Limits limits;
        limits.noProgressMs = Limit(w.limits.noProgressS);
        limits.overallTimeoutMs = Limit(w.limits.overallS ? w.limits.overallS : 7200);
        limits.stallGraceMs = Limit(w.limits.stallGraceS);
        limits.pauseGraceMs = Limit(w.limits.pauseGraceS);
        limits.wipeGraceMs = Limit(w.limits.wipeGraceS);

        DcTestRun::Verdict const verdict = DcTestRun::Classify(obs, limits);
        if (!DcTestRun::IsTerminal(verdict))
            return;
        if (DcTestRun::IsSuccess(verdict))
        {
            w.bossesDown = w.bossesTotal;
            Finish(w, DcBotRun::Phase::Cleared, "");
            return;
        }
        std::string const why = Describe(verdict, w, disabledWhy);
        LogDiag(w, tank, "failing", true);
        if (!obs.disableFired)
            SwitchOff(members, why);
        Finish(w, DcBotRun::Phase::Failed, why);
    }

    void Step(Watch& w, uint32 dt, bool moduleEnabled)
    {
        Group* group = GroupOf(w.group);
        if (!group)
        {
            Finish(w, DcBotRun::Phase::Failed, "the group broke up");
            return;
        }
        if (SomeonePlaying(group))
        {
            // Theirs now: the clear (if one runs) is left exactly as it is.
            Finish(w, DcBotRun::Phase::HandedOver, "a player joined");
            return;
        }
        std::vector<Player*> const members = MembersOnMap(group, w.mapId, w.instanceId);
        if (!moduleEnabled)
        {
            SwitchOff(members, "dungeon clear was switched off");
            Finish(w, DcBotRun::Phase::Failed, "dungeon clear was switched off");
            return;
        }
        if (members.empty())
        {
            if (w.phase == DcBotRun::Phase::Running)
                Finish(w, DcBotRun::Phase::Failed, "everyone left the dungeon");
            else if (w.stageMs >= Limit(w.limits.startS))
                Finish(w, DcBotRun::Phase::Failed, "nobody got into the dungeon");
            else
                w.stageMs += dt;
            return;
        }
        if (w.phase == DcBotRun::Phase::Starting)
            StepStarting(w, members, dt);
        else
            StepRunning(w, group, members, dt);
    }
}

bool DcBotRun::Start(Player* member, uint32_t lfgDungeon, Limits const& limits, std::string& why)
{
    if (!member || !member->IsInWorld())
    {
        why = "not in the world";
        return false;
    }
    if (!DcModule::IsEnabled())
    {
        why = "dungeon clear is switched off";
        return false;
    }
    Map* map = member->GetMap();
    if (!map || !map->IsDungeon() || map->IsRaid())
    {
        why = "not in a dungeon";
        return false;
    }
    Group* group = member->GetGroup();
    if (!group || group->isRaidGroup() || group->isBGGroup() || group->isBFGroup())
    {
        why = "not in a party";
        return false;
    }
    if (SomeonePlaying(group))
    {
        why = "someone in the group is playing";
        return false;
    }
    auto const existing = g_watches.find(group->GetGUID().GetRawValue());
    if (existing != g_watches.end() && !Finished(existing->second.phase))
    {
        why = "already watched";
        return false;
    }
    std::vector<Player*> const members = MembersOnMap(group, map->GetId(), map->GetInstanceId());
    if (RunOwner(members))
    {
        why = "a clear is already running";
        return false;
    }
    Player* tank = DcLeaderSignal::FindLeaderTank(member);
    if (!tank)
    {
        why = "no tank alive in the dungeon";
        return false;
    }

    uint64 const key = group->GetGUID().GetRawValue();
    Watch w;
    w.group = group->GetGUID();
    w.tank = tank->GetGUID();
    w.lfgDungeon = lfgDungeon;
    w.limits = limits;
    w.mapId = map->GetId();
    w.instanceId = map->GetInstanceId();
    g_watches[key] = std::move(w);
    PublishMembers();
    LOG_INFO("playerbots.dungeonclear", "BOTRUN group {} on map {} (LFG dungeon {}): starting, {} to lead",
             group->GetGUID().GetCounter(), map->GetId(), lfgDungeon, tank->GetName());
    return true;
}

DcBotRun::Status DcBotRun::Get(ObjectGuid group)
{
    Status out;
    auto const it = g_watches.find(group.GetRawValue());
    if (it == g_watches.end())
        return out;
    Watch const& w = it->second;
    out.phase = w.phase;
    out.why = w.why;
    out.tank = w.tank;
    out.lfgDungeon = w.lfgDungeon;
    out.elapsedS = (w.phase == Phase::Starting ? w.stageMs : w.totalMs) / IN_MILLISECONDS;
    out.sinceProgressS = w.clock.sinceMs / IN_MILLISECONDS;
    out.bossesDown = w.bossesDown;
    out.bossesTotal = w.bossesTotal;
    out.wing = w.wing;
    return out;
}

void DcBotRun::Stop(ObjectGuid group, std::string const& why)
{
    auto const it = g_watches.find(group.GetRawValue());
    if (it == g_watches.end())
        return;
    Watch& w = it->second;
    if (!Finished(w.phase))
    {
        if (Group* g = GroupOf(group); g && !SomeonePlaying(g))
            SwitchOff(MembersOnMap(g, w.mapId, w.instanceId), why);
        Finish(w, Phase::Failed, why);
    }
    ClearOverrides(w);
    g_watches.erase(it);
    PublishMembers();
}

uint32_t DcBotRun::Active()
{
    uint32_t n = 0;
    for (auto const& [key, w] : g_watches)
        if (!Finished(w.phase))
            ++n;
    return n;
}

bool DcBotRun::WingOf(uint32_t mapId, uint32_t entry, std::string& wingName, uint32_t& lfgDungeon)
{
    DungeonWing const* wing = DungeonWingRegistry::WingOf(mapId, entry);
    if (!wing)
        return false;
    wingName = wing->name;
    lfgDungeon = wing->lfgDungeonId;
    return true;
}

bool DcBotRun::StartingFor(Player* bot)
{
    return bot && g_startingFor && bot->GetGUID() == g_startingFor;
}

void DcBotRun::OnRunDisabled(Player* leader, std::string const& reason)
{
    if (!leader)
        return;
    uint64 const guid = leader->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_noteMutex);
    if (g_members.count(guid))
        g_notes.emplace(guid, reason);   // the first reason wins
}

void DcBotRun::Tick(uint32_t diff, bool moduleEnabled)
{
    if (g_watches.empty())
        return;

    uint32 const now = getMSTime();
    bool changed = false;
    for (auto it = g_watches.begin(); it != g_watches.end();)
    {
        Watch& w = it->second;
        if (Finished(w.phase))
        {
            // Kept for the caller to read; not for ever.
            if (getMSTimeDiff(w.finishedAt, now) >= kKeepFinishedMs)
            {
                it = g_watches.erase(it);
                changed = true;
                continue;
            }
            ++it;
            continue;
        }
        w.accMs += diff;
        if (w.accMs >= kStepMs)
        {
            uint32 const dt = w.accMs;
            w.accMs = 0;
            Step(w, dt, moduleEnabled);
            changed = true;   // who is in the group may have changed too
        }
        ++it;
    }
    // Who OnRunDisabled notes for: the groups as they are now.
    if (changed)
        PublishMembers();
}
