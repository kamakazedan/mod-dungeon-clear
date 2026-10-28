/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BossSpawnIndex.h"

#include <algorithm>

#include "CreatureData.h"
#include "CreatureSpawnEntry.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "ObjectMgr.h"

std::unordered_map<uint64, std::vector<DungeonBossInfo>> BossSpawnIndex::_store;
std::once_flag BossSpawnIndex::_once;

std::vector<DungeonBossInfo> const& BossSpawnIndex::Get(uint32 mapId, Difficulty difficulty)
{
    EnsureBuilt();
    static std::vector<DungeonBossInfo> const empty;
    auto it = _store.find(MakeKey(mapId, difficulty));
    // DungeonEncounter.dbc defines ICC and Ruby Sanctum encounters for 10N/25N
    // only, so their 10H/25H buckets are never built. Fall back to the same
    // raid size's normal bucket, as Map::UpdateEncounterState does. Dungeon
    // difficulties stop at 1, so only a raid heroic takes this branch.
    if (it == _store.end() && difficulty >= RAID_DIFFICULTY_10MAN_HEROIC)
        it = _store.find(MakeKey(mapId, Difficulty(difficulty - 2)));
    if (it == _store.end())
        return empty;
    return it->second;
}

void BossSpawnIndex::EnsureBuilt()
{
    std::call_once(_once, &BossSpawnIndex::Build);
}

void BossSpawnIndex::Build()
{
    // 1. Build creditEntry -> encounter list, keyed by (mapId, difficulty).
    //    Only kill-creature encounters are usable here.
    struct EncounterRow
    {
        uint32 mapId;
        Difficulty difficulty;
        uint32 encounterIndex;
        std::string name;
        uint32 creditEntry;
    };

    std::unordered_multimap<uint32, EncounterRow> byCreditEntry;

    for (uint32 i = 0; i < sDungeonEncounterStore.GetNumRows(); ++i)
    {
        DungeonEncounterEntry const* dbc = sDungeonEncounterStore.LookupEntry(i);
        if (!dbc)
            continue;

        DungeonEncounterList const* list =
            sObjectMgr->GetDungeonEncounterList(dbc->mapId, Difficulty(dbc->difficulty));
        if (!list)
            continue;

        for (DungeonEncounter const* enc : *list)
        {
            if (!enc || enc->dbcEntry != dbc)
                continue;
            if (enc->creditType != ENCOUNTER_CREDIT_KILL_CREATURE)
                continue;

            EncounterRow row;
            row.mapId = dbc->mapId;
            row.difficulty = Difficulty(dbc->difficulty);
            row.encounterIndex = dbc->encounterIndex;
            row.name = dbc->encounterName[0] ? dbc->encounterName[0] : "";
            row.creditEntry = enc->creditEntry;
            byCreditEntry.emplace(row.creditEntry, std::move(row));
        }
    }

    if (byCreditEntry.empty())
        return;

    // 2. Walk every creature spawn once. For each spawn whose entry matches a
    //    boss credit entry on its mapId, record it.
    CreatureDataContainer const& spawns = sObjectMgr->GetAllCreatureData();
    for (auto const& kv : spawns)
    {
        CreatureData const& data = kv.second;
        uint32 const entry = DungeonClear::SpawnEntry(data);
        if (!entry)
            continue;

        auto range = byCreditEntry.equal_range(entry);
        for (auto it = range.first; it != range.second; ++it)
        {
            EncounterRow const& row = it->second;
            if (row.mapId != data.mapid)
                continue;

            // A spawn belongs to a difficulty bucket only when its spawnMask
            // bit says it exists there. Most 5-man spawns are mask 3 (both);
            // a heroic-only placement (Shattered Halls' Porung, mask 2) must
            // not leak coords into the normal bucket, nor a normal-only spawn
            // into heroic. Mask 0 is treated as "no filter" (defensive — some
            // custom rows leave it unset).
            if (data.spawnMask && !(data.spawnMask & (1u << row.difficulty)))
                continue;

            DungeonBossInfo info;
            info.entry = entry;
            info.encounterIndex = row.encounterIndex;
            info.name = row.name;
            info.mapId = row.mapId;
            info.x = data.posX;
            info.y = data.posY;
            info.z = data.posZ;
            _store[MakeKey(row.mapId, row.difficulty)].push_back(info);
        }
    }

    // 3. Sort each bucket by encounter index; deduplicate by entry (keep the
    //    first spawn as the anchor — there is usually only one per dungeon).
    //    A second spawn of the same entry is NOT dropped on the floor: it is
    //    recorded on the kept row's `spawns` list, so a pooled boss (one of
    //    several spawn points rolled per instance) can be resolved to the point
    //    the live creature actually stands at (DungeonBossInfo::spawns,
    //    DungeonBossesValue). The list carries the first spawn too, so it is
    //    the complete set whenever it is non-empty.
    for (auto& kv : _store)
    {
        auto& v = kv.second;
        std::stable_sort(v.begin(), v.end(),
                         [](DungeonBossInfo const& a, DungeonBossInfo const& b)
                         { return a.encounterIndex < b.encounterIndex; });

        std::vector<DungeonBossInfo> deduped;
        deduped.reserve(v.size());
        for (auto const& info : v)
        {
            DungeonBossInfo* kept = nullptr;
            for (auto& k : deduped)
            {
                if (k.entry == info.entry)
                {
                    kept = &k;
                    break;
                }
            }
            if (!kept)
            {
                deduped.push_back(info);
                continue;
            }
            if (kept->spawns.empty())
                kept->spawns.push_back({kept->x, kept->y, kept->z});
            kept->spawns.push_back({info.x, info.y, info.z});
        }
        v = std::move(deduped);
    }
}
