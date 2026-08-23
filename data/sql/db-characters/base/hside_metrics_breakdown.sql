-- Per-archetype and per-channel reply-vs-silence counts, sampled alongside
-- hside_metrics on the same interval (Hs_SampleMetrics, hs_metrics.cpp) but
-- kept in its own table, since these counts are variable-cardinality (one
-- row per archetype/channel per interval) and don't fit hside_metrics' flat
-- one-row-per-interval shape. `dimension` is 'archetype' or 'channel';
-- `dim_key` is the archetype enum name or the channel name
-- (hs_queue.h's Hs_ReplyChannelName -- say/whisper/party/raid/guild).
--
-- Same retention window as hside_metrics (kHsMetricsRetentionDays,
-- hs_metrics.cpp), pruned on every sample.

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
