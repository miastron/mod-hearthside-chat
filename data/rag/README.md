# `data/rag` — static world knowledge

The paragraphs a bot is handed when it needs to know something about the game world. Retrieval
lives in [`src/hs_rag.h`](../../src/hs_rag.h) / [`hs_rag.cpp`](../../src/hs_rag.cpp); the SQL load
in [`src/hs_rag_store.cpp`](../../src/hs_rag_store.cpp); the harness is
[`Tests/test_hs_rag.cpp`](../../Tests/test_hs_rag.cpp).

This closes the one grounding gap the module had. `hs_grounded` answers from live `Player*`/DB
state, `hs_topic_gate` states live facts about the bot, `hs_memory` recalls a specific player,
`hs_experience` recalls what the bot has been doing lately — none of them know anything about
Azeroth, so "where do I train blacksmithing" used to reach the backend with nothing but a persona
line, and a 1–3B local model invented an answer.

## Provenance

The original ten `wow_*.json` files were copied from `mod-ollama-chat/data/rag/` (sibling module in
this workspace); only the data was taken, not the retrieval code (see "Why the scoring looks like
this" below). Four zone/city files were authored for this module directly.

All of it has since been fact-checked and corrected against the live realm (DBCs, quest tables,
`instance_encounters`) — wrong-expansion content, invented racial abilities, and similar errors were
rewritten to match WotLK 3.3.5a. The corrected files are the current source of truth; nothing about
that history needs to be re-derived to author a new entry. `Tests/verify_rag_against_realm.py`
re-runs the checks that can be automated (city mechanics, zone level ranges) against
`10.0.10.40` over SSH.

**Known gap:** entries nobody has specifically fact-checked still carry the inherited files' overly
broad keyword lists (see Rule 2 below). Tightening them would help, but do it with the test harness
open — changing a keyword list changes which entry wins ties on unrelated queries.

## Instances and bosses are generated, not hand-written

`wow_instances.json` and `wow_bosses.json` are emitted by `build_instances_and_bosses.py`.
**Edit that script, not the JSON** — regenerating overwrites both files.

Every proper noun in them comes off the realm, not memory:

| Fact | Source |
|---|---|
| Instance titles | `Map.dbc`, name column (field 5) |
| Level ranges | `LFGDungeons.dbc`, `targetLevelMin`/`Max` (fields 21/22) |
| Boss names | `acore_world.instance_encounters` ⋈ `creature_template` |

Only the one-sentence flavor clause per entry is hand-authored, and it's the part most worth
re-checking.

### Why the title has to be the `Map.dbc` string

`hs_queue.cpp` hands `req.topicGate.instanceName` to `Hs_RagContextForKeys`, and that value comes
from `Map::GetMapName()` — which is often not what a player would type:

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
failure with no visible symptom, which is why `test_hs_rag.cpp` asserts ten of these by their
`Map.dbc` spelling.

### Two deliberate omissions

- **Hodir has no boss entry.** A one-word title takes the full aboutness bonus from any query
  containing that word, so it was beating The Storm Peaks on "where do i find the sons of hodir."
  A boss entry needs the creature name to be retrievable by the death trigger, so it's one or the
  other — and the faction is asked about far more often than the raid encounter.
- **`wow_dungeons_raids.json` was deleted, not merged.** All of its topics are covered by the
  generated instance/boss entries with realm-verified data, and duplicate entries for the same topic
  split retrieval scoring and can hand the model two overlapping paragraphs. When replacing an
  entry like this, diff its **keywords** against the replacement, not just its title — a normalized
  match (`Normalize()`) can still miss a legitimate keyword like `"van cleef"` (players type it as
  two words; the creature is `Edwin VanCleef`, one token after normalization). Recovered keywords
  like that go through `EXTRA_KEYWORDS` in the builder script.

### Coverage

Complete for Wrath instances and bosses. Classic and Burning Crusade have full instance entries
with rosters, but per-boss entries only for bosses players actually name (Ragnaros, Nefarian,
Hakkar, C'Thun, Illidan, Kael'thas, Kil'jaeden, Archimonde, and similar) — a mid-tier Classic
dungeon boss retrieves nothing today, and a death against one just falls back to a bare trigger.

## The 30 spec entries are hand-written, and their abilities are realm-verified

`wow_specs.json` carries one entry per WotLK talent tree. Unlike the instance and boss files it
is hand-authored, so it has its own verification step rather than a generator:

| Claim | Checked against |
|---|---|
| the ability exists in 3.3.5a | `Spell.dbc`, name field (auto-calibrated, not hardcoded) |
| the ability belongs to that tree | `SkillLineAbility.dbc` — `SkillLine.dbc` |

[`Tests/verify_spec_abilities.py`](../../Tests/verify_spec_abilities.py) runs both over
[`Tests/spec_ability_names.txt`](../../Tests/spec_ability_names.txt) on the realm. **Both checks
matter.** `Spell.dbc` alone only proves a name exists: `Vendetta` is a real 3.3.5a spell name and
is not the rogue ability, so name-existence is weak evidence by itself. In WotLK the per-class
spell skill lines are named after the talent trees, so `SkillLineAbility.dbc` answers the claim
the entry actually makes. Druid `Swipe` was dropped for failing exactly this second check — it
resolves only to `Pet - Bear`.

Two authoring conventions specific to this file:

- **Title is `"<Spec> <Class>"`**, never the bare spec word. Both halves earn aboutness, which is
  what lets `"how do i play frost mage"` beat *Mage Class* without `frost` having to carry it
  alone.
- **No bare spec nickname in `keywords`.** `resto`, `prot`, `bm` and friends are on
  `IsAmbiguousTerm` and are inert as a sole handle regardless, so listing them buys nothing and
  costs specificity (Rule 2). List the disambiguated pair instead — `"resto shaman"`,
  `"bm hunter"`, `"prot pally"`. Ability names are the best handles these entries have:
  `"whats killing spree"` retrieves *Combat Rogue* at 0.92 off the ability name alone.

**Watch the prose, not just the keywords.** Content no longer confers eligibility, but it still
contributes to coverage, and two collisions surfaced this way during authoring: Affliction's DoTs
originally "roll on the target", which made `"should i roll a warlock or a mage"` retrieve
*Affliction Warlock* over *Warlock Class*, and an Arcane entry described a "running judgement",
borrowing a paladin ability word. Both were reworded.

**These entries serve player questions, not the generator.** `hs_generator.cpp` buckets on
`class_tag`/`faction_tag`/`zone_tag` and has no spec bucket, so a spec entry is reachable only
through scored retrieval against something a player actually typed.

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
it's carried for future filtering only.

`id` must be unique across every file in this folder. Nothing enforces that at load time; the test
harness checks it, and a duplicate id silently double-weights a topic.

## Authoring rules

Each of these came from a measured retrieval failure, not a style preference.

**1. Title it the way a player would say it.** `hs_rag.cpp` scores a title match above a keyword
match, so title an entry with what it's *about* ("Blacksmithing", "Molten Core", "Stormwind City"),
not a category label.

> **Exception:** wherever a caller passes `Hs_RagContextForKeys` a string the *game* supplied, the
> title must be that exact string, even when players say something else — instances are titled
> from `Map.dbc`, bosses from `creature_template`. See "Instances and bosses are generated" above.
> Put the player-facing name in `keywords` instead.

**2. Keep `keywords` tight.** List a keyword only if someone asking about that term should
plausibly get *this* paragraph. A long keyword list dilutes the entry's specificity bonus — the
inherited files list every topic an entry merely *mentions*, which is why some terms match a dozen
entries at once and can't discriminate between them.

**3. Multi-word keywords are cheap precision.** A verbatim phrase match against the query earns a
small bonus — prefer `"sentinel hill"` over `"sentinel"`.

**4. Content is prose the model will paraphrase.** Two to four sentences, factual and
self-contained — it's injected as "Things you know about Azeroth: `<title>` -- `<content>`". Long
entries are truncated at the `maxChars` budget on a word boundary.

**5. Adding entries changes every existing score.** IDF is corpus-wide, so re-run
`pwsh -File Tests\run_cpp_tests.ps1 -Filter rag` after any edit — the harness asserts real
question→entry outcomes and prints the current separation margin (below).

**6. A word that appears only in an entry's prose is not a handle.** Eligibility (below) counts
title and keyword matches only. *Razorfen Downs* was retrieved by "there was a fire down the
street last night" because `fire` sits inside *Mordresh Fire Eye* in its content — prose
corroborates a hit, it never establishes one. If a term should be able to fetch an entry, put it
in `keywords`.

## The threshold and its margin

Ships at `minScore = 0.45`, `maxEntries = 2`. The harness prints something like:

```
Separation: worst answerable question 0.524, best social-chat near-miss 0.412 (threshold 0.45)
```

Answerable questions must stay above the threshold; social chat must stay below it. **The margin
does not reliably shrink as the corpus grows** — more entries also raise IDF's ability to
discriminate, so growth has both widened and narrowed the gap at different points. Read that line
after every data change rather than assuming a trend.

If the margin ever inverts, the fix is tighter keywords on the offending entry (Rule 2), not a
lower threshold — a wrong reference block derails an otherwise-fine reply, which is worse than
retrieving nothing. A subtler failure than an over-broad entry stealing a query: a short query can
reduce to a single surviving term that happens to sit in several entries' *prose* but nobody's
`keywords`, in which case ties break on `id` and an unrelated entry wins. The fix is the same —
add the term to the one entry that should own it, promoting it to keyword weight.

## The eligibility gate

The threshold answers "is this a good enough match." It cannot answer "is this a match at all,"
and the two come apart badly on short input. Because the score normalizes by *query* mass, one
matched term in a short sentence scores high — which is the whole point (`"how do i get to
dalaran"` retrieves Dalaran at 1.36 on a single term) and also the whole problem. Measured on the
shipping corpus, before this gate existed:

```
"my arms are killing me"                      → Battleground Rules      0.48
"i work in fire safety"                       → Midsummer Fire Festival 0.57
"there was a fire down the street last night" → Razorfen Downs          0.50
```

No threshold separates those from real questions — they outscore `"where do i train
blacksmithing"` (0.60). What separates them is that each hit exactly **one** authored handle, and
that handle was a word doing ordinary English duty.

So `hs_rag.cpp` gates on handles, independently of score. An entry is returned only when the
query:

- hit a **title or keyword** term (never prose — Rule 6) that is **not** on
  `IsAmbiguousTerm`'s list, **or**
- hit **two** handles of any kind — which is what keeps `"should i go cat or bear"` working,
  since neither word identifies anything alone but the pair plainly does.

`IsAmbiguousTerm` holds two kinds of word and treats them identically: ordinary English that is
also a retrieval handle (`holy`, `arms`, `fire`, `down`), and spec words or abbreviations shared
by more than one class (`frost` is mage and death knight, `restoration` is druid and shaman,
`prot` is warrior and paladin, `bm` is Beast Mastery *and* Black Morass). Corpus IDF cannot find
the first kind — it measures rarity **in this corpus**, not in English, and `blacksmithing`
and `holy` sit at the same IDF here — so the list has to be authored.

**Grow that list only from a measured collision.** A word added to it loses the ability to answer
on its own. Generic fantasy nouns (`light`, `storm`, `dark`) were tried and deliberately left
off: none produced a false positive, and `"where is the dark portal"` needs `dark` pulling its
weight. [`Tests/test_hs_rag_ambiguity.cpp`](../../Tests/test_hs_rag_ambiguity.cpp) pins both
directions and is the file to run after any edit to the list — its section 3 exists
specifically to catch a real question going quiet.

Being listed costs an entry nothing when the query also carries a real handle: `"shadow priest"`
(1.16), `"holy paladin"` (1.15), `"hows blood furnace"` (0.95) and `"is the blood queen hard"`
(0.87) all resolve normally.

## Why the scoring looks like this

`mod-ollama-chat`'s RAG scores cosine similarity over a corpus-wide vocabulary, normalized by the
*entry's* length — which means a short chat question against a long entry scores too low to clear
a sane threshold. `hs_rag.cpp` normalizes by the **query's** information mass instead: the score
reads as "what share of what the player asked about does this entry cover," independent of entry
length, plus small aboutness/phrase/specificity bonuses. It isn't clamped, so a strong hit can
exceed 1.0.

## How this reaches a bot

**Edit the JSON, then regenerate the SQL:**

```
python data/rag/generate_rag_sql.py     # writes data/sql/db-characters/base/hside_rag.sql
pwsh -File Tests\run_cpp_tests.ps1 -Filter rag
```

Then re-apply the SQL on the realm and `.reload config` — `HsRagLifecycleWorldScript` re-reads the
table on reload, no restart needed.

`hside_rag.sql` is generated and is the one `base/*.sql` file in this module safe to change and
re-apply: it opens with `DELETE FROM hside_rag`, which is what makes a regeneration a clean reload
rather than a duplicate-key crash on AzerothCore's changed-hash re-apply behavior (see repo
`CLAUDE.md`'s SQL section). Don't hand-edit it — the JSON is the source of truth, and
`generate_rag_sql.py` refuses to emit on a duplicate `id` or malformed JSON.

## Who retrieves, and how

| Caller | Accessor | Why |
|---|---|---|
| Direct replies, event reactions | `Hs_RagContextFor` (scored) | The key is the player's own words — free text has to be scored. |
| Inside a named instance | `Hs_RagContextForKeys` | The map name came from the game. |
| Generator: zone/class/faction buckets | `Hs_RagContextForKeys` | The bucket label came from the server. |
| Generator: untagged and level-band buckets | `Hs_RagContextRandom` | No label to address and no query to score — one entry drawn per cycle. |
| Generator: bot-to-bot script, turn 1 | `Hs_RagContextRandom` | Opens on a fixed trigger — nothing to retrieve against yet. |
| Generator: bot-to-bot script, turns 2+ | `Hs_RagContextFor` (scored) | Retrieves against the turn being replied to, behind a term floor (below). |

**Prefer the keyed accessor whenever the caller already knows what it wants.** A zone name from
`AreaTable` or a map name from `Map::GetMapName` is an address, not a query — matching it by id or
normalized title skips the threshold (and the narrowing margin) entirely. Scoring a name you
already have can only introduce a near-miss.

**A short generated line is not a query.** Scoring normalizes by query mass, so a one-term line
scores that term at full weight and can retrieve nonsense with high confidence — measured examples:
`"good run."` retrieved *Stratholme* at 0.820, `"nice, one down."` retrieved *Razorfen Downs* at
0.647, both above genuine zone matches. No threshold catches this; term count does.
`Hs_RagQueryTermCount` is the guard, and `hs_generator.cpp` requires three surviving terms before
treating a script turn's own text as a query. This does not apply to a player's message — someone
who types two words meant those two words.

**The random draw is not a fallback, it's the answer to "no query exists."** Before this module had
one, several generator categories and every script's opening turn were prompted with no ground
truth at all — that's where invented game vocabulary came from. A drawn entry isn't necessarily
*about* the bucket, but the generator frames it as background detail, and an ordinary line informed
by a real mechanic beats a fluent one about a mechanic that doesn't exist.

The generator is the safer of the two RAG consumers and is on by default
(`Rag.Generator.Enable`); a wrong paragraph there still has to clear the quality gate and dedup
before a player ever sees it. The reactive path has no such buffer — a bad retrieval there is
already in the chat window — so it's the more cautious knob to turn on.

Config: the six `HearthsideChat.Rag.*` keys, documented in
[`conf/mod_hearthside_chat.conf.dist`](../../conf/mod_hearthside_chat.conf.dist).
