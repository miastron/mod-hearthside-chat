-- The corpus row table: pre-generated chat lines selected by the corpus
-- tier with no runtime GPU work. enUS only; text_locN stay NULL and are
-- read with COALESCE(text_locN, text). No style baked in here: text is
-- clean, grammatical prose. Typos, abbreviation, and casing are applied at
-- delivery time by the style pass (hs_style.cpp), never stored.
--
-- Seed content matching hside_corpus_category.sql's categories. All 10 classes
-- and all 16 generator zones are seeded; card-gated lines beyond the
-- proof-of-concept pair remain a follow-up pass.
--
-- These rows are not only the corpus tier's fallback content: hs_generator.cpp
-- samples up to five of them per bucket as the tone reference in its generation
-- prompt, and a zone bucket holds only one or two, so a single row here
-- effectively sets the voice for everything generated into that bucket.
--
-- That is why the zone rows were rewritten once RAG grounding reached every
-- bucket. They had been authored under the same no-ground-truth constraint the
-- generator was, which left scenery description ("the views are worth it") as
-- about the only safe thing to say about a place. With the zone's own hside_rag
-- entry now in the prompt, a row like that is the weakest content in the table
-- and pulls generation back toward the vagueness the grounding exists to fix.
-- Anything added here should say what a player *does* in a place, not what it
-- looks like.

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

-- Makes this file safe to edit, which it was not before 2026-09-13.
--
-- UpdateFetcher hashes every file under base/ and re-runs any whose SHA1
-- changed (Updates.Redundancy = 1, the core default and this realm's
-- setting), a comment-only edit included. hside_corpus has no UNIQUE key, so
-- a plain re-apply of the INSERTs below duplicates every seeded row rather
-- than colliding loudly -- which is why the repo CLAUDE.md has had to treat
-- this file as frozen once shipped.
--
-- Scoped two ways, and both are load-bearing:
--
--   generated_at IS NULL -- that column is exactly the hand-authored /
--   generator-authored split (hs_generator.cpp sets it on insert), so a
--   re-apply reloads the seed content and leaves accumulated generator rows
--   alone. base/hside_rag.sql can open with a bare DELETE because nothing
--   writes to hside_rag at runtime; this table is written to constantly.
--
--   name IN (...) -- hside_corpus is seeded by more than one file.
--   base/hside_corpus_category_ambient_group.sql owns the three ambient_*
--   categories, and an unscoped DELETE here wipes its 30 rows on re-apply
--   with no way to get them back: that file's own hash is already tracked,
--   so UpdateFetcher will never run it again. (Learned the hard way on
--   2026-09-13 -- the first version of this header did exactly that.) A new
--   category added below must be added to this list too.
DELETE FROM `hside_corpus` WHERE `generated_at` IS NULL AND `name` IN (
  'chat_gripe_general', 'chat_class_banter', 'chat_levelband_musing',
  'channel_trade_wts', 'channel_general_chat', 'chat_faction_banter',
  'chat_zone_musing', 'chat_carded_focus',
  'opener_group_formed', 'opener_rez', 'opener_joint_kill',
  'opener_dungeon_complete', 'opener_prolonged_proximity'
);

