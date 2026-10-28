/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCBREATHSOURCE_H
#define _PLAYERBOT_DCBREATHSOURCE_H

#include <cstdint>
#include <vector>

// PURE planning for the EnsureWaterBreathing event step: which source gives
// each party member its water-breathing aura. The order is the plan's priority
// table — a shaman's Water Breathing (131), a warlock's Unending Breath (5697),
// else an Elixir of Water Breathing (item 5996, spell 7178) the member drinks
// itself — and the rules around it: a member that already carries the aura
// needs nothing, a HUMAN is never buffed, fed or shapeshifted (logged once so a
// streamer sees why they drowned), the dead belong to the rez flow. No game
// state in here; DcBreathProvision reads the party into `Member`s and acts on
// the plan. Unit-tested for the four party shapes (t/TestBlackfathomDeeps.cpp).
namespace DcBreathSource
{
    constexpr std::uint32_t SPELL_WATER_BREATHING = 131;    // shaman, level 22
    constexpr std::uint32_t SPELL_UNENDING_BREATH = 5697;   // warlock, level 16
    constexpr std::uint32_t ITEM_ELIXIR_OF_WATER_BREATHING = 5996;
    constexpr std::uint32_t SPELL_ELIXIR_OF_WATER_BREATHING = 7178;

    enum class Kind : std::uint8_t
    {
        Covered,   // already has the aura — nothing to do
        Human,     // a real player — never touched, logged once
        Dead,      // the rez flow's job
        Shaman,    // caster casts 131 on the member
        Warlock,   // caster casts 5697 on the member
        Elixir,    // the member drinks item 5996 (granted if missing)
    };

    struct Member
    {
        bool isBot = false;
        bool alive = false;
        bool hasAura = false;          // Unit::HasWaterBreathingAura
        bool canCastShamanBreath = false;   // a shaman bot that knows spell 131
        bool canCastWarlockBreath = false;  // a warlock bot that knows spell 5697
    };

    struct Assignment
    {
        int target = -1;               // index into the members
        Kind kind = Kind::Covered;
        int caster = -1;               // Shaman/Warlock: index of the caster
    };

    // One assignment per member, in member order. A caster can cover others
    // and itself alike; the first eligible shaman wins, then the first warlock.
    inline std::vector<Assignment> Plan(std::vector<Member> const& members)
    {
        int shaman = -1;
        int warlock = -1;
        for (std::size_t i = 0; i < members.size(); ++i)
        {
            Member const& m = members[i];
            if (!m.isBot || !m.alive)
                continue;
            if (shaman < 0 && m.canCastShamanBreath)
                shaman = static_cast<int>(i);
            if (warlock < 0 && m.canCastWarlockBreath)
                warlock = static_cast<int>(i);
        }

        std::vector<Assignment> out;
        out.reserve(members.size());
        for (std::size_t i = 0; i < members.size(); ++i)
        {
            Member const& m = members[i];
            Assignment a;
            a.target = static_cast<int>(i);
            if (!m.alive)
                a.kind = Kind::Dead;
            else if (m.hasAura)
                a.kind = Kind::Covered;
            else if (!m.isBot)
                a.kind = Kind::Human;
            else if (shaman >= 0)
            {
                a.kind = Kind::Shaman;
                a.caster = shaman;
            }
            else if (warlock >= 0)
            {
                a.kind = Kind::Warlock;
                a.caster = warlock;
            }
            else
                a.kind = Kind::Elixir;
            out.push_back(a);
        }
        return out;
    }

    // True when no bot still needs a buff — the step's completion test.
    inline bool AllBotsCovered(std::vector<Assignment> const& plan)
    {
        for (Assignment const& a : plan)
            if (a.kind == Kind::Shaman || a.kind == Kind::Warlock || a.kind == Kind::Elixir)
                return false;
        return true;
    }

    // PURE breath WATCHDOG rule (the backstop under the provisioning): what to
    // do about a bot that has been under water for `uwMs`.
    //   * Not on an authored swim leg and not in combat, 20s under => it is
    //     stranded (a follower that missed a surface, a bot that sank after a
    //     teleport): rescue it to the tank, who outside a leg stands on a
    //     surface or dry island.
    //   * 150s of the 180s breath timer, whatever it is doing: rescue if out of
    //     combat; in combat only log (surfacing mid-boss is worse) and keep
    //     fighting.
    enum class Verdict : std::uint8_t
    {
        None,
        Rescue,
        LogOnly,
    };

    constexpr std::uint32_t UW_STRANDED_MS = 20000;
    constexpr std::uint32_t UW_BREATH_CRITICAL_MS = 150000;

    inline Verdict DecideUnderwater(std::uint32_t uwMs, bool onSwimLeg, bool inCombat)
    {
        if (uwMs >= UW_BREATH_CRITICAL_MS)
            return inCombat ? Verdict::LogOnly : Verdict::Rescue;
        if (uwMs >= UW_STRANDED_MS && !onSwimLeg && !inCombat)
            return Verdict::Rescue;
        return Verdict::None;
    }
}

#endif  // _PLAYERBOT_DCBREATHSOURCE_H
