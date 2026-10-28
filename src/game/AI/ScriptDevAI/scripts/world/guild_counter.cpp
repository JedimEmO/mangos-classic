/* This file is part of the ScriptDev2 Project. See AUTHORS file for Copyright information
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

/* ScriptData
SDName: Guild_Counter
SD%Complete: 100
SDComment: The counters where a player buys their guild's upgrades in person: the capitals' Guild Masters
           (slots, features, order budget) and the emissaries of the Bronze Flight (raid lockout resets).
           Offers come from the guild service's guildd_price; a purchase paid here goes into guildd_purchase.
           COUNTERS.md describes them.
SDCategory: NPCs
EndScriptData */

#include "AI/ScriptDevAI/include/sc_common.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Bag.h"
#include "Globals/ObjectMgr.h"

#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    enum
    {
        // npc_text rows of server/sql/030-counters.sql in the main repository
        TEXT_ID_EMISSARY_GREETING       = 200100,
        TEXT_ID_EMISSARY_NOTHING        = 200101,
        TEXT_ID_EMISSARY_UNDER_WAY      = 200102,
        TEXT_ID_GUILD_MASTER_CONFIRM    = 200103,
        TEXT_ID_EMISSARY_CONFIRM        = 200104,

        // The core's own options from gossip_menu_option come with sender 0
        SENDER_CORE                     = 0,
        SENDER_OFFER                    = 50,               // action: the offer's place in the list shown
        SENDER_PAY                      = 51,
        SENDER_BACK                     = 52,
        SENDER_UNDER_WAY                = 53,
    };

    char const* const COUNTER_GUILD_MASTER = "guild_master";
    char const* const COUNTER_EMISSARY = "emissary";

    bool IsGuildMaster(char const* counter)
    {
        return strcmp(counter, COUNTER_GUILD_MASTER) == 0;
    }

    char const* const MSG_UNDER_WAY = "The guild already has a purchase under way; come back when it is done.";
    char const* const MSG_GONE = "That is no longer offered.";
    char const* const MSG_PRICE_CHANGED = "The price has changed.";

    // guildd sees a counter purchase on its next ten-second tick, and may publish the guild's rows once more
    // with open = 1 from a read made before the purchase; a guild that just bought waits this long for the next.
    time_t const PURCHASE_HOLD_SECONDS = 30;

    struct Offer
    {
        std::string guild;
        std::string goods;
        std::string counter;
        std::string title;
        uint64 copper = 0;
        std::string items;                                  // as in the row: entry:count,entry:count
        std::vector<std::pair<uint32, uint32>> itemList;
        bool open = false;

        bool SamePrice(Offer const& other) const
        {
            return copper == other.copper && items == other.items && title == other.title;
        }
    };

    // What a player was last shown at a counter: the goods by their place in the menu, and the offer being confirmed.
    struct Shown
    {
        std::vector<std::string> goods;
        Offer confirming;
    };

    std::mutex counterMutex;
    std::map<uint32, Shown> shownByPlayer;                  // by the player character's guid
    std::map<std::string, time_t> heldGuilds;               // guilds that just bought, by guildd's guild key

    Shown& ShownFor(Player const* player)
    {
        return shownByPlayer[player->GetGUIDLow()];
    }

    // Reserves the guild for a purchase; false when it bought within the hold
    bool HoldGuild(std::string const& guild)
    {
        time_t const now = time(nullptr);
        std::lock_guard<std::mutex> lock(counterMutex);
        for (auto itr = heldGuilds.begin(); itr != heldGuilds.end();)
        {
            if (now - itr->second >= PURCHASE_HOLD_SECONDS)
                itr = heldGuilds.erase(itr);
            else
                ++itr;
        }
        return heldGuilds.emplace(guild, now).second;
    }

    void ReleaseGuild(std::string const& guild)
    {
        std::lock_guard<std::mutex> lock(counterMutex);
        heldGuilds.erase(guild);
    }

    std::vector<std::pair<uint32, uint32>> ParseItems(std::string const& items)
    {
        std::vector<std::pair<uint32, uint32>> list;
        size_t start = 0;
        while (start < items.size())
        {
            size_t end = items.find(',', start);
            if (end == std::string::npos)
                end = items.size();
            std::string const part = items.substr(start, end - start);
            size_t const colon = part.find(':');
            if (colon != std::string::npos)
            {
                uint32 const entry = uint32(strtoul(part.substr(0, colon).c_str(), nullptr, 10));
                uint32 const count = uint32(strtoul(part.substr(colon + 1).c_str(), nullptr, 10));
                if (entry && count)
                    list.emplace_back(entry, count);
            }
            start = end + 1;
        }
        return list;
    }

    Offer ReadRow(Field const* fields)
    {
        Offer offer;
        offer.guild = fields[0].GetCppString();
        offer.goods = fields[1].GetCppString();
        offer.counter = fields[2].GetCppString();
        offer.title = fields[3].GetCppString();
        offer.copper = fields[4].GetUInt64();
        offer.items = fields[5].GetCppString();
        offer.itemList = ParseItems(offer.items);
        offer.open = fields[6].GetUInt8() != 0;
        return offer;
    }

    // The account's offers at one counter: the slot first, then the budget, then the rest by title
    std::vector<Offer> ReadOffers(uint32 account, char const* counter)
    {
        std::vector<Offer> offers;
        auto result = CharacterDatabase.PQuery(
            "SELECT guild, goods, counter, title, copper, items, open FROM guildd_price"
            " WHERE account = '%u' AND counter = '%s'"
            " ORDER BY goods = 'slot' DESC, goods = 'budget' DESC, title",
            account, counter);
        if (!result)
            return offers;
        do
            offers.push_back(ReadRow(result->Fetch()));
        while (result->NextRow());
        return offers;
    }

    bool ReadOffer(uint32 account, std::string goods, Offer& offer)
    {
        CharacterDatabase.escape_string(goods);
        auto result = CharacterDatabase.PQuery(
            "SELECT guild, goods, counter, title, copper, items, open FROM guildd_price"
            " WHERE account = '%u' AND goods = '%s'",
            account, goods.c_str());
        if (!result)
            return false;
        offer = ReadRow(result->Fetch());
        return true;
    }

    std::string ItemName(Player const* player, uint32 entry)
    {
        ItemPrototype const* proto = ObjectMgr::GetItemPrototype(entry);
        if (!proto)
            return "item " + std::to_string(entry);
        std::string name = proto->Name1;
        sObjectMgr.GetItemLocaleStrings(entry, player->GetSession()->GetSessionDbLocaleIndex(), &name);
        return name;
    }

    // 6g 25s, as the game writes money
    std::string MoneyText(uint64 copper)
    {
        std::string text;
        auto add = [&text](uint64 amount, char const* unit)
        {
            if (!amount)
                return;
            if (!text.empty())
                text += ' ';
            text += std::to_string(amount) + unit;
        };
        add(copper / 10000, "g");
        add(copper / 100 % 100, "s");
        add(copper % 100, "c");
        return text;
    }

    std::string ItemText(Player const* player, uint32 entry, uint32 count)
    {
        std::string const name = ItemName(player, entry);
        return count == 1 ? name : std::to_string(count) + " " + name;
    }

    // 6g 25s, 3 Sands of Renewal
    std::string PriceText(Player const* player, Offer const& offer)
    {
        std::string text = MoneyText(offer.copper);
        for (auto const& item : offer.itemList)
        {
            if (!text.empty())
                text += ", ";
            text += ItemText(player, item.first, item.second);
        }
        return text.empty() ? "free" : text;
    }

    // The items of an entry in the backpack and the bags, never what is worn, in the bank or being traded
    std::vector<Item*> BagItems(Player* player, uint32 entry)
    {
        std::vector<Item*> items;
        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (item->GetEntry() == entry && !item->IsInTrade())
                    items.push_back(item);

        for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = dynamic_cast<Bag*>(player->GetItemByPos(INVENTORY_SLOT_BAG_0, i)))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* item = bag->GetItemByPos(j))
                        if (item->GetEntry() == entry && !item->IsInTrade())
                            items.push_back(item);
        return items;
    }

    uint32 BagCount(Player* player, uint32 entry)
    {
        uint32 count = 0;
        for (Item const* item : BagItems(player, entry))
            count += item->GetCount();
        return count;
    }

    void TakeFromBags(Player* player, uint32 entry, uint32 count)
    {
        for (Item* item : BagItems(player, entry))
        {
            player->DestroyItemCount(*item, count, true);
            if (!count)
                return;
        }
    }

    // Bots buy through the guild service's pay job, not at a counter
    bool CanBuy(Player const* player)
    {
#ifdef ENABLE_PLAYERBOTS
        return player->isRealPlayer();
#else
        (void)player;
        return true;
#endif
    }

    // Adds the open offers to the menu being built and remembers them; returns whether the account has offers here at all
    bool AddOffers(Player* player, char const* counter, bool& anyOpen)
    {
        std::vector<Offer> const offers = CanBuy(player) ? ReadOffers(player->GetSession()->GetAccountId(), counter) : std::vector<Offer>();
        GossipMenu& menu = player->GetPlayerMenu()->GetGossipMenu();

        std::vector<std::string> goods;
        anyOpen = false;
        for (Offer const& offer : offers)
        {
            if (!offer.open || menu.MenuItemCount() >= GOSSIP_MAX_MENU_ITEMS)
                continue;
            menu.AddMenuItem(GOSSIP_ICON_MONEY_BAG, offer.title + " (" + PriceText(player, offer) + ")", SENDER_OFFER, uint32(goods.size()), "", false);
            goods.push_back(offer.goods);
            anyOpen = true;
        }

        std::lock_guard<std::mutex> lock(counterMutex);
        ShownFor(player).goods = std::move(goods);
        return !offers.empty();
    }

    void ShowConfirm(Player* player, Creature* creature, Offer const& offer)
    {
        {
            std::lock_guard<std::mutex> lock(counterMutex);
            ShownFor(player).confirming = offer;
        }

        GossipMenu& menu = player->GetPlayerMenu()->GetGossipMenu();
        menu.AddMenuItem(GOSSIP_ICON_MONEY_BAG, "Pay " + PriceText(player, offer) + " for " + offer.title + ".", SENDER_PAY, 0, "", false);
        menu.AddMenuItem(GOSSIP_ICON_CHAT, "Not now.", SENDER_BACK, 0, "", false);

        bool const guildMaster = IsGuildMaster(offer.counter.c_str());
        player->SEND_GOSSIP_MENU(guildMaster ? TEXT_ID_GUILD_MASTER_CONFIRM : TEXT_ID_EMISSARY_CONFIRM, creature->GetObjectGuid());
    }

    void Refuse(Player* player, char const* message)
    {
        player->CLOSE_GOSSIP_MENU();
        player->GetSession()->SendNotification("%s", message);
    }

    // Charges the player for the offer they confirmed and records the purchase for the guild service
    void Buy(Player* player, Creature* creature, char const* counter)
    {
        Offer confirmed;
        {
            std::lock_guard<std::mutex> lock(counterMutex);
            confirmed = ShownFor(player).confirming;
        }

        // The price as it is now: the guild service rewrites it every ten seconds
        Offer offer;
        if (confirmed.goods.empty() || !ReadOffer(player->GetSession()->GetAccountId(), confirmed.goods, offer) || offer.counter != counter)
        {
            Refuse(player, MSG_GONE);
            return;
        }

        if (!offer.open)
        {
            Refuse(player, MSG_UNDER_WAY);
            return;
        }

        if (!offer.SamePrice(confirmed))
        {
            player->GetSession()->SendNotification("%s", MSG_PRICE_CHANGED);
            ShowConfirm(player, creature, offer);
            return;
        }

        if (!HoldGuild(offer.guild))
        {
            Refuse(player, MSG_UNDER_WAY);
            return;
        }

        if (offer.copper > player->GetMoney())
        {
            ReleaseGuild(offer.guild);
            player->CLOSE_GOSSIP_MENU();
            player->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, creature, 0, 0);
            return;
        }

        for (auto const& item : offer.itemList)
        {
            uint32 const carried = BagCount(player, item.first);
            if (carried < item.second)
            {
                ReleaseGuild(offer.guild);
                player->CLOSE_GOSSIP_MENU();
                player->GetSession()->SendNotification("You need %s in your bags; you have %u.",
                                                       ItemText(player, item.first, item.second).c_str(), carried);
                return;
            }
        }

        // Every check passed: take the price, then write it and the purchase in one transaction
        player->ModifyMoney(-int32(offer.copper));
        for (auto const& item : offer.itemList)
            TakeFromBags(player, item.first, item.second);

        std::string guild = offer.guild;
        std::string goods = offer.goods;
        std::string items = offer.items;
        std::string payerName = player->GetName();
        CharacterDatabase.escape_string(guild);
        CharacterDatabase.escape_string(goods);
        CharacterDatabase.escape_string(items);
        CharacterDatabase.escape_string(payerName);

        CharacterDatabase.BeginTransaction();
        player->SaveInventoryAndGoldToDB();
        CharacterDatabase.PExecute(
            "INSERT INTO guildd_purchase (guild, goods, copper, items, payer, payer_name, job_seq, status, reason,"
            " previous_job, previous_args, created_at)"
            " VALUES ('%s', '%s', '" UI64FMTD "', '%s', '%u', '%s', 0, 'counter', '', '', '', UNIX_TIMESTAMP())",
            guild.c_str(), goods.c_str(), offer.copper, items.c_str(), player->GetGUIDLow(), payerName.c_str());
        // Until the guild service's next tick: no second purchase from a click made before it
        CharacterDatabase.PExecute("UPDATE guildd_price SET open = 0 WHERE guild = '%s'", guild.c_str());
        CharacterDatabase.CommitTransaction();

        sLog.outString("Counter: %s (guid %u, account %u) paid " UI64FMTD " copper and '%s' for %s of guild %s",
                       player->GetName(), player->GetGUIDLow(), player->GetSession()->GetAccountId(),
                       offer.copper, offer.items.c_str(), offer.goods.c_str(), offer.guild.c_str());

        player->CLOSE_GOSSIP_MENU();
        std::string const thanks = IsGuildMaster(counter)
            ? "Paid in full. Your guild will have " + offer.title + " shortly."
            : "It is done. The sand runs back through the glass: " + offer.title + ".";
        creature->MonsterWhisper(thanks.c_str(), player);
    }

    bool GossipHello_counter(Player* player, Creature* creature, char const* counter)
    {
        player->GetPlayerMenu()->ClearMenus();

        if (IsGuildMaster(counter))
        {
            // The Guild Master's own options first: the guild charter and the tabard, from its gossip menu
            player->PrepareGossipMenu(creature, creature->GetDefaultGossipMenuId());
            bool anyOpen = false;
            if (AddOffers(player, counter, anyOpen) && !anyOpen && player->GetPlayerMenu()->GetGossipMenu().MenuItemCount() < GOSSIP_MAX_MENU_ITEMS)
                player->GetPlayerMenu()->GetGossipMenu().AddMenuItem(GOSSIP_ICON_CHAT, "What of my guild's purchase?", SENDER_UNDER_WAY, 0, "", false);
            player->SendPreparedGossip(creature);
            return true;
        }

        bool anyOpen = false;
        bool const anyOffer = AddOffers(player, counter, anyOpen);
        uint32 const textId = anyOpen ? TEXT_ID_EMISSARY_GREETING : anyOffer ? TEXT_ID_EMISSARY_UNDER_WAY : TEXT_ID_EMISSARY_NOTHING;
        player->SEND_GOSSIP_MENU(textId, creature->GetObjectGuid());
        return true;
    }

    // One of the core's own options: rebuild the default menu as the core showed it and let the core act on it,
    // so the charter petition and the tabard designer work exactly as they do without this script.
    bool SelectCoreOption(Player* player, Creature* creature, uint32 action)
    {
        player->PrepareGossipMenu(creature, creature->GetDefaultGossipMenuId());
        GossipMenu& menu = player->GetPlayerMenu()->GetGossipMenu();
        for (uint32 i = 0; i < menu.MenuItemCount(); ++i)
        {
            GossipMenuItem const& item = menu.GetItem(i);
            if (item.m_gSender == SENDER_CORE && item.m_gOptionId == action)
            {
                player->OnGossipSelect(creature, i);
                return true;
            }
        }
        player->CLOSE_GOSSIP_MENU();
        return true;
    }

    bool GossipSelect_counter(Player* player, Creature* creature, uint32 sender, uint32 action, char const* counter)
    {
        switch (sender)
        {
            case SENDER_CORE:
                if (IsGuildMaster(counter))
                    return SelectCoreOption(player, creature, action);
                player->CLOSE_GOSSIP_MENU();
                return true;

            case SENDER_OFFER:
            {
                std::string goods;
                {
                    std::lock_guard<std::mutex> lock(counterMutex);
                    Shown const& shown = ShownFor(player);
                    if (action < shown.goods.size())
                        goods = shown.goods[action];
                }

                Offer offer;
                if (goods.empty() || !CanBuy(player) || !ReadOffer(player->GetSession()->GetAccountId(), goods, offer) || offer.counter != counter)
                {
                    Refuse(player, MSG_GONE);
                    return true;
                }
                if (!offer.open)
                {
                    Refuse(player, MSG_UNDER_WAY);
                    return true;
                }
                ShowConfirm(player, creature, offer);
                return true;
            }

            case SENDER_PAY:
                if (CanBuy(player))
                    Buy(player, creature, counter);
                else
                    player->CLOSE_GOSSIP_MENU();
                return true;

            case SENDER_BACK:
                return GossipHello_counter(player, creature, counter);

            case SENDER_UNDER_WAY:
                Refuse(player, MSG_UNDER_WAY);
                return true;
        }

        player->CLOSE_GOSSIP_MENU();
        return true;
    }
}

