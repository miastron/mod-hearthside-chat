# `data/rag` — static world knowledge for the reactive tier

The paragraphs a bot is handed when a player asks a question about the game world. Retrieval
lives in [`src/hs_rag.h`](../../src/hs_rag.h) / [`hs_rag.cpp`](../../src/hs_rag.cpp); the harness
is [`Tests/test_hs_rag.cpp`](../../Tests/test_hs_rag.cpp).

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
were authored for this module and verified against the test realm. The inherited ten have **not**
been fact-checked and are left byte-for-byte as received, by operator decision.

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
Separation: worst answerable question 0.500, best social-chat near-miss 0.437 (threshold 0.45)
```

Answerable questions must stay above the threshold, social chat below it. That gap was 0.077 at
173 entries and 0.063 at 236 — **it narrows as the corpus grows**. Read that line after every data
change. If it inverts, the fix is tighter keywords on the offending entry, not a lower threshold:
a wrong reference block is worse than none, because it derails a reply that would otherwise have
been fine.

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

## Where this data is going

The shipping module will load this table from **SQL** (`hside_rag`), like every other authored
table in this module (`hside_archetype`, `hside_grounded_question`, `hside_corpus`) — not from
files. These JSON files are the authoring source the SQL seed is generated from, and what the
harness reads so retrieval quality is testable at corpus scale without a database.

Not built yet: the `hside_rag` table, `hs_rag_store.cpp`, the `HearthsideChat.Rag.*` config keys,
and the prompt wiring in `hs_queue.cpp`. **Nothing in this folder reaches a bot yet.**

## Known data defects

- `quest_lore_quests` (inherited) claims `"dark portal"` among ~50 keywords, so "where is the dark
  portal" ranks it 0.01 above `zone_blasted_lands`. The harness asserts the weaker
  "Blasted Lands is in the top 2" for that question. Left alone deliberately — tuning the scorer
  around one query is how a retriever ends up fitted to its own test set.
- The inherited files' keyword arrays are uniformly too broad (rule 2). Tightening them would
  likely improve precision across the board, but it means editing data the operator asked to leave
  as-is; treat it as a separate, deliberate decision rather than drive-by cleanup.
