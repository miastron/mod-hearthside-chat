-- §4.20 grounded answers, reply side. Companion to hside_grounded_question
-- (see that file for the split rationale). `kind` must match hs_grounded.h's
-- HsGroundedKind enum name. `has_fact` selects which fact-state (1 = has,
-- 0 = lacks/negative) this row answers -- Mount/Level/Gold/Zone only ever
-- get called with has_fact=1 (hs_handler.cpp treats them as always
-- resolvable), so they carry no has_fact=0 rows. `uses_fact` = 1 means the
-- reply is `prefix + fact + suffix` (the fact is interpolated); = 0 means
-- `prefix` is the full canned reply verbatim and `fact`/`suffix` are
-- unused -- covers the flat has/lacks response pools (Guild/Profession/
-- Gear's lacks set, RecallMet/RecallGrouped's has and lacks sets,
-- RecallDungeon's lacks set).

CREATE TABLE IF NOT EXISTS `hside_grounded_template` (
  `id`        INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `kind`      VARCHAR(24) NOT NULL COMMENT 'must match hs_grounded.h''s HsGroundedKind enum name',
  `has_fact`  TINYINT(1) NOT NULL DEFAULT 1 COMMENT '1 = template for a resolvable fact, 0 = the lacks/negative-answer set',
  `uses_fact` TINYINT(1) NOT NULL DEFAULT 1 COMMENT '1 = reply is prefix+fact+suffix, 0 = prefix is the full canned reply verbatim',
  `prefix`    VARCHAR(96) NOT NULL DEFAULT '',
  `suffix`    VARCHAR(96) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO `hside_grounded_template` (`kind`, `has_fact`, `uses_fact`, `prefix`, `suffix`) VALUES
-- MOUNT -- always resolvable, templated
('MOUNT', 1, 1, 'it''s a ', ''),
('MOUNT', 1, 1, 'just my ', ''),
('MOUNT', 1, 1, '', ', nothing special'),
('MOUNT', 1, 1, 'picked up my ', ' a while back'),
('MOUNT', 1, 1, 'riding my ', ''),
('MOUNT', 1, 1, '', ', found it a while back'),

-- LEVEL -- always resolvable, templated
('LEVEL', 1, 1, 'level ', ''),
('LEVEL', 1, 1, 'I''m level ', ''),
('LEVEL', 1, 1, '', ', why'),
('LEVEL', 1, 1, 'just hit ', ''),
('LEVEL', 1, 1, '', ' atm'),
('LEVEL', 1, 1, 'lvl ', ''),

-- GOLD -- always resolvable, templated. `fact` already carries its own
-- denomination suffix (e.g. "47g", "12s" -- hs_handler.cpp's Gold case), so
-- templates wrap it plainly rather than appending another one.
('GOLD', 1, 1, '', ''),
('GOLD', 1, 1, 'got about ', ''),
('GOLD', 1, 1, '', ' right now'),
('GOLD', 1, 1, 'sitting on ', ''),
('GOLD', 1, 1, '', ' ish'),

-- ZONE -- always resolvable, templated
('ZONE', 1, 1, '', ''),
('ZONE', 1, 1, '', ', you?'),
('ZONE', 1, 1, 'just in ', ''),
('ZONE', 1, 1, 'hanging around ', ''),
('ZONE', 1, 1, 'in ', ''),
('ZONE', 1, 1, '', ' right now'),

-- GUILD -- has (templated) / lacks (flat)
('GUILD', 1, 1, 'I''m in ', ''),
('GUILD', 1, 1, '', ', why'),
('GUILD', 1, 1, 'running with ', ''),
('GUILD', 1, 1, 'guilded with ', ''),
('GUILD', 1, 1, 'in ', ' atm'),
('GUILD', 0, 0, 'nah, no guild atm', ''),
('GUILD', 0, 0, 'not guilded right now', ''),
('GUILD', 0, 0, 'solo for now', ''),
('GUILD', 0, 0, 'nope, no guild', ''),
('GUILD', 0, 0, 'not in one right now', ''),
('GUILD', 0, 0, 'guildless atm', ''),
('GUILD', 0, 0, 'flying solo right now', ''),

-- PROFESSION -- has (templated) / lacks (flat)
('PROFESSION', 1, 1, '', ''),
('PROFESSION', 1, 1, 'just ', ''),
('PROFESSION', 1, 1, 'picked up ', ''),
('PROFESSION', 1, 1, 'got ', ''),
('PROFESSION', 1, 1, '', ' so far'),
('PROFESSION', 0, 0, 'haven''t picked one up yet', ''),
('PROFESSION', 0, 0, 'nothing right now', ''),
('PROFESSION', 0, 0, 'nope, none atm', ''),
('PROFESSION', 0, 0, 'not yet', ''),
('PROFESSION', 0, 0, 'none so far', ''),
('PROFESSION', 0, 0, 'haven''t bothered yet', ''),

-- GEAR -- has (templated) / lacks (flat)
('GEAR', 1, 1, 'just this ', ''),
('GEAR', 1, 1, '', ', nothing fancy'),
('GEAR', 1, 1, 'picked up this ', ' a while back'),
('GEAR', 1, 1, 'wearing this ', ''),
('GEAR', 1, 1, 'got this ', ' recently'),
('GEAR', 0, 0, 'not much really', ''),
('GEAR', 0, 0, 'still gearing up', ''),
('GEAR', 0, 0, 'nothing special honestly', ''),
('GEAR', 0, 0, 'working on it', ''),

-- CURRENT_GOAL -- has only; hasFact=false returns "" by design (no active card)
('CURRENT_GOAL', 1, 1, '', ''),
('CURRENT_GOAL', 1, 1, 'mostly ', ''),
('CURRENT_GOAL', 1, 1, 'honestly, ', ''),
('CURRENT_GOAL', 1, 1, 'right now, ', ''),
('CURRENT_GOAL', 1, 1, 'just ', ''),
('CURRENT_GOAL', 1, 1, '', ', mostly'),

-- PLAYED_SINCE -- has only; hasFact=false returns "" by design
('PLAYED_SINCE', 1, 1, 'since ', ''),
('PLAYED_SINCE', 1, 1, 'playing since ', ''),
('PLAYED_SINCE', 1, 1, 'started back in ', ''),
('PLAYED_SINCE', 1, 1, '', ' baby'),
('PLAYED_SINCE', 1, 1, '', ' for me'),
('PLAYED_SINCE', 1, 1, 'playing ', ' content'),

-- ALT -- has only; hasFact=false returns "" by design
('ALT', 1, 1, 'I''ve got a ', ' on the side'),
('ALT', 1, 1, 'mostly this, but I dabble on a ', ''),
('ALT', 1, 1, 'a ', ' when I need a break'),
('ALT', 1, 1, '', ', mostly'),
('ALT', 1, 1, 'my alt is a ', ''),
('ALT', 1, 1, 'also play a ', ' sometimes'),

-- RECALL_MET -- has/lacks, both flat canned pools
('RECALL_MET', 1, 0, 'yeah, we''ve talked before', ''),
('RECALL_MET', 1, 0, 'of course I remember you', ''),
('RECALL_MET', 1, 0, 'we go back a bit', ''),
('RECALL_MET', 1, 0, 'yeah we''ve chatted before', ''),
('RECALL_MET', 1, 0, 'sure do', ''),
('RECALL_MET', 1, 0, 'course I remember', ''),
('RECALL_MET', 0, 0, 'don''t think we''ve met', ''),
('RECALL_MET', 0, 0, 'can''t say I remember, sorry', ''),
('RECALL_MET', 0, 0, 'not that I recall', ''),
('RECALL_MET', 0, 0, 'doesn''t ring a bell', ''),
('RECALL_MET', 0, 0, 'not that I remember', ''),
('RECALL_MET', 0, 0, 'don''t think so', ''),

-- RECALL_DUNGEON -- has (templated, wraps hside_memory's own sentence) / lacks (flat)
('RECALL_DUNGEON', 1, 1, '', ''),
('RECALL_DUNGEON', 1, 1, 'yeah, ', ''),
('RECALL_DUNGEON', 1, 1, 'iirc, ', ''),
('RECALL_DUNGEON', 1, 1, 'if I remember right, ', ''),
('RECALL_DUNGEON', 1, 1, 'pretty sure ', ''),
('RECALL_DUNGEON', 0, 0, 'can''t think of anything we''ve run together', ''),
('RECALL_DUNGEON', 0, 0, 'don''t remember running anything with you', ''),
('RECALL_DUNGEON', 0, 0, 'nothing comes to mind', ''),
('RECALL_DUNGEON', 0, 0, 'not that I recall', ''),
('RECALL_DUNGEON', 0, 0, 'can''t place it', ''),

-- RECALL_GROUPED -- has/lacks, both flat canned pools
('RECALL_GROUPED', 1, 0, 'yeah, we have', ''),
('RECALL_GROUPED', 1, 0, 'we have, actually', ''),
('RECALL_GROUPED', 1, 0, 'yeah, a couple times', ''),
('RECALL_GROUPED', 1, 0, 'definitely', ''),
('RECALL_GROUPED', 1, 0, 'a few times now', ''),
('RECALL_GROUPED', 0, 0, 'don''t think so', ''),
('RECALL_GROUPED', 0, 0, 'not that I recall', ''),
('RECALL_GROUPED', 0, 0, 'not yet, I don''t think', ''),
('RECALL_GROUPED', 0, 0, 'don''t believe so', ''),
('RECALL_GROUPED', 0, 0, 'not so far', '');