/*######
## npc_guild_master_counter: a capital's Guild Master, selling slots, features and order budget
######*/

bool GossipHello_npc_guild_master_counter(Player* player, Creature* creature)
{
    return GossipHello_counter(player, creature, COUNTER_GUILD_MASTER);
}

bool GossipSelect_npc_guild_master_counter(Player* player, Creature* creature, uint32 sender, uint32 action)
{
    return GossipSelect_counter(player, creature, sender, action, COUNTER_GUILD_MASTER);
}

/*######
## npc_bronze_emissary: the emissary of the Bronze Flight beside each Guild Master, selling raid lockout resets
######*/

bool GossipHello_npc_bronze_emissary(Player* player, Creature* creature)
{
    return GossipHello_counter(player, creature, COUNTER_EMISSARY);
}

bool GossipSelect_npc_bronze_emissary(Player* player, Creature* creature, uint32 sender, uint32 action)
{
    return GossipSelect_counter(player, creature, sender, action, COUNTER_EMISSARY);
}

void AddSC_guild_counter()
{
    Script* pNewScript = new Script;
    pNewScript->Name = "npc_guild_master_counter";
    pNewScript->pGossipHello = &GossipHello_npc_guild_master_counter;
    pNewScript->pGossipSelect = &GossipSelect_npc_guild_master_counter;
    pNewScript->RegisterSelf();

    pNewScript = new Script;
    pNewScript->Name = "npc_bronze_emissary";
    pNewScript->pGossipHello = &GossipHello_npc_bronze_emissary;
    pNewScript->pGossipSelect = &GossipSelect_npc_bronze_emissary;
    pNewScript->RegisterSelf();
}
