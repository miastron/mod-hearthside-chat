-- PLAN.md §4.12/§7 step 16: memory and recall. Squashed into base 2026-08-21
-- (originally data/sql/db-characters/updates/2026_08_21_03.sql) ahead of the
-- module's first real deploy (test-realm only, confirmed disposable) so
-- v1.0 installs as one import.
--
-- One row per (bot, player) shared-event beat -- pair-scoped, unlike
-- hside_identity's per-bot rows. Rows are looked up and templated by
-- hs_grounded.h's recall kinds (§4.20) and are never injected into a prompt
-- raw; familiarity itself is read straight off hside_identity.interaction_score
-- and needs no storage here. No hside_relationship in v1 (decided).
CREATE TABLE IF NOT EXISTS `hside_memory` (
  `id`          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid`    BIGINT UNSIGNED NOT NULL,
  `player_guid` BIGINT UNSIGNED NOT NULL,
  `event_type`  VARCHAR(32) NOT NULL COMMENT 'first_meeting | dungeon_completed | grouped_in_zone | died_together | joined_same_guild -- selects the recall answer template, makes dedup exact',
  `occurred_at` DATETIME NOT NULL,
  `text`        TEXT NOT NULL COMMENT 'clean, never styled (trap 12) -- the style pass runs at recall delivery, same as every other lookup-and-template source',
  `source`      VARCHAR(32) NOT NULL DEFAULT 'template' COMMENT 'template in v1 -- keeps the column meaningful if a generated source ever ships (§4.4)',
  PRIMARY KEY (`id`),
  KEY `idx_hside_memory_pair` (`bot_guid`, `player_guid`),
  KEY `idx_hside_memory_pair_type` (`bot_guid`, `player_guid`, `event_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
