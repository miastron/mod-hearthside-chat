-- The corpus row table: pre-generated chat lines selected by the corpus
-- tier with no runtime GPU work. enUS only; text_locN stay NULL and are
-- read with COALESCE(text_locN, text). No style baked in here -- text is
-- clean, grammatical prose. Typos, abbreviation, and casing are applied at
-- delivery time by the style pass (hs_style.cpp), never stored.
--
-- This is a proof-of-concept seed: 6 categories (3 /say, 3 global-channel,
-- matching hside_corpus_category.sql). Breadth (more categories, all 11
-- classes instead of 3, card-gated lines) is a follow-up pass.

CREATE TABLE IF NOT EXISTS `hside_corpus` (
  `id`             INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name`           VARCHAR(64) NOT NULL COMMENT 'category, matches hside_corpus_category.name',
  `text`           VARCHAR(255) NOT NULL,
  `text_loc1`      VARCHAR(255) DEFAULT NULL,
  `text_loc2`      VARCHAR(255) DEFAULT NULL,
  `text_loc3`      VARCHAR(255) DEFAULT NULL,
  `text_loc4`      VARCHAR(255) DEFAULT NULL,
  `text_loc5`      VARCHAR(255) DEFAULT NULL,
  `text_loc6`      VARCHAR(255) DEFAULT NULL,
  `text_loc7`      VARCHAR(255) DEFAULT NULL,
  `text_loc8`      VARCHAR(255) DEFAULT NULL,
  `class_tag`      TINYINT UNSIGNED DEFAULT NULL COMMENT 'WoW class id, set only when category tag_axis = class',
  `faction_tag`    TINYINT UNSIGNED DEFAULT NULL COMMENT '0 = Alliance, 1 = Horde, set only when tag_axis = faction',
  `level_band_tag` VARCHAR(16) DEFAULT NULL COMMENT 'low|mid|high|endgame, set only when tag_axis = level_band',
  `zone_tag`       INT UNSIGNED DEFAULT NULL COMMENT 'Area/Zone id, set only when tag_axis = zone',
  `locale`         VARCHAR(8) NOT NULL DEFAULT 'enUS' COMMENT 'locale this row was authored/generated for',
  `event_id`       INT UNSIGNED DEFAULT NULL COMMENT 'core GameEvent id; NULL = not seasonal',
  `times_used`     INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'exposure counter, primary eviction signal',
  `last_used_at`   TIMESTAMP NULL DEFAULT NULL COMMENT 'drives eviction and anti-repeat selection',
  `generated_at`   TIMESTAMP NULL DEFAULT NULL COMMENT 'NULL for hand-authored rows; set for generator output',
  `model`          VARCHAR(64) DEFAULT NULL COMMENT 'NULL for hand-authored rows; generation model otherwise',
  `prompt_version` VARCHAR(32) DEFAULT NULL COMMENT 'NULL for hand-authored rows; lets a bad run be bulk-evicted',
  PRIMARY KEY (`id`),
  KEY `idx_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO `hside_corpus` (`name`, `text`, `class_tag`) VALUES
-- chat_gripe_general: tag_axis none; unfalsifiable opinions/gripes
('chat_gripe_general', 'man this zone has been a grind lately', NULL),
('chat_gripe_general', 'still can''t believe that pack respawned so fast', NULL),
('chat_gripe_general', 'not gonna lie, today''s been a rough one', NULL),
('chat_gripe_general', 'starting to think my luck is just bad this week', NULL),
('chat_gripe_general', 'these fetch quests never end', NULL),
('chat_gripe_general', 'some days you''re the hammer, some days you''re the nail', NULL),
('chat_gripe_general', 'i swear this mob has a personal vendetta against me', NULL),
('chat_gripe_general', 'at least the scenery''s nice out here', NULL),
-- chat_class_banter: tag_axis class; 3 of 11 classes seeded (warrior=1, rogue=4, mage=8)
('chat_class_banter', 'another day, another shield to bash things with', 1),
('chat_class_banter', 'rage''s easy to build when everything here wants me dead', 1),
('chat_class_banter', 'charge in, ask questions never', 1),
('chat_class_banter', 'my arms are getting tired from all this swinging', 1),
('chat_class_banter', 'you never saw me, i was never here', 4),
('chat_class_banter', 'picked more locks today than i can count', 4),
('chat_class_banter', 'stealth makes everything easier, honestly', 4),
('chat_class_banter', 'sharpened my daggers this morning, feeling good', 4),
('chat_class_banter', 'portals are so much more convenient than walking everywhere', 8),
('chat_class_banter', 'conjured a whole feast, help yourselves', 8),
('chat_class_banter', 'sometimes i just blink for the fun of it', 8),
('chat_class_banter', 'frost or fire, can never decide', 8);

INSERT INTO `hside_corpus` (`name`, `text`, `level_band_tag`) VALUES
-- chat_levelband_musing: tag_axis level_band; all 4 bands seeded
('chat_levelband_musing', 'still figuring out where everything is around here', 'low'),
('chat_levelband_musing', 'everything in this zone can still kill me, be careful', 'low'),
('chat_levelband_musing', 'haven''t even left the starting zones really', 'low'),
('chat_levelband_musing', 'learning the ropes, one quest at a time', 'low'),
('chat_levelband_musing', 'finally starting to feel like i know what i''m doing', 'mid'),
('chat_levelband_musing', 'these quest chains are getting long', 'mid'),
('chat_levelband_musing', 'gear''s coming together slowly but surely', 'mid'),
('chat_levelband_musing', 'halfway there, i think', 'mid'),
('chat_levelband_musing', 'almost geared enough for the real stuff', 'high'),
('chat_levelband_musing', 'these instances are no joke at this point', 'high'),
('chat_levelband_musing', 'getting closer to endgame, can feel it', 'high'),
('chat_levelband_musing', 'grinding rep is the real end boss', 'high'),
('chat_levelband_musing', 'back to dailies again, as always', 'endgame'),
('chat_levelband_musing', 'raid nights really do fly by', 'endgame'),
('chat_levelband_musing', 'still chasing that one drop', 'endgame'),
('chat_levelband_musing', 'feels like i''ve seen everything twice now', 'endgame');

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
-- channel_trade_wts: Trade channel; bag-stock only, universal %item_link, never AH listings
('channel_trade_wts', 'WTS %item_link, make an offer'),
('channel_trade_wts', 'selling %item_link, pst'),
('channel_trade_wts', 'got a stack of %item_link if anyone needs it'),
('channel_trade_wts', 'WTS %item_link cheap, just clearing bag space'),
('channel_trade_wts', 'anyone need %item_link? selling'),
('channel_trade_wts', '%item_link for sale, reasonable price'),
-- channel_general_chat: General channel; zone flavour, questions, gripes, nothing checkable
('channel_general_chat', 'anyone else think this zone is bigger than it looks'),
('channel_general_chat', 'is it just me or has it been quiet today'),
('channel_general_chat', 'does anyone know a good spot to farm around here'),
('channel_general_chat', 'this place always feels a little eerie at night'),
('channel_general_chat', 'kind of a slow day so far'),
('channel_general_chat', 'always something new to see around every corner'),
-- channel_world_chat: World channel; same register as General
('channel_world_chat', 'world feels pretty alive today'),
('channel_world_chat', 'anyone else just wandering around right now'),
('channel_world_chat', 'always someone doing something interesting somewhere'),
('channel_world_chat', 'feels like a good day to explore'),
('channel_world_chat', 'quiet out here, kind of nice for a change'),
('channel_world_chat', 'world''s a big place, still finding new corners of it');

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
-- opener_*: fired only by hs_opener.cpp's shared-context triggers
('opener_group_formed', 'hey, thanks for the invite.'),
('opener_group_formed', 'alright, let''s do this.'),
('opener_group_formed', 'good to have another hand.'),
('opener_group_formed', 'ready when you are.'),
('opener_joint_kill', 'nice work on that one.'),
('opener_joint_kill', 'gg, that one hit harder than it looked.'),
('opener_joint_kill', 'good teamwork there.'),
('opener_joint_kill', 'that was closer than i''d like.'),
('opener_rez', 'thanks for the pick-up.'),
('opener_rez', 'appreciate it.'),
('opener_rez', 'back up, thanks.'),
('opener_rez', 'owe you one for that.'),
('opener_dungeon_complete', 'good run.'),
('opener_dungeon_complete', 'that went smoother than i expected.'),
('opener_dungeon_complete', 'solid group, that one.'),
('opener_dungeon_complete', 'nice, one down.'),
-- chat_carded_focus: card-gated (%main_focus, %current_goal)
('chat_carded_focus', 'still grinding away at %main_focus, honestly'),
('chat_carded_focus', 'lately it is all about %current_goal for me'),
('chat_carded_focus', 'main focus right now is %main_focus'),
('chat_carded_focus', 'not gonna lie, %current_goal has been eating all my playtime');
