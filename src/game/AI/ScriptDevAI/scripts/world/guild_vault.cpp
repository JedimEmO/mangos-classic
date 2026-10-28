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

namespace
{
    enum
    {
        // npc_text rows of server/sql/031-vault.sql in the main repository
        TEXT_ID_VAULTKEEPER_GREETING    = 200105,
        TEXT_ID_VAULTKEEPER_NO_VAULT    = 200106,

        ACTION_SHOW_VAULT               = 1,
    };
}

/*######
## npc_guild_vaultkeeper: shows the vault of the player's guild as a vendor's window
######*/

bool GossipHello_npc_guild_vaultkeeper(Player* player, Creature* creature)
{
    player->GetPlayerMenu()->ClearMenus();

    if (sGuildVault.GuildOf(player->GetSession()->GetAccountId()).empty())
    {
        player->SEND_GOSSIP_MENU(TEXT_ID_VAULTKEEPER_NO_VAULT, creature->GetObjectGuid());
        return true;
    }

    player->GetPlayerMenu()->GetGossipMenu().AddMenuItem(GOSSIP_ICON_VENDOR, "Let me see the vault.", GOSSIP_SENDER_MAIN, ACTION_SHOW_VAULT, "", false);
    player->SEND_GOSSIP_MENU(TEXT_ID_VAULTKEEPER_GREETING, creature->GetObjectGuid());
    return true;
}

bool GossipSelect_npc_guild_vaultkeeper(Player* player, Creature* creature, uint32 /*sender*/, uint32 action)
{
    player->CLOSE_GOSSIP_MENU();
    if (action == ACTION_SHOW_VAULT)
        player->SEND_VENDORLIST(creature->GetObjectGuid());
    return true;
}

void AddSC_guild_vault()
{
    Script* pNewScript = new Script;
    pNewScript->Name = "npc_guild_vaultkeeper";
    pNewScript->pGossipHello = &GossipHello_npc_guild_vaultkeeper;
    pNewScript->pGossipSelect = &GossipSelect_npc_guild_vaultkeeper;
    pNewScript->RegisterSelf();
}
