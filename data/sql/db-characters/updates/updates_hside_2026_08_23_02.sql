-- typing_base_ms/typing_per_char_ms: per-archetype tier-2 typing-delay
-- formula (hs_queue.cpp), replacing the flat global
-- HearthsideChat.TypingDelay.BaseMs/PerCharMs (Claude/ISSUES.md). The
-- operator's TypingDelay.Enable/MaxMs stay the kill switch and ceiling.

ALTER TABLE `hside_archetype`
  ADD COLUMN `typing_base_ms`     SMALLINT UNSIGNED NOT NULL DEFAULT 800 COMMENT 'hs_queue.cpp tier-2 typing delay: flat "notice and start typing" cost' AFTER `profanity_level`,
  ADD COLUMN `typing_per_char_ms` SMALLINT UNSIGNED NOT NULL DEFAULT 45  COMMENT 'ms per character of the styled reply; capped by HearthsideChat.TypingDelay.MaxMs' AFTER `typing_base_ms`;

UPDATE `hside_archetype` SET typing_base_ms = 600,  typing_per_char_ms = 35 WHERE enum_name = 'RAIDER_SERIOUS';
UPDATE `hside_archetype` SET typing_base_ms = 800,  typing_per_char_ms = 45 WHERE enum_name = 'RAIDER_CASUAL';
UPDATE `hside_archetype` SET typing_base_ms = 600,  typing_per_char_ms = 30 WHERE enum_name = 'PVP_SERIOUS';
UPDATE `hside_archetype` SET typing_base_ms = 750,  typing_per_char_ms = 40 WHERE enum_name = 'PVP_CASUAL';
UPDATE `hside_archetype` SET typing_base_ms = 650,  typing_per_char_ms = 35 WHERE enum_name = 'TRADER';
UPDATE `hside_archetype` SET typing_base_ms = 600,  typing_per_char_ms = 35 WHERE enum_name = 'LOOTGOBLIN';
UPDATE `hside_archetype` SET typing_base_ms = 800,  typing_per_char_ms = 45 WHERE enum_name = 'CASUAL';
UPDATE `hside_archetype` SET typing_base_ms = 900,  typing_per_char_ms = 50 WHERE enum_name = 'GRUMPY_VETERAN';
UPDATE `hside_archetype` SET typing_base_ms = 1400, typing_per_char_ms = 50 WHERE enum_name = 'LONE_WOLF';
UPDATE `hside_archetype` SET typing_base_ms = 1000, typing_per_char_ms = 55 WHERE enum_name = 'MENTOR';
UPDATE `hside_archetype` SET typing_base_ms = 900,  typing_per_char_ms = 60 WHERE enum_name = 'YOUNG_APPRENTICE';
UPDATE `hside_archetype` SET typing_base_ms = 550,  typing_per_char_ms = 35 WHERE enum_name = 'SOCIALITE';
UPDATE `hside_archetype` SET typing_base_ms = 1600, typing_per_char_ms = 60 WHERE enum_name = 'DISTRACTED';
UPDATE `hside_archetype` SET typing_base_ms = 700,  typing_per_char_ms = 40 WHERE enum_name = 'TROLL_MILD';
UPDATE `hside_archetype` SET typing_base_ms = 500,  typing_per_char_ms = 35 WHERE enum_name = 'TROLL_AGGRESSIVE';
