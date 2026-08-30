-- Claude/archive/PLAN-TRADE.md (frozen 2026-08-25): the TRADE_PRICE grounded answer: "how much?" for
-- something in the bot's bags, priced with mod-playerbots' own selling
-- arithmetic so the number said in chat is the number the trade window then
-- demands.
--
-- ---- Why this is a new base/ file, not edits to the two it extends
--
-- Same reasoning as hside_corpus_category_ambient_group.sql. Both
-- hside_grounded_question.sql and hside_grounded_template.sql are already
-- SHA1-tracked in the `updates` table on the deployed realm, and
-- UpdateFetcher re-applies a base/ file whose hash changed. Re-applying
-- hside_grounded_question.sql would collide on its UNIQUE phrase key and
-- hard-fail the boot; re-applying hside_grounded_template.sql would silently
-- double every template, since that table has no unique key to collide on.
-- A brand-new base/ file has no tracked hash, so it applies exactly once on
-- both a fresh install and an already-deployed realm. No companion updates/
-- file, because that would duplicate this content: the specific mistake
-- CLAUDE.md records breaking a realm boot three times.
--
-- The filename sorts after both files it depends on ('q' < 't', and
-- "template" < "trade_price" at the first differing character), which
-- matters only on a fresh install where those files CREATE the tables.
--
-- ---- On the fact shape
--
-- hs_handler.cpp builds `fact` as the price AND the item together --
-- "2g50s for [Bolt of Mageweave]", or "5g for 20x [Bolt of Mageweave]" for a
-- stack. The item is named rather than left implicit because this module
-- does not record which item a past WTS line advertised, so the bot quotes
-- for whatever tradeable item it draws now. Naming it keeps the answer
-- coherent on its own terms rather than a non-sequitur about a different
-- item than the player had in mind.
--
-- The templates below therefore wrap a phrase that already reads as a
-- complete offer; they must not add their own "for" or name an item.

INSERT INTO `hside_grounded_question` (`kind`, `phrase`) VALUES
('TRADE_PRICE', 'how much'),
('TRADE_PRICE', 'how much for it'),
('TRADE_PRICE', 'how much for that'),
('TRADE_PRICE', 'how much for'),
('TRADE_PRICE', 'how much is it'),
('TRADE_PRICE', 'how much do you want'),
('TRADE_PRICE', 'how much you want'),
('TRADE_PRICE', 'how much you asking'),
('TRADE_PRICE', 'how much are you asking'),
('TRADE_PRICE', 'how much does it cost'),
('TRADE_PRICE', 'what do you want for it'),
('TRADE_PRICE', 'what are you asking'),
('TRADE_PRICE', 'what''s the price'),
('TRADE_PRICE', 'whats the price'),
('TRADE_PRICE', 'what''s it cost'),
('TRADE_PRICE', 'whats it cost'),
('TRADE_PRICE', 'wat u want for it'),
('TRADE_PRICE', 'name your price'),
('TRADE_PRICE', 'price'),
('TRADE_PRICE', 'cost');

-- has_fact=1 wraps the price-and-item phrase; has_fact=0 is the flat
-- "carrying nothing sellable" pool, which is a true answer rather than an
-- invented one. It also covers the two cases where a price would be a lie:
-- an item below normal quality (which mod-playerbots prices at 0) and an
-- item with no SellPrice (which it refuses outright as "not for sale").
INSERT INTO `hside_grounded_template` (`kind`, `has_fact`, `uses_fact`, `prefix`, `suffix`) VALUES
('TRADE_PRICE', 1, 1, '', ''),
('TRADE_PRICE', 1, 1, 'asking ', ''),
('TRADE_PRICE', 1, 1, 'looking for ', ''),
('TRADE_PRICE', 1, 1, 'want ', ''),
('TRADE_PRICE', 1, 1, 'about ', ''),
('TRADE_PRICE', 1, 1, '', ', pst'),
('TRADE_PRICE', 1, 1, '', ' if you want it'),
('TRADE_PRICE', 1, 1, '', ' and it''s yours'),
('TRADE_PRICE', 0, 0, 'nothing worth selling on me right now', ''),
('TRADE_PRICE', 0, 0, 'not selling anything at the moment', ''),
('TRADE_PRICE', 0, 0, 'got nothing to sell right now', ''),
('TRADE_PRICE', 0, 0, 'nothing in my bags worth your gold', ''),
('TRADE_PRICE', 0, 0, 'not much on me at the moment', '');
