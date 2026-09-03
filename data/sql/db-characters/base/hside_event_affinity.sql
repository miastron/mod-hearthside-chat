-- Claude/archive/PLAN-ARBITER.md §2: per-event archetype affinity for the event arbiter
-- (hs_event_arbiter.h). Loaded into memory at startup and on `.reload
-- config` by hs_event_affinity_store.cpp's Hs_LoadEventAffinityFromDb().
--
-- This table authors ONLY the exceptions. Any (event_type, archetype) pair
-- with no row here weighs 1.0, so an archetype that should react to an
-- event about as often as anyone else needs no row at all. Weight is one
-- factor of a four-way product: proximity x recency x involvement x
-- affinity: so it shapes the draw, it does not decide it: a 0.3 archetype
-- standing alone still speaks, and a 3.0 one that answered ten seconds ago
-- usually loses to a quiet bystander.
--
-- Weight 0.0 is the deliberate "never speaks to this event" floor; the
-- arbiter's cumulative selection can never draw a zero-weight candidate.
-- Negative weights are clamped to 0 at load with an error, since a negative
-- would corrupt that selection rather than express a preference.
--
-- `event_type` must match hs_event_arbiter.h's HsEventType enum name
-- (kEventTypeNames), and `archetype` must match an hside_archetype row's
-- `enum_name`: both are validated at load, and a row failing either is
-- skipped and logged rather than silently applied.
--
-- ⚠️ This is a runtime-draw weighting, NOT a training-data partition. The
-- fine-tune matrix (Claude/finetune/matrix/event.txt) stays fully crossed:
-- every archetype gets a row for every trigger, including the ones it is
-- weighted *down* for here. If TRADER only ever appeared next to loot
-- events in the dataset the model would predict the reply from the event
-- type and stop reading the `Archetype:` tag at all: and the one time
-- affinity does draw TRADER for a duel, it would have nothing to draw
-- on. See PLAN-TUNING.md §1b and Claude/archive/PLAN-ARBITER.md §2's warning.
--
-- The four OPENER_* event types are reserved: hs_opener.cpp's openers are
-- still tier-1 corpus and no fire site routes to them, so rows here for
-- those types are inert until Claude/archive/PLAN-ARBITER.md §8's migration question is
-- settled. They are seeded anyway so the vocabulary is complete in one
-- place.

