-- The corpus row table: pre-generated chat lines selected by the corpus
-- tier with no runtime GPU work. enUS only; text_locN stay NULL and are
-- read with COALESCE(text_locN, text). No style baked in here -- text is
-- clean, grammatical prose. Typos, abbreviation, and casing are applied at
-- delivery time by the style pass (hs_style.cpp), never stored.
--
-- Seed content matching hside_corpus_category.sql's categories. class_tag
-- coverage (3 of 11 classes) and card-gated lines beyond the
-- proof-of-concept pair remain a follow-up pass -- the generator grows both
-- over time once seeded.

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
('channel_trade_wts', 'clearing out bags, WTS %item_link'),
('channel_trade_wts', 'got extra %item_link, make an offer'),
('channel_trade_wts', 'selling off some %item_link, pst if interested'),
('channel_trade_wts', 'WTS %item_link, first come first served'),
('channel_trade_wts', 'have a spare %item_link if anyone''s after one'),
('channel_trade_wts', '%item_link up for grabs, pst'),
-- channel_general_chat: General channel; zone flavour phrased as a general
-- statement (not "here"/"this place" -- readers are scattered across the
-- realm and have no idea where the speaker actually is), questions, gripes,
-- nothing checkable
('channel_general_chat', 'anyone else think some of these zones are way bigger than the map makes them look'),
('channel_general_chat', 'feels like professions never get enough love from anybody'),
('channel_general_chat', 'some questlines really drag once you''re past the halfway point'),
('channel_general_chat', 'gearing up an alt always takes longer than i remember'),
('channel_general_chat', 'bag space is never enough, no matter how many bags you buy'),
('channel_general_chat', 'leveling a second character always goes faster than the first one did'),
('channel_general_chat', 'some zones just have way better music than others'),
('channel_general_chat', 'flight paths could really use a rework'),
('channel_general_chat', 'professions feel like a second job some days'),
('channel_general_chat', 'some fights just aren''t fun no matter how many times you run them'),
('channel_general_chat', 'funny how one class always ends up over-represented in every group'),
('channel_general_chat', 'never really understood why some zones get so little traffic'),
-- channel_world_chat: World channel; general opinions/banter only, nothing
-- tied to a zone or place, nothing that assumes the reader is nearby
('channel_world_chat', 'feels like there''s always something going on somewhere on the server'),
('channel_world_chat', 'never gets old finding a new questline to dig into'),
('channel_world_chat', 'some days this game just clicks and other days it just doesn''t'),
('channel_world_chat', 'hard to beat a group that actually knows what it''s doing'),
('channel_world_chat', 'still surprises me how much content there is to get through'),
('channel_world_chat', 'some builds just feel better than others no matter what the numbers say'),
('channel_world_chat', 'never underestimate a good addon setup'),
('channel_world_chat', 'always another goal worth chasing in this game'),
('channel_world_chat', 'some nights everything just goes right'),
('channel_world_chat', 'funny how a slow session can turn into a good one out of nowhere'),
('channel_world_chat', 'still finding mechanics in this game that surprise me'),
('channel_world_chat', 'always somebody grinding something a little unusual');

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

INSERT INTO `hside_corpus` (`name`, `text`, `faction_tag`) VALUES
-- chat_faction_banter: tag_axis faction; generic pride/rivalry, nothing hostile or player-targeted
('chat_faction_banter', 'proud to fly Alliance colors out here', 0),
('chat_faction_banter', 'nothing like a stormwind sunset, honestly', 0),
('chat_faction_banter', 'gotta say, our side''s architecture just hits different', 0),
('chat_faction_banter', 'always good to see fellow Alliance out and about', 0),
('chat_faction_banter', 'ironforge''s still my favorite city, hard to beat', 0),
('chat_faction_banter', 'been Alliance since day one, never looked back', 0),
('chat_faction_banter', 'love running into other Alliance out in the world', 0),
('chat_faction_banter', 'darnassus at night is something else', 0),
('chat_faction_banter', 'proud to fly Horde colors out here', 1),
('chat_faction_banter', 'orgrimmar''s always felt like home to me', 1),
('chat_faction_banter', 'gotta say, our side''s got the better music honestly', 1),
('chat_faction_banter', 'always good to see fellow Horde out and about', 1),
('chat_faction_banter', 'thunder bluff''s still my favorite city, hard to beat', 1),
('chat_faction_banter', 'been Horde since day one, never looked back', 1),
('chat_faction_banter', 'love running into other Horde out in the world', 1),
('chat_faction_banter', 'undercity at night has a vibe you can''t get anywhere else', 1);

