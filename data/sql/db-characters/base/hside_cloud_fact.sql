-- Cloud facts (hs_cloud.cpp, Claude/PLAN-CLOUD-FACTS.md): answers a cloud model
-- gave to real players' off-topic questions, kept so a repeated or rephrased
-- question is answered without another paid call, across restarts.
--
-- Written once per new fact (fire-and-forget), read once at startup into the
-- in-memory store, and swept of expired rows at that same startup. An answer
-- given from the model's own knowledge lasts HearthsideChat.Cloud.CacheDays;
-- a web-searched one lasts the [days:N] the model labeled it, capped by
-- Cloud.WebSearch.MaxCacheDays. Times are unix seconds.
CREATE TABLE IF NOT EXISTS `hside_cloud_fact` (
  `id`         INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `question`   VARCHAR(255) NOT NULL COMMENT 'the player line, as typed',
  `fact`       VARCHAR(512) NOT NULL COMMENT 'Title -- sentence., as it goes into the prompt',
  `searched`   TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = answered with a web search',
  `created_at` BIGINT UNSIGNED NOT NULL,
  `expires_at` BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_expires_at` (`expires_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