CREATE TABLE IF NOT EXISTS `hside_event_affinity` (
  `event_type` VARCHAR(48) NOT NULL COMMENT 'must match hs_event_arbiter.h''s HsEventType enum name',
  `archetype`  VARCHAR(32) NOT NULL COMMENT 'hside_archetype.enum_name',
  `weight`     FLOAT NOT NULL DEFAULT 1.0 COMMENT '1.0 = no preference; 0.0 = never speaks to this event',
  PRIMARY KEY (`event_type`, `archetype`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO `hside_event_affinity` (`event_type`, `archetype`, `weight`) VALUES

-- ---- Deaths -------------------------------------------------------------
-- Dying is a performance question for the raid archetypes and a grievance
-- for the veteran; the trader and the loot goblin have nothing invested in
-- it beyond the repair bill.
('DEATH_IN_GROUP',     'RAIDER_SERIOUS',   1.7),
('DEATH_IN_GROUP',     'GRUMPY_VETERAN',   1.6),
('DEATH_IN_GROUP',     'MENTOR',           1.4),
('DEATH_IN_GROUP',     'TROLL_AGGRESSIVE', 1.5),
('DEATH_IN_GROUP',     'TROLL_MILD',       1.3),
('DEATH_IN_GROUP',     'YOUNG_APPRENTICE', 1.2),
('DEATH_IN_GROUP',     'TRADER',           0.5),
('DEATH_IN_GROUP',     'SOCIALITE',        0.7),

('DEATH_WIPE',         'RAIDER_SERIOUS',   2.0),
('DEATH_WIPE',         'RAIDER_CASUAL',    1.5),
('DEATH_WIPE',         'GRUMPY_VETERAN',   1.8),
('DEATH_WIPE',         'MENTOR',           1.6),
('DEATH_WIPE',         'TROLL_AGGRESSIVE', 1.7),
('DEATH_WIPE',         'TROLL_MILD',       1.4),
('DEATH_WIPE',         'TRADER',           0.6),

-- Someone else died: this is the consoling/coaching slot, so the social
-- archetypes outrank the ones that would just analyse it.
('DEATH_GROUP_PLAYER', 'MENTOR',           2.0),
('DEATH_GROUP_PLAYER', 'SOCIALITE',        1.8),
('DEATH_GROUP_PLAYER', 'RAIDER_SERIOUS',   1.3),
('DEATH_GROUP_PLAYER', 'TROLL_MILD',       1.3),
('DEATH_GROUP_PLAYER', 'TROLL_AGGRESSIVE', 1.2),
('DEATH_GROUP_PLAYER', 'TRADER',           0.5),
('DEATH_GROUP_PLAYER', 'PVP_SERIOUS',      0.7),

-- Dying alone with nobody to say it to. Muttering to yourself suits the
-- grumbler and the beginner; it does not suit the serious raider.
('DEATH_SOLO',         'GRUMPY_VETERAN',   1.8),
('DEATH_SOLO',         'YOUNG_APPRENTICE', 1.6),
('DEATH_SOLO',         'CASUAL',           1.3),
('DEATH_SOLO',         'TROLL_MILD',       1.2),
('DEATH_SOLO',         'RAIDER_SERIOUS',   0.6),
('DEATH_SOLO',         'MENTOR',           0.6),
('DEATH_SOLO',         'TRADER',           0.5),

-- ---- Progress -----------------------------------------------------------
-- Announcing your own ding is a young/social move; the veteran and the
-- serious raider mostly don't bother.
('LEVEL_UP_SELF',      'YOUNG_APPRENTICE', 2.2),
('LEVEL_UP_SELF',      'SOCIALITE',        1.8),
('LEVEL_UP_SELF',      'CASUAL',           1.4),
('LEVEL_UP_SELF',      'GRUMPY_VETERAN',   0.5),
('LEVEL_UP_SELF',      'RAIDER_SERIOUS',   0.6),
('LEVEL_UP_SELF',      'TROLL_AGGRESSIVE', 0.6),
('LEVEL_UP_SELF',      'PVP_SERIOUS',      0.7),

-- Congratulating someone: nearly everyone does, but this is squarely the
-- mentor's and the socialite's beat.
('LEVEL_UP_GROUP',     'SOCIALITE',        2.2),
('LEVEL_UP_GROUP',     'MENTOR',           2.0),
('LEVEL_UP_GROUP',     'CASUAL',           1.4),
('LEVEL_UP_GROUP',     'YOUNG_APPRENTICE', 1.3),
('LEVEL_UP_GROUP',     'RAIDER_CASUAL',    1.2),
('LEVEL_UP_GROUP',     'TROLL_MILD',       1.1),
('LEVEL_UP_GROUP',     'GRUMPY_VETERAN',   0.7),
('LEVEL_UP_GROUP',     'TRADER',           0.7),

-- ---- PvP ----------------------------------------------------------------
('KILLING_BLOW',       'PVP_SERIOUS',      2.6),
('KILLING_BLOW',       'PVP_CASUAL',       2.2),
('KILLING_BLOW',       'TROLL_AGGRESSIVE', 2.0),
('KILLING_BLOW',       'TROLL_MILD',       1.5),
('KILLING_BLOW',       'RAIDER_CASUAL',    1.1),
('KILLING_BLOW',       'MENTOR',           0.5),
('KILLING_BLOW',       'TRADER',           0.4),
('KILLING_BLOW',       'YOUNG_APPRENTICE', 0.8),
('KILLING_BLOW',       'SOCIALITE',        0.6),

-- ---- Loot ---------------------------------------------------------------
-- The trader values a roll in gold rather than ilvl, which is a different
-- line, not a quieter one.
('ROLL_WON',           'TRADER',           2.2),
('ROLL_WON',           'RAIDER_SERIOUS',   1.5),
('ROLL_WON',           'YOUNG_APPRENTICE', 1.4),
('ROLL_WON',           'SOCIALITE',        1.2),
('ROLL_WON',           'GRUMPY_VETERAN',   0.7),
('ROLL_WON',           'PVP_SERIOUS',      0.7),
('ROLL_WON',           'MENTOR',           0.8),

('ROLL_LOST',          'TRADER',           1.8),
('ROLL_LOST',          'TROLL_AGGRESSIVE', 1.8),
('ROLL_LOST',          'GRUMPY_VETERAN',   1.6),
('ROLL_LOST',          'TROLL_MILD',       1.5),
('ROLL_LOST',          'RAIDER_SERIOUS',   1.3),
('ROLL_LOST',          'MENTOR',           0.7),
('ROLL_LOST',          'SOCIALITE',        0.8),

-- ---- Duels --------------------------------------------------------------
-- Duels draw the PvP archetypes and the trolls and almost nobody else. The
-- reply-count bias in hs_event_arbiter.cpp already biases duels heavily
-- toward silence; these weights decide who breaks it when it doesn't.
('DUEL_START',         'PVP_SERIOUS',      2.6),
('DUEL_START',         'PVP_CASUAL',       2.2),
('DUEL_START',         'TROLL_AGGRESSIVE', 2.0),
('DUEL_START',         'TROLL_MILD',       1.8),
('DUEL_START',         'YOUNG_APPRENTICE', 1.2),
('DUEL_START',         'TRADER',           0.3),
('DUEL_START',         'MENTOR',           0.6),
('DUEL_START',         'SOCIALITE',        0.7),

('DUEL_WON',           'PVP_SERIOUS',      2.4),
('DUEL_WON',           'PVP_CASUAL',       2.0),
('DUEL_WON',           'TROLL_AGGRESSIVE', 2.4),
('DUEL_WON',           'TROLL_MILD',       2.0),
('DUEL_WON',           'GRUMPY_VETERAN',   1.2),
('DUEL_WON',           'MENTOR',           0.6),
('DUEL_WON',           'TRADER',           0.3),

-- Losing is the half the loud archetypes are quieter about, which is a
-- separate weighting from DUEL_WON precisely because the duel-end hook
-- arbitrates both sides in one pass.
('DUEL_LOST',          'PVP_SERIOUS',      1.6),
('DUEL_LOST',          'PVP_CASUAL',       1.8),
('DUEL_LOST',          'TROLL_AGGRESSIVE', 1.2),
('DUEL_LOST',          'TROLL_MILD',       1.4),
('DUEL_LOST',          'GRUMPY_VETERAN',   1.5),
('DUEL_LOST',          'YOUNG_APPRENTICE', 1.4),
('DUEL_LOST',          'TRADER',           0.3),

-- ---- Reserved: the four corpus openers ----------------------------------
-- Inert until openers migrate to tier 2 (Claude/archive/PLAN-ARBITER.md §8).
('OPENER_GROUP_FORMED',       'SOCIALITE',        2.4),
('OPENER_GROUP_FORMED',       'MENTOR',           1.8),
('OPENER_GROUP_FORMED',       'YOUNG_APPRENTICE', 1.3),
('OPENER_GROUP_FORMED',       'GRUMPY_VETERAN',   0.6),
('OPENER_GROUP_FORMED',       'TROLL_AGGRESSIVE', 0.7),

('OPENER_REZ',                'SOCIALITE',        1.8),
('OPENER_REZ',                'MENTOR',           1.6),
('OPENER_REZ',                'YOUNG_APPRENTICE', 1.4),
('OPENER_REZ',                'TROLL_MILD',       1.2),
('OPENER_REZ',                'TRADER',           0.6),

('OPENER_DUNGEON_COMPLETE',   'RAIDER_SERIOUS',   1.7),
('OPENER_DUNGEON_COMPLETE',   'RAIDER_CASUAL',    1.5),
('OPENER_DUNGEON_COMPLETE',   'SOCIALITE',        1.6),
('OPENER_DUNGEON_COMPLETE',   'TRADER',           0.7),

('OPENER_PROXIMITY',          'SOCIALITE',        2.4),
('OPENER_PROXIMITY',          'TRADER',           1.6),
('OPENER_PROXIMITY',          'YOUNG_APPRENTICE', 1.4),
('OPENER_PROXIMITY',          'RAIDER_SERIOUS',   0.6),
('OPENER_PROXIMITY',          'GRUMPY_VETERAN',   0.6);
