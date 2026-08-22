-- New 2026-08-21 -- HearthsideChat.DebugChatLog.Enable. One row per
-- reactive-tier (LLM) exchange: the trigger, the model's pre-style output,
-- and what actually got delivered after hs_style.cpp's typo/casing/
-- abbreviation pass. An operator-reviewed debugging aid, not an automatic
-- corpus feed -- PLAN.md §4.7 already rejected automatically harvesting
-- replies into hside_corpus (no runtime quality signal exists); this table
-- doesn't feed anything automatically either. It exists so an operator can
-- read back real exchanges and hand-pick ones that need improvement, or that
-- are worth using as training/fine-tuning examples, the same "operator is
-- the quality signal" reasoning `.hearthside capture` already uses one step
-- further upstream. Off by default; not covered by the corpus eviction sweep
-- (hs_corpus.h) or the identity daily sweep -- an operator who enables this
-- on a busy realm is responsible for pruning it.
CREATE TABLE IF NOT EXISTS `hside_chat_log` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_guid`       BIGINT UNSIGNED NOT NULL,
  `bot_name`       VARCHAR(64) NOT NULL,
  `sender_guid`    BIGINT UNSIGNED NOT NULL,
  `sender_name`    VARCHAR(64) NOT NULL,
  `is_whisper`     TINYINT(1) NOT NULL DEFAULT 0,
  `archetype`      VARCHAR(32) NOT NULL,
  `trigger_text`   TEXT NOT NULL COMMENT 'the player line the bot was replying to',
  `pre_style_text` TEXT NOT NULL COMMENT 'the model''s raw output, before hs_style.cpp',
  `styled_text`    TEXT NOT NULL COMMENT 'what was actually delivered in chat',
  `created_at`     TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_hside_chat_log_created_at` (`created_at`),
  KEY `idx_hside_chat_log_bot` (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
