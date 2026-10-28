# Counters

A player buys their guild's upgrades in person, by talking to an NPC in a capital.
Each capital's Guild Master sells character slots, features and order budget;
an emissary of the Bronze Flight beside it sells raid lockout resets, paid in Sands of Renewal (item 200001).
`docs/competition.md` in the main repository has the design, and `docs/guild-service.md` the shop behind it.

The gossip of both is `src/game/AI/ScriptDevAI/scripts/world/guild_counter.cpp`,
with two script names: `npc_guild_master_counter` and `npc_bronze_emissary`.
The world database side, the emissary, its spawns and the gossip texts,
is `server/sql/030-counters.sql` in the main repository.

## What the player sees

The Guild Master shows its own options first, the guild charter and the tabard designer,
from its gossip menu as the core builds it, and then one option per open offer of the player's account,
such as `Character slot 12 (6g 25s)`.
Choosing one of its own options rebuilds that menu and hands the choice to the core,
so the petition list and the tabard designer open exactly as they do without the script.
A player whose account has no offers sees the Guild Master as it always was.

The emissary has no other functions.
It greets with npc_text 200100 when the account has open offers,
200102 when the guild has a purchase under way, and 200101 when it has nothing to sell.

The 1.12 client has no confirmation box for gossip options, so choosing an offer opens a second menu:
`Pay 6g 25s for Character slot 12.` or `Not now.`, under npc_text 200103 at the Guild Master
and 200104 at the emissary.
Bots see no offers; they pay through the guild service's `pay` job.

## Tables

Both tables live in the `characters` database, and the guild service creates them.

```sql
guildd_price (account INT UNSIGNED, guild VARCHAR(255), goods VARCHAR(64), counter VARCHAR(16),
              title VARCHAR(128), copper BIGINT UNSIGNED, items VARCHAR(255), open TINYINT UNSIGNED,
              PRIMARY KEY (account, goods))
```

The service rewrites a guild's rows every ten seconds.
The script reads the rows of the player's realmd account whose `counter` is `guild_master` or `emissary`,
and shows those with `open = 1`; `open` is 0 while the guild has a purchase under way.
`items` is `entry:count,entry:count`, shown by item name and count.

A purchase is a row in `guildd_purchase` with the guild, goods, copper and items of the price row,
the player character's guid and name as payer, `job_seq` 0, status `counter`, and `UNIX_TIMESTAMP()` as `created_at`.
The service gives the guild what was bought on its next tick, and carries out a reset itself.

## Paying

Paying reads the price row again, because the price may have changed since the menu was shown.
A row that is gone, or belongs to the other counter, ends the conversation with "That is no longer offered."
A row with `open = 0` refuses with "The guild already has a purchase under way; come back when it is done."
A price that changed shows the confirmation again with the new price.

The player pays from the backpack and the bags only;
worn items, the bank and items in an open trade window do not count.
Too little money gets the game's own "You don't have enough money" error,
and a missing item a notification such as "You need 3 Sands of Renewal in your bags; you have 1."

Only when every check passes are the gold and the items taken, in memory.
One transaction then saves the player's gold and inventory, inserts the purchase,
and sets `open = 0` on all of the guild's price rows.
It goes through the core's queue of database writes, as a trade or a mail does,
so it lands after any save of the player already queued and never under it.

That queue writes a moment later, and the service may publish the guild's rows once more
from a read made before the purchase.
So the script also holds a guild for 30 seconds after it buys,
refusing a second purchase from another click or another character with the same message.
The hold is in memory and ends on a restart; the rows with `open = 0` cover a restart.

A failed transaction, as when the database is down, leaves the gold and items taken in memory
with no purchase written; the log has the line `Counter: <name> ... paid`.
A missing `guildd_price` table gives an SQL error in the log on each greeting, and no offers.

## Testing it

With the core and `030-counters.sql` deployed, and the guild service running or not:

1. Pick a test character and its account id: `SELECT guid, account FROM characters WHERE name = 'Name';`.
2. Write an offer for each counter by hand, with the account's guild key from `guildd_guild` or any test string:
   ```sql
   REPLACE INTO guildd_price VALUES
     (ACCOUNT, 'test', 'slot', 'guild_master', 'Character slot 12', 62500, '', 1),
     (ACCOUNT, 'test', 'reset:molten_core', 'emissary', 'Reset Molten Core', 0, '200001:3', 1);
   ```
   The guild service replaces rows of guilds it knows within ten seconds, so a guild key it does not know keeps them still.
3. Talk to the Guild Master in Stormwind: the charter and crest options, then `Character slot 12 (6g 25s)`.
   Check that the charter and crest options still open their windows.
4. Choose the slot without the gold: the money error. Give the character gold with `.modify money 62500` and pay.
   The gold goes, the Guild Master whispers, and `guildd_purchase` has a row with status `counter`;
   both price rows of the guild have `open = 0`.
5. Talk to him again: `What of my guild's purchase?` refuses as under way.
6. After the 30-second hold, set `open = 1` again and try the emissary beside him with two Sands of Renewal (`.additem 200001 2`):
   the missing item notification. With three, it takes them and writes the reset purchase.
7. Change the copper of a row between choosing an offer and paying: the confirmation comes back with the new price.
