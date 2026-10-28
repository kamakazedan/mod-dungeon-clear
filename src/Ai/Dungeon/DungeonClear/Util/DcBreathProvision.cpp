/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcBreathProvision.h"

#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBreathSource.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFormGate.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPartyWideChain.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"

namespace
{
    std::vector<Player*> PartyOnMap(Player* leader)
    {
        std::vector<Player*> out;
        Group* group = leader->GetGroup();
        if (!group)
        {
            out.push_back(leader);
            return out;
        }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || !m->IsInWorld() || m->GetMapId() != leader->GetMapId())
                continue;
            out.push_back(m);
        }
        return out;
    }

    DcBreathSource::Member Describe(Player* p)
    {
        DcBreathSource::Member m;
        m.isBot = GET_PLAYERBOT_AI(p) != nullptr;
        m.alive = p->IsAlive();
        m.hasAura = p->HasWaterBreathingAura();
        if (m.isBot && m.alive)
        {
            m.canCastShamanBreath = p->getClass() == CLASS_SHAMAN &&
                                    p->HasSpell(DcBreathSource::SPELL_WATER_BREATHING);
            m.canCastWarlockBreath = p->getClass() == CLASS_WARLOCK &&
                                     p->HasSpell(DcBreathSource::SPELL_UNENDING_BREATH);
        }
        return m;
    }

    // Druids never swim this leg in Aquatic Form: it breathes, but a bear tank
    // or a resto healer in it cannot do its job. Shift out, then buff like
    // everyone else.
    void DropAquaticForm(Player* p)
    {
        if (p->GetShapeshiftForm() != FORM_AQUA)
            return;
        p->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] water breathing: shifted out of Aquatic Form for the leg (buff instead)",
                 p->GetName());
    }

    bool DrinkElixir(Player* p)
    {
        Item* elixir = p->GetItemByEntry(DcBreathSource::ITEM_ELIXIR_OF_WATER_BREATHING);
        if (!elixir)
        {
            p->AddItem(DcBreathSource::ITEM_ELIXIR_OF_WATER_BREATHING, 1);
            elixir = p->GetItemByEntry(DcBreathSource::ITEM_ELIXIR_OF_WATER_BREATHING);
            if (!elixir)
                return false;  // bags full this tick — retry
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] water breathing: granted an Elixir of Water Breathing (no shaman or "
                     "warlock to buff)",
                     p->GetName());
        }
        if (p->HasSpellCooldown(DcBreathSource::SPELL_ELIXIR_OF_WATER_BREATHING))
            return false;
        DcFormGate::DropBlockingForm(p, elixir);
        SpellCastTargets targets;
        targets.SetUnitTarget(p);
        p->CastItemUseSpell(elixir, targets, 0, 0);
        return true;
    }

    std::string JoinNames(std::vector<Player*> const& v)
    {
        std::string s;
        for (Player* p : v)
        {
            if (!s.empty())
                s += ", ";
            s += p->GetName();
        }
        return s;
    }
}

StepResult DcBreathProvision::Tick(Player* leader, DungeonEventProgress& prog, uint32 nowMs)
{
    if (!leader)
        return StepResult::Blocked;

    std::vector<Player*> const party = PartyOnMap(leader);
    std::vector<DcBreathSource::Member> members;
    members.reserve(party.size());
    for (Player* p : party)
        members.push_back(Describe(p));
    std::vector<DcBreathSource::Assignment> const plan = DcBreathSource::Plan(members);

    if (DcBreathSource::AllBotsCovered(plan))
    {
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] water breathing: every bot carries a water-breathing aura", leader->GetName());
        return StepResult::Done;
    }

    // Humans: logged ONCE per step (attempts doubles as the latch), never buffed.
    if (prog.attempts == 0)
    {
        prog.attempts = 1;
        std::vector<Player*> humans;
        for (DcBreathSource::Assignment const& a : plan)
            if (a.kind == DcBreathSource::Kind::Human)
                humans.push_back(party[a.target]);
        if (!humans.empty())
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] water breathing: unbuffed human(s) in the water leg — {} (bots never "
                     "buff, feed or shapeshift a player)",
                     leader->GetName(), JoinNames(humans));
    }

    if (nowMs - prog.stepStartMs >= DC_BREATH_PROVISION_TIMEOUT_MS)
    {
        std::vector<Player*> uncovered;
        for (DcBreathSource::Assignment const& a : plan)
            if (a.kind == DcBreathSource::Kind::Shaman || a.kind == DcBreathSource::Kind::Warlock ||
                a.kind == DcBreathSource::Kind::Elixir)
                uncovered.push_back(party[a.target]);
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] water breathing: {}s budget spent with {} still uncovered — continuing "
                 "(the breath watchdog remains)",
                 leader->GetName(), DC_BREATH_PROVISION_TIMEOUT_MS / 1000, JoinNames(uncovered));
        return StepResult::Done;
    }

    // One cast per caster per tick; the elixir route is per member.
    std::unordered_set<int> castersUsed;
    for (DcBreathSource::Assignment const& a : plan)
    {
        Player* target = party[a.target];
        switch (a.kind)
        {
            case DcBreathSource::Kind::Shaman:
            case DcBreathSource::Kind::Warlock:
            {
                DropAquaticForm(target);
                if (castersUsed.count(a.caster))
                    break;
                Player* caster = party[a.caster];
                PlayerbotAI* casterAI = GET_PLAYERBOT_AI(caster);
                if (!casterAI)
                    break;
                uint32 const spellId = a.kind == DcBreathSource::Kind::Shaman
                                           ? DcBreathSource::SPELL_WATER_BREATHING
                                           : DcBreathSource::SPELL_UNENDING_BREATH;
                if (casterAI->CanCastSpell(spellId, target, /*checkHasSpell*/ true) &&
                    casterAI->CastSpell(spellId, target))
                {
                    castersUsed.insert(a.caster);
                    LOG_INFO("playerbots.dungeonclear",
                             "[DC:{}] water breathing: {} casts {} on {}",
                             leader->GetName(), caster->GetName(),
                             spellId == DcBreathSource::SPELL_WATER_BREATHING ? "Water Breathing"
                                                                              : "Unending Breath",
                             target->GetName());
                }
                break;
            }
            case DcBreathSource::Kind::Elixir:
                DropAquaticForm(target);
                DrinkElixir(target);
                break;
            default:
                break;
        }
    }
    return StepResult::Running;
}

