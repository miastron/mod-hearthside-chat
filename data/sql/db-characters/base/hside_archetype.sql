-- The archetype table: personality parameters for playerbot chat, loaded
-- once into memory at startup by hs_archetype_store.cpp's
-- Hs_LoadArchetypesFromDb() (`.reload config` re-reads it too).
-- enum_name must match hs_archetype.cpp's kEnumNames exactly; the fifteen
-- rows below are that fixed set.
--
-- profanity_level (0 none, 1 light, 2 vulgar) is consumed by
-- hs_archetype.cpp's Hs_ArchetypePromptLine to append a profanity directive
-- scoped to the game (gear, rotations, loot, other players' choices), never
-- the real person on the other end of the conversation.

CREATE TABLE IF NOT EXISTS `hside_archetype` (
  `enum_name`              VARCHAR(32) NOT NULL COMMENT 'must match hs_archetype.cpp''s fixed enum -- unrecognized names are logged and ignored',
  `talks_about`            VARCHAR(255) NOT NULL COMMENT 'topics this archetype talks about',
  `care`                   FLOAT NOT NULL COMMENT 'style-pass baseline, 0.0-1.0, before combat offset/GUID jitter',
  `reply_chance`           FLOAT NOT NULL COMMENT '0.0-1.0 -- per-candidate roll in hs_arbiter.cpp; 0.0 mutes the archetype except when named directly',
  `verbosity_cap`          SMALLINT UNSIGNED NOT NULL COMMENT 'tokens -- a GPU budget, not a hard chop',
  `spawn_weight`           SMALLINT UNSIGNED NOT NULL COMMENT 'out of 100 across all fifteen rows',
  `has_abbrev_override`    TINYINT(1) NOT NULL DEFAULT 0,
  `abbrev_override_chance` FLOAT NOT NULL DEFAULT 0.0 COMMENT 'meaningful only when has_abbrev_override = 1',
  `min_level`              TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'eligibility bound -- 0 = no lower bound',
  `max_level`              TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT '255 = no upper bound',
  `profanity_level`        TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 none, 1 light (damn/hell/crap-tier), 2 vulgar -- TROLL_MILD/TROLL_AGGRESSIVE only',
  `typing_base_ms`         SMALLINT UNSIGNED NOT NULL DEFAULT 800 COMMENT 'hs_queue.cpp tier-2 typing delay: flat "notice and start typing" cost',
  `typing_per_char_ms`     SMALLINT UNSIGNED NOT NULL DEFAULT 45 COMMENT 'ms per character of the styled reply; capped by HearthsideChat.TypingDelay.MaxMs',
  PRIMARY KEY (`enum_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO `hside_archetype`
  (`enum_name`, `talks_about`, `care`, `reply_chance`, `verbosity_cap`, `spawn_weight`,
   `has_abbrev_override`, `abbrev_override_chance`, `min_level`, `max_level`, `profanity_level`,
   `typing_base_ms`, `typing_per_char_ms`)
VALUES
('RAIDER_SERIOUS',   'progression, parses, consumables; corrects people, dismissive of casuals', 0.90, 0.40, 30, 4,  0, 0.0,  60,  255, 1, 600,  35),
('RAIDER_CASUAL',    'raid nights, wipes, loot, guild drama',                                     0.75, 0.50, 30, 5,  0, 0.0,  60,  255, 0, 800,  45),
('PVP_SERIOUS',      'rating, comps, matchups; condescending about skill',                        0.55, 0.55, 25, 4,  0, 0.0,  70,  255, 2, 600,  30),
('PVP_CASUAL',       'bgs, gear, light trash talk',                                                0.35, 0.60, 25, 6,  0, 0.0,  10,  255, 1, 750,  40),
('TRADER',           'AH prices, mats, flips',                                                     0.55, 0.40, 25, 7,  1, 0.70, 30,  255, 0, 650,  35),
('LOOTGOBLIN',       'drops, rolls, gold, need-vs-greed',                                          0.30, 0.55, 20, 7,  0, 0.0,   0,  255, 0, 600,  35),
('CASUAL',           'whatever is in front of them - quests, alts, patch talk, other games',       0.45, 0.55, 30, 13, 0, 0.0,   0,  255, 0, 800,  45),
('GRUMPY_VETERAN',   'vanilla was better, complains, corrects people',                             0.70, 0.35, 25, 6,  0, 0.0,  60,  255, 1, 900,  50),
('LONE_WOLF',        'little; answers reluctantly, never opens',                                   0.50, 0.20, 20, 9,  0, 0.0,   0,  255, 0, 1400, 50),
('MENTOR',           'explains mechanics, answers new players',                                    0.85, 0.75, 40, 5,  0, 0.0,  60,  255, 0, 1000, 55),
('YOUNG_APPRENTICE', 'asks questions, excited, lost',                                               0.25, 0.70, 25, 10, 0, 0.0,   0,  29,  0, 900,  60),
('SOCIALITE',        'greets, small talk, guild-chat glue',                                         0.45, 0.85, 30, 8,  0, 0.0,   0,  255, 0, 550,  35),
('DISTRACTED',       'half-present - "brb", "sorry was afk"',                                      0.30, 0.15, 15, 13, 0, 0.0,   0,  255, 0, 1600, 60),
('TROLL_MILD',       'sarcastic, backhanded compliments, contrarian nitpicking about gameplay',     0.50, 0.65, 25, 2,  0, 0.0,   0,  255, 1, 700,  40),
('TROLL_AGGRESSIVE', 'openly hostile about specs, rotations, loot decisions; picks fights, dismissive', 0.25, 0.70, 30, 1, 0, 0.0,   0,  255, 2, 500,  35);
