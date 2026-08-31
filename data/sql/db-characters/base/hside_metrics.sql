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
-- (channel chat) that are not built at all: a named follow-up.
--
-- Sample interval (~5 min) and retention window are §6's own "not known yet"
-- starting shape: kHsMetricsSampleIntervalSeconds/kHsMetricsRetentionDays
-- in hs_metrics.cpp, compiled constants for the same reason every other
-- unmeasured §6 number in this module has shipped as a placeholder rather
-- than a config key nobody has grounds to tune yet.
--
-- latency_p50/p95/p99_ms and prompt_chars_ring*_mean are §4.19's fuller
-- metric list (reactive-tier call latency, hs_queue.cpp kMaxLatencySamples;
-- mean assembled-prompt length by identity ring). ttl_dropped/processed and
-- bucket_denied/attempted (both _session, cumulative like promotions_session
-- etc.) are the TTL drop rate and token-bucket saturation counts named in
-- §4.19's original list but not built until later. Folded straight into
-- base rather than a same-day updates/ delta: see CLAUDE.md's
-- `base/`-vs-`updates/` section.

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
  `ttl_dropped_session`          BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'requests dropped stale by the worker, hs_queue.cpp WorkerLoop TTL check',
  `ttl_processed_session`        BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'total requests the worker dequeued (dropped + handled): the drop-rate denominator',
  `bucket_denied_session`        BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'global tier-2 token-bucket admission attempts that found the bucket empty',
  `bucket_attempted_session`     BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'total tier-2 token-bucket admission attempts: the saturation-rate denominator',
  PRIMARY KEY (`id`),
  KEY `idx_sampled_at` (`sampled_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
