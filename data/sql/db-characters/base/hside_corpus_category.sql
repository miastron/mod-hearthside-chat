-- PLAN.md §4.5 / §7 step 11: the per-category tag-axis table.
-- One row per hside_corpus category name. This *is* the generator's bucket
-- list (§4.7) — a category's tag_axis decides which columns on hside_corpus
-- matter for it, and card_gated decides whether it may use card-only
-- placeholders (%main_focus, %current_goal — unreachable until step 15).
-- Retagging a category later means regenerating its rows, so this is
-- authored once, up front, not left to "tag on misfire".
--
-- Squashed 2026-08-21: this table originally shipped without is_opener and
-- without the opener_*/chat_carded_focus rows below, added later by
-- data/sql/db-characters/updates/2026_08_21_00.sql and _02.sql. Folded
-- straight into base ahead of the module's first real deploy (test-realm
-- only, confirmed disposable) so v1.0 installs as one import; updates/
-- starts clean from the next schema change onward.

CREATE TABLE IF NOT EXISTS `hside_corpus_category` (
  `name`        VARCHAR(64) NOT NULL COMMENT 'category name, matches hside_corpus.name',
  `tag_axis`    ENUM('none','class','faction','level_band','zone') NOT NULL DEFAULT 'none'
                COMMENT 'which hside_corpus tag column this category buckets on',
  `card_gated`  TINYINT(1) NOT NULL DEFAULT 0
                COMMENT '1 = rows may use card-only placeholders, unreachable until step 15 (§4.7)',
  `channel`     VARCHAR(16) DEFAULT NULL
                COMMENT 'trade | general | world = global-channel category (§4.17); NULL = /say and direct-reply',
  `is_opener`   TINYINT(1) NOT NULL DEFAULT 0
                COMMENT '1 = fired only by a shared-context trigger (hs_opener.cpp), never by ambient/direct-reply selection',
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `hside_corpus_category` (`name`, `tag_axis`, `card_gated`, `channel`, `is_opener`) VALUES
('chat_gripe_general',    'none',       0, NULL, 0),
('chat_class_banter',     'class',      0, NULL, 0),
('chat_levelband_musing', 'level_band', 0, NULL, 0),
('channel_trade_wts',     'none',       0, 'trade', 0),
('channel_general_chat',  'none',       0, 'general', 0),
('channel_world_chat',    'none',       0, 'world', 0),
-- §3/§7 step 13: openers -- one per first trigger slice (group formed, mob
-- killed jointly, rez given/received, dungeon completed). The fifth trigger
-- PLAN.md §3 names (prolonged proximity at a shared objective/flight master)
-- needs a periodic world-tick scan this module doesn't have yet.
('opener_group_formed',     'none', 0, NULL, 1),
('opener_joint_kill',       'none', 0, NULL, 1),
('opener_rez',              'none', 0, NULL, 1),
('opener_dungeon_complete', 'none', 0, NULL, 1),
-- §4.7/§4.12 step 15: card-gated proof-of-concept category (%main_focus,
-- %current_goal placeholders).
('chat_carded_focus', 'none', 1, NULL, 0);
