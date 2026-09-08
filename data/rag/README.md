# `data/rag` — static world knowledge

The paragraphs a bot is handed when it needs to know something about the game world. Retrieval
lives in [`src/hs_rag.h`](../../src/hs_rag.h) / [`hs_rag.cpp`](../../src/hs_rag.cpp); the SQL load
in [`src/hs_rag_store.cpp`](../../src/hs_rag_store.cpp); the harness is
[`Tests/test_hs_rag.cpp`](../../Tests/test_hs_rag.cpp).

This closes the one grounding gap the module had. `hs_grounded` answers from live `Player*`/DB
state, `hs_topic_gate` states live facts about the bot, `hs_memory` recalls a specific player —
none of them know anything about Azeroth, so "where do I train blacksmithing" used to reach the
backend with nothing but a persona line, and a 1–3B local model invented an answer.

## Provenance

The original ten `wow_*.json` files were copied from `mod-ollama-chat/data/rag/` (sibling module
in this workspace) on 2026-09-07. **Only the data was taken, not the retrieval code** — see
"Why the scoring looks like this" below.

The four files added here on 2026-09-07 (`wow_cities.json`,
`wow_zones_eastern_kingdoms.json`, `wow_zones_kalimdor.json`, `wow_zones_outland_northrend.json`)
were authored for this module and verified against the test realm.

### Fact-check status (2026-09-07) — PARTIAL, and the gap matters

A pass over the inherited ten was started and **did not finish**: the agent doing it was cut off
by a rate limit partway, and produced no report. What that leaves:

**Corrected, and these were not small.** The inherited files carried content from the wrong
expansion — Shadowfang Keep listed its *Cataclysm* boss roster (Lord Godfrey, Baron Ashbury, Lord
Walden) instead of Arugal and Baron Silverlaine; Naxxramas was still a level-60 raid over the
Eastern Plaguelands; Onyxia was pre-3.2.2; pet battles (Mists of Pandaria) were described as a live
system; the Dungeon Finder was attributed to Cataclysm rather than patch 3.3; PvP was described
with the vanilla 1–14 rank grind; Archmage Antonidas (dead before Wrath) was named as Dalaran's
quest hub; and most racial traits were simply invented ("+10% Spirit", "six-armed" draenei).
Corrections landed in `wow_classes_factions.json`, `wow_dungeons_raids.json`, `wow_general_tips.json`,
`wow_items_equipment.json`, `wow_mechanics.json`, `wow_npcs_creatures.json`, `wow_pvp.json`.

**Added:** `wow_battlegrounds.json` (7), `wow_mechanics_wotlk.json` (6), `wow_world_events.json` (8).

**NOT done, and still open:**

- **No fact-check report exists**, so there is no record of which corrected claims were verified
  against the realm (`creature_template`, the DBCs) versus corrected from knowledge. Treat the
  above as improved but unaudited.
- The remaining inherited entries were not reached at all. Assume they still carry retail- or
  wrong-expansion content until someone checks.

`wow_bosses.json` and `wow_instances.json` were written separately, on 2026-09-07, and are
covered below rather than by the partial pass above.

`Tests/verify_rag_against_realm.py` re-runs the city/zone checks; it does not cover any of the
above.

## Instances and bosses are generated, not hand-written

`wow_instances.json` (73) and `wow_bosses.json` (92) are emitted by
`build_instances_and_bosses.py`. **Edit that script, not the JSON** — regenerating overwrites both
files, which has already caught one edit made to the wrong layer.

Every proper noun in them came off the realm rather than from memory:

| Fact | Source |
|---|---|
| Instance titles | `Map.dbc`, name column (field 5) |
| Level ranges | `LFGDungeons.dbc`, `targetLevelMin`/`Max` (fields 21/22) |
| Boss names | `acore_world.instance_encounters` ⋈ `creature_template` |

Only the one-sentence flavour clause per entry is authored, and it is the part most worth
re-checking.

### Why the title has to be the `Map.dbc` string

`hs_queue.cpp` hands `req.topicGate.instanceName` to `Hs_RagContextForKeys`, and that value comes
from `Map::GetMapName()`. The name the game uses is frequently *not* what a player says:

| `Map.dbc` (the address) | what players type |
|---|---|
| `Hellfire Citadel: Ramparts` | ramparts, hfr |
| `Coilfang: The Slave Pens` | slave pens, sp |
| `Auchindoun: Shadow Labyrinth` | shadow lab, slabs |
| `Opening of the Dark Portal` | black morass, bm |
| `Magister's Terrace` | magisters terrace, mgt |
| `Violet Hold` | **not** "The Violet Hold" |

