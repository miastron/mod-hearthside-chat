-- Claude/archive/PLAN-AMBIENT.md §4: party/raid ambient content: the three categories
-- hs_ambient.cpp's Hs_SelectGroupAmbientLine draws from, plus their seed
-- rows.
--
-- ---- Why this is a new base/ file rather than edits to the two it extends
--
-- The rows below belong logically in hside_corpus_category.sql and
-- hside_corpus.sql, and they are deliberately NOT written there. Both of
-- those files are already SHA1-tracked in the `updates` table on the
-- deployed realm, and UpdateFetcher re-applies a base/ file whose hash has
-- changed. Re-applying hside_corpus.sql would insert every one of its
-- existing seed rows a second time: that table has only an AUTO_INCREMENT
-- PRIMARY KEY and a non-unique idx_name, so nothing would collide and
-- nothing would be skipped: the corpus would silently double. (This is the
-- same hazard CLAUDE.md records from three separate realm-boot breakages;
-- the failure shape there was a duplicate-key error, which at least stops
-- the boot. Silent duplication is worse, because the realm comes up fine and
-- the anti-repeat selection just quietly degrades.)
--
-- A brand-new base/ file has no tracked hash, so it applies exactly once, on
-- both a fresh install and an already-deployed realm, which is precisely the
-- semantics new seed rows need. This is also why there is no companion
-- updates/ file: writing one would duplicate this file's content, which is
-- the specific mistake CLAUDE.md warns about.
--
-- The filename sorts after both files it depends on (base/ applies
-- alphabetically, and '.' < '_' in ASCII, so hside_corpus.sql and
-- hside_corpus_category.sql both precede hside_corpus_category_ambient_group.sql).
-- That ordering matters only on a fresh install, where those two files are
-- what CREATE the tables inserted into below.
--
-- ---- Why party/raid need their own categories
--
-- The five chat_* categories do not transfer. A /say musing is overheard by
-- whoever happens to be standing nearby; a party line is addressed to four
-- specific people who are doing something together. The register is
-- different, so the content is.
--
-- All three are tag_axis 'none'. The travel category is zone-flavoured
-- through the %zone universal placeholder rather than a zone tag_axis:
-- zone-tagged rows only select for zones that have authored rows
-- (chat_zone_musing seeds a curated slice, not all of Azeroth), so a
-- zone-axis travel category would be empty and silently dead for most of
-- the map. %zone resolves off the bot's live zone for every zone there is.
--
-- The `channel` column carries 'party'/'raid' here, extending the
-- trade|general values it held before. Nothing else has to change for
-- that: Hs_SelectCorpusLine's `channel IS NULL` filter already excludes
-- every non-NULL value, so these rows stay out of the /say and direct-reply
-- pool by the same mechanism the channel_* categories already do.

INSERT IGNORE INTO `hside_corpus_category` (`name`, `tag_axis`, `card_gated`, `channel`, `is_opener`) VALUES
('ambient_party_downtime', 'none', 0, 'party', 0),
('ambient_party_travel',   'none', 0, 'party', 0),
('ambient_raid_downtime',  'none', 0, 'raid',  0);

-- Text is clean, grammatical prose with no style baked in: typos,
-- abbreviation and casing are applied at delivery by hs_style.cpp, never
-- stored (see hside_corpus.sql's header).
--
-- No INSERT IGNORE here: hside_corpus has no UNIQUE key for it to act on,
-- so it would not skip anything, it would just hide a genuine double-apply.
-- A plain INSERT in a never-before-hashed file applies exactly once.
INSERT INTO `hside_corpus` (`name`, `text`) VALUES
-- ambient_party_downtime: between pulls, addressed to the group. Small,
-- low-stakes, and deliberately not asking anything: an ambient line that
-- ends in a question invites a reply the bot has no mechanism to follow up
-- on, which reads worse than saying nothing.
('ambient_party_downtime', 'nice pull, that could have gone worse'),
('ambient_party_downtime', 'give me a sec, drinking'),
('ambient_party_downtime', 'good group so far, no complaints here'),
('ambient_party_downtime', 'that last one got a bit close'),
('ambient_party_downtime', 'taking a quick breather'),
('ambient_party_downtime', 'bags are filling up fast on this run'),
('ambient_party_downtime', 'ready when everyone else is'),
('ambient_party_downtime', 'that went smoother than i expected'),
('ambient_party_downtime', 'still got food if anyone needs some'),
('ambient_party_downtime', 'nearly out of mana, one more sec'),

-- ambient_party_travel: on the way somewhere. %zone resolves off the bot's
-- live zone, so these work anywhere on the map.
('ambient_party_travel', 'long ride across %zone'),
('ambient_party_travel', 'always forget how big %zone is'),
('ambient_party_travel', 'right behind you'),
('ambient_party_travel', 'shout if i fall behind'),
('ambient_party_travel', 'been a while since i was out in %zone'),
('ambient_party_travel', 'almost there i think'),
('ambient_party_travel', 'taking the long way round again'),
('ambient_party_travel', '%zone never gets any smaller'),
('ambient_party_travel', 'following along'),
('ambient_party_travel', 'this road goes on forever'),

-- ambient_raid_downtime: larger group, so more anonymous and less 1:1 than
-- the party set: nobody is addressing anyone in particular, and a line
-- that assumes it will be noticed reads wrong at twenty-five people.
('ambient_raid_downtime', 'ready here'),
('ambient_raid_downtime', 'repairing quick, one moment'),
('ambient_raid_downtime', 'good attempt everyone'),
('ambient_raid_downtime', 'buffs are up on my end'),
('ambient_raid_downtime', 'back in a sec, grabbing water'),
('ambient_raid_downtime', 'that was closer than the last one'),
('ambient_raid_downtime', 'set here whenever we go'),
('ambient_raid_downtime', 'still got flasks if anyone runs short'),
('ambient_raid_downtime', 'nice recovery on that one'),
('ambient_raid_downtime', 'taking five, back shortly');
