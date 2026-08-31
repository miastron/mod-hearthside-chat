-- PLAN.md §4.11 "The archetype enum". New 2026-08-21: moved out of a
-- compiled C++ table (hs_archetype.cpp) into SQL, closing the gap between
-- §4.11's own text ("weight is per-archetype config so it can be retuned
-- without touching bots") and the code, which had never actually made it
-- configurable. Loaded once into memory at startup by
-- hs_archetype_store.cpp's Hs_LoadArchetypesFromDb(): `.reload config`
-- re-reads it too. enum_name must match hs_archetype.cpp's kEnumNames
-- exactly; the thirteen rows below are that fixed set, values from PLAN.md's
-- archetype table rebalanced 2026-08-21 (CASUAL narrowed from 28 to 13,
-- the rest widened a little, two new TROLL entries added), then twice more
-- on 2026-08-24: DISTRACTED and LONE_WOLF were both dropped: "half-present"
-- and "answers reluctantly" are per-reply behaviors every personality should
-- show occasionally, not personalities of their own: and their combined
-- weight of 22 was redistributed across the remaining rows (largest share to
-- CASUAL and SOCIALITE, the two archetypes closest in spirit to what was
-- removed). No `updates/` delta was needed for either drop: the test realm's
-- character DB was wiped before this edit landed, so `base/` alone reaches
-- it fresh on the next boot: see CLAUDE.md's `base/`-vs-`updates/` section
-- if a *populated* realm ever needs this same change applied as a delta.
--
-- talks_about, care, verbosity_cap, spawn_weight, min_level, max_level carry
-- PLAN.md §4.11/§4.13's meaning unchanged.
--
-- distracted_chance replaced reply_chance outright on 2026-08-24 (same edit
-- that dropped DISTRACTED/LONE_WOLF above). reply_chance modelled "this
-- personality often doesn't answer," which is true of a real player but reads
-- as being ignored: or as a broken module: on a realm that is almost
-- entirely bots; every row had already been raised to 1.00 for live testing,
-- making the arbiter's roll a no-op gate. The replacement expresses the same
-- half-present trait as a *late* reply instead of a missing one: on a hit,
-- hs_queue.cpp delivers a canned "sorry, was afk" line, then the real reply a
-- full typing delay after it. Values are deliberately low single digits --
-- HearthsideChat.Distracted.CooldownSeconds bounds how often one bot can do
-- this to the same player regardless of what is set here.
--
-- profanity_level
-- (0 none, 1 light, 2 vulgar) is new: TROLL_MILD/TROLL_AGGRESSIVE only,
-- consumed by hs_archetype.cpp's Hs_ArchetypePromptLine to append a
-- profanity directive. Was scoped to "never at the real person you're
-- talking to" originally; that scoping was dropped 2026-08-24: see
-- Claude/PLAN-TUNING.md §3: profanity may now be aimed at the listener,
-- gated only by profanity_level's intensity.
--
-- typing_base_ms/typing_per_char_ms (hs_queue.cpp's tier-2 typing-delay
-- formula, replacing the flat global HearthsideChat.TypingDelay.BaseMs/
-- PerCharMs) and verbosity_cap's values (raised +15 tokens across every row
-- 2026-08-30 against the llama-3.2-1b-instruct fine-tune's own eos habits:
-- verified empirically, truncated_pct 28% -> 3% with no diversity/role-leak
-- regression, see git history for the benchmark) were folded in here too,
-- since the test realm's character DB was wiped before either change
-- landed: see CLAUDE.md's `base/`-vs-`updates/` section.

