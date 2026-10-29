# The guild vault

Each guild of the competition keeps a stockpile in one vault,
kept by a Steamwheedle Vaultkeeper beside the Guild Master in every capital.
It is the same vault at each, as a bank is.
It holds items and the guild's gold.
Players use it as a vendor's window, every price 0, and through the vaultkeeper's gossip for gold;
the bot engine uses the same functions through its `kit`, `deposit`, `withdraw` and `pay` jobs.
`docs/competition.md` in the main repository has the design.

The vault is `src/game/Guilds/GuildVault.h` and `.cpp`, the singleton `sGuildVault`.
The vaultkeeper's gossip is `src/game/AI/ScriptDevAI/scripts/world/guild_vault.cpp`, script name `npc_guild_vaultkeeper`.
The world database side, the vaultkeeper (entry 200101), its six spawns and its gossip texts,
is `server/sql/031-vault.sql` in the main repository.

## Whose vault

A vault belongs to a guild of the guild service, named by its guild key.
The core reads an account's guild from `guildd_account (account, guild)` in the characters database,
which the guild service writes; it reads it again on every use, so a change shows at once.
Every character of the guild's accounts shares the vault, at any vaultkeeper of either faction.
An account with no row has no vault: the vaultkeeper says so, and the functions refuse with "You have no guild vault."

## What goes in

The vault holds items as counts by entry: twenty Major Mana Potions, not twenty particular potions.
A withdrawal makes a new item of the entry, so the vault takes only items that are the same as any new one of their kind.
It refuses, each with a plain message such as "Enchanted items cannot go in the vault.":

- soulbound items, and items that bind on pickup;
- quest items: class Quest, quest binding, or an item that starts a quest;
- conjured items, and items that expire;
- items with random properties, rolled or possible, since a new one would roll its own;
- items with any enchantment, permanent or temporary;
- items below full durability, or with charges used;
- wrapped gifts, opened lockboxes and other items holding loot, and written letters;
- bags with anything in them.

Only what is in the backpack, the bags and the keyring can go in;
not what is worn, a bag while it is worn, the bank, or an item being traded or looted.
A crafted item's "Made by" goes with it; the one that comes out has no maker.

A vault holds at most 128 different items.
The 1.12 client keeps a vendor's list in an array of 128 entries and does not check the count it is sent,
so the core's `MAX_VENDOR_ITEMS` of 128 is also the client's limit.
A new kind past that is refused with "The vault holds as many kinds of items as it can."

## Gold

The vault holds one balance of copper per guild, with no limit but a 64-bit count.
A character puts in what it carries, and takes out as much as the vault holds and its purse takes;
a character carries at most `MAX_MONEY_AMOUNT`, 2147483646 copper, about 214748 gold.
So one deposit or withdrawal is at most that much, and the log's 32-bit `delta` holds it.

A player uses the vaultkeeper's gossip.
"Put gold in the vault." and "Take gold from the vault. It holds 12g 50s." are code-box options:
the player types an amount, and the vaultkeeper answers with a notice and the menu again, with the new balance.
The 1.12 client's code box shows no message of its own, so the deposit option says what to type.
An amount is a number of gold, coins with units such as `12g 50s` or `3s 20c`, or `all`.
The take option shows only while the vault holds some gold.

## The API

All of it is in `GuildVault.h`, and every function may be called from any map thread.

```cpp
static uint32 const VAULTKEEPER_ENTRY = 200101;
static uint32 const MAX_KINDS = 128;
static bool IsVaultkeeper(Creature const* creature);

std::string GuildOf(uint32 accountId) const;
uint32 Count(std::string const& guild, uint32 item) const;
std::vector<std::pair<uint32, uint32>> Contents(std::string const& guild) const;
static bool IsFungible(Item const* item, std::string& reason);

uint32 Withdraw(Player* player, uint32 item, uint32 count, std::string& reason, InventoryResult* bagError = nullptr);
bool Deposit(Player* player, Item* item, uint32 count, std::string& reason);

uint64 Gold(std::string const& guild) const;
bool DepositGold(Player* player, uint32 copper, std::string& reason);
uint32 WithdrawGold(Player* player, uint32 copper, std::string& reason);
```

`GuildOf` is empty for an account with no guild.
`Contents` is the guild's vault as (entry, count), by entry.
`IsFungible` checks an item's kind and state alone, the list above.

`Withdraw` takes up to `count` from the vault of the player's guild into the player's bags,
as much as the vault holds and the bags take, and returns how many.
When it gives none, `reason` says why, and `bagError` has the inventory error if the bags had no room.
The player gets the item as from a vendor, with the "You receive" message.

`Deposit` puts `count` of the item in the vault, or the whole stack when `count` is 0.
It checks the owner, where the item is, `IsFungible`, the guild and the room in the vault, and false comes with a reason.
On success the item may be destroyed, so the caller must not use the pointer again.

`Gold` is the guild's balance in copper.
`DepositGold` puts exactly `copper` of the player's gold in, and refuses with a reason when the player has less.
`WithdrawGold` takes up to `copper`, as much as the vault holds and the purse takes, and returns how much;
none comes with a reason, such as "The vault holds no gold."

The player must be in the world and handled by the calling thread, as for any change to a player's inventory.

## The vendor window

The vaultkeeper's gossip shows "Let me see the vault." to a player with a vault, and opens the vendor window.
Three hooks in `ItemHandler.cpp` hand a vaultkeeper to the vault before the core's own vendor code:

