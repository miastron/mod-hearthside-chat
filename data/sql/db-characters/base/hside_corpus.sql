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
  'channel_trade_wts', 'channel_general_chat', 'channel_general_band',
  'chat_faction_banter',
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
('channel_trade_wts', '%item_link up for grabs, pst');

-- channel_general_chat: General channel, band-neutral. Resolved per zone and
-- replayed in every zone at every level, so each line has to be true at any
-- level 1-80: no zone or dungeon names, no systems that only exist at one
-- stage (emblems, heirlooms, glyphs, flying). Level-specific talk lives in
-- channel_general_band below.
--
-- Rewritten 2026-10-09 to read like real General chat: mostly questions to
-- the channel, group asks, short reactions and gripes, a little banter and
-- AFK noise, in place of the earlier polished one-off observations. Many are
-- questions or group asks, which Hs_QualityGate's standalone-line rules
-- reject for generated rows, so Tests/test_hs_gen_validate_seed_corpus.cpp
-- exempts the two channel_general_* categories from the question rule.
INSERT INTO `hside_corpus` (`name`, `text`) VALUES
('channel_general_chat', 'anyone know where the flight master is'),
('channel_general_chat', 'is it just me or is the server lagging'),
('channel_general_chat', 'how do i turn off auto loot'),
('channel_general_chat', 'how do i change where my hearthstone takes me'),
('channel_general_chat', 'where do i find the auction house'),
('channel_general_chat', 'how do i link an item in chat'),
('channel_general_chat', 'what does need before greed actually mean'),
('channel_general_chat', 'can you mount up indoors'),
('channel_general_chat', 'is the bank shared between my characters'),
('channel_general_chat', 'how do i leave a party'),
('channel_general_chat', 'how long is the hearthstone cooldown supposed to be'),
('channel_general_chat', 'how do i see who is in my guild'),
('channel_general_chat', 'anyone know how to inspect another player'),
('channel_general_chat', 'where can i learn first aid'),
('channel_general_chat', 'can you trade with the other faction'),
('channel_general_chat', 'what is the fastest way to make some gold'),
('channel_general_chat', 'does anyone have a spare bag they can part with'),
('channel_general_chat', 'how do i change my keybindings'),
('channel_general_chat', 'why can''t i see trade chat'),
('channel_general_chat', 'anyone else getting kicked to the login screen'),
('channel_general_chat', 'how do i feed my pet'),
('channel_general_chat', 'where do i go to get my corpse back'),
('channel_general_chat', 'does dying cost anything besides repairs'),
('channel_general_chat', 'how do i check my reputation'),
('channel_general_chat', 'how do i invite someone to a guild'),
('channel_general_chat', 'is it worth picking up a second profession'),
('channel_general_chat', 'what does the exclamation mark over an npc mean'),
('channel_general_chat', 'how do i get to the other continent'),
('channel_general_chat', 'anyone know how to set up a macro'),
('channel_general_chat', 'which is better, a cheap bag now or saving for a big one'),
('channel_general_chat', 'what do the colors on item names mean'),
('channel_general_chat', 'is there a way to hide my helmet'),
('channel_general_chat', 'anyone want to group for quests'),
('channel_general_chat', 'need one more for an elite quest'),
('channel_general_chat', 'lfg, any quests anyone needs help with'),
('channel_general_chat', 'looking for a group to do some quests'),
('channel_general_chat', 'anyone need a tank'),
('channel_general_chat', 'healer here, anyone want to group'),
('channel_general_chat', 'any dps want to group up'),
('channel_general_chat', 'anyone want to run a dungeon'),
('channel_general_chat', 'lf1m dps for a dungeon, pst'),
('channel_general_chat', 'anyone want to team up for an elite'),
('channel_general_chat', 'need a couple more people for this elite'),
('channel_general_chat', 'this escort is so slow'),
('channel_general_chat', 'died to a mob that was gray two levels ago'),
('channel_general_chat', 'i keep pulling extra mobs by accident'),
('channel_general_chat', 'my bags are full again already'),
('channel_general_chat', 'someone just took the node i was running to'),
('channel_general_chat', 'lost another half hour to a corpse run'),
('channel_general_chat', 'the repair bill is brutal today'),
('channel_general_chat', 'wiped on a trash pack, i can''t even'),
('channel_general_chat', 'the respawn timer on this mob is ridiculous'),
('channel_general_chat', 'forty kills and not one drop'),
('channel_general_chat', 'flight path took forever, should have walked'),
('channel_general_chat', 'stood in fire again, my own fault'),
('channel_general_chat', 'somebody pulled the whole room, great'),
('channel_general_chat', 'pulled one mob and brought three friends'),
('channel_general_chat', 'ran out of food at the worst time'),
('channel_general_chat', 'lag spike killed me, thanks'),
('channel_general_chat', 'the real boss is the flight path'),
('channel_general_chat', 'my hearthstone is on cooldown, naturally'),
('channel_general_chat', 'one more quest and then i log off, famous last words'),
('channel_general_chat', 'caught another boot while fishing'),
('channel_general_chat', 'tank pulls, healer panics, dps blames the healer'),
('channel_general_chat', 'if you see someone running in circles that was me'),
('channel_general_chat', 'who needs a map when you have confidence'),
('channel_general_chat', 'my bags have bags now'),
('channel_general_chat', 'gold goes in and nothing comes out'),
('channel_general_chat', 'my pet is more useful than my group last night'),
('channel_general_chat', 'anyone else just get disconnected');