CREATE TABLE IF NOT EXISTS `hside_archetype` (
  `enum_name`              VARCHAR(32) NOT NULL COMMENT 'must match hs_archetype.cpp''s fixed enum; unrecognized names are logged and ignored',
  `talks_about`            VARCHAR(255) NOT NULL COMMENT 'PLAN.md §4.11 "Talks about" column, verbatim',
  `care`                   FLOAT NOT NULL COMMENT '§4.11 style-pass baseline, 0.0-1.0, before combat offset/GUID jitter',
  `distracted_chance`      FLOAT NOT NULL COMMENT '0.0-1.0, per-reply odds of a "sorry, was afk" line before the real reply (hs_queue.cpp); 0.0 disables for this archetype',
  `verbosity_cap`          SMALLINT UNSIGNED NOT NULL COMMENT 'tokens: a GPU budget, not a hard chop (§4.11)',
  `spawn_weight`           SMALLINT UNSIGNED NOT NULL COMMENT 'out of 100 across all thirteen rows',
  `has_abbrev_override`    TINYINT(1) NOT NULL DEFAULT 0,
  `abbrev_override_chance` FLOAT NOT NULL DEFAULT 0.0 COMMENT 'meaningful only when has_abbrev_override = 1',
  `min_level`              TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '§4.13 eligibility: 0 = no lower bound',
  `max_level`              TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT '255 = no upper bound',
  `profanity_level`        TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 none, 1 light (damn/hell/crap-tier), 2 vulgar, TROLL_MILD/TROLL_AGGRESSIVE only',
  `typing_base_ms`         SMALLINT UNSIGNED NOT NULL DEFAULT 800 COMMENT 'hs_queue.cpp tier-2 typing delay: flat "notice and start typing" cost',
  `typing_per_char_ms`     SMALLINT UNSIGNED NOT NULL DEFAULT 45 COMMENT 'ms per character of the styled reply; capped by HearthsideChat.TypingDelay.MaxMs',
  PRIMARY KEY (`enum_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO `hside_archetype`
  (`enum_name`, `talks_about`, `care`, `distracted_chance`, `verbosity_cap`, `spawn_weight`,
   `has_abbrev_override`, `abbrev_override_chance`, `min_level`, `max_level`, `profanity_level`,
   `typing_base_ms`, `typing_per_char_ms`)
VALUES
-- distracted_chance rationale: focus is the axis. RAIDER_SERIOUS/PVP_SERIOUS/
-- MENTOR are attentive by definition and barely step away; CASUAL and
-- SOCIALITE sit highest, being the two rows that absorbed the retired
-- DISTRACTED/LONE_WOLF weight; TRADER and LOOTGOBLIN are plausibly alt-tabbed
-- (AH, loot tables) and YOUNG_APPRENTICE is simply scattered.
('RAIDER_SERIOUS',   'progression, parses, consumables; corrects others harshly, dismisses casuals, contemptuous',                               0.90, 0.02, 45, 4,  0, 0.0,  60,  255, 1, 600,  35),
('RAIDER_CASUAL',    'raid nights, wipes, loot, guild turnover/drama; cynical about officers, grumbles about teammates, never self-deprecating', 0.75, 0.05, 45, 5,  0, 0.0,  60,  255, 0, 800,  45),
('PVP_SERIOUS',      'rating, comps, matchups; contemptuous of bad players, condescending about skill, never doubts own skill',                  0.55, 0.02, 40, 4,  0, 0.0,  70,  255, 2, 600,  30),
('PVP_CASUAL',       'bgs, gearing up, light trash talk, winning; competitive edge, confident trash talk instead of self-doubt',                 0.35, 0.05, 40, 9,  0, 0.0,  10,  255, 1, 750,  40),
('TRADER',           'AH prices, mats, flips',                                                                                                   0.55, 0.08, 40, 9,  1, 0.70, 30,  255, 0, 650,  35),
('LOOTGOBLIN',       'drops, rolls, gold, need-vs-greed',                                                                                        0.30, 0.07, 35, 10, 0, 0.0,   0,  255, 0, 600,  35),
('CASUAL',           'whatever is in front of them - quests, alts, patch talk, other games',                                                     0.45, 0.10, 45, 17, 0, 0.0,   0,  255, 0, 800,  45),
('GRUMPY_VETERAN',   'vanilla was better, complains, corrects people',                                                                           0.70, 0.05, 40, 8,  0, 0.0,  60,  255, 1, 900,  50),
('MENTOR',           'explains mechanics, answers new players',                                                                                  0.85, 0.02, 55, 5,  0, 0.0,  60,  255, 0, 1000, 55),
('YOUNG_APPRENTICE', 'asks questions, excited, lost',                                                                                            0.25, 0.08, 40, 12, 0, 0.0,   0,  29,  0, 900,  60),
('SOCIALITE',        'greets, small talk, guild-chat glue',                                                                                      0.45, 0.09, 45, 12, 0, 0.0,   0,  255, 0, 550,  35),
('TROLL_MILD',       'sarcastic, backhanded compliments, contrarian nitpicking about gameplay',                                                  0.50, 0.03, 40, 3,  0, 0.0,   0,  255, 1, 700,  40),
('TROLL_AGGRESSIVE', 'immature, openly hostile, rotations, loot decisions; picks fights, dismissive',                                            0.25, 0.01, 45, 2,  0, 0.0,   0,  255, 2, 500,  35);
