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

#include "Guilds/GuildVault.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Bag.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"

#include <algorithm>
#include <cstring>

namespace
{
    // A vault's count of one item stays well inside a signed 32-bit change
    uint32 const MAX_COUNT = 0x7FFFFFFF;

    char const* const MSG_NO_VAULT = "You have no guild vault.";
    char const* const MSG_NONE_LEFT = "The vault has none of those.";

    // Bots use the vault through the engine's jobs; at a vaultkeeper they find an ordinary merchant
    bool IsRealPlayer(Player const* player)
    {
#ifdef ENABLE_PLAYERBOTS
        return player->isRealPlayer();
#else
        (void)player;
        return true;
#endif
    }

    bool Refuse(std::string& reason, char const* message)
    {
        reason = message;
        return false;
    }

    uint32 GiveNone(std::string& reason, char const* message)
    {
        reason = message;
        return 0;
    }
}

GuildVault& GuildVault::Instance()
{
    static GuildVault instance;
    return instance;
}

void GuildVault::LoadAtStartup()
{
    // The guild service reads both tables; the core alone writes them. Guild keys compare as bytes, as in memory.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_vault ("
        " guild VARCHAR(255) NOT NULL,"
        " item INT UNSIGNED NOT NULL,"
        " count INT UNSIGNED NOT NULL,"
        " PRIMARY KEY (guild, item)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin");

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_vault_gold ("
        " guild VARCHAR(255) NOT NULL PRIMARY KEY,"
        " copper BIGINT UNSIGNED NOT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin");

    // item 0 is the gold, with delta in copper
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_vault_log ("
        " id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        " guild VARCHAR(255) NOT NULL,"
        " at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " guid INT UNSIGNED NOT NULL,"
        " name VARCHAR(12) NOT NULL,"
        " item INT UNSIGNED NOT NULL,"
        " delta INT NOT NULL,"
        " KEY (guild, at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_vaults.clear();
    m_gold.clear();

    uint32 rows = 0;
    if (auto result = CharacterDatabase.Query("SELECT guild, item, count FROM guildbot_vault WHERE count > 0"))
    {
        do
        {
            Field const* fields = result->Fetch();
            std::string const guild = fields[0].GetCppString();
            uint32 const item = fields[1].GetUInt32();
            uint32 const count = std::min(fields[2].GetUInt32(), MAX_COUNT);
            if (!ObjectMgr::GetItemPrototype(item))
                sLog.outError("Guild vault: guild %s holds %u of item %u, which does not exist; it is kept but not shown", guild.c_str(), count, item);
            m_vaults[guild][item] = count;
            ++rows;
        }
        while (result->NextRow());
    }

    if (auto result = CharacterDatabase.Query("SELECT guild, copper FROM guildbot_vault_gold WHERE copper > 0"))
    {
        do
        {
            Field const* fields = result->Fetch();
            m_gold[fields[0].GetCppString()] = fields[1].GetUInt64();
        }
        while (result->NextRow());
    }

    sLog.outString(">> Loaded %u kinds of items in %u guild vaults, and gold in %u", rows, uint32(m_vaults.size()), uint32(m_gold.size()));
    sLog.outString();
}

bool GuildVault::IsVaultkeeper(Creature const* creature)
{
    return creature && creature->GetEntry() == VAULTKEEPER_ENTRY;
}

std::string GuildVault::GuildOf(uint32 accountId) const
{
    auto result = CharacterDatabase.PQuery("SELECT guild FROM guildd_account WHERE account = '%u'", accountId);
    if (!result)
        return std::string();
    return result->Fetch()[0].GetCppString();
}

uint32 GuildVault::Count(std::string const& guild, uint32 item) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto vault = m_vaults.find(guild);
    if (vault == m_vaults.end())
        return 0;
    auto stock = vault->second.find(item);
    return stock == vault->second.end() ? 0 : stock->second;
}

std::vector<std::pair<uint32, uint32>> GuildVault::Contents(std::string const& guild) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::pair<uint32, uint32>> contents;
    auto vault = m_vaults.find(guild);
    if (vault != m_vaults.end())
        contents.assign(vault->second.begin(), vault->second.end());
    return contents;
}

