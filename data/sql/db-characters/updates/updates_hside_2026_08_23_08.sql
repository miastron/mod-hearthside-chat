-- Corpus breadth pass: faction/zone tag axes wired end-to-end
-- (hs_corpus.cpp's TagWhereFor, previously a stub for both), plus seasonal
-- content on the event_id/EventDormancyWhere mechanism that was already
-- fully built but had zero rows using it. Also widens the channel_* pool
-- (§4.17) from 6 to 12 rows per channel. See base/hside_corpus_category.sql
-- and base/hside_corpus.sql for the full seed this mirrors.

INSERT INTO `hside_corpus_category` (`name`, `tag_axis`, `card_gated`, `channel`, `is_opener`) VALUES
('chat_faction_banter', 'faction', 0, NULL, 0),
('chat_zone_musing',    'zone',    0, NULL, 0);

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
('channel_trade_wts', 'clearing out bags, WTS %item_link'),
('channel_trade_wts', 'got extra %item_link, make an offer'),
('channel_trade_wts', 'selling off some %item_link, pst if interested'),
('channel_trade_wts', 'WTS %item_link, first come first served'),
('channel_trade_wts', 'have a spare %item_link if anyone''s after one'),
('channel_trade_wts', '%item_link up for grabs, pst'),
('channel_general_chat', 'anyone else losing track of what day it is'),
('channel_general_chat', 'server''s felt busier than usual lately'),
('channel_general_chat', 'does this zone ever feel like a maze to anyone else'),
('channel_general_chat', 'nice change of pace out here today'),
('channel_general_chat', 'anyone know if there''s a shortcut through here'),
('channel_general_chat', 'been a pretty chill session so far'),
('channel_world_chat', 'never gets old seeing new places out here'),
('channel_world_chat', 'lot going on across the realm today'),
('channel_world_chat', 'always somewhere new worth checking out'),
('channel_world_chat', 'good day to just take it all in'),
('channel_world_chat', 'feels like everyone''s out doing something today'),
('channel_world_chat', 'still finding spots I''ve never seen before');

INSERT INTO `hside_corpus` (`name`, `text`, `faction_tag`) VALUES
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
