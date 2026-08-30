-- PLAN.md §4.12/§7 step 15: identity rings. Squashed into base 2026-08-21
-- (originally data/sql/db-characters/updates/2026_08_21_02.sql) ahead of the
-- module's first real deploy (test-realm only, confirmed disposable) so
-- v1.0 installs as one import. (That update also seeded the
-- chat_carded_focus category and its rows: those now live in
-- hside_corpus_category.sql/hside_corpus.sql, where the rest of the corpus
-- content is, rather than here.)
--
-- One row per bot that has ever scored (not one per bot in the realm;
-- created lazily on first qualifying utterance). interaction_score is a
-- per-bot scalar, not per (bot, player) pair: promotion and the card are
-- about the bot's own conversational weight, not any one relationship
-- (§4.12 "everything identity-shaped is per-bot, except what is about a
-- pair"; memory, the pair-scoped half, is hside_memory).
CREATE TABLE IF NOT EXISTS `hside_identity` (
  `bot_guid`            BIGINT UNSIGNED NOT NULL,
  `archetype`           VARCHAR(64) NOT NULL COMMENT 'snapshot at row creation: deterministic from GUID+level (hs_archetype.h), not re-read from here yet',
  `style_flags`         INT NOT NULL DEFAULT 0 COMMENT 'reserved: style is already fully deterministic from GUID+archetype (Hs_StyleCareForBot); no consumer reads this column yet',
  `last_known_level`    TINYINT UNSIGNED NOT NULL COMMENT 'snapshot at row creation/last score bump: §4.13 level-reset detection',
  `level_checked_at`    DATETIME NULL DEFAULT NULL,
  `card_voice`          TEXT NULL COMMENT '~50 tok prose, the only card text ever injected into a prompt',
  `card_facts`          JSON NULL COMMENT 'structured fields, read by code only, never injected (§4.12)',
  `card_model`          VARCHAR(64) NULL,
  `card_prompt_version` VARCHAR(32) NULL,
  `card_active`         TINYINT(1) NOT NULL DEFAULT 0,
  `pinned_by_friend`    TINYINT(1) NOT NULL DEFAULT 0,
  `interaction_score`   INT NOT NULL DEFAULT 0,
  `promoted_at`         DATETIME NULL DEFAULT NULL,
  `last_used_at`        DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
