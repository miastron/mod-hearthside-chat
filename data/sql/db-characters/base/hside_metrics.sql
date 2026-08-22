-- Metrics history table. In-memory counters (Hs_*ThisSession, Hs_*Count,
-- etc.) already answer "what is true right now"; this table answers trend
-- questions ("promotions/day", "per-channel lines/min actual vs
-- configured") that a restart-resetting in-memory counter cannot.
--
-- Scoped to metrics this module can already read without new
-- instrumentation. Request latency percentiles, assembled prompt length per
-- ring, replies-vs-silences per archetype, and per-channel lines/min are not
-- captured here -- they need timing/tracking machinery this module doesn't
-- have yet, and some describe surfaces (channel chat) that aren't built at
-- all.
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
  PRIMARY KEY (`id`),
  KEY `idx_sampled_at` (`sampled_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