-- channel_general_band: General channel lines for one level band, drawn by
-- the speaker's level (level_band_tag). Same register as channel_general_chat
-- but free to name what that band actually does in 3.3.5a.
INSERT INTO `hside_corpus` (`name`, `text`, `level_band_tag`) VALUES
('channel_general_band', 'anyone know where the next quest hub is', 'low'),
('channel_general_band', 'when do i get my first talent point', 'low'),
('channel_general_band', 'anyone want to run ragefire chasm', 'low'),
('channel_general_band', 'lf1m for deadmines, need a healer', 'low'),
('channel_general_band', 'how do i get to wailing caverns', 'low'),
('channel_general_band', 'anyone want to do shadowfang keep', 'low'),
('channel_general_band', 'is deadmines worth running at this level', 'low'),
('channel_general_band', 'at what level do i get a mount', 'low'),
('channel_general_band', 'no mount until twenty, this is rough', 'low'),
('channel_general_band', 'died to a boar again', 'low'),
('channel_general_band', 'anyone else stuck on the kill ten quest', 'low'),
('channel_general_band', 'first time on this class, any tips', 'low'),
('channel_general_band', 'where do i learn my new spells', 'low'),
('channel_general_band', 'are the starting zones always this crowded', 'low'),
('channel_general_band', 'is it worth picking up two gathering professions', 'low'),
('channel_general_band', 'anyone selling small bags cheap', 'low'),
('channel_general_band', 'need a few more levels before i try the dungeon', 'low'),
('channel_general_band', 'anyone want to help with a group quest', 'low'),
('channel_general_band', 'should i finish the starting zone or move on', 'low'),
('channel_general_band', 'what do i do with all this linen', 'low'),
('channel_general_band', 'where do i buy a better weapon at this level', 'low'),
('channel_general_band', 'ran past a mob and now i have six on me', 'low'),
('channel_general_band', 'how do hunters get their first pet', 'low'),
('channel_general_band', 'lost in the starting zone again', 'low'),
('channel_general_band', 'how do i use my hearthstone', 'low'),
('channel_general_band', 'just bought my first mount and now i''m broke', 'mid'),
('channel_general_band', 'just hit forty, time to save for the riding skill', 'mid'),
('channel_general_band', 'lf1m for scarlet monastery', 'mid'),
('channel_general_band', 'anyone want to run zul''farrak', 'mid'),
('channel_general_band', 'lfg for uldaman', 'mid'),
('channel_general_band', 'lf tank for maraudon', 'mid'),
('channel_general_band', 'lfg blackrock depths, need a healer', 'mid'),
('channel_general_band', 'how do i get to stranglethorn from here', 'mid'),
('channel_general_band', 'stranglethorn is full of tigers that hate me', 'mid'),
('channel_general_band', 'tanaris is huge, anyone know where the next quest is', 'mid'),
('channel_general_band', 'where do i get dual talent specialization', 'mid'),
('channel_general_band', 'the escort quests at this level are so long', 'mid'),
('channel_general_band', 'anyone need help with an elite quest in the barrens', 'mid'),
('channel_general_band', 'which scarlet monastery wing is best for xp', 'mid'),
('channel_general_band', 'lf group for dire maul', 'mid'),
('channel_general_band', 'does anyone need a healer for uldaman', 'mid'),
('channel_general_band', 'almost sixty, so ready for outland', 'mid'),
('channel_general_band', 'grinding mobs for riding gold, send help', 'mid'),
('channel_general_band', 'dungeon finder queue is taking forever for dps', 'mid'),
('channel_general_band', 'how long is the run to blackrock mountain', 'mid'),
('channel_general_band', 'is blackrock depths really that long', 'mid'),
('channel_general_band', 'lf2m for maraudon, whisper me', 'mid'),
('channel_general_band', 'anyone know where the epic mount trainer is', 'mid'),
('channel_general_band', 'need two more for the elite quest', 'mid'),
('channel_general_band', 'can''t believe the mount costs this much', 'mid'),
('channel_general_band', 'anyone know where to get flying', 'high'),
('channel_general_band', 'how do i get to outland', 'high'),
('channel_general_band', 'anyone know how to get to northrend', 'high'),
('channel_general_band', 'lf1m hellfire ramparts', 'high'),
('channel_general_band', 'lfg for the blood furnace', 'high'),
('channel_general_band', 'need a tank for shattered halls', 'high'),
('channel_general_band', 'anyone running utgarde keep', 'high'),
('channel_general_band', 'lfg for the nexus', 'high'),
('channel_general_band', 'azjol-nerub, need a healer', 'high'),
('channel_general_band', 'still saving up for cold weather flying', 'high'),
('channel_general_band', 'dragonblight is huge, where is the next quest hub', 'high'),
('channel_general_band', 'lf heroic outland, need a tank', 'high'),
('channel_general_band', 'is it worth running outland dungeons for gear', 'high'),
('channel_general_band', 'borean tundra or howling fjord first', 'high'),
('channel_general_band', 'ran out of gold again, flying mount', 'high'),
('channel_general_band', 'how do i get to the dark portal', 'high'),
('channel_general_band', 'anyone need help with a group quest in zul''drak', 'high'),
('channel_general_band', 'anyone know a good place to level from 70', 'high'),
('channel_general_band', 'lfg for gundrak', 'high'),
('channel_general_band', 'just hit seventy, where to next', 'high'),
('channel_general_band', 'anyone up for the culling of stratholme', 'high'),
('channel_general_band', 'hellfire is so crowded tonight', 'high'),
('channel_general_band', 'where do i train expert riding', 'high'),
('channel_general_band', 'can i fly in northrend yet', 'high'),
('channel_general_band', 'lf2m for the slave pens', 'high'),
('channel_general_band', 'lf2m heroic, need a tank and healer', 'endgame'),
('channel_general_band', 'how many emblems of triumph for the chest piece', 'endgame'),
('channel_general_band', 'emblems of frost or triumph, which first', 'endgame'),
('channel_general_band', 'need a healer for icc, anyone', 'endgame'),
('channel_general_band', 'lf2m icc 10, need tank and healer', 'endgame'),
('channel_general_band', 'anyone running vault of archavon', 'endgame'),
('channel_general_band', 'wintergrasp is up, anyone coming', 'endgame'),
('channel_general_band', 'wintergrasp queue is so long', 'endgame'),
('channel_general_band', 'how do i get started with the argent tournament dailies', 'endgame'),
('channel_general_band', 'what gearscore do i need for trial of the crusader', 'endgame'),
('channel_general_band', 'lfg trial of the champion heroic', 'endgame'),
('channel_general_band', 'random heroic queue for dps is so long', 'endgame'),
('channel_general_band', 'need one more for toc 10', 'endgame'),
('channel_general_band', 'what gear do i need before icc', 'endgame'),
('channel_general_band', 'anyone need the daily heroic done for emblems', 'endgame'),
('channel_general_band', 'gearscore check before anything, as usual', 'endgame'),
('channel_general_band', 'lf more for naxxramas 10', 'endgame'),
('channel_general_band', 'obsidian sanctum, anyone need a tank', 'endgame'),
('channel_general_band', 'anyone running ulduar tonight', 'endgame'),
('channel_general_band', 'need a tank for ulduar 10', 'endgame'),
('channel_general_band', 'anyone know when the next wintergrasp battle starts', 'endgame'),
('channel_general_band', 'tank here, lf heroic, any healers', 'endgame'),
('channel_general_band', 'lfm icc 25, need ranged dps', 'endgame'),
('channel_general_band', 'daily heroic done, now to wait for the reset', 'endgame'),
('channel_general_band', 'my gearscore is fine, my attention span isn''t', 'endgame');

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
-- opener_*: fired only by hs_opener.cpp's shared-context triggers
('opener_group_formed', 'alright, let''s do this.'),
('opener_group_formed', 'good to have another hand.'),
('opener_group_formed', 'ready when you are.'),
('opener_joint_kill', 'nice work on that one.'),
('opener_joint_kill', 'gg, that one hit harder than it looked.'),
('opener_joint_kill', 'good teamwork there.'),
('opener_joint_kill', 'that was closer than i''d like.'),
('opener_rez', 'back up, thanks.'),
('opener_dungeon_complete', 'good run.'),
('opener_dungeon_complete', 'that went smoother than i expected.'),
('opener_dungeon_complete', 'solid group, that one.'),
('opener_dungeon_complete', 'nice, one down.'),
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