INSERT INTO `hside_corpus` (`name`, `text`, `zone_tag`) VALUES
-- chat_zone_musing: tag_axis zone; curated slice, ids verified against
-- azerothcore-wotlk-pb/data/sql/base/db_world/graveyard_zone.sql
('chat_zone_musing', 'elwynn''s such a nice starting spot, still comes back to visit sometimes', 12),
('chat_zone_musing', 'goldshire''s always got someone around, never really empty', 12),
('chat_zone_musing', 'dun morogh gets cold, but there''s something homey about it', 1),
('chat_zone_musing', 'ironforge''s tunnels still get me turned around sometimes', 1),
('chat_zone_musing', 'teldrassil''s trees never stop being impressive honestly', 141),
('chat_zone_musing', 'darnassus is quiet in a way i actually like', 141),
('chat_zone_musing', 'durotar''s harsher than it looks, respect anyone who started here', 14),
('chat_zone_musing', 'razor hill''s a good little hub, underrated honestly', 14),
('chat_zone_musing', 'tirisfal''s fog never really gets old', 85),
('chat_zone_musing', 'undercity''s layout still confuses me sometimes, not gonna lie', 85),
('chat_zone_musing', 'mulgore''s wide open plains are weirdly relaxing', 215),
('chat_zone_musing', 'thunder bluff''s a climb, but the view''s worth it', 215),
('chat_zone_musing', 'redridge always feels a bit sleepy, in a good way', 44),
('chat_zone_musing', 'lakeshire''s a nice quiet stop honestly', 44),
('chat_zone_musing', 'loch modan''s waterfall never gets old to look at', 38),
('chat_zone_musing', 'silverpine''s always felt a little gloomy, fits the vibe though', 130),
('chat_zone_musing', 'the barrens are as big as everyone says', 17),
('chat_zone_musing', 'crossroads is always busy, good spot to regroup', 17),
('chat_zone_musing', 'stranglethorn''s a lot louder than i remembered', 33),
('chat_zone_musing', 'booty bay''s chaos is honestly kind of charming', 33),
('chat_zone_musing', 'dustwallow''s swampy but grows on you after a while', 15),
('chat_zone_musing', 'borean tundra''s cold but the views are worth it', 3537),
('chat_zone_musing', 'howling fjord''s coastline is honestly underrated', 495),
('chat_zone_musing', 'dragonblight lives up to the name, bones everywhere', 65),
('chat_zone_musing', 'icecrown still gives me a chill every time, and not just the weather', 210);

INSERT INTO `hside_corpus` (`name`, `text`, `event_id`) VALUES
-- chat_gripe_general seasonal rows: real AzerothCore game_event ids
-- (azerothcore-wotlk-pb/data/sql/base/db_world/game_event.sql), unfalsifiable
-- flavor only -- dormant outside the event window via hs_corpus.cpp's
-- EventDormancyWhere
('chat_gripe_general', 'this whole zone smells like pumpkin, hallow''s end is really something', 12),
('chat_gripe_general', 'been dodging trick-or-treaters all week, hallow''s end never gets old', 12),
('chat_gripe_general', 'winter veil''s got the whole place looking festive', 2),
('chat_gripe_general', 'been unwrapping presents all morning, love this time of year', 2),
('chat_gripe_general', 'lunar festival elders are everywhere this year', 7),
('chat_gripe_general', 'love is in the air out there, hard to miss honestly', 8),
('chat_gripe_general', 'been handing out valentines all week for love is in the air', 8),
('chat_gripe_general', 'noblegarden eggs are hidden everywhere this year, wild', 9),
('chat_gripe_general', 'kids running around everywhere for children''s week, kind of nice actually', 10),
('chat_gripe_general', 'brewfest tents are up, place smells like beer and sausages', 24),
('chat_gripe_general', 'pilgrim''s bounty spread looks incredible this year', 26);