uint64 GuildVault::Gold(std::string const& guild) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto gold = m_gold.find(guild);
    return gold == m_gold.end() ? 0 : gold->second;
}

bool GuildVault::DepositGold(Player* player, uint32 copper, std::string& reason)
{
    if (!player || !player->IsInWorld())
        return Refuse(reason, "Only a character in the world can put in the vault.");

    if (!copper)
        return Refuse(reason, "Nothing was put in.");

    // Below MAX_MONEY_AMOUNT, so the change stays inside a signed 32-bit delta
    if (copper > uint32(MAX_MONEY_AMOUNT) || player->GetMoney() < copper)
        return Refuse(reason, "You do not have that much gold.");

    std::string const guild = GuildOf(player->GetSession()->GetAccountId());
    if (guild.empty())
        return Refuse(reason, MSG_NO_VAULT);

    std::lock_guard<std::mutex> lock(m_mutex);

    uint64& held = m_gold[guild];
    held += copper;
    player->ModifyMoney(-int32(copper));

    SaveGold(player, guild, held, int32(copper));
    DETAIL_LOG("Guild vault: %s (guid %u) put %u copper in the vault of guild %s, " UI64FMTD " held",
               player->GetName(), player->GetGUIDLow(), copper, guild.c_str(), held);
    return true;
}

uint32 GuildVault::WithdrawGold(Player* player, uint32 copper, std::string& reason)
{
    if (!player || !player->IsInWorld())
        return GiveNone(reason, "Only a character in the world can take from the vault.");

    if (!copper)
        return GiveNone(reason, "Nothing was asked for.");

    std::string const guild = GuildOf(player->GetSession()->GetAccountId());
    if (guild.empty())
        return GiveNone(reason, MSG_NO_VAULT);

    // Held from reading the balance until the change is queued, as for items
    std::lock_guard<std::mutex> lock(m_mutex);

    auto gold = m_gold.find(guild);
    if (gold == m_gold.end() || !gold->second)
        return GiveNone(reason, "The vault holds no gold.");

    uint32 const money = player->GetMoney();
    uint32 const room = money < uint32(MAX_MONEY_AMOUNT) ? uint32(MAX_MONEY_AMOUNT) - money : 0;
    if (!room)
        return GiveNone(reason, "You cannot carry more gold.");

    uint32 const taken = uint32(std::min<uint64>(std::min(copper, room), gold->second));
    uint64 const left = gold->second - taken;
    player->ModifyMoney(int32(taken));
    if (left)
        gold->second = left;
    else
        m_gold.erase(gold);

    SaveGold(player, guild, left, -int32(taken));
    DETAIL_LOG("Guild vault: %s (guid %u) took %u copper from the vault of guild %s, " UI64FMTD " left",
               player->GetName(), player->GetGUIDLow(), taken, guild.c_str(), left);
    return taken;
}

bool GuildVault::IsFungible(Item const* item, std::string& reason)
{
    ItemPrototype const* proto = item ? item->GetProto() : nullptr;
    if (!proto)
        return Refuse(reason, "That cannot go in the vault.");

    // A withdrawal makes a new item of the entry: whatever makes one item unlike another of its kind is refused
    if (item->IsSoulBound() || proto->Bonding == BIND_WHEN_PICKED_UP)
        return Refuse(reason, "Soulbound items cannot go in the vault.");

    if (proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1 || proto->StartQuest)
        return Refuse(reason, "Quest items cannot go in the vault.");

    if (proto->Flags & ITEM_FLAG_CONJURED)
        return Refuse(reason, "Conjured items cannot go in the vault.");

    if (proto->Duration)
        return Refuse(reason, "Items that expire cannot go in the vault.");

    // A new item of such an entry would roll new properties
    if (proto->RandomProperty || item->GetItemRandomPropertyId())
        return Refuse(reason, "Items with random properties cannot go in the vault.");

    for (uint32 slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
        if (item->GetEnchantmentId(EnchantmentSlot(slot)))
            return Refuse(reason, "Enchanted items cannot go in the vault.");

    if (item->GetUInt32Value(ITEM_FIELD_DURABILITY) < proto->MaxDurability)
        return Refuse(reason, "Damaged items cannot go in the vault.");

    for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (item->GetSpellCharges(uint8(i)) != proto->Spells[i].SpellCharges)
            return Refuse(reason, "Used items cannot go in the vault.");

    if (item->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_WRAPPED) || !item->GetGuidValue(ITEM_FIELD_GIFTCREATOR).IsEmpty())
        return Refuse(reason, "Wrapped gifts cannot go in the vault.");

    if (item->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_UNLOCKED) || item->HasGeneratedLoot() || item->HasSavedLoot())
        return Refuse(reason, "Opened items cannot go in the vault.");

    if (item->GetUInt32Value(ITEM_FIELD_ITEM_TEXT_ID))
        return Refuse(reason, "Written items cannot go in the vault.");

    if (item->IsBag() && !static_cast<Bag const*>(item)->IsEmpty())
        return Refuse(reason, "Bags with anything in them cannot go in the vault.");

    return true;
}