-- Hand-authored expansion, 2026-10-07. The idle-time generator (1B model) had
-- filled every bucket with lines like "boryns feel like a never-ending, small
-- world" and "gz, u opened the door on that one" (as the reply to a rez), and
-- the realm now runs with HearthsideChat.Generator.Enable = 0, so this is the
-- content the corpus tier speaks. Opener lines are written to who actually
-- speaks them (hs_opener.cpp): opener_rez is the bot that was rezzed, by a
-- spirit healer or another bot as often as not, so it does not thank anyone;
-- opener_prolonged_proximity fires between strangers, so nothing says "again";
-- opener_group_formed usually fires when the player joins the bot's group,
-- so nothing thanks anyone for an invite. The seed rows that said those three
-- things were removed above.

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
-- chat_gripe_general: tag_axis none (40)
('chat_gripe_general', 'my bags are full again and i just emptied them'),
('chat_gripe_general', 'every quest wants ten of something that drops one in five'),
('chat_gripe_general', 'repair bill is getting out of hand'),
('chat_gripe_general', 'got dazed off my mount by something two levels lower than me'),
('chat_gripe_general', 'spent more time running back to my corpse than questing tonight'),
('chat_gripe_general', 'the one mob i need is always the one somebody just tagged'),
('chat_gripe_general', 'the quest turn-in is always on the other side of the zone'),
('chat_gripe_general', 'need to vendor stuff and the nearest vendor is miles away'),
('chat_gripe_general', 'patrols always show up right when i drink'),
('chat_gripe_general', 'forgot to train again and wondered why my damage felt low'),
('chat_gripe_general', 'just missed the boat by about two seconds'),
('chat_gripe_general', 'zeppelins and boats take longer than the quests themselves'),
('chat_gripe_general', 'gathered for an hour and the auction house says its worth nothing'),
('chat_gripe_general', 'lost every single roll tonight'),
('chat_gripe_general', 'used my last potion on a fight i would have won anyway'),
('chat_gripe_general', 'someone ninja''d the herb i was walking to'),
('chat_gripe_general', 'i swear the drop rate on quest items gets worse when you need them'),
('chat_gripe_general', 'resisted three times in a row, cool'),
('chat_gripe_general', 'hearth is on cooldown, of course it is'),
('chat_gripe_general', 'pulled one mob and got five'),
('chat_gripe_general', 'every bank slot full of things i might need one day'),
('chat_gripe_general', 'clicked the wrong flight point and now i get to sit through it'),
('chat_gripe_general', 'hearthstone is set to the wrong inn again'),
('chat_gripe_general', 'that mob ran away at low health and brought friends back'),
('chat_gripe_general', 'need one more gold for the next skill and i am broke'),
('chat_gripe_general', 'forgot to buy food again'),
('chat_gripe_general', 'nothing i loot today is even worth vendoring'),
('chat_gripe_general', 'escort npc aggroed the whole camp and then died'),
('chat_gripe_general', 'accidentally vendored something i needed for a quest'),
('chat_gripe_general', 'that quest item was in the last crate i checked, every time'),
('chat_gripe_general', 'lost the roll on the only blue that dropped all night'),
('chat_gripe_general', 'some days the respawn timer is faster than my kill speed'),
('chat_gripe_general', 'every flight path stops in two places i do not care about'),
('chat_gripe_general', 'stood in the wrong spot and died, my own fault honestly'),
('chat_gripe_general', 'cant find the last item for this quest anywhere'),
('chat_gripe_general', 'evaded mobs reset right when they were almost dead'),
('chat_gripe_general', 'walked into a pack i did not see behind the tree'),
('chat_gripe_general', 'the npc i need to talk to is always the one that wanders around'),
('chat_gripe_general', 'my durability is red and the repair guy is nowhere'),
('chat_gripe_general', 'spent all my gold on training and forgot about repairs');

