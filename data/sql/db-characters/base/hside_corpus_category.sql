-- The per-category tag-axis table: one row per hside_corpus category name,
-- and the generator's bucket list: a category's tag_axis decides which
-- columns on hside_corpus matter for it, and card_gated decides whether it
-- may use card-only placeholders (%main_focus, %current_goal). Retagging a
-- category later means regenerating its rows, so this is authored once, up
-- front.

CREATE TABLE IF NOT EXISTS `hside_corpus_category` (
  `name`        VARCHAR(64) NOT NULL COMMENT 'category name, matches hside_corpus.name',
  `tag_axis`    ENUM('none','class','faction','level_band','zone') NOT NULL DEFAULT 'none'
                COMMENT 'which hside_corpus tag column this category buckets on',
  `card_gated`  TINYINT(1) NOT NULL DEFAULT 0
                COMMENT '1 = rows may use card-only placeholders (%main_focus, %current_goal)',
  `channel`     VARCHAR(16) DEFAULT NULL
                COMMENT 'trade | general = global-channel category; NULL = /say and direct-reply',
  `is_opener`   TINYINT(1) NOT NULL DEFAULT 0
                COMMENT '1 = fired only by a shared-context trigger (hs_opener.cpp), never by ambient/direct-reply selection',
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `hside_corpus_category` (`name`, `tag_axis`, `card_gated`, `channel`, `is_opener`) VALUES
('chat_gripe_general',    'none',       0, NULL, 0),
('chat_class_banter',     'class',      0, NULL, 0),
('chat_levelband_musing', 'level_band', 0, NULL, 0),
('chat_faction_banter',   'faction',    0, NULL, 0),
('chat_zone_musing',      'zone',       0, NULL, 0),
('channel_trade_wts',     'none',       0, 'trade', 0),
('channel_general_chat',  'none',       0, 'general', 0),
-- Openers: one per trigger slice (group formed, mob killed jointly, rez
-- given/received, dungeon completed, prolonged proximity at a shared quest
-- objective or flight master — hs_engagement.cpp's periodic scan WorldScript).
('opener_group_formed',     'none', 0, NULL, 1),
('opener_joint_kill',       'none', 0, NULL, 1),
('opener_rez',              'none', 0, NULL, 1),
('opener_dungeon_complete', 'none', 0, NULL, 1),
('opener_prolonged_proximity', 'none', 0, NULL, 1),
-- Card-gated proof-of-concept category (%main_focus, %current_goal placeholders).
('chat_carded_focus', 'none', 1, NULL, 0);