INSERT INTO `hside_corpus` (`name`, `text`, `class_tag`) VALUES
-- chat_gripe_general: tag_axis none; unfalsifiable opinions/gripes
('chat_gripe_general', 'man this zone has been a grind lately', NULL),
('chat_gripe_general', 'still can''t believe that pack respawned so fast', NULL),
('chat_gripe_general', 'today''s been a rough one', NULL),
('chat_gripe_general', 'starting to think my luck is just bad this week', NULL),
('chat_gripe_general', 'these fetch quests never end', NULL),
('chat_gripe_general', 'some days you''re the hammer, some days you''re the nail', NULL),
('chat_gripe_general', 'i swear this mob has a personal vendetta against me', NULL),
('chat_gripe_general', 'at least the vendor trash from this lot sells for something', NULL),
-- chat_class_banter: tag_axis class; all 10 WotLK classes seeded (id 10 is
-- unused). Written against each class's own hside_rag entry, the same source
-- the generator now addresses for this bucket (hs_generator.cpp's
-- RagBlockForLabel), so these agree with the ground truth that will be in the
-- prompt beside them instead of pulling against it.
('chat_class_banter', 'another day, another shield to bash things with', 1),
('chat_class_banter', 'rage''s easy to build when everything here wants me dead', 1),
('chat_class_banter', 'charge in, ask questions never', 1),
('chat_class_banter', 'my arms are getting tired from all this swinging', 1),
('chat_class_banter', 'you never saw me, i was never here', 4),
('chat_class_banter', 'picked more locks today than i can count', 4),
('chat_class_banter', 'stealth makes everything easier', 4),
('chat_class_banter', 'sharpened my daggers this morning, feeling good', 4),
('chat_class_banter', 'portals are so much more convenient than walking everywhere', 8),
('chat_class_banter', 'conjured a whole feast, help yourselves', 8),
('chat_class_banter', 'sometimes i just blink for the fun of it', 8),
('chat_class_banter', 'frost or fire, can never decide', 8),
('chat_class_banter', 'handing out blessings before every pull is half my job', 2),
('chat_class_banter', 'bubbled out of that one, no shame in it', 2),
('chat_class_banter', 'people only notice my aura when i forget to switch it', 2),
('chat_class_banter', 'ret or prot, either way i am swinging something enormous', 2),
('chat_class_banter', 'spent longer taming this pet than i did on the last three quests', 3),
('chat_class_banter', 'feign death has gotten me out of more trouble than any cooldown', 3),
('chat_class_banter', 'traps do half the work if you set them before the pull', 3),
('chat_class_banter', 'my pet pulls more than i do and i have made peace with that', 3),
('chat_class_banter', 'shielding people before they pull beats healing them after', 5),
('chat_class_banter', 'nobody thanks you for the rez but they notice when you do not', 5),
('chat_class_banter', 'shadow feels like a completely different class from healing', 5),
('chat_class_banter', 'mind control is the most fun i have had in this game', 5),
('chat_class_banter', 'runes coming back on their own took some getting used to', 6),
('chat_class_banter', 'raising a ghoul mid-fight never stops being satisfying', 6),
('chat_class_banter', 'anti-magic shell has eaten more casts than i can count', 6),
('chat_class_banter', 'blood presence makes soloing a lot less painful', 6),
('chat_class_banter', 'dropping totems every pull is muscle memory at this point', 7),
('chat_class_banter', 'forgot to re-imbue my weapon and wondered why everything took so long', 7),
('chat_class_banter', 'enhancement feels like a warrior who can also heal a little', 7),
('chat_class_banter', 'totem placement matters more than people give it credit for', 7),
('chat_class_banter', 'everyone wants a healthstone right up until they need a summon too', 9),
('chat_class_banter', 'my dots do the work, i just have to stay alive long enough', 9),
('chat_class_banter', 'soul shard farming is the least fun part of this class', 9),
('chat_class_banter', 'the felhunter earns its keep against casters every time', 9),
('chat_class_banter', 'switching forms mid-fight is the whole appeal of this class', 11),
('chat_class_banter', 'travel form saves me more time than any mount would', 11),
('chat_class_banter', 'bear or cat depends entirely on who else showed up', 11),
('chat_class_banter', 'moonkin form looks ridiculous and i would not change a thing', 11);

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
('chat_levelband_musing', 'back to dailies again', 'endgame'),
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
-- channel_general_chat: General channel. Resolved per zone, so every reader
-- is standing in the same zone as the speaker: but one stored row is
-- replayed in every zone there is, so the text still has to be phrased as a
-- general statement (not "here"/"this place"), questions, gripes, nothing
-- checkable
--
-- Rewritten 2026-09-13. The originals were all true and none of them said
-- anything: "always another goal worth chasing in this game", "still
-- surprises me how much content there is to get through", "feels like
-- there's always something going on somewhere on the server". Every one
-- could have been typed about any MMO ever made, which is what made them
-- read as fake -- a real player complains about a *specific* thing. The
-- zone-agnostic constraint above is what pushed them that way: with "here"
-- off the table the easy move is to go abstract. The fix is to be concrete
-- about the parts of the game that are the same in every zone (travel,
-- bags, professions, repair bills, group composition) instead.
--
-- Second constraint, new: these rows are hs_generator.cpp's tone reference
-- for this bucket, so a vague row here teaches the model to generate more
-- vague rows. Every line below also passes the generator's own quality gate
-- (hs_gen_validate.cpp) -- no reply-shaped openers, no trend claims -- so
-- the sample never contradicts the rules the candidate is judged against.
('channel_general_chat', 'escort quests where the npc walks slower than you do are a special kind of pain'),
('channel_general_chat', 'the repair bill after a bad night costs more than the run brought in'),
('channel_general_chat', 'half my bag space is quest items i am scared to vendor'),
('channel_general_chat', 'riding skill costs more than the mount'),
('channel_general_chat', 'flight paths route you through three stops to go one zone over'),
('channel_general_chat', 'dual spec paid for itself the first week i had it'),
('channel_general_chat', 'engineering is a money pit and i would take it again on every alt'),
('channel_general_chat', 'cooking dailies are the only dailies i actually look forward to'),
('channel_general_chat', 'fishing is either the most relaxing thing in the game or the most boring'),
('channel_general_chat', 'every group needs a tank and somehow nobody ever wants to be one'),
('channel_general_chat', 'elite quests dropped in the middle of a zone chain stop a solo run dead'),
('channel_general_chat', 'corpse runs teach you the map faster than any guide ever did'),
-- General opinions/banter with nothing tied to a zone or place. These were
-- authored for a separate channel_world_chat category; 3.3.5a has no World
-- channel, so they were folded in here rather than discarded: they already
-- satisfy this category's "true in any zone" rule.
('channel_general_chat', 'heirlooms make leveling an alt feel like cheating in the best way'),
('channel_general_chat', 'emblems pile up faster than the vendors give me reasons to spend them'),
('channel_general_chat', 'glyphs are the cheapest upgrade in the game and half the server skips them'),
('channel_general_chat', 'the hearthstone cooldown is always five minutes longer than i need it to be'),
('channel_general_chat', 'gathering nodes vanish the second somebody else lands next to you'),
('channel_general_chat', 'resist gear sits in my bank taking up room for one fight a patch'),
('channel_general_chat', 'a good tank makes a pug feel like a guild run'),
('channel_general_chat', 'nothing humbles you like pulling two patrols at once'),
('channel_general_chat', 'the auction house cut always stings more on a slow sale'),
('channel_general_chat', 'ganking a lowbie says more about the ganker than the lowbie'),
('channel_general_chat', 'summoning stones have saved more raid nights than any buff'),
('channel_general_chat', 'quest text tells you exactly where to go and i end up running the wrong way anyway');

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
('opener_prolonged_proximity', 'we keep ending up in the same spot, huh.'),
('opener_prolonged_proximity', 'small world, running into you out here again.'),
('opener_prolonged_proximity', 'you sticking around this area too?'),
('opener_prolonged_proximity', 'guess we had the same idea coming out here.'),
-- chat_carded_focus: card-gated (%main_focus, %current_goal)
('chat_carded_focus', 'still grinding away at %main_focus'),
('chat_carded_focus', 'lately it is all about %current_goal for me'),
('chat_carded_focus', 'main focus right now is %main_focus'),
('chat_carded_focus', '%current_goal has been eating all my playtime');