INSERT INTO `hside_corpus` (`name`, `text`, `class_tag`) VALUES
-- chat_class_banter: warrior (1)
('chat_class_banter', 'no rage at the start of a fight is the worst part of leveling a warrior', 1),
('chat_class_banter', 'victory rush is the only self heal i get so i spam it', 1),
('chat_class_banter', 'titan''s grip means two two-handers and i am not giving that up', 1),
('chat_class_banter', 'bladestorm into a pack never gets old', 1),
('chat_class_banter', 'swapping stances just to use one ability is still annoying', 1),
('chat_class_banter', 'heroic strike queued every swing and my rage is gone', 1),
('chat_class_banter', 'shield wall and hope the healer is paying attention', 1),
('chat_class_banter', 'execute range is the only time i feel strong', 1),
-- paladin (2)
('chat_class_banter', 'consecration down and everything just comes to me', 2),
('chat_class_banter', 'lay on hands has saved more runs than i can count', 2),
('chat_class_banter', 'everyone wants kings and nobody says thanks', 2),
('chat_class_banter', 'forgot righteous fury again and nothing would stay on me', 2),
('chat_class_banter', 'divine storm crits are the reason i went ret', 2),
('chat_class_banter', 'rebuffing everyone after a wipe takes longer than the pull', 2),
('chat_class_banter', 'a ret pally out of mana is just a guy holding a big sword', 2),
('chat_class_banter', 'bubble hearth is not cowardice, its strategy', 2),
-- hunter (3)
('chat_class_banter', 'ran out of arrows halfway through the zone again', 3),
('chat_class_banter', 'my pet is unhappy again, need to go buy it food', 3),
('chat_class_banter', 'misdirect on the tank every pull and the tank still complains', 3),
('chat_class_banter', 'went beast mastery for the exotic pets, no regrets', 3),
('chat_class_banter', 'the quiver taking a bag slot still bugs me', 3),
('chat_class_banter', 'aspect of the pack is great until someone takes one hit', 3),
('chat_class_banter', 'hunter weapon jokes are funny to everyone except me', 3),
('chat_class_banter', 'half my bag is pet food and ammo', 3),
-- rogue (4)
('chat_class_banter', 'reapplying poisons every hour is a rogue tax', 4),
('chat_class_banter', 'combo points vanish the second i switch targets', 4),
('chat_class_banter', 'vanish and walk away, that is my whole escape plan', 4),
('chat_class_banter', 'pickpocket everything with pockets, it adds up', 4),
('chat_class_banter', 'fan of knives on a pack feels great after years of single target', 4),
('chat_class_banter', 'i sap the patrol, the tank sorts out the rest', 4),
('chat_class_banter', 'sprint plus stealth gets me past basically anything', 4),
('chat_class_banter', 'waiting on energy ticks is half of being a rogue', 4),
-- priest (5)
('chat_class_banter', 'psychic scream sent one right into the next pack, my bad', 5),
('chat_class_banter', 'sacred candles for fortitude add up fast', 5),
('chat_class_banter', 'penance is the best button i have', 5),
('chat_class_banter', 'dispersion is my answer to running out of mana', 5),
('chat_class_banter', 'weakened soul is the only thing stopping me shielding everyone forever', 5),
('chat_class_banter', 'levitating off a cliff never stops being fun', 5),
('chat_class_banter', 'in shadowform people mostly stop asking me to heal', 5),
('chat_class_banter', 'prayer of mending bouncing around feels like free healing', 5),
-- death knight (6)
('chat_class_banter', 'death grip the caster over and nobody has to chase anything', 6),
('chat_class_banter', 'starting at 55 felt like skipping half the game', 6),
('chat_class_banter', 'fallen crusader goes on every weapon i get', 6),
('chat_class_banter', 'death gate back to acherus beats waiting on my hearth', 6),
('chat_class_banter', 'path of frost and i just jog across the lake', 6),
('chat_class_banter', 'army of the dead takes forever to cast but worth it', 6),
('chat_class_banter', 'everyone assumes dks are bad, some of us read the tooltips', 6),
('chat_class_banter', 'death and decay on a pack and suddenly i am the tank', 6),
-- shaman (7)
('chat_class_banter', 'always keep ankhs in my bag, reincarnation is the best panic button', 7),
('chat_class_banter', 'chain heal bouncing through the group is the best feeling as resto', 7),
('chat_class_banter', 'ghost wolf makes the walk between quests bearable', 7),
('chat_class_banter', 'feral spirit wolves out-damage me sometimes', 7),
('chat_class_banter', 'water walking turns any lake into a shortcut', 7),
('chat_class_banter', 'totemic recall before running off is a habit now', 7),
('chat_class_banter', 'earth shield on the tank and forget about it for a bit', 7),
('chat_class_banter', 'after a wipe i am the one who gets back up', 7),
-- mage (8)
('chat_class_banter', 'aoe frost grinding is the only way i level now', 8),
('chat_class_banter', 'sheeped a mob and someone broke it two seconds later', 8),
('chat_class_banter', 'everyone treats me like a water vending machine', 8),
('chat_class_banter', 'ice block and just wait it out', 8),
('chat_class_banter', 'evocation and pray nobody pulls in the meantime', 8),
('chat_class_banter', 'portal requests in every city, should start charging more', 8),
('chat_class_banter', 'mirror image is for when i pull threat, which is often', 8),
('chat_class_banter', 'frost nova then blink is how every fight ends', 8),
-- warlock (9)
('chat_class_banter', 'life tap, dot everything, life tap again', 9),
('chat_class_banter', 'my voidwalker tanks better than half the warriors i group with', 9),
('chat_class_banter', 'soulstone on the healer before every boss', 9),
('chat_class_banter', 'seed of corruption on a pack is the most fun aoe there is', 9),
('chat_class_banter', 'metamorphosis makes me feel like a raid boss for a bit', 9),
('chat_class_banter', 'feared it and it ran straight into another pack', 9),
('chat_class_banter', 'the imp only comes out for blood pact', 9),
('chat_class_banter', 'drain soul at the end of every fight to keep shards up', 9),
-- druid (11)
('chat_class_banter', 'battle rezzed the healer and nobody even noticed', 11),
('chat_class_banter', 'innervate goes to the healer, not the mage who asked', 11),
('chat_class_banter', 'flight form means i never wait at a flight master', 11),
('chat_class_banter', 'aquatic form makes underwater quests easy', 11),
('chat_class_banter', 'keeping lifebloom rolling on the tank is all i think about', 11),
('chat_class_banter', 'everyone forgets druids can tank until the warrior leaves', 11),
('chat_class_banter', 'shifting out of form just to drink is the worst part', 11),
('chat_class_banter', 'hurricane on a pack and they all just stand in it', 11);

