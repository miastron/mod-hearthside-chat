-- GM-set per-bot archetype pins (`.hearthside archetype`, hs_command.cpp).
-- One row per overridden bot; a bot with no row draws its archetype
-- normally (hs_archetype.cpp's GUID-weighted Hs_ArchetypeForBot). archetype
-- must name a currently-loaded hside_archetype.enum_name -- validated at
-- write time by the GM command and again at load time
-- (Hs_LoadArchetypeOverridesFromDb skips and logs an unrecognized name
-- rather than applying it).

CREATE TABLE IF NOT EXISTS `hside_archetype_override` (
  `bot_guid`   BIGINT UNSIGNED NOT NULL,
  `archetype`  VARCHAR(64) NOT NULL COMMENT 'must match a currently-loaded hside_archetype.enum_name',
  `set_at`     DATETIME NOT NULL,
  PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
