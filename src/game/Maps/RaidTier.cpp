/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Maps/RaidTier.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Groups/Group.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "Maps/Map.h"
#include "Server/DBCStructure.h"
#include "Server/WorldSession.h"
#include "Util/Util.h"
#include "World/World.h"

#include <algorithm>

namespace
{
    // The tier a creature fights at: its map's, unless it serves a player.
    // At creation a summon's owner may not be set yet; pets and guardians have theirs, totems are known by their kind.
    uint8 TierOf(Creature const* creature)
    {
        if (!sWorld.getConfig(CONFIG_BOOL_RAID_TIER_ENABLE))
            return 0;

        if (creature->IsTotem() || creature->IsPlayerControlled() || creature->GetMasterGuid().IsPlayer())
            return 0;

        return creature->GetMap()->GetRaidTier();
    }

    // AtTier1 x (1 + Step x (tier - 1))
    float Multiplier(eConfigFloatValues atTier1, uint8 tier)
    {
        float const step = sWorld.getConfig(CONFIG_FLOAT_RAID_TIER_STEP_PER_TIER);
        return sWorld.getConfig(atTier1) * (1.0f + step * float(tier - 1));
    }

    // The guild that opens an instance: the account of the creator's group leader, or of the creator.
    uint32 AccountOf(Player* creator)
    {
        if (Group* group = creator->GetGroup())
            if (uint32 account = sObjectMgr.GetPlayerAccountIdByGUID(group->GetLeaderGuid()))
                return account;

        return creator->GetSession()->GetAccountId();
    }

    uint8 ReadTier(QueryResult* result)
    {
        return std::max<uint8>(1, result->Fetch()[0].GetUInt8());
    }
}

void RaidTier::LoadAtStartup()
{
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_instance_tier ("
        " instance INT UNSIGNED NOT NULL PRIMARY KEY,"
        " map SMALLINT UNSIGNED NOT NULL,"
        " tier TINYINT UNSIGNED NOT NULL,"
        " account INT UNSIGNED NOT NULL,"
        " created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // Instance ids are renumbered at startup and reused after a restart, so a row outlives no instance.
    CharacterDatabase.DirectExecute("DELETE FROM guildbot_instance_tier WHERE instance NOT IN (SELECT id FROM instance)");
}

uint8 RaidTier::SelectForInstance(MapEntry const* mapEntry, uint32 instanceId, Player* creator, bool knownInstance)
{
    if (!sWorld.getConfig(CONFIG_BOOL_RAID_TIER_ENABLE) || !mapEntry || !mapEntry->IsRaid() || !creator)
        return 0;

    // An instance loaded again, after it was unloaded or the server restarted, keeps its tier.
    if (knownInstance)
        if (auto result = CharacterDatabase.PQuery("SELECT tier FROM guildbot_instance_tier WHERE instance = '%u' AND map = '%u'", instanceId, mapEntry->MapID))
            return ReadTier(result.get());

    uint32 const account = AccountOf(creator);
    uint8 tier = 1;
    if (auto result = CharacterDatabase.PQuery("SELECT tier FROM guildd_raid_tier WHERE account = '%u' AND map = '%u'", account, mapEntry->MapID))
        tier = ReadTier(result.get());

    CharacterDatabase.PExecute("REPLACE INTO guildbot_instance_tier (instance, map, tier, account) VALUES ('%u', '%u', '%u', '%u')",
                               instanceId, mapEntry->MapID, uint32(tier), account);

    sLog.outString("Raid tier: instance %u of map %u opens at tier %u for account %u (entered by %s)",
                   instanceId, mapEntry->MapID, uint32(tier), account, creator->GetName());
    return tier;
}

float RaidTier::HealthMultiplier(Creature const* creature)
{
    uint8 const tier = TierOf(creature);
    return tier ? Multiplier(CONFIG_FLOAT_RAID_TIER_HEALTH_AT_TIER1, tier) : 1.0f;
}

float RaidTier::DamageMultiplier(Creature const* creature)
{
    uint8 const tier = TierOf(creature);
    return tier ? Multiplier(CONFIG_FLOAT_RAID_TIER_DAMAGE_AT_TIER1, tier) : 1.0f;
}

void RaidTier::AddExtraLoot(Loot& loot, LootTemplate const& lootTemplate, Player const* lootOwner, bool rate)
{
    WorldObject const* target = loot.GetLootTarget();
    if (!target || target->GetTypeId() != TYPEID_UNIT)
        return;

    Creature const* creature = static_cast<Creature const*>(target);
    uint8 const tier = TierOf(creature);
    if (tier <= 1)
        return;

    if (!creature->IsWorldBoss() && !sObjectMgr.IsEncounter(creature->GetEntry(), creature->GetMapId()))
        return;

    // ExtraLootPerTier x (tier - 1) extra draws on average: the whole part always, the rest by chance.
    float const expected = sWorld.getConfig(CONFIG_FLOAT_RAID_TIER_EXTRA_LOOT_PER_TIER) * float(tier - 1);
    uint32 const most = sWorld.getConfig(CONFIG_UINT32_RAID_TIER_EXTRA_LOOT_MAX);
    uint32 draws = uint32(std::min(expected, float(most)));
    if (draws < most && roll_chance_f((expected - float(draws)) * 100.0f))
        ++draws;

    for (uint32 i = 0; i < draws; ++i)
        lootTemplate.ProcessExtraDraw(loot, lootOwner, rate);
}
