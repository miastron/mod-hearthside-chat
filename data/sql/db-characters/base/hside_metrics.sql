-- Metrics history table. In-memory counters (Hs_*ThisSession, Hs_*Count,
-- etc.) already answer "what is true right now"; this table answers trend
-- questions ("promotions/day", "latency creeping up") that a
-- restart-resetting in-memory counter cannot.
--
-- Request latency percentiles (rolling sample window, not full history) and
-- mean assembled-prompt length by identity ring are flat per-interval
-- columns here. Replies-vs-silences per archetype/channel are
-- variable-cardinality and don't fit this row shape -- see
-- hside_metrics_breakdown.sql. Per-channel lines/min stays unbuilt; it
-- describes the global-channel surface (§4.17), which doesn't exist yet.
--
-- Sample interval (~5 min) and retention window are compiled constants
-- (kHsMetricsSampleIntervalSeconds / kHsMetricsRetentionDays in
-- hs_metrics.cpp), not config keys, since neither has a tuned value yet.

CREATE TABLE IF NOT EXISTS `hside_metrics` (
  `id`                           INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `sampled_at`                   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `backend_down`                 TINYINT UNSIGNED NOT NULL,
  `queue_depth`                  INT UNSIGNED NOT NULL,
  `corpus_row_count`             INT UNSIGNED NOT NULL,
  `corpus_rows_added_session`    INT UNSIGNED NOT NULL,
  `corpus_rows_evicted_session`  INT UNSIGNED NOT NULL,
  `script_reserve_depth`         INT UNSIGNED NOT NULL,
  `script_active_runs`           INT UNSIGNED NOT NULL,
  `script_consumed_24h`          INT UNSIGNED NOT NULL,
  `identity_row_count`           INT UNSIGNED NOT NULL,
  `card_active_count`            INT UNSIGNED NOT NULL,
  `promotions_session`           INT UNSIGNED NOT NULL,
  `demotions_session`            INT UNSIGNED NOT NULL,
  `retirements_session`          INT UNSIGNED NOT NULL,
  `memory_row_count`             INT UNSIGNED NOT NULL,
  `openers_fired_session`        INT UNSIGNED NOT NULL,
  `latency_p50_ms`               INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'rolling-window reactive-tier call latency, hs_queue.cpp kMaxLatencySamples',
  `latency_p95_ms`               INT UNSIGNED NOT NULL DEFAULT 0,
  `latency_p99_ms`               INT UNSIGNED NOT NULL DEFAULT 0,
  `prompt_chars_ring1_mean`      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'mean assembled-prompt length, stranger ring',
  `prompt_chars_ring2_mean`      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'known ring',
  `prompt_chars_ring3_mean`      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'carded ring',
  PRIMARY KEY (`id`),
  KEY `idx_sampled_at` (`sampled_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
