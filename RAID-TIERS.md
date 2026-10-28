# Raid tiers

Every raid instance has a tier, 1 or higher, fixed when the instance is created.
Tier 1 is the raid tuned for fifteen players instead of forty:
its creatures have less health and deal less damage.
Each tier above adds a step of both, and gives bosses a chance at more loot.
`docs/competition.md` in the main repository has the design, under "Raids for fifteen" and "Tiers".

Only raid maps (`map_type` raid in `Map.dbc`) have a tier.
Dungeons, battlegrounds and the open world have tier 0, and nothing changes on them.

## Configuration

The options are in `mangosd.conf`, read at startup and on `.reload config`.

| Option | Default | Meaning |
|---|---|---|
| `Raid.Tier.Enable` | 1 | The master switch; 0 leaves every raid as it is in vanilla |
| `Raid.Tier.HealthAtTier1` | 0.40 | Creature health at tier 1 |
| `Raid.Tier.DamageAtTier1` | 0.60 | Creature damage at tier 1 |
| `Raid.Tier.StepPerTier` | 0.15 | What each tier above 1 adds |
| `Raid.Tier.ExtraLootPerTier` | 0.25 | Extra loot draws per tier above 1, on average |
| `Raid.Tier.ExtraLootMax` | 3 | The most extra draws a boss gets |

A multiplier is `AtTier1 x (1 + StepPerTier x (tier - 1))`.
At the defaults, tier 1 is 0.40 health and 0.60 damage,
tier 2 is 0.46 and 0.69, and tier 5 is 0.64 and 0.96.

Zul'Gurub and the Ruins of Ahn'Qiraj are raids too, so they get the same multipliers,
although they stay twenty players.

## Where the tier comes from

The guild service writes the tier a guild has chosen for each raid;
the core only reads it.

```sql
guildd_raid_tier (account INT UNSIGNED, map SMALLINT UNSIGNED, tier TINYINT UNSIGNED, PRIMARY KEY (account, map))
```

When a raid instance is created, the core takes the account of the entering player's group leader,
or of the player when they are not in a group, and looks up its row for that map.
No row, or a tier of 0, means tier 1.
A missing `guildd_raid_tier` table also means tier 1, with an SQL error in the log.

The tier is recorded in a table the core creates at startup if it is missing:

```sql
guildbot_instance_tier (instance INT UNSIGNED PRIMARY KEY, map SMALLINT UNSIGNED, tier TINYINT UNSIGNED,
                        account INT UNSIGNED, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)
```

An instance that is loaded again, after it was unloaded or the server restarted, keeps the tier in its row,
so changing `guildd_raid_tier` affects only instances created afterwards.
A row lives as long as its instance does.
It is deleted with the instance, renumbered with it when the core packs instance ids at startup,
and dropped at startup when its instance is gone; instance ids are reused after a restart.

## Health and damage

The multipliers apply to every creature of the instance, bosses, trash and anything summoned later,
except creatures that serve a player: pets, guardians, totems and charmed or player-controlled creatures.

Health is multiplied in `Creature::SelectLevel`, next to the `Rate.Creature.*.HP` rates,
before the creature is set to full health, so it spawns full.
A creature that changes entry runs `SelectLevel` again and gets the same multiplier.

Damage is multiplied in three places, each beside the core's own creature damage rates:

- `Creature::SelectLevel` multiplies the base weapon damage, which covers melee and ranged attacks
  and spells that deal weapon damage.
- `Unit::SpellDamageBonusDone` covers the direct and periodic damage of magic spells.
- `Unit::MeleeDamageBonusDone` covers melee and ranged spells with their own damage, and physical periodic damage.

Some damage does not go through these and is not scaled.
Spells with `SPELL_ATTR_EX3_IGNORE_CASTER_MODIFIERS` skip the bonus functions,
as does damage that scripts deal with `DealDamage`, environmental damage and damage in percent of health.
A creature charmed by a player keeps its scaled weapon damage but deals full spell damage.

## Loot

A boss is a creature of rank world boss, or the kill credit of an encounter on that map.
When a boss's corpse loot is filled on a raid of tier T,
it gets `ExtraLootPerTier x (T - 1)` extra draws on average:
the whole part always, the fraction as a chance, and no more than `ExtraLootMax`.

A draw rolls once more one of the references the boss's loot always rolls,
weighted by how many items each reference gives,
so the extra item comes from a pool the boss already drops from.
A boss without such references draws once more from one of its loot groups that holds no quest items.
The extra items are added before the core assigns loot rights and rolls,
so they are looted like the rest; the client shows no more than 16 items.

Gold is not scaled, and chests such as the Cache of the Firelord get no extra draws.

## Hooks

| File | Hook |
|---|---|
| `src/game/Maps/RaidTier.cpp`, `.h` | The tier lookup, the multipliers and the extra loot |
| `Maps/Map.h` | `Map::GetRaidTier()`, set before the map loads its creatures |
| `Maps/MapManager.cpp` | `CreateInstance` selects the tier and `CreateDungeonMap` sets it before `Initialize` |
| `Maps/MapPersistentStateMgr.cpp` | Deletes, prunes and renumbers rows with their instances |
| `World/World.cpp`, `World.h` | The options, and `RaidTier::LoadAtStartup()` before instances are packed |
| `Entities/Creature.cpp` | Health and weapon damage in `SelectLevel` |
| `Entities/Unit.cpp` | Spell damage in `SpellDamageBonusDone` and `MeleeDamageBonusDone` |
| `Loot/LootMgr.cpp`, `.h` | `LootTemplate::ProcessExtraDraw`, called from `Loot::FillLoot` for creature loot |

## Testing in game

Choose a tier for your account and a raid in the characters database, here tier 3 of Molten Core:

```sql
REPLACE INTO guildd_raid_tier (account, map, tier) VALUES (<account id>, 409, 3);
```

Enter Molten Core with a character of that account, as group leader or alone.
The log says `Raid tier: instance <id> of map 409 opens at tier 3`,
and `guildbot_instance_tier` has a row for the instance.

Target a creature and compare `.npc info` with its health outside the feature;
a Molten Giant at tier 3 has 0.40 x 1.30 = 0.52 of its usual health.
Watch its hits in the combat log against the same at `Raid.Tier.Enable = 0`.
Kill a boss and count the items; at tier 3 and the defaults, half the kills give one more.

To test tier 1 or a new tier, the instance must be new:
run `.instance unbind 409` outside the raid on every character that entered it, and form a new group,
because a group keeps its own bind; or wait for the reset.