uint32 GuildVault::Withdraw(Player* player, uint32 item, uint32 count, std::string& reason, InventoryResult* bagError)
{
    if (bagError)
        *bagError = EQUIP_ERR_OK;

    if (!player || !player->IsInWorld())
        return GiveNone(reason, "Only a character in the world can take from the vault.");

    if (!count)
        return GiveNone(reason, "Nothing was asked for.");

    if (!ObjectMgr::GetItemPrototype(item))
        return GiveNone(reason, "There is no such item.");

    std::string const guild = GuildOf(player->GetSession()->GetAccountId());
    if (guild.empty())
        return GiveNone(reason, MSG_NO_VAULT);

    // Held from reading the count until the change is queued, so two characters of a guild,
    // on two maps at once, never both take the last of an item
    std::lock_guard<std::mutex> lock(m_mutex);

    auto vault = m_vaults.find(guild);
    if (vault == m_vaults.end())
        return GiveNone(reason, MSG_NONE_LEFT);
    auto stock = vault->second.find(item);
    if (stock == vault->second.end() || !stock->second)
        return GiveNone(reason, MSG_NONE_LEFT);

    uint32 const held = stock->second;
    uint32 taken = std::min(count, held);

    // As much as the bags take: CanStoreNewItem tells how many did not fit when some did
    ItemPosCountVec dest;
    uint32 noSpace = 0;
    InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, taken, &noSpace);
    if (msg != EQUIP_ERR_OK && noSpace && noSpace < taken)
    {
        taken -= noSpace;
        dest.clear();
        msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, taken);
    }
    if (msg != EQUIP_ERR_OK)
    {
        if (bagError)
            *bagError = msg;
        return GiveNone(reason, msg == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS ? "You cannot carry more of those." : "Your bags are full.");
    }

    Item* stored = player->StoreNewItem(dest, item, true);
    if (!stored)
        return GiveNone(reason, "The item could not be made.");
    player->SendNewItem(stored, taken, true, false);

    uint32 const left = held - taken;
    if (left)
        stock->second = left;
    else
    {
        vault->second.erase(stock);
        if (vault->second.empty())
            m_vaults.erase(vault);
    }

    Save(player, guild, item, left, -int32(taken));
    DETAIL_LOG("Guild vault: %s (guid %u) took %u of item %u from the vault of guild %s, %u left",
               player->GetName(), player->GetGUIDLow(), taken, item, guild.c_str(), left);
    return taken;
}