So: **title = the `Map.dbc` address, keywords = what players call it.** Titling an entry to the
player-facing name passes every scored test and still misses every keyed lookup at runtime — a
failure with no visible symptom, which is why `test_hs_rag.cpp` now asserts ten of these by their
`Map.dbc` spelling.

### Two deliberate omissions

- **Hodir has no boss entry.** A one-word title takes the full aboutness bonus from any query
  containing the word, so it beat The Storm Peaks on "where do i find the sons of hodir". The title
  can't be disambiguated — a boss entry has to carry the creature name or the death trigger can't
  retrieve it — so it is one or the other, and the daily-quest faction is asked about far more often
  than the raid encounter.
- **`wow_dungeons_raids.json` was deleted**, not merged. All 21 of its topics are covered here with
  realm-verified data, and two entries for one topic is worse than a duplicate id: they split the
  IDF, compete for the same queries, and `maxEntries=2` can hand the model two overlapping
  paragraphs about the same dungeon. Its Stratholme entry also had the level range wrong (55-60;
  the DBC says 58-60).

  **Covering the topics is not the same as covering the handles, and the first pass conflated
  them.** An audit of the deletion found 133 keywords with no counterpart in the replacements. Most
  deserved to go — filler sitting on a dozen entries at once (`loot`, `experience`, `complex`),
  every `level NN` keyword (nearly all disagreeing with `LFGDungeons.dbc`; Naxxramas carried
  `level 60`), and outright errors (Shadowfang Keep's Cataclysm roster, `thunderfury` on Onyxia).
  But roughly forty were real, including `mine cart` — which rule 3 above cites as the model of a
  good multi-word keyword — and `van cleef`, which players type as two words while the creature is
  `Edwin VanCleef`, one token after normalisation. Those are restored through `EXTRA_KEYWORDS` in
  the builder.

  The lesson generalises past this one deletion: when replacing an entry, diff its **keywords**
  against the replacement's content and keywords under `Normalize()` semantics, not just its title.
  Apostrophes are where this bites — `Atal'ai` normalises to `{atal, ai}` and does not match a
  keyword written `atalai`.

### Coverage

Complete for Wrath instances and their bosses. Classic and Burning Crusade have full **instance**
entries with rosters in the content, but per-boss entries only for the ones players name
(Ragnaros, Nefarian, Hakkar, C'Thun, Illidan, Kael'thas, Kil'jaeden, Archimonde, and similar).
`instance_encounters` holds 578 encounters in total, so a bot killed by a mid-tier Classic dungeon
boss still retrieves nothing — which degrades to a bare death trigger, the pre-2026-09-07 behaviour.

## Schema

```json
{
  "id": "zone_westfall",
  "title": "Westfall",
  "content": "The paragraph handed to the model.",
  "keywords": ["westfall", "sentinel hill", "defias brotherhood"],
  "tags": ["zone", "alliance", "leveling"]
}
```

`Hs_SetRagTable` reads `id`, `title`, `content`, `keywords`. **`tags` is parsed by nothing today** —
it is carried for future filtering, so don't rely on it doing anything.

`id` must be unique across *every* file in this folder. Nothing enforces it at load time; the
harness checks it, and duplicate ids silently double-weight a topic.

## Authoring rules

These are not style preferences. Each one was learned from a measured retrieval failure.

**1. The title is the aboutness signal — weight it accordingly.** `hs_rag.cpp` scores a title match
above a keyword match. Title an entry with the thing it is *about*, as a player would say it
("Blacksmithing", "Molten Core", "Stormwind City"), not with a category label.

> **Exception — anything the code addresses by name.** Where a caller passes `Hs_RagContextForKeys`
> a string the *game* supplied, the title must be that string exactly, even when players say
> something else: instances are titled from `Map.dbc` and bosses from `creature_template`, because
> those are what `Map::GetMapName()` and `Creature::GetName()` return. See "Instances and bosses
> are generated" above, and rule 6 below. Player-facing names go in `keywords` instead, which
> costs only the difference between title and keyword weight.

**2. Keep `keywords` tight.** This is the big one. The inherited files list every topic an entry
*mentions*: "blacksmithing" is a keyword of **11** different entries, so keyword weight alone
cannot separate the Blacksmithing entry from Gold Making Methods. Add a keyword only if someone
asking about that term should plausibly be handed *this* paragraph. A long keyword list actively
degrades the entry — it dilutes the specificity bonus, which divides matched handles by the
entry's total handles.

**3. Multi-word keywords are cheap precision.** Phrases matched verbatim against the query earn a
small bonus ("cleft of shadow", "mine cart"), so prefer `"sentinel hill"` over `"sentinel"`.

**4. Content is prose the model will paraphrase.** Keep it factual and self-contained; it is
injected as "Things you know about Azeroth: <title> -- <content>". Two to four sentences. Long
entries are truncated at the `maxChars` budget, first-entry-only, on a word boundary.

**5. Adding entries changes every existing score.** IDF is corpus-wide. Re-run
`pwsh -File Tests\run_cpp_tests.ps1 -Filter rag` after any edit here — the harness asserts ~32
question→entry outcomes and prints the separation margin. Going from 173 to 236 entries shifted
every score without breaking an assertion, but that was luck as much as design.

## The threshold and its margin

Ships at `minScore = 0.45`, `maxEntries = 2`. The harness prints, at the end:

```
Separation: worst answerable question 0.524, best social-chat near-miss 0.412 (threshold 0.45)
```

Answerable questions must stay above the threshold, social chat below it. Measured:

| Entries | Worst answerable | Best near-miss | Margin |
|--------:|-----------------:|---------------:|-------:|
| 173     | —                | —              | 0.077  |
| 236     | 0.500            | 0.437          | 0.063  |
| 257     | 0.529            | 0.407          | 0.122  |
| 401     | 0.524            | 0.412          | 0.112  |

This README used to state flatly that the margin **narrows as the corpus grows**. The 257-entry
measurement falsifies that as a rule: the gap nearly doubled, and adding 166 instance and boss
entries on top of that barely moved it. Growth cuts both ways — more entries
raise IDF's power to discriminate (the best near-miss *fell*, 0.437 → 0.407), while also creating
more chances for a new entry to collide with an existing query. Which effect wins is a property of
the entries added, not of the count.

So the instruction is unchanged but the reasoning is not: **read that line after every data change**
because the margin is unpredictable, not because it is on a known downward slide.

If it inverts, the fix is tighter keywords on the offending entry, not a lower threshold: a wrong
reference block is worse than none, because it derails a reply that would otherwise have been fine.
Rule 2 governs the common case, where an over-broad entry steals a query — but the 236→257 growth
produced the *inverse* failure too, and it is worth knowing the shape of it. "anyone got a summon"
reduces to the single term `summon`, which sat in the **prose** of four entries and the `keywords`
of none; four-way ties at content weight break on `id`, so a Christmas event won a warlock
question. The fix there is to *add* the keyword to the one entry that should own the term, which
lifts it to keyword weight and earns it the specificity bonus. Tight keywords are not the same as
few keywords.

## Why the scoring looks like this

`mod-ollama-chat`'s RAG scores cosine similarity between term-frequency vectors over a corpus-wide
vocabulary. That divides by the *entry's* length, so a five-word chat question against a 55-word
entry scores ≈0.09 against a default threshold of 0.3 — it retrieves nothing on realistic chat
lines. `hs_rag.cpp` normalises by the **query's** information mass instead: the score reads as
"what share of what the player asked about does this entry cover", independent of entry length,
plus small aboutness/phrase/specificity bonuses. It is not clamped, so a strong hit exceeds 1.0.

## Verified against the realm (2026-09-07)

`Tests/verify_rag_against_realm.py` re-runs both checks below over SSH to `10.0.10.40`.

**City mechanics — confirmed exactly.** Parsed `/azerothcore-wotlk/env/dist/bin/dbc/AreaTable.dbc`
(2307 records, 36 fields, WotLK layout). Every ordinary zone carries `AREA_FLAG_ALLOW_DUELS`
(`0x40`); all ten capitals — Stormwind, Ironforge, Darnassus, The Exodar, Orgrimmar, Thunder
Bluff, Undercity, Silvermoon, Shattrath, Dalaran — carry `AREA_FLAG_CAPITAL` and **do not** carry
`ALLOW_DUELS`. Shattrath and Dalaran additionally carry `AREA_FLAG_SANCTUARY` (`0x800`, PvP
disabled); the other eight do not. Dueling is opt-in per area
([`SpellEffects.cpp:4121`](../../../azerothcore-wotlk-pb/src/server/game/Spells/SpellEffects.cpp#L4121)
sends `SPELL_FAILED_NO_DUELING` when the flag is absent), which is exactly what `city_rules_general`
asserts.

**Zone level ranges — 42 of 48 clean.** Compared each claimed range against the 15th–85th
percentile of `QuestLevel` per `QuestSortID` in `acore_world.quest_template`. The six outliers
(Tanaris, Searing Gorge, Thousand Needles, Hillsbrad, Duskwood, Felwood) are **artifacts of the
metric, not errors**: they are endgame chains anchored in low-level zones. Tanaris's level-60 tail
is the Ahn'Qiraj scepter questline ("The Path of the Conqueror"), Searing Gorge's is the Thorium
Brotherhood rep grind. Verified by inspection, not assumed. Two claims were corrected to match
realm data: Dalaran 74→75 (`AreaTable` exploration level), Sholazar Basin 75-78→76-78.

Sparse-zone claims also confirmed: Deadwind Pass has **1** quest sorted to it and Crystalsong
Forest **0**, which is why both entries say the zone exists mostly as a transit corridor.

**Still unverified:** the inherited ten files entirely, and the per-city service lists (bank,
auction house, barber shop) in `wow_cities.json`, which came from knowledge rather than a query.

## How this reaches a bot

**Edit the JSON, then regenerate the SQL:**

```
python data/rag/generate_rag_sql.py     # writes data/sql/db-characters/base/hside_rag.sql
pwsh -File Tests
un_cpp_tests.ps1 -Filter rag
```

Then re-apply the SQL on the realm and `.reload config` (`HsRagLifecycleWorldScript` re-reads the
table on reload, so no restart).

`hside_rag.sql` is a **generated file** and the one `base/*.sql` in this module that is safe to
change and re-apply. It opens with `DELETE FROM hside_rag`, precisely because AzerothCore
re-applies a `base/` file whose SHA1 changed (repo `CLAUDE.md`) — so a regeneration reloads the
corpus instead of failing on a duplicate key the way every other, hand-authored `base/hside_*.sql`
would. Do not hand-edit it; the JSON is the source of truth.

`generate_rag_sql.py` refuses to emit on a duplicate `id` or malformed JSON, which is the only
place that check happens — nothing enforces it at load time.

## Who retrieves, and how

Two different access patterns, and the difference matters more than it looks:

| Caller | Accessor | Why |
|---|---|---|
| Direct replies, event reactions | `Hs_RagContextFor` (scored) | The key is the player's own words. Free text, so it has to be scored. |
| Inside a named instance | `Hs_RagContextForKeys` | The map name came from the *game*. |
| Generator: zone/class/faction buckets | `Hs_RagContextForKeys` | The bucket label came from the *server*. |
| Generator: bot-to-bot script turns | `Hs_RagContextFor` (scored) | Retrieves against the turn being replied to. |

**Prefer the keyed accessor whenever the caller already knows what it wants.** A zone name from
`AreaTable` or a map name from `Map::GetMapName` is an *address*, not a query — matching it by id
or normalized title skips the threshold entirely, and so skips the narrowing separation margin
below. Scoring a name you already have can only introduce a near-miss.

The **generator** is the safest consumer and is on by default (`Rag.Generator.Enable`) while the
reactive path is the second knob to turn: a wrong paragraph there produces a candidate that still
has to clear the quality gate, placeholder discipline and dedup, lands in `hside_corpus` tagged
with the run's `prompt_version`, and can be reviewed and evicted wholesale before a player sees
it. The same mistake on the reactive path is already in the chat window.

Config: the six `HearthsideChat.Rag.*` keys, documented in
[`conf/mod_hearthside_chat.conf.dist`](../../conf/mod_hearthside_chat.conf.dist).

## Known data defects

- `quest_lore_quests` (inherited) claims `"dark portal"` among ~50 keywords, so "where is the dark
  portal" ranks it 0.01 above `zone_blasted_lands`. The harness asserts the weaker
  "Blasted Lands is in the top 2" for that question. Left alone deliberately — tuning the scorer
  around one query is how a retriever ends up fitted to its own test set.
- The inherited files' keyword arrays are uniformly too broad (rule 2). Tightening them would
  likely improve precision across the board, but it means editing data the operator asked to leave
  as-is; treat it as a separate, deliberate decision rather than drive-by cleanup.
