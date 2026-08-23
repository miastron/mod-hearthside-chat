-- Scripted bot-to-bot conversations. A script is a header (hside_script)
-- plus ordered turns (hside_script_turn), single-use (consumed_at, not
-- times_used -- the exposure/eviction/anti-repeat model used elsewhere does
-- not apply here). speaker_slot is 0 or 1, never a bot GUID, so any two bots
-- in range can cast a script and %my_*/%other_* placeholder binding
-- (hs_corpus.h's Hs_ResolveScriptPlaceholders) stays resolvable per turn by
-- flipping the slot -- a claim is only ever true of whichever pair actually
-- performs it.
CREATE TABLE IF NOT EXISTS `hside_script` (
  `id`               INT UNSIGNED NOT NULL,
  `turn_count`       TINYINT UNSIGNED NOT NULL,
  `generated_at`     TIMESTAMP NULL DEFAULT NULL,
  `model`            VARCHAR(64) DEFAULT NULL,
  `prompt_version`   VARCHAR(32) DEFAULT NULL,
  `consumed_at`      TIMESTAMP NULL DEFAULT NULL COMMENT 'NULL = available -- the whole lifecycle in one column',
  `consumed_by_zone` INT UNSIGNED DEFAULT NULL COMMENT 'diagnostics only',
  `consumed_witness` BIGINT UNSIGNED DEFAULT NULL COMMENT 'the real player GUID present when it fired, diagnostics only',
  PRIMARY KEY (`id`),
  KEY `idx_available` (`consumed_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `hside_script_turn` (
  `script_id`    INT UNSIGNED NOT NULL,
  `turn_no`      TINYINT UNSIGNED NOT NULL,
  `speaker_slot` TINYINT UNSIGNED NOT NULL COMMENT '0 or 1 -- which cast bot speaks, never a GUID',
  `text`         VARCHAR(255) NOT NULL COMMENT 'clean, never styled -- the style pass runs per speaker at delivery',
  PRIMARY KEY (`script_id`, `turn_no`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
