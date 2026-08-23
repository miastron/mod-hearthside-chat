-- New table backing `.hearthside archetype` (hs_command.cpp), the GM
-- command to pin a bot to one specific existing archetype, bypassing the
-- normal GUID-weighted draw. See base/hside_archetype_override.sql.

CREATE TABLE IF NOT EXISTS `hside_archetype_override` (
  `bot_guid`   BIGINT UNSIGNED NOT NULL,
  `archetype`  VARCHAR(64) NOT NULL COMMENT 'must match a currently-loaded hside_archetype.enum_name',
  `set_at`     DATETIME NOT NULL,
  PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
