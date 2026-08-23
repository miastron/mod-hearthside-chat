-- PLAN.md §4.19/§7 step 19: the observability half of "an authenticated
-- HTTP API, plus GM commands, plus a metrics history table." Squashed into
-- base 2026-08-21 (originally
-- data/sql/db-characters/updates/2026_08_21_04.sql) ahead of the module's
-- first real deploy (test-realm only, confirmed disposable) so v1.0 installs
-- as one import.
--
-- In-memory counters (Hs_*ThisSession, Hs_*Count, etc.) already answer "what
-- is true right now"; this table answers the trend questions §6 actually
-- asks ("promotions/day", "per-channel lines/min actual vs configured") that
-- a restart-resetting in-memory counter cannot.
--
-- Scoped to the metrics this module can already read without inventing new
-- instrumentation. §4.19's fuller metric list (request latency percentiles,
-- assembled prompt length per ring, replies-vs-silences per archetype,
-- per-channel lines/min) would need new timing/tracking machinery this
-- module does not have yet, and several of those metrics describe surfaces
-- (channel chat) that are not built at all -- a named follow-up.
--
-- Sample interval (~5 min) and retention window are §6's own "not known yet"
-- starting shape -- kHsMetricsSampleIntervalSeconds/kHsMetricsRetentionDays
-- in hs_metrics.cpp, compiled constants for the same reason every other
-- unmeasured §6 number in this module has shipped as a placeholder rather
-- than a config key nobody has grounds to tune yet.

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
