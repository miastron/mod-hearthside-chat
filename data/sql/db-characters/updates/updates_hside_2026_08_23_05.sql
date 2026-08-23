-- §4.17 channel scripts: hside_script.channel tags a 2-turn script for the
-- global-channel surface (trade/general/world), distinct from the existing
-- NULL-channel 4-turn /say scripts. hs_script.cpp's ClaimAndSchedule (the
-- /say path) now filters `channel IS NULL` explicitly; ClaimAndScheduleChannel
-- filters `channel = <name>`.

ALTER TABLE `hside_script`
  ADD COLUMN `channel` VARCHAR(16) DEFAULT NULL COMMENT 'trade | general | world = §4.17 2-turn channel script; NULL = /say 4-turn script' AFTER `turn_count`;
