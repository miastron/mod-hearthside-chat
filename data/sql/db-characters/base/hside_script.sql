-- PLAN.md §4.16/§7 step 14: scripted bot-to-bot conversations.
-- Squashed into base 2026-08-21 (originally
-- data/sql/db-characters/updates/2026_08_21_01.sql) ahead of the module's
-- first real deploy (test-realm only, confirmed disposable) so v1.0 installs
-- as one import.
--
-- A script is a header (hside_script) plus ordered turns (hside_script_turn),
-- single-use (consumed_at, not times_used: §4.5's exposure/eviction/
-- anti-repeat model does not apply, §4.16). speaker_slot is 0 or 1, never a
-- bot GUID, so any two bots in range can cast a script and %my_*/%other_*
-- style binding (not built in v1) stays resolvable per turn by flipping the
-- slot.
CREATE TABLE IF NOT EXISTS `hside_script` (
  `id`               INT UNSIGNED NOT NULL,
  `turn_count`       TINYINT UNSIGNED NOT NULL,
  `channel`          VARCHAR(16) DEFAULT NULL COMMENT 'trade | general = §4.17 2-turn channel script; NULL = /say 4-turn script',
  `generated_at`     TIMESTAMP NULL DEFAULT NULL,
  `model`            VARCHAR(64) DEFAULT NULL,
  `prompt_version`   VARCHAR(32) DEFAULT NULL,
  `consumed_at`      TIMESTAMP NULL DEFAULT NULL COMMENT 'NULL = available; the whole lifecycle in one column (§4.16)',
  `consumed_by_zone` INT UNSIGNED DEFAULT NULL COMMENT 'diagnostics only',
  `consumed_witness` BIGINT UNSIGNED DEFAULT NULL COMMENT 'the real player GUID present when it fired, diagnostics only; always NULL for a channel script (no single witness, §4.17)',
  PRIMARY KEY (`id`),
  KEY `idx_available` (`consumed_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `hside_script_turn` (
  `script_id`    INT UNSIGNED NOT NULL,
  `turn_no`      TINYINT UNSIGNED NOT NULL,
  `speaker_slot` TINYINT UNSIGNED NOT NULL COMMENT '0 or 1: which cast bot speaks, never a GUID',
  `text`         VARCHAR(255) NOT NULL COMMENT 'clean, never styled: the style pass runs per speaker at delivery (trap 12)',
  PRIMARY KEY (`script_id`, `turn_no`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