INSERT INTO `hside_corpus` (`name`, `text`, `faction_tag`) VALUES
-- chat_faction_banter: tag_axis faction; generic pride/rivalry, nothing hostile or player-targeted
('chat_faction_banter', 'proud to fly Alliance colors out here', 0),
('chat_faction_banter', 'hard to beat stormwind for getting everywhere quickly', 0),
('chat_faction_banter', 'our side''s architecture just hits different', 0),
('chat_faction_banter', 'always good to see fellow Alliance out and about', 0),
('chat_faction_banter', 'ironforge''s still my favorite city, hard to beat', 0),
('chat_faction_banter', 'been Alliance since day one, never looked back', 0),
('chat_faction_banter', 'love running into other Alliance out in the world', 0),
('chat_faction_banter', 'long boat ride out to darnassus but it has everything in one place', 0),
('chat_faction_banter', 'proud to fly Horde colors out here', 1),
('chat_faction_banter', 'orgrimmar''s always felt like home to me', 1),
('chat_faction_banter', 'our side''s got the better music', 1),
('chat_faction_banter', 'always good to see fellow Horde out and about', 1),
('chat_faction_banter', 'thunder bluff''s still my favorite city, hard to beat', 1),
('chat_faction_banter', 'been Horde since day one, never looked back', 1),
('chat_faction_banter', 'love running into other Horde out in the world', 1),
('chat_faction_banter', 'the undercity zeppelin tower gets me to northrend quicker than anything', 1);

INSERT INTO `hside_corpus` (`name`, `text`, `zone_tag`) VALUES
-- chat_zone_musing: tag_axis zone; curated slice, ids verified against
-- azerothcore-wotlk-pb/data/sql/base/db_world/graveyard_zone.sql
('chat_zone_musing', 'elwynn''s such a nice starting spot, still comes back to visit sometimes', 12),
('chat_zone_musing', 'goldshire''s always got someone around, never really empty', 12),
('chat_zone_musing', 'dun morogh gets cold, but there''s something homey about it', 1),
('chat_zone_musing', 'ironforge''s tunnels still get me turned around sometimes', 1),
('chat_zone_musing', 'spiders all through these woods, watch your step around dolanaar', 141),
('chat_zone_musing', 'darnassus is quiet in a way i actually like', 141),
('chat_zone_musing', 'durotar''s harsher than it looks, respect anyone who started here', 14),
('chat_zone_musing', 'razor hill''s a good little hub', 14),
('chat_zone_musing', 'tirisfal''s fog never really gets old', 85),
('chat_zone_musing', 'undercity''s layout still turns me around', 85),
('chat_zone_musing', 'cougars past bloodhoof village will jump you if you pull more than one', 215),
('chat_zone_musing', 'stepped off a bridge again, the lifts are the only safe way between rises', 215),
('chat_zone_musing', 'redridge always feels a bit sleepy', 44),
('chat_zone_musing', 'lakeshire''s a nice quiet stop', 44),
('chat_zone_musing', 'the troggs pushing down on thelsamar respawn faster than i can clear them', 38),
('chat_zone_musing', 'silverpine''s always felt a little gloomy', 130),
('chat_zone_musing', 'the barrens are as big as everyone says', 17),
('chat_zone_musing', 'crossroads is always busy, good spot to regroup', 17),
('chat_zone_musing', 'stranglethorn''s a lot louder than i remembered', 33),
('chat_zone_musing', 'booty bay''s got a brawl on the docks most days', 33),
('chat_zone_musing', 'dustwallow''s swampy the whole way through', 15),
('chat_zone_musing', 'nerubians in the tunnels are rough solo, worth finding a group first', 3537),
('chat_zone_musing', 'vrykul around utgarde keep hit a lot harder than the wildlife does', 495),
('chat_zone_musing', 'dragonblight lives up to the name, bones everywhere', 65),
('chat_zone_musing', 'icecrown still gives me a chill every time', 210);