- `SendListInventory` sends the vault's contents, by item class and name, at price 0 and one item per purchase.
  The count each item shows is what the vault holds.
- `HandleBuyItemOpcode` and `HandleBuyItemInSlotOpcode` call `Withdraw`.
  Full bags get the game's own error, an item gone get "That item is currently sold out" and the list again,
  and the item's new count goes to the client as after any purchase.
- `HandleSellItemOpcode` calls `Deposit`. A refusal shows its reason; a deposit sends the list again, with what went in.

Selling an item whose vendor price is 0 works.
The 1.12 client sends `CMSG_SELL_ITEM` for any item, from a right click and from a drop on the window, without looking at the price;
only the core's sell handler refuses a price of 0, and the vault's branch comes before that check.
So there is no separate deposit option in the gossip.

The hooks act for real players only.
A bot finds an ordinary merchant with nothing for sale that buys its junk at the usual price,
so the playerbots' sell action, which picks the nearest vendor, does not stick on a vaultkeeper.
Bots use the vault through `Withdraw` and `Deposit`.

## Atomicity

The vault in memory is the truth: it is loaded at startup and changed under one mutex.
That mutex is held from reading a count until the change is queued,
so two characters of one guild in two capitals cannot both take the last potion; the second finds none.

A withdrawal checks `CanStoreNewItem`, stores the new item, lowers the count in memory, and queues the change.
A deposit runs every check first, then destroys the item from the player and raises the count.
Nothing between the checks and the change can fail.

Each change is one transaction on the characters database's write queue, as a trade or a mail is:
the player's inventory and gold, the vault's new count of the item (or the row deleted at 0), and a line in the log.
The queue is one thread that writes in order, and changes are queued while the mutex is held,
so the database gets them in the order memory had them, each after any save of the player queued before it.
Each writes the item's absolute count, so a later change of the item mends a row that an earlier failure left wrong.
Gold works the same way: the player's gold, the guild's new balance (or the row deleted at 0), and a log line with item 0.

What a crash or a failed transaction costs is the same as for a trade.
A crash before the queue writes loses the change on both sides together.
A transaction that fails, as when the database is down, leaves the change in memory unwritten,
and the player's later saves do not repeat it: that deposit's item can come back on the next login while the vault keeps it too.
The log has an SQL error for it.

## Tables

The core creates all three in the characters database at startup if they are missing, with a binary collation,
so guild keys compare exactly as they do in memory. The guild service reads them; only the core writes.

```sql
guildbot_vault (guild VARCHAR(255) NOT NULL, item INT UNSIGNED NOT NULL, count INT UNSIGNED NOT NULL,
                PRIMARY KEY (guild, item))
guildbot_vault_gold (guild VARCHAR(255) NOT NULL PRIMARY KEY, copper BIGINT UNSIGNED NOT NULL)
guildbot_vault_log (id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY, guild VARCHAR(255) NOT NULL,
                    at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, guid INT UNSIGNED NOT NULL,
                    name VARCHAR(12) NOT NULL, item INT UNSIGNED NOT NULL, delta INT NOT NULL, KEY (guild, at))
```

`guildbot_vault` has a row per item a vault holds, never one at 0.
It is read once, at startup; a row changed by hand while the server runs is overwritten by the next change of that item.
`guildbot_vault_gold` has a row per guild with gold in its vault, read once at startup as the items are.
`guildbot_vault_log` has a line per change, the character's guid and name, and `delta` positive for a deposit;
item 0 is gold, its `delta` in copper.

## Testing it

With the core and `031-vault.sql` deployed, and the guild service running or not:

1. Pick two test characters on two accounts: `SELECT guid, account FROM characters WHERE name IN ('One', 'Two');`.
2. Give both accounts one guild, with any test key the guild service does not know:
   ```sql
   CREATE TABLE IF NOT EXISTS guildd_account (account INT UNSIGNED NOT NULL PRIMARY KEY, guild VARCHAR(255) NOT NULL);
   REPLACE INTO guildd_account VALUES (ACCOUNT_ONE, 'vault-test'), (ACCOUNT_TWO, 'vault-test');
   ```
3. With a third character on an account without a row, talk to the vaultkeeper beside the Stormwind Guild Master:
   the "no vault" text and no option.
4. With the first, open the vault: the window opens empty.
   `.additem 13444 5` and sell them by a right click: they go, and the window shows 5 Major Mana Potions at no price.
   `guildbot_vault` has `('vault-test', 13444, 5)` and the log a line with delta 5.
5. Try an enchanted weapon, a soulbound item, a damaged one and a Hearthstone: each refused with its reason, and still in the bags.
6. Buy one potion, then shift-click and buy three: the count goes to 4 and to 1, and the log has -1 and -3.
7. Fill the bags and buy the last one: the bags-full error, and the vault still has 1.
8. With the second character at the Orgrimmar vaultkeeper, or the same one in Ironforge: the same vault.
9. Take the last potion with one character while the other has the window open, then buy with the other: sold out, and the list without it.
10. With the first character and some gold, choose "Put gold in the vault." and type `1g 50s`:
    the notice says it holds 1g 50s, the purse is 1g 50s lighter, `guildbot_vault_gold` has `('vault-test', 15000)`
    and the log a line with item 0 and delta 15000. Type `lots`: asked for an amount, nothing moves.
11. With the second, "Take gold from the vault. It holds 1g 50s.", type `all`: 1g 50s in its purse, the row gone,
    and the take option gone from the menu.
12. Restart mangosd: the log shows `Loaded N kinds of items in M guild vaults, and gold in K`, and the vault is as it was.
