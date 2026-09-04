-- Review B4 (2026-09-03): persistence for the module's once-daily sweeps.
--
-- Both daily sweeps (Hs_RunIdentityDailySweep, and the corpus eviction pair
-- Hs_RunEvictionSweep/Hs_RunUnusedRowEvictionSweep) used to accumulate
-- worldserver `diff` from process start with no persistence, so a realm
-- restarted more often than once a day never reached the 86400000ms
-- threshold and neither sweep ran at all -- score decay, friend-poll pinning,
-- card demotion, level-drop retirement, orphan cleanup, corpus over-quota
-- eviction and unused-row eviction were all silently dead on a
-- nightly-restart realm. Nothing logged it.
--
-- One row per sweep, keyed by a module-supplied name (never player input).
-- hs_main.cpp reads it at OnStartup to seed the in-process accumulator and
-- writes it after each fire, so the accumulator still does the per-tick work
-- and this table only carries state across a restart.
CREATE TABLE IF NOT EXISTS `hside_sweep_state` (
  `sweep_name`  VARCHAR(32) NOT NULL COMMENT 'module constant: identity_daily | corpus_eviction',
  `last_run_at` TIMESTAMP NULL DEFAULT NULL COMMENT 'NULL = never run; the sweep fires immediately on the next startup',
  PRIMARY KEY (`sweep_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