INSERT INTO `hside_corpus` (`name`, `text`, `level_band_tag`) VALUES
-- chat_levelband_musing: low (1-19)
('chat_levelband_musing', 'spent all my silver at the class trainer again', 'low'),
('chat_levelband_musing', 'first real bag finally, no more tiny pouches', 'low'),
('chat_levelband_musing', 'hit 10 and got my first talent point, no idea what to pick', 'low'),
('chat_levelband_musing', 'walking everywhere until 20 is rough', 'low'),
('chat_levelband_musing', 'picked two professions and have no clue which is good', 'low'),
('chat_levelband_musing', 'died to a mob two levels below me, embarrassing', 'low'),
('chat_levelband_musing', 'first dungeon and nobody knew the way', 'low'),
('chat_levelband_musing', 'still wearing whatever the quests hand me', 'low'),
('chat_levelband_musing', 'counting copper until the next skill rank', 'low'),
('chat_levelband_musing', 'bags full of grey junk and linen cloth', 'low'),
-- mid (20-59)
('chat_levelband_musing', 'mount at 20 changed everything', 'mid'),
('chat_levelband_musing', 'saving every gold for the epic mount at 40', 'mid'),
('chat_levelband_musing', 'ran scarlet monastery so many times i know every patrol', 'mid'),
('chat_levelband_musing', 'the 30s are the slowest stretch of leveling', 'mid'),
('chat_levelband_musing', 'quest log full of stuff i already outleveled', 'mid'),
('chat_levelband_musing', 'gnomeregan is a maze and i hate it', 'mid'),
('chat_levelband_musing', 'ran zul''farrak three times for one item', 'mid'),
('chat_levelband_musing', 'every zone has one quest chain that sends me back and forth five times', 'mid'),
('chat_levelband_musing', 'ran out of green quests, back to grinding', 'mid'),
('chat_levelband_musing', 'outland still feels a long way off', 'mid'),
-- high (60-79)
('chat_levelband_musing', 'first outland quest green replaced half my gear', 'high'),
('chat_levelband_musing', 'hellfire peninsula is packed every time i go through', 'high'),
('chat_levelband_musing', 'aldor or scryers, still cant decide', 'high'),
('chat_levelband_musing', 'saving up for cold weather flying at 77', 'high'),
('chat_levelband_musing', 'nearly done with outland, northrend next', 'high'),
('chat_levelband_musing', 'ran ramparts until i was sick of it', 'high'),
('chat_levelband_musing', 'utgarde keep is my new favorite xp run', 'high'),
('chat_levelband_musing', 'northrend quest gear makes my outland stuff look silly', 'high'),
('chat_levelband_musing', 'the last few levels before 80 drag on forever', 'high'),
('chat_levelband_musing', 'flying changed how i quest completely', 'high'),
-- endgame (80)
('chat_levelband_musing', 'heroic daily done, regular daily next', 'endgame'),
('chat_levelband_musing', 'frost emblems every day, slowly getting there', 'endgame'),
('chat_levelband_musing', 'argent tournament dailies again, i know the route by heart', 'endgame'),
('chat_levelband_musing', 'vault of archavon is always worth a look', 'endgame'),
('chat_levelband_musing', 'one more piece and i can finally go for icc', 'endgame'),
('chat_levelband_musing', 'random dungeon queue and its forge of souls again', 'endgame'),
('chat_levelband_musing', 'gearscore checks for a heroic, really', 'endgame'),
('chat_levelband_musing', 'triumph emblems piling up and nothing left to buy', 'endgame'),
('chat_levelband_musing', 'weekly raid quest done, now what', 'endgame'),
('chat_levelband_musing', 'leveling an alt because my main has nothing to do', 'endgame');