bool GuildVault::Deposit(Player* player, Item* item, uint32 count, std::string& reason)
{
    if (!player || !player->IsInWorld())
        return Refuse(reason, "Only a character in the world can put in the vault.");

    if (!item || item->GetOwnerGuid() != player->GetObjectGuid() || player->GetItemByGuid(item->GetObjectGuid()) != item)
        return Refuse(reason, "That is not yours to put in the vault.");

    // The backpack, the bags and the keyring; not what is worn, the bags themselves while worn, or the bank
    if (!Player::IsInventoryPos(item->GetBagSlot(), item->GetSlot()))
        return Refuse(reason, "Only what is in your bags can go in the vault.");

    if (item->IsInTrade() || player->GetLootGuid() == item->GetObjectGuid())
        return Refuse(reason, "That item is in use.");

    if (!IsFungible(item, reason))
        return false;

    if (!count)
        count = item->GetCount();
    if (count > item->GetCount())
        return Refuse(reason, "You do not have that many.");

    std::string const guild = GuildOf(player->GetSession()->GetAccountId());
    if (guild.empty())
        return Refuse(reason, MSG_NO_VAULT);

    uint32 const entry = item->GetEntry();

    std::lock_guard<std::mutex> lock(m_mutex);

    Stock& vault = m_vaults[guild];
    auto stock = vault.find(entry);
    if (stock == vault.end() && vault.size() >= MAX_KINDS)
        return Refuse(reason, "The vault holds as many kinds of items as it can.");

    uint32 const held = stock == vault.end() ? 0 : stock->second;
    if (held > MAX_COUNT - count)
        return Refuse(reason, "The vault cannot hold more of those.");

    // Every check passed: nothing below can fail. The item may be deleted once destroyed.
    player->InterruptNonMeleeSpells(false);
    uint32 remaining = count;
    player->DestroyItemCount(*item, remaining, true);
    item = nullptr;

    vault[entry] = held + count;

    Save(player, guild, entry, held + count, int32(count));
    DETAIL_LOG("Guild vault: %s (guid %u) put %u of item %u in the vault of guild %s, %u held",
               player->GetName(), player->GetGUIDLow(), count, entry, guild.c_str(), held + count);
    return true;
}

void GuildVault::Save(Player* player, std::string const& guild, uint32 item, uint32 newCount, int32 delta) const
{
    std::string guildKey = guild;
    std::string name = player->GetName();
    CharacterDatabase.escape_string(guildKey);
    CharacterDatabase.escape_string(name);

    // Queued while the vault's mutex is held, so the database's single write queue gets the changes in their order
    // in memory, each after any save of the player queued before it
    CharacterDatabase.BeginTransaction();
    player->SaveInventoryAndGoldToDB();
    if (newCount)
        CharacterDatabase.PExecute("REPLACE INTO guildbot_vault (guild, item, count) VALUES ('%s', '%u', '%u')",
                                   guildKey.c_str(), item, newCount);
    else
        CharacterDatabase.PExecute("DELETE FROM guildbot_vault WHERE guild = '%s' AND item = '%u'", guildKey.c_str(), item);
    CharacterDatabase.PExecute("INSERT INTO guildbot_vault_log (guild, guid, name, item, delta) VALUES ('%s', '%u', '%s', '%u', '%d')",
                               guildKey.c_str(), player->GetGUIDLow(), name.c_str(), item, delta);
    CharacterDatabase.CommitTransaction();
}

void GuildVault::SaveGold(Player* player, std::string const& guild, uint64 newCopper, int32 delta) const
{
    std::string guildKey = guild;
    std::string name = player->GetName();
    CharacterDatabase.escape_string(guildKey);
    CharacterDatabase.escape_string(name);

    // As Save: queued under the vault's mutex, after any save of the player queued before it
    CharacterDatabase.BeginTransaction();
    player->SaveInventoryAndGoldToDB();
    if (newCopper)
        CharacterDatabase.PExecute("REPLACE INTO guildbot_vault_gold (guild, copper) VALUES ('%s', '" UI64FMTD "')",
                                   guildKey.c_str(), newCopper);
    else
        CharacterDatabase.PExecute("DELETE FROM guildbot_vault_gold WHERE guild = '%s'", guildKey.c_str());
    CharacterDatabase.PExecute("INSERT INTO guildbot_vault_log (guild, guid, name, item, delta) VALUES ('%s', '%u', '%s', '0', '%d')",
                               guildKey.c_str(), player->GetGUIDLow(), name.c_str(), delta);
    CharacterDatabase.CommitTransaction();
}

