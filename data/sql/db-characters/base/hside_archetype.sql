-- PLAN.md §4.11 "The archetype enum". New 2026-08-21: moved out of a
-- compiled C++ table (hs_archetype.cpp) into SQL, closing the gap between
-- §4.11's own text ("weight is per-archetype config so it can be retuned
-- without touching bots") and the code, which had never actually made it
-- configurable. Loaded once into memory at startup by
-- hs_archetype_store.cpp's Hs_LoadArchetypesFromDb() -- `.reload config`
-- re-reads it too. enum_name must match hs_archetype.cpp's kEnumNames
-- exactly; the fifteen rows below are that fixed set, values from PLAN.md's
-- archetype table rebalanced 2026-08-21 (CASUAL narrowed from 28 to 13,
-- the rest widened a little, two new TROLL entries added).
--
-- talks_about, care, reply_chance, verbosity_cap, spawn_weight, min_level,
-- max_level carry PLAN.md §4.11/§4.13's meaning unchanged. profanity_level
-- (0 none, 1 light, 2 vulgar) is new: TROLL_MILD/TROLL_AGGRESSIVE only,
-- consumed by hs_archetype.cpp's Hs_ArchetypePromptLine to append a
-- profanity directive that is explicitly scoped to the game -- gear,
-- rotations, loot, other players' choices -- and never the real person on
-- the other end of the conversation.

CREATE TABLE IF NOT EXISTS `hside_archetype` (
  `enum_name`              VARCHAR(32) NOT NULL COMMENT 'must match hs_archetype.cpp''s fixed enum -- unrecognized names are logged and ignored',
  `talks_about`            VARCHAR(255) NOT NULL COMMENT 'PLAN.md §4.11 "Talks about" column, verbatim',
  `care`                   FLOAT NOT NULL COMMENT '§4.11 style-pass baseline, 0.0-1.0, before combat offset/GUID jitter',
  `reply_chance`           FLOAT NOT NULL COMMENT '0.0-1.0 -- stored, not wired to a consumer yet (§4.15 doesn''t read archetype)',
  `verbosity_cap`          SMALLINT UNSIGNED NOT NULL COMMENT 'tokens -- a GPU budget, not a hard chop (§4.11)',
  `spawn_weight`           SMALLINT UNSIGNED NOT NULL COMMENT 'out of 100 across all fifteen rows',
  `has_abbrev_override`    TINYINT(1) NOT NULL DEFAULT 0,
  `abbrev_override_chance` FLOAT NOT NULL DEFAULT 0.0 COMMENT 'meaningful only when has_abbrev_override = 1',
  `min_level`              TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '§4.13 eligibility -- 0 = no lower bound',
  `max_level`              TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT '255 = no upper bound',
  `profanity_level`        TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 none, 1 light (damn/hell/crap-tier), 2 vulgar -- TROLL_MILD/TROLL_AGGRESSIVE only',
  PRIMARY KEY (`enum_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO `hside_archetype`
  (`enum_name`, `talks_about`, `care`, `reply_chance`, `verbosity_cap`, `spawn_weight`,
   `has_abbrev_override`, `abbrev_override_chance`, `min_level`, `max_level`, `profanity_level`)
VALUES
('RAIDER_SERIOUS',   'progression, parses, consumables; corrects others harshly, dismisses casuals, contemptuous',                               0.90, 1.00, 30, 4,  0, 0.0,  60,  255, 1),
('RAIDER_CASUAL',    'raid nights, wipes, loot, guild turnover/drama; cynical about officers, grumbles about teammates, never self-deprecating', 0.75, 1.00, 30, 5,  0, 0.0,  60,  255, 0),
('PVP_SERIOUS',      'rating, comps, matchups; contemptuous of bad players, condescending about skill, never doubts own skill',                  0.55, 1.00, 25, 4,  0, 0.0,  70,  255, 2),
('PVP_CASUAL',       'bgs, gearing up, light trash talk, winning; competitive edge, confident trash talk instead of self-doubt',                 0.35, 1.00, 25, 6,  0, 0.0,  10,  255, 1),
('TRADER',           'AH prices, mats, flips',                                                                                                   0.55, 1.00, 25, 7,  1, 0.70, 30,  255, 0),
('LOOTGOBLIN',       'drops, rolls, gold, need-vs-greed',                                                                                        0.30, 1.00, 20, 7,  0, 0.0,   0,  255, 0),
('CASUAL',           'whatever is in front of them - quests, alts, patch talk, other games',                                                     0.45, 1.00, 30, 13, 0, 0.0,   0,  255, 0),
('GRUMPY_VETERAN',   'vanilla was better, complains, corrects people',                                                                           0.70, 1.00, 25, 6,  0, 0.0,  60,  255, 1),
('LONE_WOLF',        'little; answers reluctantly, never opens',                                                                                 0.50, 0.75, 20, 9,  0, 0.0,   0,  255, 0),
('MENTOR',           'explains mechanics, answers new players',                                                                                  0.85, 1.00, 40, 5,  0, 0.0,  60,  255, 0),
('YOUNG_APPRENTICE', 'asks questions, excited, lost',                                                                                            0.25, 1.00, 25, 10, 0, 0.0,   0,  29,  0),
('SOCIALITE',        'greets, small talk, guild-chat glue',                                                                                      0.45, 1.00, 30, 8,  0, 0.0,   0,  255, 0),
('DISTRACTED',       'half-present - "brb", "sorry was afk"',                                                                                    0.30, 0.60, 15, 13, 0, 0.0,   0,  255, 0),
('TROLL_MILD',       'sarcastic, backhanded compliments, contrarian nitpicking about gameplay',                                                  0.50, 1.00, 25, 2,  0, 0.0,   0,  255, 1),
('TROLL_AGGRESSIVE', 'openly hostile about specs, rotations, loot decisions; picks fights, dismissive',                                          0.25, 1.00, 30, 1,  0, 0.0,   0,  255, 2);
