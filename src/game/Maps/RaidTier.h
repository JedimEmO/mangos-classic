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

#ifndef MANGOS_RAIDTIER_H
#define MANGOS_RAIDTIER_H

#include "Common.h"

class Creature;
class Loot;
class LootTemplate;
class Player;
struct MapEntry;

/*
 * Raid tiers. Every raid instance has a tier, 1 or higher, fixed when the instance is created:
 * the tier the creator's guild (its account) chose for that raid in guildd_raid_tier, or 1.
 * Tier 1 is the raid tuned for fifteen players; each tier above adds a step of health and damage,
 * and gives bosses a chance at more loot. The multipliers are Raid.Tier.* in mangosd.conf;
 * the raids for twenty (Zul'Gurub, the Ruins of Ahn'Qiraj) have a milder tier 1 of their own.
 * A map that is not a raid has tier 0, which changes nothing.
 */
namespace RaidTier
{
    // Creates guildbot_instance_tier in the characters database if it is missing, and drops the rows of
    // instances that no longer exist. At startup, before the instances are packed.
    void LoadAtStartup();

    // The tier of a raid instance about to be created; 0 if the feature is off or the map is not a raid.
    // A known instance (one with a saved state) keeps the tier recorded for it; any other takes the tier
    // the creator's guild chose, and the tier is recorded in guildbot_instance_tier.
    uint8 SelectForInstance(MapEntry const* mapEntry, uint32 instanceId, Player* creator, bool knownInstance);

    // What a creature's health and damage are multiplied by; 1 for a creature that serves a player
    // (a pet, guardian, totem or charmed creature) and for every creature off a tiered raid.
    float HealthMultiplier(Creature const* creature);
    float DamageMultiplier(Creature const* creature);

    // Extra draws from a boss's loot template, on a tier above 1. Called while the loot is filled.
    void AddExtraLoot(Loot& loot, LootTemplate const& lootTemplate, Player const* lootOwner, bool rate);

    // Records a clear in guildbot_raid_clear when the final boss of a tiered raid instance is killed.
    void OnCreatureKilled(Creature const* victim);
}

#endif