bool DcBreathProvision::AnyBotLacksBreath(Player* bot)
{
    if (!bot)
        return false;
    for (Player* p : PartyOnMap(bot))
        if (GET_PLAYERBOT_AI(p) && p->IsAlive() && !p->HasWaterBreathingAura())
            return true;
    return false;
}

void DcBreathProvision::WatchdogTick(Player* bot)
{
    if (!bot || !DcLeaderSignal::IsDungeonClearLeader(bot))
        return;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai)
        return;
    DcRunState& run = DcRun::Of(ai);
    if (!run.enabled || run.paused)
        return;

    uint32 const now = getMSTime();
    uint32 moved = 0;
    for (Player* m : PartyOnMap(bot))
    {
        uint64 const key = m->GetGUID().GetRawValue();
        if (!m->IsAlive() || !m->IsUnderWater())
        {
            run.underwaterSinceMs.erase(key);
            continue;
        }
        auto it = run.underwaterSinceMs.find(key);
        if (it == run.underwaterSinceMs.end())
            it = run.underwaterSinceMs.emplace(key, now).first;
        uint32 const uwMs = now - it->second;

        if (!GET_PLAYERBOT_AI(m) || m == bot)
            continue;  // humans: stamped for the diag, never moved; the tank is the anchor

        switch (DcBreathSource::DecideUnderwater(uwMs, DcPartyWideChain::IsOnSwimLeg(m), m->IsInCombat()))
        {
            case DcBreathSource::Verdict::None:
                break;
            case DcBreathSource::Verdict::LogOnly:
                if ((uwMs / 1000) % 10 == 0 && (uwMs % 1000) < 300)
                    LOG_INFO("playerbots.dungeonclear",
                             "[DC:{}] breath watchdog: {} has been under water {}s in combat — "
                             "staying in the fight",
                             bot->GetName(), m->GetName(), uwMs / 1000);
                break;
            case DcBreathSource::Verdict::Rescue:
            {
                if (m->GetVehicle())
                    break;
                float const angle = bot->GetOrientation() + static_cast<float>(moved) * 0.7f;
                float const off = 1.5f + 0.5f * static_cast<float>(moved);
                float const tx = bot->GetPositionX() + std::cos(angle) * off;
                float const ty = bot->GetPositionY() + std::sin(angle) * off;
                m->GetMotionMaster()->Clear();
                m->NearTeleportTo(tx, ty, bot->GetPositionZ(), m->GetOrientation(),
                                  /*casting*/ false, /*vehicle*/ false, /*withPet*/ true);
                ++moved;
                run.underwaterSinceMs.erase(key);
                LOG_INFO("playerbots.dungeonclear",
                         "[DC:{}] breath watchdog: {} under water {}s with no swim leg driving it -> "
                         "teleported to the tank",
                         bot->GetName(), m->GetName(), uwMs / 1000);
                break;
            }
        }
    }
}

uint32 DcBreathProvision::UnderwaterSeconds(Player* member, uint32 nowMs)
{
    if (!member)
        return 0;
    Player* owner = DcLeaderSignal::FindRunOwner(member);
    PlayerbotAI* ai = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    if (!ai)
        return 0;
    DcRunState const& run = DcRun::Of(ai);
    auto it = run.underwaterSinceMs.find(member->GetGUID().GetRawValue());
    if (it == run.underwaterSinceMs.end())
        return 0;
    return (nowMs - it->second) / 1000;
}