bool GuildVault::SendList(Player* player, Creature* creature)
{
    if (!IsVaultkeeper(creature) || !IsRealPlayer(player))
        return false;

    std::string const guild = GuildOf(player->GetSession()->GetAccountId());
    if (guild.empty())
    {
        player->GetSession()->SendNotification("%s", MSG_NO_VAULT);
        return true;
    }

    // By kind, then name, as a merchant's goods are laid out
    std::vector<std::pair<ItemPrototype const*, uint32>> goods;
    for (auto const& stock : Contents(guild))
        if (ItemPrototype const* proto = ObjectMgr::GetItemPrototype(stock.first))
            goods.emplace_back(proto, stock.second);
    std::sort(goods.begin(), goods.end(), [](std::pair<ItemPrototype const*, uint32> const& a, std::pair<ItemPrototype const*, uint32> const& b)
    {
        if (a.first->Class != b.first->Class)
            return a.first->Class < b.first->Class;
        if (a.first->SubClass != b.first->SubClass)
            return a.first->SubClass < b.first->SubClass;
        int const byName = strcmp(a.first->Name1, b.first->Name1);
        return byName ? byName < 0 : a.first->ItemId < b.first->ItemId;
    });
    if (goods.size() > MAX_KINDS)
        goods.resize(MAX_KINDS);

    std::vector<uint32> shown;
    WorldPacket data(SMSG_LIST_INVENTORY, 8 + 1 + goods.size() * 7 * 4 + 1);
    data << creature->GetObjectGuid();
    data << uint8(goods.size());
    for (auto const& good : goods)
    {
        shown.push_back(good.first->ItemId);
        data << uint32(shown.size());                       // numbered from 1 at the client
        data << uint32(good.first->ItemId);
        data << uint32(good.first->DisplayInfoID);
        data << uint32(good.second);                        // shown as the merchant's stock
        data << uint32(0);                                  // the price
        data << uint32(good.first->MaxDurability);
        data << uint32(1);                                  // one at a time, whatever the item's vendor stack
    }
    if (goods.empty())
        data << uint8(0);                                   // opens the window empty, so the player can put things in

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shown[player->GetGUIDLow()] = std::move(shown);
    }
    player->GetSession()->SendPacket(data);
    return true;
}

bool GuildVault::HandleBuy(Player* player, ObjectGuid vendorGuid, uint32 item, uint32 count)
{
    Creature* creature = player->GetNPCIfCanInteractWith(vendorGuid, UNIT_NPC_FLAG_VENDOR);
    if (!IsVaultkeeper(creature) || !IsRealPlayer(player))
        return false;

    if (!player->IsAlive())
        return true;

    std::string reason;
    InventoryResult bagError = EQUIP_ERR_OK;
    uint32 const taken = Withdraw(player, item, std::max<uint32>(count, 1), reason, &bagError);
    if (!taken)
    {
        if (bagError != EQUIP_ERR_OK)
            player->SendEquipError(bagError, nullptr, nullptr, 0, item);
        else if (reason == MSG_NONE_LEFT)
        {
            player->SendBuyError(BUY_ERR_ITEM_ALREADY_SOLD, creature, item, 0);
            SendList(player, creature);                     // someone else took the last of it
        }
        else
            player->GetSession()->SendNotification("%s", reason.c_str());
        return true;
    }

    // The new count at the item's place in the list the client shows
    uint32 slot = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<uint32> const& shown = m_shown[player->GetGUIDLow()];
        auto place = std::find(shown.begin(), shown.end(), item);
        if (place != shown.end())
            slot = uint32(place - shown.begin()) + 1;
    }
    if (!slot)
        return SendList(player, creature);

    WorldPacket data(SMSG_BUY_ITEM, 8 + 4 + 4 + 4);
    data << creature->GetObjectGuid();
    data << uint32(slot);
    data << uint32(Count(GuildOf(player->GetSession()->GetAccountId()), item));
    data << uint32(taken);
    player->GetSession()->SendPacket(data);
    return true;
}

bool GuildVault::HandleSell(Player* player, Creature* creature, ObjectGuid itemGuid, uint32 count)
{
    if (!IsVaultkeeper(creature) || !IsRealPlayer(player))
        return false;

    Item* item = player->GetItemByGuid(itemGuid);
    if (!item)
    {
        player->SendSellError(SELL_ERR_CANT_FIND_ITEM, creature, itemGuid, 0);
        return true;
    }

    std::string reason;
    if (!Deposit(player, item, count, reason))
    {
        // The client's own message would be "the merchant doesn't want that"; this error shows nothing and frees the item
        player->SendSellError(SELL_ERR_UNK, creature, itemGuid, 0);
        player->GetSession()->SendNotification("%s", reason.c_str());
        return true;
    }

    // The list again, with what was put in
    SendList(player, creature);
    return true;
}
