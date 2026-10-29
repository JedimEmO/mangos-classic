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

#ifndef MANGOS_GUILDVAULT_H
#define MANGOS_GUILDVAULT_H

#include "Common.h"
#include "Entities/Item.h"
#include "Entities/ObjectGuid.h"

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class Creature;
class Player;

/*
 * The guild vault: one stockpile per guild of the competition, kept by a vaultkeeper beside the Guild Master
 * in every capital, the same vault at each, as a bank is. VAULT.md in the repository root describes it.
 *
 * A vault belongs to a guild of the guild service (guildd), named by its guild key; the core learns an account's
 * guild from guildd_account in the characters database, which the service writes. Every character of the guild's
 * accounts shares the vault.
 *
 * The vault holds items as counts by item entry, so it takes only items that are like any other of their kind:
 * not soulbound or bound on pickup, not a quest item, no enchantment, no random properties, full durability and
 * charges, not conjured or expiring, not wrapped, written on or holding loot, and no bag with anything in it.
 * It holds at most MAX_KINDS different items, what the vendor window can show.
 *
 * The vault also holds the guild's gold, as one balance of copper. A character puts in or takes out what it carries:
 * a player through the vaultkeeper's gossip, a bot through the engine's jobs.
 *
 * The vault in memory is the truth: it is loaded at startup and changed under one mutex. Each deposit and withdrawal
 * changes the player's inventory and the vault in memory, then queues one transaction on the characters database's
 * write queue that saves the player's inventory, writes the vault's new count of that item and logs the change.
 * Nothing creates an item without taking it from the vault, and nothing takes from the vault without storing the item.
 *
 * All functions are safe to call from any map thread. The player must be in the world and handled by the calling thread.
 */
class GuildVault
{
    public:
        // creature_template entry of the vaultkeeper (server/sql/031-vault.sql in the main repository)
        static uint32 const VAULTKEEPER_ENTRY = 200101;
        // Different items one vault may hold: the 1.12 client keeps a vendor's list in an array of 128
        static uint32 const MAX_KINDS = 128;

        static GuildVault& Instance();

        // Creates guildbot_vault, guildbot_vault_gold and guildbot_vault_log in the characters database if they are missing,
        // and loads the vaults.
        void LoadAtStartup();

        static bool IsVaultkeeper(Creature const* creature);

        // The guild key of an account, from guildd_account; empty when the account has no guild.
        std::string GuildOf(uint32 accountId) const;

        // How many of an item the guild's vault holds.
        uint32 Count(std::string const& guild, uint32 item) const;

        // Everything in the guild's vault as (item entry, count), by entry.
        std::vector<std::pair<uint32, uint32>> Contents(std::string const& guild) const;

        // How much gold the guild's vault holds, in copper.
        uint64 Gold(std::string const& guild) const;

        // Puts copper of the player's gold in the vault of the player's guild; false with a reason when the player has
        // less or the guild no vault.
        bool DepositGold(Player* player, uint32 copper, std::string& reason);

        // Takes up to copper from the vault of the player's guild into the player's purse, as much as the vault holds
        // and the purse takes; returns how much, 0 with a reason when none.
        uint32 WithdrawGold(Player* player, uint32 copper, std::string& reason);

        // Whether the item could go in a vault, by its kind and state alone; reason says why not.
        // Deposit checks this and more (the owner, where the item is, the guild and the room in the vault).
        static bool IsFungible(Item const* item, std::string& reason);

        // Takes up to count of an item from the vault of the player's guild into the player's bags, as much as
        // the vault holds and the bags take; returns how many, 0 with a reason when none.
        // bagError, when given, gets the inventory error when the bags have no room (EQUIP_ERR_OK otherwise).
        uint32 Withdraw(Player* player, uint32 item, uint32 count, std::string& reason, InventoryResult* bagError = nullptr);

        // Puts count of the item (0 for the whole stack) from the player's backpack or bags in the vault of the
        // player's guild; false with a reason when nothing moved. On success the item may be destroyed: do not use it after.
        bool Deposit(Player* player, Item* item, uint32 count, std::string& reason);

        // The vendor window of a vaultkeeper, from WorldSession's handlers. Each returns false, doing nothing,
        // when the creature is not a vaultkeeper or the player is a bot, so the caller goes on as for any vendor.
        bool SendList(Player* player, Creature* creature);
        bool HandleBuy(Player* player, ObjectGuid vendorGuid, uint32 item, uint32 count);
        bool HandleSell(Player* player, Creature* creature, ObjectGuid itemGuid, uint32 count);

    private:
        GuildVault() = default;

        typedef std::map<uint32, uint32> Stock;             // item entry -> count

        // Queues the transaction of one change: the player's inventory, the vault's count of the item, the log
        void Save(Player* player, std::string const& guild, uint32 item, uint32 newCount, int32 delta) const;
        // The same for the gold: the player's gold, the vault's balance, the log with item 0
        void SaveGold(Player* player, std::string const& guild, uint64 newCopper, int32 delta) const;

        mutable std::mutex m_mutex;
        std::map<std::string, Stock> m_vaults;              // by guildd's guild key
        std::map<std::string, uint64> m_gold;               // copper, by guildd's guild key
        std::map<uint32, std::vector<uint32>> m_shown;      // the list last sent to a player, by guid low
};

#define sGuildVault GuildVault::Instance()

#endif