INSERT INTO `hside_corpus` (`name`, `text`, `faction_tag`) VALUES
-- chat_faction_banter: alliance (0)
('chat_faction_banter', 'the deeprun tram is still the best way between ironforge and stormwind', 0),
('chat_faction_banter', 'stormwind trade district is always packed', 0),
('chat_faction_banter', 'darnassus is beautiful but nobody ever goes there', 0),
('chat_faction_banter', 'gnomes are underrated and i will die on that hill', 0),
('chat_faction_banter', 'draenei shamans showing up still feels new somehow', 0),
('chat_faction_banter', 'the exodar is so far out of the way', 0),
('chat_faction_banter', 'boat from stormwind harbor to valiance keep, every alt', 0),
('chat_faction_banter', 'ironforge forge is where all the crafters hang out', 0),
('chat_faction_banter', 'lost another alterac valley, the horde turtled again', 0),
('chat_faction_banter', 'night elf shadowmeld saved me from a gank once', 0),
('chat_faction_banter', 'alliance players just jump around on mailboxes all day', 0),
('chat_faction_banter', 'for the alliance, i guess', 0),
-- horde (1)
('chat_faction_banter', 'orgrimmar has the best bank and auction house layout', 1),
('chat_faction_banter', 'zeppelins are slow but the view is great', 1),
('chat_faction_banter', 'undercity elevators have killed more horde than the alliance', 1),
('chat_faction_banter', 'thunder bluff is so peaceful compared to org', 1),
('chat_faction_banter', 'silvermoon is pretty but its so far from everything', 1),
('chat_faction_banter', 'tauren take up half the screen in any city', 1),
('chat_faction_banter', 'will of the forsaken is the best pvp racial, not even close', 1),
('chat_faction_banter', 'warsong hold is the first place i see in northrend every time', 1),
('chat_faction_banter', 'won another alterac valley, they never defend', 1),
('chat_faction_banter', 'trolls have the best dance', 1),
('chat_faction_banter', 'orc stun resist has saved me more than once in pvp', 1),
('chat_faction_banter', 'for the horde, obviously', 1);

