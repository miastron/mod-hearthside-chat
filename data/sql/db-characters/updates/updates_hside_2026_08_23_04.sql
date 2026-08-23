-- §4.19's fuller metric list: reactive-tier latency percentiles and mean
-- assembled-prompt length by identity ring, added to hside_metrics; and a
-- new hside_metrics_breakdown table for the variable-cardinality
-- per-archetype/per-channel reply-vs-silence counts that don't fit
-- hside_metrics' flat per-interval row (Claude/ISSUES.md).

ALTER TABLE `hside_metrics`
  ADD COLUMN `latency_p50_ms`          INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'rolling-window reactive-tier call latency, hs_queue.cpp kMaxLatencySamples' AFTER `openers_fired_session`,
  ADD COLUMN `latency_p95_ms`          INT UNSIGNED NOT NULL DEFAULT 0 AFTER `latency_p50_ms`,
  ADD COLUMN `latency_p99_ms`          INT UNSIGNED NOT NULL DEFAULT 0 AFTER `latency_p95_ms`,
  ADD COLUMN `prompt_chars_ring1_mean` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'mean assembled-prompt length, stranger ring' AFTER `latency_p99_ms`,
  ADD COLUMN `prompt_chars_ring2_mean` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'known ring' AFTER `prompt_chars_ring1_mean`,
  ADD COLUMN `prompt_chars_ring3_mean` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'carded ring' AFTER `prompt_chars_ring2_mean`;

CREATE TABLE IF NOT EXISTS `hside_metrics_breakdown` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `sampled_at`     TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `dimension`      VARCHAR(16) NOT NULL COMMENT 'archetype | channel',
  `dim_key`        VARCHAR(32) NOT NULL COMMENT 'archetype enum name, or channel name',
  `replied_count`  INT UNSIGNED NOT NULL DEFAULT 0,
  `silent_count`   INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_hside_metrics_breakdown_sampled_at` (`sampled_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
