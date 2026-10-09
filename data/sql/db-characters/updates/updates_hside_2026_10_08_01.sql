-- Typing delay at human speed. The old 30-60 ms/char was 200-400 wpm, so with LLM latency
-- subtracted replies landed in 1-3 s. Now 40-90 wpm plus 1.5-2.5 s to notice and read.
UPDATE `hside_archetype` SET `typing_base_ms` = 1500, `typing_per_char_ms` = 140 WHERE `enum_name` = 'RAIDER_SERIOUS';
UPDATE `hside_archetype` SET `typing_base_ms` = 2000, `typing_per_char_ms` = 170 WHERE `enum_name` = 'RAIDER_CASUAL';
UPDATE `hside_archetype` SET `typing_base_ms` = 1500, `typing_per_char_ms` = 130 WHERE `enum_name` = 'PVP_SERIOUS';
UPDATE `hside_archetype` SET `typing_base_ms` = 1800, `typing_per_char_ms` = 160 WHERE `enum_name` = 'PVP_CASUAL';
UPDATE `hside_archetype` SET `typing_base_ms` = 1800, `typing_per_char_ms` = 150 WHERE `enum_name` = 'TRADER';
UPDATE `hside_archetype` SET `typing_base_ms` = 2500, `typing_per_char_ms` = 190 WHERE `enum_name` = 'CASUAL';
UPDATE `hside_archetype` SET `typing_base_ms` = 2500, `typing_per_char_ms` = 210 WHERE `enum_name` = 'GRUMPY_VETERAN';
UPDATE `hside_archetype` SET `typing_base_ms` = 2500, `typing_per_char_ms` = 180 WHERE `enum_name` = 'MENTOR';
UPDATE `hside_archetype` SET `typing_base_ms` = 2500, `typing_per_char_ms` = 260 WHERE `enum_name` = 'YOUNG_APPRENTICE';
UPDATE `hside_archetype` SET `typing_base_ms` = 1500, `typing_per_char_ms` = 150 WHERE `enum_name` = 'SOCIALITE';
UPDATE `hside_archetype` SET `typing_base_ms` = 2000, `typing_per_char_ms` = 170 WHERE `enum_name` = 'TROLL_MILD';
UPDATE `hside_archetype` SET `typing_base_ms` = 1500, `typing_per_char_ms` = 150 WHERE `enum_name` = 'TROLL_AGGRESSIVE';