INSERT INTO `hside_corpus` (`name`, `text`, `zone_tag`) VALUES
-- chat_zone_musing: elwynn forest (12)
('chat_zone_musing', 'hogger killed me twice before i got a group for him', 12),
('chat_zone_musing', 'kobold candles out of the mine take forever', 12),
('chat_zone_musing', 'defias at the pumpkin patch are always dead when i get there', 12),
-- dun morogh (1)
('chat_zone_musing', 'the wendigo cave is packed with people fighting over tags', 1),
('chat_zone_musing', 'leper gnomes outside gnomeregan hit harder than they look', 1),
('chat_zone_musing', 'need more crag boar ribs for the cooking quest in kharanos', 1),
-- teldrassil (141)
('chat_zone_musing', 'gnarlpine furbolgs everywhere, cleared that camp twice already', 141),
('chat_zone_musing', 'ban''ethil barrow den is a maze, kept running in circles', 141),
('chat_zone_musing', 'boat from rut''theran always leaves right as i get there', 141),
-- durotar (14)
('chat_zone_musing', 'zalazane on echo isles always gets tagged before i reach him', 14),
('chat_zone_musing', 'tiragarde keep is just me and ten other people killing marines', 14),
('chat_zone_musing', 'skull rock is full of burning blade cultists', 14),
-- tirisfal glades (85)
('chat_zone_musing', 'agamand mills is wall to wall zombies', 85),
('chat_zone_musing', 'scarlet crusade camps here have way too many casters', 85),
('chat_zone_musing', 'the zeppelin tower by undercity always has a crowd waiting', 85),
-- mulgore (215)
('chat_zone_musing', 'everyone in the plains is killing plainstriders for the same quest', 215),
('chat_zone_musing', 'venture co mine is crowded with goblins and players', 215),
('chat_zone_musing', 'palemane gnolls drop nothing worth looting', 215),
-- redridge mountains (44)
('chat_zone_musing', 'gnolls everywhere and they all want my coin purse', 44),
('chat_zone_musing', 'stonewatch keep is full of blackrock orcs', 44),
('chat_zone_musing', 'murlocs along lake everstill never come one at a time', 44),
-- loch modan (38)
('chat_zone_musing', 'stonesplinter troggs at the dig site are rough solo', 38),
('chat_zone_musing', 'mo''grosh ogres hit like trucks', 38),
('chat_zone_musing', 'thelsamar flight path is the only reason i stop there', 38),
-- silverpine forest (130)
('chat_zone_musing', 'pyrewood villagers turn into worgen at night', 130),
('chat_zone_musing', 'everyone at the sepulcher is looking for a shadowfang group', 130),
('chat_zone_musing', 'running between the sepulcher and pyrewood takes ages', 130),
-- the barrens (17)
('chat_zone_musing', 'still haven''t found mankrik''s wife', 17),
('chat_zone_musing', 'kolkar centaurs everywhere you look', 17),
('chat_zone_musing', 'wailing caverns groups form at crossroads constantly', 17),
-- stranglethorn vale (33)
('chat_zone_musing', 'nesingwary wants me to kill every panther in the jungle', 33),
('chat_zone_musing', 'still missing a few green hills of stranglethorn pages', 33),
('chat_zone_musing', 'gankers everywhere in stranglethorn, as usual', 33),
-- dustwallow marsh (15)
('chat_zone_musing', 'theramore guards wreck anyone who wanders in', 15),
('chat_zone_musing', 'mudsprocket is the only neutral flight point out here', 15),
('chat_zone_musing', 'crocolisks in the swamp aggro from way too far', 15),
-- borean tundra (3537)
('chat_zone_musing', 'the nexus is the first dungeon most people run up here', 3537),
('chat_zone_musing', 'coldarra quests are full of blue dragonflight', 3537),
('chat_zone_musing', 'half of northrend arrives here off the boat or the zeppelin', 3537),
-- howling fjord (495)
('chat_zone_musing', 'utgarde keep is right there so groups form fast', 495),
('chat_zone_musing', 'kamagua is a long run down the coast but the tuskarr quests are good', 495),
('chat_zone_musing', 'vrykul everywhere, they all hit hard', 495),
-- dragonblight (65)
('chat_zone_musing', 'the wrathgate questline is still the best in the game', 65),
('chat_zone_musing', 'naxxramas floating over the zone is creepy', 65),
('chat_zone_musing', 'azjol-nerub and ahn''kahet are both right here', 65),
-- icecrown (210)
('chat_zone_musing', 'argent tournament dailies every single day', 210),
('chat_zone_musing', 'shadow vault quests are worth doing for the ebon blade', 210),
('chat_zone_musing', 'forge of souls, pit of saron and halls of reflection, all right here', 210);

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
-- channel_trade_wts: 15, %item_link exactly once each
('channel_trade_wts', 'wts %item_link, cheaper than the ah'),
('channel_trade_wts', '%item_link for sale, whisper me'),
('channel_trade_wts', 'selling %item_link, need the bag space'),
('channel_trade_wts', 'wts %item_link, will take offers'),
('channel_trade_wts', 'anyone want %item_link before i vendor it'),
('channel_trade_wts', 'got %item_link, whisper an offer'),
('channel_trade_wts', 'wts %item_link, no lowballs please'),
('channel_trade_wts', 'clearing my bank, %item_link up for sale'),
('channel_trade_wts', '%item_link, selling cheap, pst'),
('channel_trade_wts', 'have %item_link if anyone is looking'),
('channel_trade_wts', 'wts %item_link, quick sale'),
('channel_trade_wts', 'selling %item_link, can meet at the bank'),
('channel_trade_wts', 'wts %item_link, ah is a ripoff right now'),
('channel_trade_wts', 'got a spare %item_link, make me an offer'),
('channel_trade_wts', 'wts %item_link, whisper if interested');

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
-- opener_group_formed: 15 (party/raid chat; joiner may be bot or player)
('opener_group_formed', 'hey all'),
('opener_group_formed', 'hi, what are we doing'),
('opener_group_formed', 'o/'),
('opener_group_formed', 'sup'),
('opener_group_formed', 'hey, where we heading'),
('opener_group_formed', 'ready when you are'),
('opener_group_formed', 'hi, just need to repair first'),
('opener_group_formed', 'hey, let me grab some food first'),
('opener_group_formed', 'yo'),
('opener_group_formed', 'hey, what''s the plan'),
('opener_group_formed', 'hi, give me a sec'),
('opener_group_formed', 'hey, who''s tanking'),
('opener_group_formed', 'hey everyone'),
('opener_group_formed', 'hi, i''ll be right there'),
('opener_group_formed', 'cool, let''s go'),
-- opener_joint_kill: 15 (/say; bot landed killing blow on any creature)
('opener_joint_kill', 'nice'),
('opener_joint_kill', 'got it'),
('opener_joint_kill', 'next'),
('opener_joint_kill', 'that''s one'),
('opener_joint_kill', 'easy'),
('opener_joint_kill', 'nice, keep going'),
('opener_joint_kill', 'that one went down fast'),
('opener_joint_kill', 'anyone need to loot that'),
('opener_joint_kill', 'down'),
('opener_joint_kill', 'that one hit harder than i thought'),
('opener_joint_kill', 'took long enough'),
('opener_joint_kill', 'dead, moving on'),
('opener_joint_kill', 'good, on to the next'),
('opener_joint_kill', 'smooth'),
('opener_joint_kill', 'nice teamwork'),
-- opener_rez: 15 (/say; the speaker is the one who was resurrected)
('opener_rez', 'ok, back up'),
('opener_rez', 'i''m up'),
('opener_rez', 'back'),
('opener_rez', 'didn''t see that coming'),
('opener_rez', 'that was dumb of me'),
('opener_rez', 'my bad on that one'),
('opener_rez', 'ok, need a sec to rebuff'),
('opener_rez', 'need to drink before we go again'),
('opener_rez', 'up, let''s try that again'),
('opener_rez', 'well that was embarrassing'),
('opener_rez', 'alright, i''m good'),
('opener_rez', 'my gear is gonna need a repair after that'),
('opener_rez', 'never again'),
('opener_rez', 'lost all my buffs, great'),
('opener_rez', 'phew, back in it'),
-- opener_dungeon_complete: 15 (/say; last boss of the instance just died)
('opener_dungeon_complete', 'gg'),
('opener_dungeon_complete', 'gg all'),
('opener_dungeon_complete', 'anything good drop'),
('opener_dungeon_complete', 'another one?'),
('opener_dungeon_complete', 'that''s the run, ty all'),
('opener_dungeon_complete', 'i need to repair after that'),
('opener_dungeon_complete', 'nice, done'),
('opener_dungeon_complete', 'good group'),
('opener_dungeon_complete', 'smooth run'),
('opener_dungeon_complete', 'bags are full of junk now'),
('opener_dungeon_complete', 'nothing for me but that was fun'),
('opener_dungeon_complete', 'quick one'),
('opener_dungeon_complete', 'ty for the run'),
('opener_dungeon_complete', 'that went well'),
('opener_dungeon_complete', 'done, time to hearth'),
-- opener_prolonged_proximity: 15 (/say; ungrouped stranger, both idle 90s+)
('opener_prolonged_proximity', 'hey'),
('opener_prolonged_proximity', 'how''s it going'),
('opener_prolonged_proximity', 'you waiting on something too'),
('opener_prolonged_proximity', 'you afk or just chilling'),
('opener_prolonged_proximity', 'quiet around here'),
('opener_prolonged_proximity', 'what are you working on'),
('opener_prolonged_proximity', 'need a hand with anything'),
('opener_prolonged_proximity', 'been standing here a while, huh'),
('opener_prolonged_proximity', 'sup'),
('opener_prolonged_proximity', 'busy spot'),
('opener_prolonged_proximity', 'you questing around here'),
('opener_prolonged_proximity', 'hey, you need anything'),
('opener_prolonged_proximity', 'just taking a break'),
('opener_prolonged_proximity', 'waiting on a respawn?'),
('opener_prolonged_proximity', 'how''s your day');
