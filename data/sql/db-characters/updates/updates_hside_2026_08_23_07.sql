-- TTL drop rate and token-bucket saturation: named in PLAN.md §4.19's
-- original metric list but never actually built (found while building the
-- rest of §4.19, tracked as a gap in Claude/ISSUES.md until now). Session-
-- cumulative counts, not a rolling window -- same shape as promotions_session/
-- retirements_session etc. already on this table.

ALTER TABLE `hside_metrics`
  ADD COLUMN `ttl_dropped_session`     BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'requests dropped stale by the worker, hs_queue.cpp WorkerLoop TTL check' AFTER `prompt_chars_ring3_mean`,
  ADD COLUMN `ttl_processed_session`   BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'total requests the worker dequeued (dropped + handled) -- the drop-rate denominator' AFTER `ttl_dropped_session`,
  ADD COLUMN `bucket_denied_session`   BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'global tier-2 token-bucket admission attempts that found the bucket empty' AFTER `ttl_processed_session`,
  ADD COLUMN `bucket_attempted_session` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'total tier-2 token-bucket admission attempts -- the saturation-rate denominator' AFTER `bucket_denied_session`;

-- §4.17's per-channel token buckets get their own breakdown dimension rather
-- than new columns, since they're variable-cardinality (one row per channel
-- kind) like the existing archetype/channel reply-vs-silence rows. Reuses
-- replied_count/silent_count as granted/denied for this dimension -- see
-- hs_metrics.h's HsMetricsBreakdownRow doc comment.
