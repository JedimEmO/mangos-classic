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
SDName: Guild_Vault
SD%Complete: 100
SDComment: The vaultkeeper beside each capital's Guild Master, who keeps a guild's vault of the competition.
           The vault itself, its vendor window and the engine's calls are the core's GuildVault; VAULT.md describes them.
SDCategory: NPCs
EndScriptData */

#include "AI/ScriptDevAI/include/sc_common.h"
#include "Guilds/GuildVault.h"

#include <cctype>
#include <string>

namespace
{
    enum
    {
        // npc_text rows of server/sql/031-vault.sql in the main repository
        TEXT_ID_VAULTKEEPER_GREETING    = 200105,
        TEXT_ID_VAULTKEEPER_NO_VAULT    = 200106,

        ACTION_SHOW_VAULT               = 1,
        ACTION_DEPOSIT_GOLD             = 2,
        ACTION_WITHDRAW_GOLD            = 3,
    };

    // As the game writes money: 12g 5s 30c, the empty parts left out
    std::string Money(uint64 copper)
    {
        if (!copper)
            return "0c";
        std::string text;
        auto part = [&text](uint64 amount, char const* unit)
        {
            if (!amount)
                return;
            if (!text.empty())
                text += ' ';
            text += std::to_string(amount) + unit;
        };
        part(copper / 10000, "g");
        part(copper / 100 % 100, "s");
        part(copper % 100, "c");
        return text;
    }

    // An amount as typed in the code box: "12" is 12 gold, "12g 50s", "3s 20c", or "all".
    // all is set for "all"; false when the text is no amount.
    bool ParseMoney(char const* code, uint64& copper, bool& all)
    {
        copper = 0;
        all = false;
        std::string text;
        for (char const* c = code; c && *c; ++c)
            text += char(std::tolower(static_cast<unsigned char>(*c)));
        size_t const first = text.find_first_not_of(' ');
        if (first == std::string::npos)
            return false;
        size_t const last = text.find_last_not_of(' ');
        if (text.substr(first, last - first + 1) == "all")
            return all = true;

        bool any = false;
        size_t i = first;
        while (i <= last)
        {
            if (text[i] == ' ' || text[i] == ',')
            {
                ++i;
                continue;
            }
            if (!std::isdigit(static_cast<unsigned char>(text[i])))
                return false;
            uint64 amount = 0;
            while (i <= last && std::isdigit(static_cast<unsigned char>(text[i])))
            {
                amount = amount * 10 + uint64(text[i] - '0');
                if (amount > uint64(MAX_MONEY_AMOUNT))
                    return false;
                ++i;
            }
            uint64 unit = 10000;
            if (i <= last && (text[i] == 'g' || text[i] == 's' || text[i] == 'c'))
            {
                unit = text[i] == 'g' ? 10000 : text[i] == 's' ? 100 : 1;
                ++i;
            }
            copper += amount * unit;
            if (copper > uint64(MAX_MONEY_AMOUNT))
                return false;
            any = true;
        }
        return any;
    }

    void SendVaultMenu(Player* player, Creature* creature)
    {
        player->GetPlayerMenu()->ClearMenus();
        std::string const guild = sGuildVault.GuildOf(player->GetSession()->GetAccountId());
        if (guild.empty())
        {
            player->SEND_GOSSIP_MENU(TEXT_ID_VAULTKEEPER_NO_VAULT, creature->GetObjectGuid());
            return;
        }

        GossipMenu& menu = player->GetPlayerMenu()->GetGossipMenu();
        menu.AddMenuItem(GOSSIP_ICON_VENDOR, "Let me see the vault.", GOSSIP_SENDER_MAIN, ACTION_SHOW_VAULT, "", false);
        // The 1.12 code box shows no message of its own, so the option says what to type
        menu.AddMenuItem(GOSSIP_ICON_MONEY_BAG, "Put gold in the vault. (Type an amount: 12, 12g 50s, or all.)",
                         GOSSIP_SENDER_MAIN, ACTION_DEPOSIT_GOLD, "", true);
        uint64 const held = sGuildVault.Gold(guild);
        if (held)
            menu.AddMenuItem(GOSSIP_ICON_MONEY_BAG, "Take gold from the vault. It holds " + Money(held) + ".",
                             GOSSIP_SENDER_MAIN, ACTION_WITHDRAW_GOLD, "", true);
        player->SEND_GOSSIP_MENU(TEXT_ID_VAULTKEEPER_GREETING, creature->GetObjectGuid());
    }
}

/*######
## npc_guild_vaultkeeper: shows the vault of the player's guild as a vendor's window
######*/

bool GossipHello_npc_guild_vaultkeeper(Player* player, Creature* creature)
{
    SendVaultMenu(player, creature);
    return true;
}

bool GossipSelect_npc_guild_vaultkeeper(Player* player, Creature* creature, uint32 /*sender*/, uint32 action)
{
    player->CLOSE_GOSSIP_MENU();
    if (action == ACTION_SHOW_VAULT)
        player->SEND_VENDORLIST(creature->GetObjectGuid());
    return true;
}

bool GossipSelectWithCode_npc_guild_vaultkeeper(Player* player, Creature* creature, uint32 /*sender*/, uint32 action, char const* code)
{
    if (action != ACTION_DEPOSIT_GOLD && action != ACTION_WITHDRAW_GOLD)
    {
        player->CLOSE_GOSSIP_MENU();
        return true;
    }

    uint64 copper = 0;
    bool all = false;
    if (!ParseMoney(code, copper, all))
    {
        player->GetSession()->SendNotification("Type an amount, such as 12, 12g 50s or all.");
        SendVaultMenu(player, creature);
        return true;
    }

    std::string reason;
    std::string const guild = sGuildVault.GuildOf(player->GetSession()->GetAccountId());
    if (action == ACTION_DEPOSIT_GOLD)
    {
        uint32 const amount = all ? player->GetMoney() : uint32(copper);
        if (sGuildVault.DepositGold(player, amount, reason))
            player->GetSession()->SendNotification("You put %s in the vault. It holds %s.", Money(amount).c_str(), Money(sGuildVault.Gold(guild)).c_str());
        else
            player->GetSession()->SendNotification("%s", reason.c_str());
    }
    else
    {
        uint32 const amount = all ? uint32(MAX_MONEY_AMOUNT) : uint32(copper);
        if (uint32 const taken = sGuildVault.WithdrawGold(player, amount, reason))
            player->GetSession()->SendNotification("You take %s from the vault. It holds %s.", Money(taken).c_str(), Money(sGuildVault.Gold(guild)).c_str());
        else
            player->GetSession()->SendNotification("%s", reason.c_str());
    }
    SendVaultMenu(player, creature);
    return true;
}

void AddSC_guild_vault()
{
    Script* pNewScript = new Script;
    pNewScript->Name = "npc_guild_vaultkeeper";
    pNewScript->pGossipHello = &GossipHello_npc_guild_vaultkeeper;
    pNewScript->pGossipSelect = &GossipSelect_npc_guild_vaultkeeper;
    pNewScript->pGossipSelectWithCode = &GossipSelectWithCode_npc_guild_vaultkeeper;
    pNewScript->RegisterSelf();
}
