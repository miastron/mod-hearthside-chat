-- Identity rings: one row per bot that has ever scored (not one per bot in
-- the realm -- created lazily on first qualifying utterance).
-- interaction_score is a per-bot scalar, not per (bot, player) pair --
-- promotion and the card are about the bot's own conversational weight, not
-- any one relationship. The pair-scoped half lives in hside_memory.
CREATE TABLE IF NOT EXISTS `hside_identity` (
  `bot_guid`            BIGINT UNSIGNED NOT NULL,
  `archetype`           VARCHAR(64) NOT NULL COMMENT 'snapshot at row creation -- deterministic from GUID+level (hs_archetype.h), not re-read from here yet',
  `last_known_level`    TINYINT UNSIGNED NOT NULL COMMENT 'snapshot at row creation/last score bump -- used for level-reset detection',
  `level_checked_at`    DATETIME NULL DEFAULT NULL,
  `card_voice`          TEXT NULL COMMENT '~50 tok prose, the only card text ever injected into a prompt',
  `card_facts`          JSON NULL COMMENT 'structured fields, read by code only, never injected into a prompt',
  `card_model`          VARCHAR(64) NULL,
  `card_prompt_version` VARCHAR(32) NULL,
  `card_active`         TINYINT(1) NOT NULL DEFAULT 0,
  `pinned_by_friend`    TINYINT(1) NOT NULL DEFAULT 0,
  `interaction_score`   INT NOT NULL DEFAULT 0,
  `promoted_at`         DATETIME NULL DEFAULT NULL,
  `last_used_at`        DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