INSERT INTO `hside_corpus` (`name`, `text`, `event_id`) VALUES
-- chat_gripe_general seasonal rows: real AzerothCore game_event ids
-- (azerothcore-wotlk-pb/data/sql/base/db_world/game_event.sql), unfalsifiable
-- flavor only: dormant outside the event window via hs_corpus.cpp's
-- EventDormancyWhere
('chat_gripe_general', 'this whole zone smells like pumpkin for hallow''s end', 12),
('chat_gripe_general', 'been dodging trick-or-treaters all week', 12),
('chat_gripe_general', 'winter veil''s got the whole place looking festive', 2),
('chat_gripe_general', 'been unwrapping presents all morning, love this time of year', 2),
('chat_gripe_general', 'lunar festival elders are everywhere this year', 7),
('chat_gripe_general', 'love is in the air out there', 8),
('chat_gripe_general', 'been handing out valentines all week for love is in the air', 8),
('chat_gripe_general', 'noblegarden eggs are hidden everywhere this year', 9),
('chat_gripe_general', 'kids running around everywhere for children''s week', 10),
('chat_gripe_general', 'brewfest tents are up, place smells like beer and sausages', 24),
('chat_gripe_general', 'pilgrim''s bounty spread looks incredible this year', 26);

-- Retrospective register, added 2026-09-20. WotLK is a 2010 expansion and the
-- people playing it now know that: they are replaying content they half
-- remember, catching up on content they missed the first time, or comparing it
-- to where the game went afterwards. None of the seed rows carried that voice,
-- so the generator had no tone reference for it and wrote every line as though
-- the content were brand new -- which is also where a lot of its invented
-- mechanics came from ("every single profession in this zone is on cooldown
-- right now", realm 2026-09-20), since a model with nothing true to say about
-- the present will make something up.
--
-- Deliberately a sprinkle, not a sweep: roughly one row in six across these two
-- categories. A bot that ALWAYS talks about how it used to be is its own tic,
-- and the generator amplifies whatever it is shown (see the hedge-tag pass,
-- 2026-09-17).
--
-- Safe under R3/the anti-invention rule for the same reason it reads well:
-- memory and opinion are unfalsifiable. "the group finder changed this game
-- more than any patch did" cannot be checked and found wrong the way an
-- invented item or quest can.
INSERT INTO `hside_corpus` (`name`, `text`) VALUES
('channel_general_chat', 'played this the first time around and remember almost none of it correctly'),
('channel_general_chat', 'the group finder changed this game more than any patch ever did'),
('channel_general_chat', 'everything after this expansion lost the plot as far as i am concerned'),
('channel_general_chat', 'back when this was new nobody had any of the fights figured out'),
('channel_general_chat', 'retail went a direction i never followed and i do not regret it'),
('channel_general_chat', 'people used to spend an hour forming a group for one run'),
('channel_general_chat', 'missed most of this content when it was current, catching up on it now'),
('channel_general_chat', 'half the tricks everyone takes for granted now took months to work out'),
('channel_general_chat', 'this is the version everyone says they miss and they actually meant it'),
('channel_general_chat', 'my memory of this place was a lot kinder than the actual grind is'),
('chat_gripe_general', 'forgot how long all of this took before people optimised it to death'),
('chat_gripe_general', 'coming back years later means relearning things i never really knew'),
('chat_gripe_general', 'everyone knows the route now, felt a lot less solved at the time'),
('chat_zone_musing', 'remember this zone being bigger, everything shrinks with time'),
('chat_zone_musing', 'first ran through here years ago and barely recognise half of it');
