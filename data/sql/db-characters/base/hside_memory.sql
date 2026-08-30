-- Memory and recall: one row per (bot, player) shared-event beat, pair-
-- scoped, unlike hside_identity's per-bot rows. Rows are looked up and
-- templated by hs_grounded.h's recall kinds and are never injected into a
-- prompt raw. Familiarity itself is read straight off
-- hside_identity.interaction_score and needs no storage here; there is no
-- separate relationship-strength table.
CREATE TABLE IF NOT EXISTS `hside_memory` (
  `id`          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid`    BIGINT UNSIGNED NOT NULL,
  `player_guid` BIGINT UNSIGNED NOT NULL,
  `event_type`  VARCHAR(32) NOT NULL COMMENT 'first_meeting | dungeon_completed | grouped_in_zone | died_together | joined_same_guild: selects the recall answer template, makes dedup exact',
  `occurred_at` DATETIME NOT NULL,
  `text`        TEXT NOT NULL COMMENT 'clean, never styled: the style pass runs at recall delivery, same as every other lookup-and-template source',
  `source`      VARCHAR(32) NOT NULL DEFAULT 'template' COMMENT 'template in v1, keeps the column meaningful if a generated source ever ships',
  PRIMARY KEY (`id`),
  KEY `idx_hside_memory_pair` (`bot_guid`, `player_guid`),
  KEY `idx_hside_memory_pair_type` (`bot_guid`, `player_guid`, `event_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
